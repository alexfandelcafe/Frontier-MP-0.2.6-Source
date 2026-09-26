#include "frontier/client/cef_overlay.hpp"
#include "frontier/game/rdr_bridge.hpp"

#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_render_handler.h"

namespace frontier::client {

namespace {

using Microsoft::WRL::ComPtr;

void log_line(const std::string& line) {
    char localAppData[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;

    const std::filesystem::path dir =
        std::filesystem::path(localAppData) / "FrontierMP" / "logs";

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream file(dir / "client.log", std::ios::app);
    if (file) file << line << "\n";
}

std::filesystem::path module_directory() {
    HMODULE module = GetModuleHandleA("FrontierClient.dll");
    if (module == nullptr) return {};

    char path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};

    return std::filesystem::path(path).parent_path();
}

HWND find_rdr_window() {
    struct Candidate {
        HWND hwnd{};
        long long area{};
    } candidate;

    EnumWindows(
        [](HWND hwnd, LPARAM parameter) -> BOOL {
            auto* candidate = reinterpret_cast<Candidate*>(parameter);
            DWORD processId = 0;
            GetWindowThreadProcessId(hwnd, &processId);
            if (processId != GetCurrentProcessId()) return TRUE;
            if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;

            const LONG_PTR style = GetWindowLongPtrA(hwnd, GWL_STYLE);
            if ((style & WS_CHILD) != 0) return TRUE;

            RECT rect{};
            if (!GetWindowRect(hwnd, &rect)) return TRUE;

            const long long width = std::max<LONG>(0, rect.right - rect.left);
            const long long height = std::max<LONG>(0, rect.bottom - rect.top);
            const long long area = width * height;
            if (area > candidate->area) {
                candidate->hwnd = hwnd;
                candidate->area = area;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&candidate));

    return candidate.hwnd;
}

std::string file_url(const std::filesystem::path& path) {
    std::string native = path.lexically_normal().string();
    std::replace(native.begin(), native.end(), '\\', '/');

    std::string encoded;
    encoded.reserve(native.size() + 32);
    for (const unsigned char ch : native) {
        if ((ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '/' || ch == ':') {
            encoded.push_back(static_cast<char>(ch));
        } else {
            constexpr char hex[] = "0123456789ABCDEF";
            encoded.push_back('%');
            encoded.push_back(hex[(ch >> 4) & 0x0F]);
            encoded.push_back(hex[ch & 0x0F]);
        }
    }

    return "file:///" + encoded;
}

class FrontierCefApp final : public CefApp {
public:
    void OnBeforeCommandLineProcessing(
        const CefString& processType,
        CefRefPtr<CefCommandLine> commandLine) override {
        if (commandLine != nullptr) {
            commandLine->AppendSwitch("do-not-de-elevate");
            commandLine->AppendSwitch("disable-gpu");
            commandLine->AppendSwitch("disable-gpu-compositing");
        }

        log_line(
            "[FrontierCEF] command line hook processType=" +
            processType.ToString() +
            " switches=do-not-de-elevate,disable-gpu,disable-gpu-compositing");
    }

    IMPLEMENT_REFCOUNTING(FrontierCefApp);
};

class RenderHandler final : public CefRenderHandler {
public:
    explicit RenderHandler(CefOverlay* owner) : owner_(owner) {}

    bool GetViewRect(
        CefRefPtr<CefBrowser>,
        CefRect& rect) override {
        if (owner_ == nullptr) return false;

        const HWND window = owner_window_;
        if (window == nullptr || !IsWindow(window)) {
            return false;
        }

        RECT client{};
        if (!GetClientRect(window, &client)) return false;

        const int width = std::max<LONG>(1, client.right - client.left);
        const int height = std::max<LONG>(1, client.bottom - client.top);
        rect = CefRect(0, 0, width, height);
        return true;
    }

    void OnPaint(
        CefRefPtr<CefBrowser>,
        PaintElementType type,
        const RectList&,
        const void* buffer,
        int width,
        int height) override {
        if (type != PET_VIEW || owner_ == nullptr || buffer == nullptr) return;
        owner_->accept_paint(buffer, width, height);
    }

    void set_window(HWND window) {
        owner_window_ = window;
    }

private:
    CefOverlay* owner_{};
    HWND owner_window_{};

    IMPLEMENT_REFCOUNTING(RenderHandler);
};

class FrontierCefClient final
    : public CefClient
    , public CefLifeSpanHandler
    , public CefLoadHandler {
public:
    FrontierCefClient(CefOverlay* owner, RenderHandler* renderHandler)
        : owner_(owner),
          renderHandler_(renderHandler) {}

    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
        return this;
    }

    CefRefPtr<CefLoadHandler> GetLoadHandler() override {
        return this;
    }

    CefRefPtr<CefRenderHandler> GetRenderHandler() override {
        return renderHandler_;
    }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        browser_ = browser;
        log_line("[FrontierCEF] browser created (OSR)");
    }

    void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
        if (browser_ != nullptr &&
            browser != nullptr &&
            browser_->GetIdentifier() == browser->GetIdentifier()) {
            browser_ = nullptr;
        }
        log_line("[FrontierCEF] browser closing");
    }

    void OnLoadEnd(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        int httpStatusCode) override {
        if (frame != nullptr && frame->IsMain()) {
            std::ostringstream message;
            message << "[FrontierCEF] page loaded browser="
                    << (browser != nullptr ? browser->GetIdentifier() : 0)
                    << " httpStatus=" << httpStatusCode;
            log_line(message.str());
        }
    }

    void OnLoadError(
        CefRefPtr<CefBrowser>,
        CefRefPtr<CefFrame> frame,
        ErrorCode errorCode,
        const CefString& errorText,
        const CefString& failedUrl) override {
        if (frame == nullptr || !frame->IsMain()) return;

        std::ostringstream message;
        message << "[FrontierCEF] page load error code="
                << static_cast<int>(errorCode)
                << " text=" << errorText.ToString()
                << " url=" << failedUrl.ToString();
        log_line(message.str());
    }

    CefRefPtr<CefBrowser> browser() const {
        return browser_;
    }

private:
    CefOverlay* owner_{};
    CefRefPtr<RenderHandler> renderHandler_{};
    CefRefPtr<CefBrowser> browser_{};

    IMPLEMENT_REFCOUNTING(FrontierCefClient);
};

struct ImportPatch final {
    void** slot{};
    void* original{};
};

struct PresentHookState final {
    using CreateDeviceAndSwapChainProc = HRESULT(WINAPI*)(
        IDXGIAdapter*,
        D3D_DRIVER_TYPE,
        HMODULE,
        UINT,
        const D3D_FEATURE_LEVEL*,
        UINT,
        UINT,
        const DXGI_SWAP_CHAIN_DESC*,
        IDXGISwapChain**,
        ID3D11Device**,
        D3D_FEATURE_LEVEL*,
        ID3D11DeviceContext**);

    using PresentProc = HRESULT(STDMETHODCALLTYPE*)(
        IDXGISwapChain*,
        UINT,
        UINT);

    std::mutex mutex;
    std::atomic<CefOverlay*> overlay{nullptr};
    HMODULE rdrModule{};
    ImportPatch createImport{};
    CreateDeviceAndSwapChainProc createOriginal{};
    PresentProc presentOriginal{};
    IDXGISwapChain* hookedSwapChain{};
    std::array<void*, 18> hookedVtable{};
    std::array<void*, 18> originalVtable{};
    void** originalVtableAddress{};
    bool swapchainHooked{};
    bool installed{};
};

PresentHookState g_presentHook{};
std::atomic<WNDPROC> g_originalWindowProc{nullptr};

bool replace_pointer(void** slot, void* replacement, void*& original, std::string& error) {
    if (slot == nullptr || replacement == nullptr) {
        error = "invalid IAT patch slot/replacement";
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(
            slot,
            sizeof(void*),
            PAGE_READWRITE,
            &oldProtect)) {
        error = "VirtualProtect(IAT) failed";
        return false;
    }

    original = *slot;
    *slot = replacement;

    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

bool find_and_patch_import(
    HMODULE module,
    const char* importedModule,
    const char* importedName,
    void* replacement,
    ImportPatch& outPatch,
    std::string& error) {
    error.clear();
    outPatch = {};

    if (module == nullptr) {
        error = "null module";
        return false;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto* dos =
        reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        error = "invalid DOS header";
        return false;
    }

    const auto* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        error = "invalid PE64 header";
        return false;
    }

    const auto& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) {
        error = "no import directory";
        return false;
    }

    const auto* descriptors =
        reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(
            base + directory.VirtualAddress);

    for (const auto* descriptor = descriptors;
         descriptor->Name != 0;
         ++descriptor) {
        const char* moduleName =
            reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(moduleName, importedModule) != 0) continue;

        auto* lookup =
            reinterpret_cast<IMAGE_THUNK_DATA64*>(
                base + descriptor->OriginalFirstThunk);
        auto* iat =
            reinterpret_cast<IMAGE_THUNK_DATA64*>(
                base + descriptor->FirstThunk);

        if (lookup == nullptr || descriptor->OriginalFirstThunk == 0) {
            error = "import lookup table unavailable";
            return false;
        }

        for (std::size_t index = 0;
             lookup[index].u1.AddressOfData != 0;
             ++index) {
            const auto ordinal =
                lookup[index].u1.Ordinal;
            if (IMAGE_SNAP_BY_ORDINAL64(ordinal)) continue;

            const auto* byName =
                reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                    base + lookup[index].u1.AddressOfData);
            if (std::strcmp(
                    reinterpret_cast<const char*>(byName->Name),
                    importedName) != 0) {
                continue;
            }

            void** slot =
                reinterpret_cast<void**>(&iat[index].u1.Function);
            if (!replace_pointer(
                    slot,
                    replacement,
                    outPatch.original,
                    error)) {
                return false;
            }

            outPatch.slot = slot;
            return true;
        }
    }

    error = "import not found";
    return false;
}

void restore_import(ImportPatch& patch) {
    if (patch.slot == nullptr || patch.original == nullptr) return;

    DWORD oldProtect = 0;
    if (VirtualProtect(
            patch.slot,
            sizeof(void*),
            PAGE_READWRITE,
            &oldProtect)) {
        *patch.slot = patch.original;
        DWORD ignored = 0;
        VirtualProtect(patch.slot, sizeof(void*), oldProtect, &ignored);
        FlushInstructionCache(
            GetCurrentProcess(),
            patch.slot,
            sizeof(void*));
    }

    patch = {};
}

HRESULT STDMETHODCALLTYPE frontier_present(
    IDXGISwapChain* swapChain,
    UINT syncInterval,
    UINT flags);

void hook_swapchain(PresentHookState& state, IDXGISwapChain* swapChain) {
    if (swapChain == nullptr || state.swapchainHooked) return;

    void*** vtable =
        reinterpret_cast<void***>(swapChain);
    if (vtable == nullptr || *vtable == nullptr) return;

    for (std::size_t i = 0; i < state.originalVtable.size(); ++i) {
        state.originalVtable[i] = (*vtable)[i];
        state.hookedVtable[i] = (*vtable)[i];
    }

    state.presentOriginal =
        reinterpret_cast<PresentHookState::PresentProc>(
            state.originalVtable[8]);
    if (state.presentOriginal == nullptr) {
        state.originalVtable.fill(nullptr);
        state.hookedVtable.fill(nullptr);
        return;
    }

    state.hookedVtable[8] =
        reinterpret_cast<void*>(&frontier_present);

    state.originalVtableAddress = *vtable;
    *vtable = state.hookedVtable.data();
    state.hookedSwapChain = swapChain;
    state.swapchainHooked = true;

    DXGI_SWAP_CHAIN_DESC desc{};
    if (SUCCEEDED(swapChain->GetDesc(&desc))) {
        std::ostringstream message;
        message << "[FrontierD3D] hooked swapchain="
                << static_cast<const void*>(swapChain)
                << " hwnd=" << desc.OutputWindow
                << " size=" << desc.BufferDesc.Width
                << "x" << desc.BufferDesc.Height
                << " format=" << static_cast<int>(desc.BufferDesc.Format);
        log_line(message.str());
    } else {
        log_line("[FrontierD3D] hooked swapchain (GetDesc failed)");
    }
}

HRESULT WINAPI frontier_create_device_and_swapchain(
    IDXGIAdapter* adapter,
    D3D_DRIVER_TYPE driverType,
    HMODULE software,
    UINT flags,
    const D3D_FEATURE_LEVEL* featureLevels,
    UINT featureLevelCount,
    UINT sdkVersion,
    const DXGI_SWAP_CHAIN_DESC* swapChainDesc,
    IDXGISwapChain** swapChain,
    ID3D11Device** device,
    D3D_FEATURE_LEVEL* featureLevel,
    ID3D11DeviceContext** immediateContext) {
    std::lock_guard lock(g_presentHook.mutex);
    const auto original = g_presentHook.createOriginal;
    if (original == nullptr) return E_FAIL;

    const HRESULT result = original(
        adapter,
        driverType,
        software,
        flags,
        featureLevels,
        featureLevelCount,
        sdkVersion,
        swapChainDesc,
        swapChain,
        device,
        featureLevel,
        immediateContext);

    if (SUCCEEDED(result) &&
        swapChain != nullptr &&
        *swapChain != nullptr) {
        hook_swapchain(g_presentHook, *swapChain);
    }

    return result;
}

void unhook_render_path() {
    std::lock_guard lock(g_presentHook.mutex);

    g_presentHook.overlay.store(nullptr, std::memory_order_release);

    if (g_presentHook.swapchainHooked &&
        g_presentHook.hookedSwapChain != nullptr) {
        void*** vtable =
            reinterpret_cast<void***>(g_presentHook.hookedSwapChain);
        if (vtable != nullptr &&
            g_presentHook.originalVtableAddress != nullptr) {
            *vtable = g_presentHook.originalVtableAddress;
        }
    }

    g_presentHook.hookedSwapChain = nullptr;
    g_presentHook.hookedVtable.fill(nullptr);
    g_presentHook.originalVtable.fill(nullptr);
    g_presentHook.originalVtableAddress = nullptr;
    g_presentHook.presentOriginal = nullptr;
    g_presentHook.swapchainHooked = false;

    restore_import(g_presentHook.createImport);
    g_presentHook.createOriginal = nullptr;
    g_presentHook.rdrModule = nullptr;
    g_presentHook.installed = false;

    log_line("[FrontierD3D] render path unhooked");
}

bool install_render_path(CefOverlay* overlay) {
    std::lock_guard lock(g_presentHook.mutex);

    if (g_presentHook.installed) {
        g_presentHook.overlay.store(overlay, std::memory_order_release);
        return true;
    }

    g_presentHook.rdrModule = GetModuleHandleA("RDR.exe");
    if (g_presentHook.rdrModule == nullptr) {
        log_line("[FrontierD3D] RDR.exe module unavailable while installing render hook");
        return false;
    }

    std::string error;
    void* original = nullptr;
    if (!find_and_patch_import(
            g_presentHook.rdrModule,
            "d3d11.dll",
            "D3D11CreateDeviceAndSwapChain",
            reinterpret_cast<void*>(&frontier_create_device_and_swapchain),
            g_presentHook.createImport,
            error)) {
        log_line(
            "[FrontierD3D] D3D11CreateDeviceAndSwapChain IAT hook unavailable: " +
            error);
        return false;
    }

    g_presentHook.createOriginal =
        reinterpret_cast<PresentHookState::CreateDeviceAndSwapChainProc>(
            original);
    g_presentHook.overlay.store(overlay, std::memory_order_release);
    g_presentHook.installed = true;

    log_line("[FrontierD3D] D3D11CreateDeviceAndSwapChain IAT hook installed");
    return true;
}

HRESULT STDMETHODCALLTYPE frontier_present(
    IDXGISwapChain* swapChain,
    UINT syncInterval,
    UINT flags) {
    PresentHookState::PresentProc original = nullptr;
    CefOverlay* overlay = nullptr;

    bool shouldRender = false;
    {
        std::lock_guard lock(g_presentHook.mutex);
        original = g_presentHook.presentOriginal;
        overlay = g_presentHook.overlay.load(std::memory_order_acquire);
        shouldRender =
            overlay != nullptr &&
            original != nullptr &&
            swapChain == g_presentHook.hookedSwapChain;
    }

    if (shouldRender && overlay != nullptr) {
        overlay->on_present(swapChain);
    }

    return original != nullptr
        ? original(swapChain, syncInterval, flags)
        : E_FAIL;
}

constexpr char kVertexShader[] = R"(
struct VSInput {
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.position = float4(input.position, 1.0);
    output.uv = input.uv;
    return output;
}
)";

constexpr char kPixelShader[] = R"(
Texture2D uiTexture : register(t0);
SamplerState uiSampler : register(s0);

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 PSMain(PSInput input) : SV_TARGET {
    return uiTexture.Sample(uiSampler, input.uv);
}
)";

bool compile_shader(
    const char* source,
    const char* entry,
    const char* target,
    ComPtr<ID3DBlob>& shader,
    std::string& error) {
    ComPtr<ID3DBlob> diagnostics;

    const HRESULT result = D3DCompile(
        source,
        std::strlen(source),
        "FrontierMP_CEF_OSR",
        nullptr,
        nullptr,
        entry,
        target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        shader.GetAddressOf(),
        diagnostics.GetAddressOf());

    if (FAILED(result)) {
        error = "D3DCompile failed";
        if (diagnostics != nullptr && diagnostics->GetBufferPointer() != nullptr) {
            error += ": ";
            error.append(
                static_cast<const char*>(diagnostics->GetBufferPointer()),
                diagnostics->GetBufferSize());
        }
        return false;
    }

    return true;
}

} // namespace

struct CefOverlay::State final {
    std::filesystem::path moduleDir;
    std::filesystem::path cefDir;
    std::filesystem::path subprocess;
    std::filesystem::path ui;

    HWND gameWindow{};
    WNDPROC originalWindowProc{};
    bool windowSubclassed{};

    CefRefPtr<FrontierCefApp> app{};
    CefRefPtr<RenderHandler> renderHandler{};
    CefRefPtr<FrontierCefClient> client{};
    CefRefPtr<CefBrowser> browser{};
    bool initialized{};

    std::mutex frameMutex;
    std::vector<std::uint8_t> frame{};
    int frameWidth{};
    int frameHeight{};
    std::uint64_t frameGeneration{};
    bool firstPaintLogged{};

    ComPtr<ID3D11Device> d3dDevice{};
    ComPtr<ID3D11DeviceContext> d3dContext{};
    ComPtr<ID3D11Texture2D> uiTexture{};
    ComPtr<ID3D11ShaderResourceView> uiTextureView{};
    ComPtr<ID3D11VertexShader> vertexShader{};
    ComPtr<ID3D11PixelShader> pixelShader{};
    ComPtr<ID3D11InputLayout> inputLayout{};
    ComPtr<ID3D11SamplerState> sampler{};
    ComPtr<ID3D11BlendState> blendState{};
    ComPtr<ID3D11RasterizerState> rasterizerState{};
    std::uint64_t uploadedGeneration{};
};

CefOverlay::CefOverlay() = default;

CefOverlay::~CefOverlay() {
    stop();
}

bool CefOverlay::start(frontier::game::RdrBridge& bridge) {
    if (thread_.joinable()) return true;

    bridge_ = &bridge;
    stopRequested_.store(false, std::memory_order_release);

    // Install the IAT hook before FrontierClient signals the suspended
    // launcher. This catches RDR's D3D11 swapchain creation without requiring
    // a risky global detour of dxgi.dll/d3d11.dll.
    if (!install_render_path(this)) {
        log_line("[FrontierCEF] D3D render path not installed; CEF will still initialize for diagnostics");
    }

    try {
        thread_ = std::thread([this] { thread_main(); });
    } catch (...) {
        unhook_render_path();
        bridge_ = nullptr;
        log_line("[FrontierCEF] failed to create coordinator thread");
        return false;
    }

    log_line("[FrontierCEF] coordinator thread started; waiting for game-thread dispatcher");
    return true;
}

void CefOverlay::stop() {
    stopRequested_.store(true, std::memory_order_release);

    if (bridge_ != nullptr &&
        bridge_->game_thread_dispatcher_attached()) {
        std::string error;
        if (!bridge_->submit_game_thread_and_wait(
                [this] { shutdown_on_game_thread(); },
                5000u,
                error) &&
            !error.empty()) {
            log_line("[FrontierCEF] game-thread shutdown request failed: " + error);
        }
    }

    if (thread_.joinable()) {
        thread_.join();
    }

    unhook_render_path();
    bridge_ = nullptr;
}

void CefOverlay::thread_main() {
    while (!stopRequested_.load(std::memory_order_acquire)) {
        if (bridge_ != nullptr &&
            bridge_->game_thread_dispatcher_attached()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (stopRequested_.load(std::memory_order_acquire)) return;
    if (bridge_ == nullptr) {
        log_line("[FrontierCEF] coordinator lost RDR bridge");
        return;
    }

    std::string error;
    const bool submitted =
        bridge_->submit_game_thread_and_wait(
            [this] {
                const bool initialized = initialize_on_game_thread();
                log_line(
                    std::string("[FrontierCEF] game-thread initialization result=") +
                    (initialized ? "success" : "failure"));
            },
            15000u,
            error);

    if (!submitted) {
        log_line(
            "[FrontierCEF] game-thread initialization request failed: " +
            (error.empty() ? "unknown error" : error));
        return;
    }

    while (!stopRequested_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

bool CefOverlay::initialize_on_game_thread() {
    if (state_ == nullptr) {
        state_ = std::make_unique<State>();
    }

    State& state = *state_;
    if (state.initialized) return true;

    state.moduleDir = module_directory();
    if (state.moduleDir.empty()) {
        log_line("[FrontierCEF] module directory unavailable");
        return false;
    }

    state.cefDir = state.moduleDir / "cef";
    state.subprocess = state.moduleDir / "FrontierCefSubprocess.exe";
    state.ui = state.cefDir / "cef_ui" / "mainmenu" / "index.html";

    if (!std::filesystem::exists(state.subprocess)) {
        log_line("[FrontierCEF] subprocess executable missing: " + state.subprocess.string());
        return false;
    }

    if (!std::filesystem::exists(state.ui)) {
        log_line("[FrontierCEF] main menu HTML missing: " + state.ui.string());
        return false;
    }

    log_line(
        "[FrontierCEF] CefInitialize thread=" +
        std::to_string(static_cast<unsigned long>(GetCurrentThreadId())));
    log_line("[FrontierCEF] using historical OSR/D3D11 composition path");

    CefMainArgs mainArgs(GetModuleHandleA(nullptr));
    state.app = new FrontierCefApp();

    const int executeResult =
        CefExecuteProcess(mainArgs, state.app, nullptr);
    if (executeResult >= 0) {
        std::ostringstream message;
        message << "[FrontierCEF] subprocess path returned executeResult="
                << executeResult;
        log_line(message.str());
        state.app = nullptr;
        return false;
    }

    CefSettings settings{};
    settings.no_sandbox = true;
    settings.external_message_pump = false;
    settings.multi_threaded_message_loop = false;
    settings.windowless_rendering_enabled = true;
    settings.log_severity = LOGSEVERITY_WARNING;

    CefString(&settings.browser_subprocess_path) =
        state.subprocess.string();
    CefString(&settings.resources_dir_path) =
        state.moduleDir.string();
    CefString(&settings.locales_dir_path) =
        (state.moduleDir / "locales").string();

    char localAppData[MAX_PATH]{};
    const DWORD envLength =
        GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH);
    if (envLength != 0 && envLength < MAX_PATH) {
        const std::filesystem::path cacheDir =
            std::filesystem::path(localAppData) /
            "FrontierMP" /
            "cef-cache";
        std::error_code ec;
        std::filesystem::create_directories(cacheDir, ec);
        CefString(&settings.cache_path) = cacheDir.string();

        const std::filesystem::path logPath =
            std::filesystem::path(localAppData) /
            "FrontierMP" /
            "logs" /
            "cef.log";
        CefString(&settings.log_file) = logPath.string();
    }

    const std::array<const char*, 4> requiredFiles{
        "icudtl.dat",
        "chrome_100_percent.pak",
        "chrome_200_percent.pak",
        "resources.pak"};
    for (const char* fileName : requiredFiles) {
        const auto path = state.moduleDir / fileName;
        if (!std::filesystem::exists(path)) {
            log_line("[FrontierCEF] required resource missing: " + path.string());
            state.app = nullptr;
            return false;
        }
    }

    log_line("[FrontierCEF] initializing on RDR game thread");
    if (!CefInitialize(mainArgs, settings, state.app, nullptr)) {
        const int cefExitCode = CefGetExitCode();
        std::ostringstream message;
        message << "[FrontierCEF] CefInitialize failed exitCode=0x"
                << std::hex << cefExitCode;
        log_line(message.str());
        state.app = nullptr;
        return false;
    }

    state.gameWindow = find_rdr_window();
    if (state.gameWindow == nullptr) {
        log_line("[FrontierCEF] RDR window not found after CEF initialization");
        CefShutdown();
        state.app = nullptr;
        return false;
    }

    state.renderHandler = new RenderHandler(this);
    state.renderHandler->set_window(state.gameWindow);
    state.client = new FrontierCefClient(this, state.renderHandler);

    CefWindowInfo windowInfo;
    windowInfo.SetAsWindowless(state.gameWindow, true);

    CefBrowserSettings browserSettings;
    browserSettings.background_color = CefColorSetARGB(0, 0, 0, 0);
    browserSettings.windowless_frame_rate = 60;

    state.browser = CefBrowserHost::CreateBrowserSync(
        windowInfo,
        state.client,
        file_url(state.ui),
        browserSettings,
        nullptr,
        nullptr);

    if (state.browser == nullptr) {
        log_line("[FrontierCEF] CreateBrowserSync(OSR) failed");
        state.client = nullptr;
        state.renderHandler = nullptr;
        state.app = nullptr;
        CefShutdown();
        return false;
    }

    log_line("[FrontierCEF] browser created without a native window");
    attach_window_input_on_game_thread();

    state.initialized = true;
    pump_on_game_thread();
    return true;
}

void CefOverlay::accept_paint(const void* buffer, int width, int height) {
    if (buffer == nullptr || width <= 0 || height <= 0) return;
    if (state_ == nullptr || !state_->initialized) return;

    const std::size_t byteCount =
        static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height) *
        4u;

    std::lock_guard lock(state_->frameMutex);
    state_->frame.resize(byteCount);
    std::memcpy(state_->frame.data(), buffer, byteCount);
    state_->frameWidth = width;
    state_->frameHeight = height;
    ++state_->frameGeneration;

    if (!state_->firstPaintLogged) {
        std::ostringstream message;
        message << "[FrontierCEF] first OnPaint frame="
                << width << "x" << height
                << " bytes=" << byteCount;
        log_line(message.str());
        state_->firstPaintLogged = true;
    }
}

void CefOverlay::pump_on_game_thread() {
    if (state_ == nullptr ||
        !state_->initialized ||
        stopRequested_.load(std::memory_order_acquire)) {
        return;
    }

    if (state_->gameWindow == nullptr ||
        !IsWindow(state_->gameWindow)) {
        log_line("[FrontierCEF] RDR window was destroyed");
        shutdown_on_game_thread();
        return;
    }

    CefDoMessageLoopWork();

    if (bridge_ != nullptr &&
        bridge_->game_thread_dispatcher_attached() &&
        !stopRequested_.load(std::memory_order_acquire)) {
        std::string error;
        if (!bridge_->submit_game_thread(
                [this] { pump_on_game_thread(); },
                error) &&
            !error.empty()) {
            log_line("[FrontierCEF] game-thread pump reschedule failed: " + error);
        }
    }
}

bool create_d3d_resources(
    CefOverlay::State& state,
    ID3D11Device* device,
    int width,
    int height) {
    if (device == nullptr || width <= 0 || height <= 0) return false;

    if (state.d3dDevice.Get() != device) {
        state.d3dDevice.Reset();
        state.d3dContext.Reset();
        state.uiTexture.Reset();
        state.uiTextureView.Reset();
        state.vertexShader.Reset();
        state.pixelShader.Reset();
        state.inputLayout.Reset();
        state.sampler.Reset();
        state.blendState.Reset();
        state.rasterizerState.Reset();
        state.uploadedGeneration = 0;
        state.d3dDevice = device;
        device->GetImmediateContext(&state.d3dContext);
    }

    bool recreateTexture = false;
    if (state.uiTexture != nullptr) {
        D3D11_TEXTURE2D_DESC existing{};
        state.uiTexture->GetDesc(&existing);
        recreateTexture =
            existing.Width != static_cast<UINT>(width) ||
            existing.Height != static_cast<UINT>(height);
    } else {
        recreateTexture = true;
    }

    if (recreateTexture) {
        state.uiTexture.Reset();
        state.uiTextureView.Reset();

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        HRESULT result = device->CreateTexture2D(
            &desc,
            nullptr,
            &state.uiTexture);
        if (FAILED(result)) {
            log_line("[FrontierD3D] CreateTexture2D failed");
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = desc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        result = device->CreateShaderResourceView(
            state.uiTexture.Get(),
            &srvDesc,
            &state.uiTextureView);
        if (FAILED(result)) {
            log_line("[FrontierD3D] CreateShaderResourceView failed");
            state.uiTexture.Reset();
            return false;
        }

        state.uploadedGeneration = 0;
    }

    if (state.vertexShader == nullptr ||
        state.pixelShader == nullptr ||
        state.inputLayout == nullptr) {
        ComPtr<ID3DBlob> vertexBytecode;
        ComPtr<ID3DBlob> pixelBytecode;
        std::string error;

        if (!compile_shader(
                kVertexShader,
                "VSMain",
                "vs_4_0",
                vertexBytecode,
                error)) {
            log_line("[FrontierD3D] " + error);
            return false;
        }

        if (!compile_shader(
                kPixelShader,
                "PSMain",
                "ps_4_0",
                pixelBytecode,
                error)) {
            log_line("[FrontierD3D] " + error);
            return false;
        }

        HRESULT result = device->CreateVertexShader(
            vertexBytecode->GetBufferPointer(),
            vertexBytecode->GetBufferSize(),
            nullptr,
            &state.vertexShader);
        if (FAILED(result)) {
            log_line("[FrontierD3D] CreateVertexShader failed");
            return false;
        }

        result = device->CreatePixelShader(
            pixelBytecode->GetBufferPointer(),
            pixelBytecode->GetBufferSize(),
            nullptr,
            &state.pixelShader);
        if (FAILED(result)) {
            log_line("[FrontierD3D] CreatePixelShader failed");
            return false;
        }

        const D3D11_INPUT_ELEMENT_DESC elements[]{
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0}
        };

        result = device->CreateInputLayout(
            elements,
            static_cast<UINT>(std::size(elements)),
            vertexBytecode->GetBufferPointer(),
            vertexBytecode->GetBufferSize(),
            &state.inputLayout);
        if (FAILED(result)) {
            log_line("[FrontierD3D] CreateInputLayout failed");
            return false;
        }

        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
        samplerDesc.MinLOD = 0.0f;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

        result = device->CreateSamplerState(
            &samplerDesc,
            &state.sampler);
        if (FAILED(result)) {
            log_line("[FrontierD3D] CreateSamplerState failed");
            return false;
        }

        D3D11_BLEND_DESC blendDesc{};
        blendDesc.RenderTarget[0].BlendEnable = TRUE;
        blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_ALL;

        result = device->CreateBlendState(
            &blendDesc,
            &state.blendState);
        if (FAILED(result)) {
            log_line("[FrontierD3D] CreateBlendState failed");
            return false;
        }

        D3D11_RASTERIZER_DESC rasterizerDesc{};
        rasterizerDesc.FillMode = D3D11_FILL_SOLID;
        rasterizerDesc.CullMode = D3D11_CULL_NONE;
        rasterizerDesc.DepthClipEnable = TRUE;

        result = device->CreateRasterizerState(
            &rasterizerDesc,
            &state.rasterizerState);
        if (FAILED(result)) {
            log_line("[FrontierD3D] CreateRasterizerState failed");
            return false;
        }
    }

    return true;
}

bool upload_and_draw(
    CefOverlay::State& state,
    IDXGISwapChain* swapChain,
    const std::uint8_t* frame,
    int width,
    int height,
    std::uint64_t generation) {
    if (swapChain == nullptr ||
        frame == nullptr ||
        state.d3dContext == nullptr ||
        state.uiTexture == nullptr ||
        state.uiTextureView == nullptr ||
        state.vertexShader == nullptr ||
        state.pixelShader == nullptr ||
        state.inputLayout == nullptr ||
        state.sampler == nullptr ||
        state.blendState == nullptr ||
        state.rasterizerState == nullptr) {
        return false;
    }

    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(swapChain->GetBuffer(
            0,
            IID_PPV_ARGS(&backBuffer)))) {
        log_line("[FrontierD3D] GetBuffer(backbuffer) failed");
        return false;
    }

    ComPtr<ID3D11RenderTargetView> renderTarget;
    if (FAILED(state.d3dDevice->CreateRenderTargetView(
            backBuffer.Get(),
            nullptr,
            &renderTarget))) {
        log_line("[FrontierD3D] CreateRenderTargetView failed");
        return false;
    }

    if (generation != state.uploadedGeneration) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT mapResult =
            state.d3dContext->Map(
                state.uiTexture.Get(),
                0,
                D3D11_MAP_WRITE_DISCARD,
                0,
                &mapped);
        if (FAILED(mapResult)) {
            log_line("[FrontierD3D] Map(UI texture) failed");
            return false;
        }

        const std::size_t rowBytes =
            static_cast<std::size_t>(width) * 4u;
        for (int y = 0; y < height; ++y) {
            std::memcpy(
                static_cast<std::uint8_t*>(mapped.pData) +
                    static_cast<std::size_t>(y) * mapped.RowPitch,
                frame + static_cast<std::size_t>(y) * rowBytes,
                rowBytes);
        }

        state.d3dContext->Unmap(state.uiTexture.Get(), 0);
        state.uploadedGeneration = generation;
    }

    DXGI_SWAP_CHAIN_DESC swapDesc{};
    if (FAILED(swapChain->GetDesc(&swapDesc))) return false;

    D3D11_VIEWPORT viewport{};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = static_cast<float>(swapDesc.BufferDesc.Width);
    viewport.Height = static_cast<float>(swapDesc.BufferDesc.Height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    state.d3dContext->OMSetRenderTargets(
        1,
        renderTarget.GetAddressOf(),
        nullptr);
    state.d3dContext->RSSetViewports(1, &viewport);
    state.d3dContext->RSSetState(state.rasterizerState.Get());
    state.d3dContext->IASetInputLayout(state.inputLayout.Get());
    state.d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const struct Vertex final {
        float x;
        float y;
        float z;
        float u;
        float v;
    } vertices[3]{
        {-1.0f, -1.0f, 0.0f, 0.0f, 1.0f},
        {-1.0f,  3.0f, 0.0f, 0.0f, -1.0f},
        { 3.0f, -1.0f, 0.0f, 2.0f, 1.0f}
    };

    ComPtr<ID3D11Buffer> vertexBuffer;
    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = sizeof(vertices);
    bufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
    bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA bufferData{};
    bufferData.pSysMem = vertices;

    if (FAILED(state.d3dDevice->CreateBuffer(
            &bufferDesc,
            &bufferData,
            &vertexBuffer))) {
        log_line("[FrontierD3D] vertex buffer creation failed");
        return false;
    }

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ID3D11Buffer* vertexBuffers[]{
        vertexBuffer.Get()
    };
    state.d3dContext->IASetVertexBuffers(
        0,
        1,
        vertexBuffers,
        &stride,
        &offset);

    state.d3dContext->VSSetShader(
        state.vertexShader.Get(),
        nullptr,
        0);
    state.d3dContext->PSSetShader(
        state.pixelShader.Get(),
        nullptr,
        0);

    ID3D11ShaderResourceView* views[]{
        state.uiTextureView.Get()
    };
    state.d3dContext->PSSetShaderResources(
        0,
        1,
        views);

    ID3D11SamplerState* samplers[]{
        state.sampler.Get()
    };
    state.d3dContext->PSSetSamplers(
        0,
        1,
        samplers);

    const FLOAT blendFactor[4]{0.0f, 0.0f, 0.0f, 0.0f};
    state.d3dContext->OMSetBlendState(
        state.blendState.Get(),
        blendFactor,
        0xffffffffu);

    state.d3dContext->Draw(3, 0);

    ID3D11ShaderResourceView* nullViews[]{nullptr};
    state.d3dContext->PSSetShaderResources(0, 1, nullViews);
    return true;
}

void window_to_cef_modifiers(WPARAM wParam, UINT message, uint32_t& modifiers) {
    modifiers = EVENTFLAG_NONE;

    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0) {
        modifiers |= EVENTFLAG_SHIFT_DOWN;
    }
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        modifiers |= EVENTFLAG_CONTROL_DOWN;
    }
    if ((GetKeyState(VK_MENU) & 0x8000) != 0) {
        modifiers |= EVENTFLAG_ALT_DOWN;
    }
    if ((wParam & MK_LBUTTON) != 0) {
        modifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON;
    }
    if ((wParam & MK_MBUTTON) != 0) {
        modifiers |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
    }
    if ((wParam & MK_RBUTTON) != 0) {
        modifiers |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
    }

    if (message == WM_LBUTTONDBLCLK ||
        message == WM_RBUTTONDBLCLK ||
        message == WM_MBUTTONDBLCLK) {
        modifiers |= EVENTFLAG_IS_KEY_PAD;
    }
}

LRESULT CALLBACK frontier_window_proc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam) {
    CefOverlay* overlay =
        g_presentHook.overlay.load(std::memory_order_acquire);

    if (overlay != nullptr) {
        std::intptr_t result = 0;
        if (overlay->handle_window_message(
                message,
                static_cast<std::uintptr_t>(wParam),
                static_cast<std::intptr_t>(lParam),
                result)) {
            return static_cast<LRESULT>(result);
        }
    }

    // The original procedure is looked up through the overlay state. This
    // path executes only for messages that CEF did not consume.
    const WNDPROC originalProc =
        g_originalWindowProc.load(std::memory_order_acquire);
    if (originalProc != nullptr) {
        return CallWindowProcA(
            originalProc,
            hwnd,
            message,
            wParam,
            lParam);
    }

    return DefWindowProcA(hwnd, message, wParam, lParam);
}

} // namespace

void CefOverlay::attach_window_input_on_game_thread() {
    if (state_ == nullptr ||
        state_->gameWindow == nullptr ||
        state_->windowSubclassed) {
        return;
    }

    const LONG_PTR previous =
        SetWindowLongPtrA(
            state_->gameWindow,
            GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(&frontier_window_proc));
    if (previous == 0 &&
        GetLastError() != ERROR_SUCCESS) {
        log_line("[FrontierCEF] failed to subclass RDR window");
        return;
    }

    state_->originalWindowProc =
        reinterpret_cast<WNDPROC>(previous);
    g_originalWindowProc.store(
        state_->originalWindowProc,
        std::memory_order_release);
    state_->windowSubclassed = true;
    log_line("[FrontierCEF] RDR window procedure hooked for CEF input");
}

void CefOverlay::detach_window_input_on_game_thread() {
    if (state_ == nullptr ||
        state_->gameWindow == nullptr ||
        !state_->windowSubclassed) {
        return;
    }

    SetWindowLongPtrA(
        state_->gameWindow,
        GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(state_->originalWindowProc));

    g_originalWindowProc.store(nullptr, std::memory_order_release);
    state_->originalWindowProc = nullptr;
    state_->windowSubclassed = false;
    log_line("[FrontierCEF] RDR window procedure restored");
}

bool CefOverlay::handle_window_message(
    unsigned int message,
    std::uintptr_t wParam,
    std::intptr_t lParam,
    std::intptr_t& result) {
    if (state_ == nullptr ||
        !state_->initialized ||
        state_->client == nullptr ||
        state_->client->browser() == nullptr) {
        return false;
    }

    CefRefPtr<CefBrowser> browser = state_->client->browser();
    CefRefPtr<CefBrowserHost> host = browser->GetHost();
    if (host == nullptr) return false;

    switch (message) {
    case WM_MOUSEMOVE: {
        CefMouseEvent event{};
        event.x = GET_X_LPARAM(static_cast<LPARAM>(lParam));
        event.y = GET_Y_LPARAM(static_cast<LPARAM>(lParam));
        window_to_cef_modifiers(
            static_cast<WPARAM>(wParam),
            message,
            event.modifiers);
        host->SendMouseMoveEvent(event, false);
        result = 0;
        return true;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK: {
        CefMouseEvent event{};
        event.x = GET_X_LPARAM(static_cast<LPARAM>(lParam));
        event.y = GET_Y_LPARAM(static_cast<LPARAM>(lParam));
        window_to_cef_modifiers(
            static_cast<WPARAM>(wParam),
            message,
            event.modifiers);

        CefBrowserHost::MouseButtonType button =
            MBT_LEFT;
        if (message == WM_RBUTTONDOWN ||
            message == WM_RBUTTONUP ||
            message == WM_RBUTTONDBLCLK) {
            button = MBT_RIGHT;
        } else if (message == WM_MBUTTONDOWN ||
                   message == WM_MBUTTONUP ||
                   message == WM_MBUTTONDBLCLK) {
            button = MBT_MIDDLE;
        }

        const bool mouseUp =
            message == WM_LBUTTONUP ||
            message == WM_RBUTTONUP ||
            message == WM_MBUTTONUP;
        const int clickCount =
            message == WM_LBUTTONDBLCLK ||
            message == WM_RBUTTONDBLCLK ||
            message == WM_MBUTTONDBLCLK
                ? 2
                : 1;

        host->SendFocusEvent(true);
        host->SendMouseClickEvent(
            event,
            button,
            mouseUp,
            clickCount);
        result = 0;
        return true;
    }
    case WM_MOUSEWHEEL: {
        POINT screenPoint{
            GET_X_LPARAM(static_cast<LPARAM>(lParam)),
            GET_Y_LPARAM(static_cast<LPARAM>(lParam))};
        ScreenToClient(state_->gameWindow, &screenPoint);

        CefMouseEvent event{};
        event.x = screenPoint.x;
        event.y = screenPoint.y;
        window_to_cef_modifiers(
            static_cast<WPARAM>(GET_KEYSTATE_WPARAM(
                static_cast<WPARAM>(wParam))),
            message,
            event.modifiers);

        const int delta = GET_WHEEL_DELTA_WPARAM(
            static_cast<WPARAM>(wParam));
        host->SendMouseWheelEvent(
            event,
            0,
            delta);
        result = 0;
        return true;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        CefKeyEvent event{};
        event.windows_key_code = static_cast<int>(wParam);
        event.native_key_code = static_cast<int>(wParam);
        event.is_system_key =
            message == WM_SYSKEYDOWN ||
            message == WM_SYSKEYUP;

        const bool keyDown =
            message == WM_KEYDOWN ||
            message == WM_SYSKEYDOWN;
        event.type =
            keyDown
                ? KEYEVENT_RAWKEYDOWN
                : KEYEVENT_KEYUP;

        window_to_cef_modifiers(
            static_cast<WPARAM>(wParam),
            message,
            event.modifiers);
        host->SendKeyEvent(event);
        result = 0;
        return true;
    }
    case WM_CHAR:
    case WM_SYSCHAR: {
        CefKeyEvent event{};
        event.type = KEYEVENT_CHAR;
        event.windows_key_code = static_cast<int>(wParam);
        event.character = static_cast<int>(wParam);
        event.unmodified_character = static_cast<int>(wParam);
        event.native_key_code = static_cast<int>(wParam);
        event.is_system_key = message == WM_SYSCHAR;
        host->SendKeyEvent(event);
        result = 0;
        return true;
    }
    case WM_KILLFOCUS:
        host->SendFocusEvent(false);
        return false;
    default:
        break;
    }

    return false;
}

void CefOverlay::on_present(::IDXGISwapChain* swapChain) {
    if (swapChain == nullptr ||
        state_ == nullptr ||
        !state_->initialized) {
        return;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(&desc))) return;
    if (state_->gameWindow == nullptr ||
        desc.OutputWindow != state_->gameWindow) {
        return;
    }

    std::lock_guard frameLock(state_->frameMutex);
    if (state_->frame.empty() ||
        state_->frameWidth <= 0 ||
        state_->frameHeight <= 0 ||
        state_->frameGeneration == 0) {
        return;
    }

    ComPtr<ID3D11Device> device;
    if (FAILED(swapChain->GetDevice(
            IID_PPV_ARGS(&device))) ||
        device == nullptr) {
        return;
    }

    if (!create_d3d_resources(
            *state_,
            device.Get(),
            state_->frameWidth,
            state_->frameHeight)) {
        return;
    }

    if (!upload_and_draw(
            *state_,
            swapChain,
            state_->frame.data(),
            state_->frameWidth,
            state_->frameHeight,
            state_->frameGeneration)) {
        return;
    }
}

void CefOverlay::shutdown_on_game_thread() {
    if (state_ == nullptr || !state_->initialized) return;

    detach_window_input_on_game_thread();

    if (state_->browser != nullptr) {
        state_->browser->GetHost()->CloseBrowser(true);

        for (int i = 0; i < 60; ++i) {
            if (state_->client == nullptr ||
                state_->client->browser() == nullptr) {
                break;
            }
            CefDoMessageLoopWork();
            Sleep(5);
        }

        state_->browser = nullptr;
    }

    state_->client = nullptr;
    state_->renderHandler = nullptr;
    state_->gameWindow = nullptr;

    CefShutdown();

    state_->d3dContext.Reset();
    state_->d3dDevice.Reset();
    state_->uiTexture.Reset();
    state_->uiTextureView.Reset();
    state_->vertexShader.Reset();
    state_->pixelShader.Reset();
    state_->inputLayout.Reset();
    state_->sampler.Reset();
    state_->blendState.Reset();
    state_->rasterizerState.Reset();
    state_->uploadedGeneration = 0;

    state_->initialized = false;
    log_line("[FrontierCEF] shut down on RDR game thread");
}

} // namespace frontier::client
