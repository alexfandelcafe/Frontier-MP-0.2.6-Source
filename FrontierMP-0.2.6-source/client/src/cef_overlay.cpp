#include "frontier/client/cef_overlay.hpp"
#include "frontier/game/inline_hook.hpp"
#include "frontier/game/rdr_bridge.hpp"

#include <windows.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <dxgi1_4.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_process_message.h"
#include "include/cef_render_handler.h"
#include "include/cef_v8.h"

#include <windowsx.h>

namespace frontier::client {

namespace {

HRESULT STDMETHODCALLTYPE frontier_present1(
    IDXGISwapChain1* swapChain,
    UINT syncInterval,
    UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters);

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

class FrontierAppV8Handler final : public CefV8Handler {
public:
    explicit FrontierAppV8Handler(std::string command)
        : command_(std::move(command)) {}

    bool Execute(
        const CefString&,
        CefRefPtr<CefV8Value>,
        const CefV8ValueList& arguments,
        CefRefPtr<CefV8Value>& retval,
        CefString& exception) override {
        retval = CefV8Value::CreateBool(false);

        if (arguments.size() > 8) {
            exception = "too many app command arguments";
            return true;
        }

        auto message = CefProcessMessage::Create("frontier.app_command");
        auto values = message->GetArgumentList();
        values->SetString(0, command_);

        for (std::size_t i = 0; i < arguments.size(); ++i) {
            const auto& value = arguments[i];
            if (value == nullptr) continue;

            const std::size_t index = i + 1;
            if (value->IsString()) {
                values->SetString(index, value->GetStringValue());
            } else if (value->IsInt()) {
                values->SetInt(index, value->GetIntValue());
            } else if (value->IsBool()) {
                values->SetBool(index, value->GetBoolValue());
            } else {
                exception = "unsupported app command argument type";
                return true;
            }
        }

        if (auto context = CefV8Context::GetCurrentContext()) {
            auto frame = context->GetFrame();
            if (frame != nullptr) {
                frame->SendProcessMessage(PID_BROWSER, message);
                retval = CefV8Value::CreateBool(true);
            }
        }

        return true;
    }

private:
    std::string command_;

    IMPLEMENT_REFCOUNTING(FrontierAppV8Handler);
};

class FrontierCefApp final : public CefApp, public CefRenderProcessHandler {
public:
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
        return this;
    }

    void OnContextCreated(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        CefRefPtr<CefV8Context> context) override {
        if (browser == nullptr || frame == nullptr || context == nullptr) return;

        auto app = CefV8Value::CreateObject(nullptr, nullptr);
        app->SetValue(
            "connect",
            CefV8Value::CreateFunction(
                "connect",
                new FrontierAppV8Handler("connect")),
            V8_PROPERTY_ATTRIBUTE_NONE);
        app->SetValue(
            "quit",
            CefV8Value::CreateFunction(
                "quit",
                new FrontierAppV8Handler("quit")),
            V8_PROPERTY_ATTRIBUTE_NONE);

        context->GetGlobal()->SetValue(
            "app",
            app,
            V8_PROPERTY_ATTRIBUTE_NONE);

        log_line("[FrontierCEF] frontend app bridge installed");
    }

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

    void GetViewRect(
        CefRefPtr<CefBrowser>,
        CefRect& rect) override {
        const HWND window = owner_window_;
        if (owner_ == nullptr ||
            window == nullptr ||
            !IsWindow(window)) {
            rect = CefRect(0, 0, 1, 1);
            return;
        }

        RECT client{};
        if (!GetClientRect(window, &client)) {
            rect = CefRect(0, 0, 1, 1);
            return;
        }

        const int width = std::max<LONG>(1, client.right - client.left);
        const int height = std::max<LONG>(1, client.bottom - client.top);
        rect = CefRect(0, 0, width, height);
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
    FrontierCefClient(
        CefOverlay* owner,
        CefRefPtr<RenderHandler> renderHandler)
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

    bool OnProcessMessageReceived(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        CefProcessId sourceProcess,
        CefRefPtr<CefProcessMessage> message) override {
        if (sourceProcess != PID_RENDERER ||
            browser == nullptr ||
            message == nullptr ||
            message->GetName() != "frontier.app_command" ||
            owner_ == nullptr) {
            return false;
        }

        const auto values = message->GetArgumentList();
        if (values == nullptr || values->GetSize() == 0 ||
            values->GetType(0) != VTYPE_STRING) {
            return false;
        }

        std::vector<std::string> arguments;
        for (std::size_t i = 1; i < values->GetSize(); ++i) {
            switch (values->GetType(i)) {
            case VTYPE_STRING:
                arguments.push_back(values->GetString(i).ToString());
                break;
            case VTYPE_INT:
                arguments.push_back(std::to_string(values->GetInt(i)));
                break;
            case VTYPE_BOOL:
                arguments.push_back(values->GetBool(i) ? "true" : "false");
                break;
            default:
                arguments.emplace_back();
                break;
            }
        }

        return owner_->handle_frontend_command(
            values->GetString(0).ToString(),
            arguments);
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

    using CreateDXGIFactoryProc = HRESULT(WINAPI*)(
        REFIID,
        void**);

    using CreateDXGIFactory2Proc = HRESULT(WINAPI*)(
        UINT,
        REFIID,
        void**);

    using CreateSwapChainProc = HRESULT(STDMETHODCALLTYPE*)(
        IDXGIFactory*,
        IUnknown*,
        const DXGI_SWAP_CHAIN_DESC*,
        IDXGISwapChain**);

    using CreateSwapChainForHwndProc = HRESULT(STDMETHODCALLTYPE*)(
        IDXGIFactory2*,
        IUnknown*,
        HWND,
        const DXGI_SWAP_CHAIN_DESC1*,
        const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
        IDXGIOutput*,
        IDXGISwapChain1**);

    using CreateSwapChainForCompositionProc = HRESULT(STDMETHODCALLTYPE*)(
        IDXGIFactory2*,
        IUnknown*,
        const DXGI_SWAP_CHAIN_DESC1*,
        IDXGIOutput*,
        IDXGISwapChain1**);

    using PresentProc = HRESULT(STDMETHODCALLTYPE*)(
        IDXGISwapChain*,
        UINT,
        UINT);

    using Present1Proc = HRESULT(STDMETHODCALLTYPE*)(
        IDXGISwapChain1*,
        UINT,
        UINT,
        const DXGI_PRESENT_PARAMETERS*);

    std::mutex mutex;
    std::atomic<CefOverlay*> overlay{nullptr};
    HMODULE rdrModule{};

    // RDRMP's DirectXHook detoured the shared Present implementation
    // discovered from an auxiliary swapchain instead of modifying RDR's
    // DXGI factory/swapchain COM vtables.
    std::unique_ptr<frontier::game::InlineHook> historicalPresentHook{};
    bool historicalPresentDetourAttached{};

    ImportPatch createImport{};
    CreateDeviceAndSwapChainProc createOriginal{};

    std::array<ImportPatch, 3> factoryImports{};
    CreateDXGIFactoryProc createFactoryOriginal{};
    CreateDXGIFactoryProc createFactory1Original{};
    CreateDXGIFactory2Proc createFactory2Original{};
    std::size_t factoryImportCount{};

    struct FactoryHookData final {
        ComPtr<IDXGIFactory> factory{};
        ComPtr<IDXGIFactory2> factory2{};
        std::array<void*, 64> hookedBaseVtable{};
        std::array<void*, 64> hookedFactory2Vtable{};
        std::size_t baseVtableEntryCount{};
        std::size_t factory2VtableEntryCount{};
        void** originalBaseVtable{};
        void** originalFactory2Vtable{};
        void* createSwapChainOriginal{};
        void* createSwapChainForHwndOriginal{};
        void* createSwapChainForCompositionOriginal{};
        bool factory2SharesBaseObject{};
    };

    std::deque<FactoryHookData> factoryHooks{};

    PresentProc presentOriginal{};
    Present1Proc present1Original{};
    IDXGISwapChain* hookedSwapChain{};
    ComPtr<IDXGISwapChain> hookedSwapChainRef{};
    ComPtr<IDXGISwapChain1> hookedSwapChain1Ref{};
    // Keep enough of the concrete COM object's vtable to preserve
    // implementation-specific/private entries that are not part of the
    // public IDXGISwapChain4 interface.
    std::array<void*, 64> hookedVtable{};
    std::array<void*, 64> originalVtable{};
    std::array<void*, 128> hookedVtable1{};
    std::array<void*, 128> originalVtable1{};
    std::size_t extendedVtableEntryCount{};
    void** originalVtableAddress{};
    void** originalVtable1Address{};
    bool swapchainHooked{};
    bool swapchain1Hooked{};
    bool swapchainUsesExtendedVtable{};

    void** sharedSwapChainVtable{};
    void* sharedPresentOriginal{};
    bool sharedPresentPatched{};
    void** sharedSwapChain1Vtable{};
    void* sharedPresent1Original{};
    bool sharedPresent1Patched{};
    std::array<std::size_t, 8> commandQueueOffsets{};
    std::size_t commandQueueOffsetCount{};
    std::size_t activeCommandQueueOffset{static_cast<std::size_t>(-1)};
    ComPtr<ID3D12CommandQueue> capturedCommandQueue{};

    // Never patch a real RDR swapchain while still inside DXGI's
    // CreateSwapChain* call. RDR/DXGI may continue touching the freshly-created
    // COM object on the same stack after the factory method returns internally.
    // Keep the object alive and patch it from the next game-thread tick instead.
    ComPtr<IDXGISwapChain> pendingSwapChain{};
    ComPtr<ID3D12CommandQueue> pendingSwapChainQueue{};

    // Retained only by legacy factory/swapchain helper code below. The active
    // RDRMP-compatible Present detour does not gate on world state.
    std::atomic<bool> renderActivationAllowed{false};

    bool installed{};
};

PresentHookState g_presentHook{};
std::atomic<CefOverlay*> g_activeOverlay{nullptr};
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

struct FrontierDelayImportDescriptor final {
    DWORD attributes{};
    DWORD dllNameRva{};
    DWORD moduleHandleRva{};
    DWORD iatRva{};
    DWORD intRva{};
    DWORD boundIatRva{};
    DWORD unloadIatRva{};
    DWORD timeDateStamp{};
};

bool find_and_patch_delay_import(
    HMODULE module,
    const char* importedModule,
    const char* importedName,
    void* replacement,
    ImportPatch& outPatch,
    void*& resolvedOriginal,
    std::string& error) {
    error.clear();
    outPatch = {};
    resolvedOriginal = nullptr;

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
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (directory.VirtualAddress == 0 || directory.Size < sizeof(FrontierDelayImportDescriptor)) {
        error = "no delay-import directory";
        return false;
    }

    const auto* descriptors =
        reinterpret_cast<const FrontierDelayImportDescriptor*>(
            base + directory.VirtualAddress);

    for (const auto* descriptor = descriptors;
         descriptor->dllNameRva != 0;
         ++descriptor) {
        if ((descriptor->attributes & 1u) == 0u) {
            error = "delay-import descriptor uses VA form";
            return false;
        }

        const char* moduleName =
            reinterpret_cast<const char*>(base + descriptor->dllNameRva);
        if (_stricmp(moduleName, importedModule) != 0) continue;

        if (descriptor->iatRva == 0 || descriptor->intRva == 0) {
            error = "delay-import IAT/INT unavailable";
            return false;
        }

        auto* lookup =
            reinterpret_cast<const IMAGE_THUNK_DATA64*>(
                base + descriptor->intRva);
        auto* iat =
            reinterpret_cast<IMAGE_THUNK_DATA64*>(
                base + descriptor->iatRva);

        for (std::size_t index = 0;
             lookup[index].u1.AddressOfData != 0;
             ++index) {
            if (IMAGE_SNAP_BY_ORDINAL64(lookup[index].u1.Ordinal)) continue;

            const auto* byName =
                reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                    base + lookup[index].u1.AddressOfData);
            if (std::strcmp(
                    reinterpret_cast<const char*>(byName->Name),
                    importedName) != 0) {
                continue;
            }

            HMODULE importedHandle = GetModuleHandleA(importedModule);
            if (importedHandle == nullptr) {
                importedHandle = LoadLibraryA(importedModule);
            }
            if (importedHandle == nullptr) {
                error = "unable to load delay-import module";
                return false;
            }

            resolvedOriginal =
                reinterpret_cast<void*>(
                    GetProcAddress(importedHandle, importedName));
            if (resolvedOriginal == nullptr) {
                error = "GetProcAddress failed for delay-import";
                return false;
            }

            void** slot =
                reinterpret_cast<void**>(&iat[index].u1.Function);
            if (!replace_pointer(
                    slot,
                    replacement,
                    outPatch.original,
                    error)) {
                resolvedOriginal = nullptr;
                return false;
            }

            outPatch.slot = slot;
            return true;
        }
    }

    error = "delay import not found";
    return false;
}

HRESULT STDMETHODCALLTYPE frontier_factory_create_swap_chain(
    IDXGIFactory* factory,
    IUnknown* device,
    const DXGI_SWAP_CHAIN_DESC* desc,
    IDXGISwapChain** swapChain);

HRESULT STDMETHODCALLTYPE frontier_factory_create_swap_chain_for_hwnd(
    IDXGIFactory2* factory,
    IUnknown* device,
    HWND window,
    const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
    IDXGIOutput* restrictToOutput,
    IDXGISwapChain1** swapChain);

HRESULT STDMETHODCALLTYPE frontier_factory_create_swap_chain_for_composition(
    IDXGIFactory2* factory,
    IUnknown* device,
    const DXGI_SWAP_CHAIN_DESC1* desc,
    IDXGIOutput* restrictToOutput,
    IDXGISwapChain1** swapChain);

HRESULT STDMETHODCALLTYPE frontier_present(
    IDXGISwapChain* swapChain,
    UINT syncInterval,
    UINT flags);

std::size_t readable_vtable_entries(
    void** vtable,
    std::size_t minimumEntries,
    std::size_t maximumEntries) {
    if (vtable == nullptr ||
        minimumEntries == 0 ||
        maximumEntries < minimumEntries) {
        return 0;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(
            vtable,
            &mbi,
            sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT ||
        (mbi.Protect & PAGE_GUARD) != 0 ||
        (mbi.Protect & PAGE_NOACCESS) != 0) {
        return 0;
    }

    const auto base =
        reinterpret_cast<std::uintptr_t>(vtable);
    const auto regionBegin =
        reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    const auto regionEnd =
        regionBegin + mbi.RegionSize;
    if (base < regionBegin || base >= regionEnd) return 0;

    const std::size_t available = static_cast<std::size_t>(
        (regionEnd - base) / sizeof(void*));
    if (available < minimumEntries) return 0;
    return std::min(available, maximumEntries);
}

bool patch_vtable_slot(
    void** vtable,
    std::size_t index,
    void* replacement,
    void*& original,
    std::string& error) {
    if (vtable == nullptr || replacement == nullptr) {
        error = "invalid vtable/replacement";
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(
            &vtable[index],
            sizeof(void*),
            PAGE_READWRITE,
            &oldProtect)) {
        error = "VirtualProtect(vtable) failed";
        return false;
    }

    original = vtable[index];
    vtable[index] = replacement;

    DWORD ignored = 0;
    VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &ignored);
    FlushInstructionCache(
        GetCurrentProcess(),
        &vtable[index],
        sizeof(void*));
    return true;
}

std::size_t collect_pointer_offsets(
    void* object,
    const void* needle,
    std::array<std::size_t, 8>& offsets) {
    offsets.fill(0);
    if (object == nullptr || needle == nullptr) return 0;

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(object, &mbi, sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT ||
        (mbi.Protect & PAGE_GUARD) != 0 ||
        (mbi.Protect & PAGE_NOACCESS) != 0) {
        return 0;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(object);
    const auto regionBegin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    const auto regionEnd = regionBegin + mbi.RegionSize;
    if (base < regionBegin || base >= regionEnd) return 0;

    const std::size_t scanBytes =
        std::min<std::size_t>(
            static_cast<std::size_t>(regionEnd - base),
            0x1000u);

    std::size_t count = 0;
    for (std::size_t offset = 0;
         offset + sizeof(void*) <= scanBytes;
         offset += sizeof(void*)) {
        void* value = nullptr;
        __try {
            value = *reinterpret_cast<void**>(base + offset);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            value = nullptr;
        }

        if (value == needle) {
            offsets[count++] = offset;
            if (count == offsets.size()) break;
        }
    }

    return count;
}

bool read_swapchain_pointer(
    IDXGISwapChain* swapChain,
    std::size_t offset,
    void*& pointer) {
    pointer = nullptr;
    if (swapChain == nullptr) return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(
            swapChain,
            &mbi,
            sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT ||
        (mbi.Protect & PAGE_GUARD) != 0 ||
        (mbi.Protect & PAGE_NOACCESS) != 0) {
        return false;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(swapChain);
    const auto regionBegin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    const auto regionEnd = regionBegin + mbi.RegionSize;
    if (base < regionBegin ||
        offset > static_cast<std::size_t>(regionEnd - base) ||
        sizeof(void*) > regionEnd - base - offset) {
        return false;
    }

    __try {
        pointer = *reinterpret_cast<void**>(base + offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        pointer = nullptr;
        return false;
    }

    return pointer != nullptr;
}

bool query_command_queue(
    void* candidate,
    ComPtr<ID3D12CommandQueue>& queue) {
    queue.Reset();
    if (candidate == nullptr) return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(
            candidate,
            &mbi,
            sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT ||
        (mbi.Protect & PAGE_GUARD) != 0 ||
        (mbi.Protect & PAGE_NOACCESS) != 0) {
        return false;
    }

    void* vtable = nullptr;
    __try {
        vtable = *reinterpret_cast<void**>(candidate);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    if (vtable == nullptr) return false;

    void* firstMethod = nullptr;
    __try {
        firstMethod = *reinterpret_cast<void**>(vtable);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    if (firstMethod == nullptr) return false;

    MEMORY_BASIC_INFORMATION vtableMbi{};
    if (VirtualQuery(
            vtable,
            &vtableMbi,
            sizeof(vtableMbi)) != sizeof(vtableMbi) ||
        vtableMbi.State != MEM_COMMIT ||
        (vtableMbi.Protect & PAGE_GUARD) != 0 ||
        (vtableMbi.Protect & PAGE_NOACCESS) != 0) {
        return false;
    }

    MEMORY_BASIC_INFORMATION methodMbi{};
    if (VirtualQuery(
            firstMethod,
            &methodMbi,
            sizeof(methodMbi)) != sizeof(methodMbi) ||
        methodMbi.State != MEM_COMMIT ||
        ((methodMbi.Protect & PAGE_EXECUTE) == 0 &&
         (methodMbi.Protect & PAGE_EXECUTE_READ) == 0 &&
         (methodMbi.Protect & PAGE_EXECUTE_READWRITE) == 0 &&
         (methodMbi.Protect & PAGE_EXECUTE_WRITECOPY) == 0)) {
        return false;
    }

    HMODULE methodModule = nullptr;
    if (!GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(firstMethod),
            &methodModule) ||
        methodModule == nullptr) {
        return false;
    }

    char moduleName[MAX_PATH]{};
    if (GetModuleFileNameA(methodModule, moduleName, sizeof(moduleName)) == 0) {
        return false;
    }

    const char* baseName = std::strrchr(moduleName, '\\');
    baseName = baseName != nullptr ? baseName + 1 : moduleName;
    if (_stricmp(baseName, "d3d12.dll") != 0) {
        return false;
    }

    __try {
        const auto unknown = reinterpret_cast<IUnknown*>(candidate);
        return SUCCEEDED(
            unknown->QueryInterface(IID_PPV_ARGS(&queue)));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        queue.Reset();
        return false;
    }
}

bool locate_command_queue(
    PresentHookState& state,
    IDXGISwapChain* swapChain,
    ComPtr<ID3D12CommandQueue>& queue,
    std::size_t& matchedOffset) {
    queue.Reset();
    matchedOffset = static_cast<std::size_t>(-1);

    auto tryOffset = [&](std::size_t offset) -> bool {
        void* candidate = nullptr;
        if (!read_swapchain_pointer(swapChain, offset, candidate)) {
            return false;
        }

        ComPtr<ID3D12CommandQueue> resolved;
        if (!query_command_queue(candidate, resolved)) {
            return false;
        }

        queue = std::move(resolved);
        matchedOffset = offset;
        return true;
    };

    if (state.activeCommandQueueOffset != static_cast<std::size_t>(-1) &&
        tryOffset(state.activeCommandQueueOffset)) {
        return true;
    }

    for (std::size_t i = 0; i < state.commandQueueOffsetCount; ++i) {
        if (tryOffset(state.commandQueueOffsets[i])) {
            state.activeCommandQueueOffset = matchedOffset;
            return true;
        }
    }

    // The historical RDRMP object layout is not part of the DXGI ABI. If the
    // private queue field moved, discover a valid command queue dynamically
    // once and cache the offset for subsequent frames.
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(
            swapChain,
            &mbi,
            sizeof(mbi)) == sizeof(mbi) &&
        mbi.State == MEM_COMMIT &&
        (mbi.Protect & PAGE_GUARD) == 0 &&
        (mbi.Protect & PAGE_NOACCESS) == 0) {
        const auto base =
            reinterpret_cast<std::uintptr_t>(swapChain);
        const auto regionBegin =
            reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto regionEnd =
            regionBegin + mbi.RegionSize;
        if (base >= regionBegin && base < regionEnd) {
            const std::size_t scanBytes =
                std::min<std::size_t>(
                    static_cast<std::size_t>(regionEnd - base),
                    0x600u);

            for (std::size_t offset = sizeof(void*);
                 offset + sizeof(void*) <= scanBytes;
                 offset += sizeof(void*)) {
                void* candidate = nullptr;
                if (!read_swapchain_pointer(
                        swapChain,
                        offset,
                        candidate)) {
                    continue;
                }

                ComPtr<ID3D12CommandQueue> resolved;
                if (!query_command_queue(candidate, resolved)) {
                    continue;
                }

                queue = std::move(resolved);
                matchedOffset = offset;
                state.activeCommandQueueOffset = offset;
                log_line(
                    "[FrontierD3D] discovered D3D12 command queue offset dynamically: " +
                    std::to_string(offset));
                return true;
            }
        }
    }

    return false;
}

bool install_historical_present_hook(
    PresentHookState& state,
    std::string& error) {
    error.clear();

    if (GetModuleHandleA("dxgi.dll") == nullptr ||
        GetModuleHandleA("d3d12.dll") == nullptr) {
        error = "dxgi.dll/d3d12.dll not loaded";
        return false;
    }

    // Historical RDRMP FUN_180058880:
    //   CreateDXGIFactory1 -> D3D12CreateDevice -> CreateCommandQueue
    //   -> CreateSwapChainForHwnd -> read Present -> DirectXHook detour.
    //
    // The auxiliary swapchain exists only to discover the shared Present
    // implementation and the swapchain/queue field relationship. We never
    // patch the probe or any RDR COM vtable.
    const char* const className = "FrontierMP_D3DProbe";
    WNDCLASSA windowClass{};
    windowClass.lpfnWndProc = DefWindowProcA;
    windowClass.hInstance = GetModuleHandleA(nullptr);
    windowClass.lpszClassName = className;

    RegisterClassA(&windowClass);
    HWND probeWindow = CreateWindowExA(
        0,
        className,
        "FrontierMP D3D Probe",
        WS_OVERLAPPEDWINDOW,
        0,
        0,
        100,
        100,
        nullptr,
        nullptr,
        windowClass.hInstance,
        nullptr);

    if (probeWindow == nullptr) {
        UnregisterClassA(className, windowClass.hInstance);
        error = "failed to create D3D probe window";
        return false;
    }

    ComPtr<IDXGIFactory2> factory;
    HRESULT result = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(result) || factory == nullptr) {
        DestroyWindow(probeWindow);
        UnregisterClassA(className, windowClass.hInstance);
        error = "CreateDXGIFactory1 probe failed";
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    result = factory->EnumAdapters1(0, &adapter);
    if (FAILED(result) || adapter == nullptr) {
        DestroyWindow(probeWindow);
        UnregisterClassA(className, windowClass.hInstance);
        error = "EnumAdapters1 probe failed";
        return false;
    }

    ComPtr<ID3D12Device> device;
    result = D3D12CreateDevice(
        adapter.Get(),
        D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(&device));
    if (FAILED(result) || device == nullptr) {
        DestroyWindow(probeWindow);
        UnregisterClassA(className, windowClass.hInstance);
        error = "D3D12CreateDevice probe failed";
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    ComPtr<ID3D12CommandQueue> queue;
    result = device->CreateCommandQueue(
        &queueDesc,
        IID_PPV_ARGS(&queue));
    if (FAILED(result) || queue == nullptr) {
        DestroyWindow(probeWindow);
        UnregisterClassA(className, windowClass.hInstance);
        error = "CreateCommandQueue probe failed";
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.Width = 100;
    swapChainDesc.Height = 100;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = 2;
    swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> probeSwapChain1;
    result = factory->CreateSwapChainForHwnd(
        queue.Get(),
        probeWindow,
        &swapChainDesc,
        nullptr,
        nullptr,
        &probeSwapChain1);
    if (FAILED(result) || probeSwapChain1 == nullptr) {
        DestroyWindow(probeWindow);
        UnregisterClassA(className, windowClass.hInstance);
        error = "CreateSwapChainForHwnd probe failed";
        return false;
    }

    ComPtr<IDXGISwapChain> probeSwapChain;
    result = probeSwapChain1.As(&probeSwapChain);
    if (FAILED(result) || probeSwapChain == nullptr) {
        DestroyWindow(probeWindow);
        UnregisterClassA(className, windowClass.hInstance);
        error = "probe swapchain QueryInterface failed";
        return false;
    }

    void*** vtableAddress =
        reinterpret_cast<void***>(probeSwapChain.Get());
    if (vtableAddress == nullptr || *vtableAddress == nullptr) {
        DestroyWindow(probeWindow);
        UnregisterClassA(className, windowClass.hInstance);
        error = "probe swapchain vtable unavailable";
        return false;
    }

    void** vtable = *vtableAddress;
    const auto presentTarget =
        reinterpret_cast<std::uintptr_t>(vtable[8]);
    if (presentTarget == 0) {
        DestroyWindow(probeWindow);
        UnregisterClassA(className, windowClass.hInstance);
        error = "probe Present entry unavailable";
        return false;
    }

    state.commandQueueOffsetCount = collect_pointer_offsets(
        probeSwapChain.Get(),
        queue.Get(),
        state.commandQueueOffsets);
    if (state.commandQueueOffsetCount == 0) {
        DestroyWindow(probeWindow);
        UnregisterClassA(className, windowClass.hInstance);
        error = "probe swapchain does not expose command queue field";
        return false;
    }

    auto presentHook = std::make_unique<frontier::game::InlineHook>();
    std::string hookError;
    constexpr std::size_t kPresentPatchSize = 14u;

    if (!presentHook->install(
            presentTarget,
            reinterpret_cast<std::uintptr_t>(&frontier_present),
            kPresentPatchSize,
            "RDRMP historical Present",
            hookError)) {
        DestroyWindow(probeWindow);
        UnregisterClassA(className, windowClass.hInstance);
        error = "Present detour failed: " + hookError;
        return false;
    }

    state.presentOriginal =
        reinterpret_cast<PresentHookState::PresentProc>(
            presentHook->trampoline());
    if (state.presentOriginal == nullptr) {
        presentHook->uninstall();
        DestroyWindow(probeWindow);
        UnregisterClassA(className, windowClass.hInstance);
        error = "Present detour installed without trampoline";
        return false;
    }

    state.historicalPresentHook = std::move(presentHook);
    state.historicalPresentDetourAttached = true;
    state.sharedPresentPatched = true;
    state.activeCommandQueueOffset = static_cast<std::size_t>(-1);

    std::ostringstream message;
    message << "[FrontierD3D] historical shared Present detour installed"
            << " presentTarget=" << reinterpret_cast<const void*>(presentTarget)
            << " trampoline=" << reinterpret_cast<const void*>(state.presentOriginal)
            << " queueFieldOffsets=" << state.commandQueueOffsetCount
            << " firstQueueOffset=" << state.commandQueueOffsets[0];
    log_line(message.str());

    DestroyWindow(probeWindow);
    UnregisterClassA(className, windowClass.hInstance);
    return true;
}

HRESULT STDMETHODCALLTYPE frontier_present1(
    IDXGISwapChain1* swapChain,
    UINT syncInterval,
    UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters);

void hook_swapchain(
    PresentHookState& state,
    IDXGISwapChain* swapChain,
    ID3D12CommandQueue* commandQueueHint = nullptr);

void defer_swapchain_hook(
    PresentHookState& state,
    IDXGISwapChain* swapChain,
    ID3D12CommandQueue* commandQueueHint) {
    if (swapChain == nullptr) return;

    std::lock_guard lock(state.mutex);
    if (state.swapchainHooked || state.swapchain1Hooked) return;

    state.pendingSwapChain = swapChain;
    state.pendingSwapChainQueue.Reset();
    if (commandQueueHint != nullptr) {
        state.pendingSwapChainQueue = commandQueueHint;
    }

    log_line("[FrontierD3D] swapchain hook queued for next RDR game-thread tick");
}

void activate_pending_swapchain_hook(PresentHookState& state) {
    if (!state.renderActivationAllowed.load(std::memory_order_acquire)) {
        return;
    }

    ComPtr<IDXGISwapChain> pendingSwapChain;
    ComPtr<ID3D12CommandQueue> pendingQueue;
    {
        std::lock_guard lock(state.mutex);
        if (state.swapchainHooked || state.swapchain1Hooked ||
            state.pendingSwapChain == nullptr) {
            return;
        }

        pendingSwapChain = state.pendingSwapChain;
        pendingQueue = state.pendingSwapChainQueue;
        state.pendingSwapChain.Reset();
        state.pendingSwapChainQueue.Reset();
    }

    log_line("[FrontierD3D] activating deferred swapchain hook on RDR game-thread tick");
    hook_swapchain(
        state,
        pendingSwapChain.Get(),
        pendingQueue.Get());
}

void hook_factory(PresentHookState& state, IDXGIFactory* factory) {
    if (factory == nullptr) return;

    std::lock_guard lock(state.mutex);

    for (const auto& existing : state.factoryHooks) {
        if (existing.factory.Get() == factory ||
            (existing.factory2 != nullptr &&
             existing.factory2.Get() == reinterpret_cast<IDXGIFactory2*>(factory))) {
            return;
        }
    }

    void*** baseVtable = reinterpret_cast<void***>(factory);
    if (baseVtable == nullptr || *baseVtable == nullptr) return;

    // The shadow vtable must live at a stable address for as long as DXGI can
    // call through the returned COM object. A local FactoryHookData would make
    // the vtable pointer dangle as soon as this function returns. Keep the
    // hook object in a deque-backed container and populate it in-place so its
    // vtable arrays have stable storage.
    state.factoryHooks.emplace_back();
    auto& hook = state.factoryHooks.back();
    hook.factory = factory;

    ComPtr<IDXGIFactory2> factory2;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory2))) &&
        factory2 != nullptr) {
        hook.factory2 = factory2;
    }

    if (hook.factory2 != nullptr) {
        void*** vtable2 = reinterpret_cast<void***>(hook.factory2.Get());
        if (vtable2 == nullptr || *vtable2 == nullptr) {
            hook.factory2.Reset();
        }
    }

    if (hook.factory2 != nullptr) {
        void*** vtable2 = reinterpret_cast<void***>(hook.factory2.Get());
        void** originalFactory2Vtable = *vtable2;
        hook.originalFactory2Vtable = originalFactory2Vtable;

        hook.factory2VtableEntryCount = readable_vtable_entries(
            originalFactory2Vtable,
            25u,
            hook.hookedFactory2Vtable.size());
        if (hook.factory2VtableEntryCount < 25u) {
            log_line("[FrontierD3D] IDXGIFactory2 vtable is shorter than the public ABI; refusing factory shadow hook");
            state.factoryHooks.pop_back();
            return;
        }

        for (std::size_t i = 0; i < hook.factory2VtableEntryCount; ++i) {
            hook.hookedFactory2Vtable[i] = originalFactory2Vtable[i];
        }

        hook.createSwapChainOriginal = hook.hookedFactory2Vtable[10];
        hook.createSwapChainForHwndOriginal = hook.hookedFactory2Vtable[15];
        hook.createSwapChainForCompositionOriginal = hook.hookedFactory2Vtable[24];

        hook.hookedFactory2Vtable[10] =
            reinterpret_cast<void*>(&frontier_factory_create_swap_chain);
        hook.hookedFactory2Vtable[15] =
            reinterpret_cast<void*>(&frontier_factory_create_swap_chain_for_hwnd);
        hook.hookedFactory2Vtable[24] =
            reinterpret_cast<void*>(&frontier_factory_create_swap_chain_for_composition);

        if (hook.factory2.Get() == factory) {
            hook.originalBaseVtable = *baseVtable;
            hook.baseVtableEntryCount = hook.factory2VtableEntryCount;
            hook.factory2SharesBaseObject = true;
            *baseVtable = hook.hookedFactory2Vtable.data();

            {
                std::ostringstream message;
                message << "[FrontierD3D] IDXGIFactory2 shadow-vtable hooks installed for returned factory instance (shared base object)"
                        << " shadowEntries=" << hook.factory2VtableEntryCount;
                log_line(message.str());
            }
        } else {
            hook.originalBaseVtable = *baseVtable;
            hook.baseVtableEntryCount = readable_vtable_entries(
                hook.originalBaseVtable,
                12u,
                hook.hookedBaseVtable.size());
            if (hook.baseVtableEntryCount < 12u) {
                log_line("[FrontierD3D] IDXGIFactory base vtable is shorter than the public ABI; refusing factory shadow hook");
                if (hook.originalBaseVtable != nullptr) *baseVtable = hook.originalBaseVtable;
                if (hook.factory2 != nullptr && hook.originalFactory2Vtable != nullptr) {
                    void*** vtable2 = reinterpret_cast<void***>(hook.factory2.Get());
                    if (vtable2 != nullptr && *vtable2 == hook.hookedFactory2Vtable.data()) {
                        *vtable2 = hook.originalFactory2Vtable;
                    }
                }
                state.factoryHooks.pop_back();
                return;
            }

            for (std::size_t i = 0; i < hook.baseVtableEntryCount; ++i) {
                hook.hookedBaseVtable[i] = (*baseVtable)[i];
            }

            hook.hookedBaseVtable[10] =
                reinterpret_cast<void*>(&frontier_factory_create_swap_chain);
            *baseVtable = hook.hookedBaseVtable.data();
            *vtable2 = hook.hookedFactory2Vtable.data();

            {
                std::ostringstream message;
                message << "[FrontierD3D] IDXGIFactory/IDXGIFactory2 shadow-vtable hooks installed for returned factory instance"
                        << " baseShadowEntries=" << hook.baseVtableEntryCount
                        << " factory2ShadowEntries=" << hook.factory2VtableEntryCount;
                log_line(message.str());
            }
        }
    } else {
        hook.originalBaseVtable = *baseVtable;
        hook.baseVtableEntryCount = readable_vtable_entries(
            hook.originalBaseVtable,
            12u,
            hook.hookedBaseVtable.size());
        if (hook.baseVtableEntryCount < 12u) {
            log_line("[FrontierD3D] IDXGIFactory vtable is shorter than the public ABI; refusing factory shadow hook");
            state.factoryHooks.pop_back();
            return;
        }

        for (std::size_t i = 0; i < hook.baseVtableEntryCount; ++i) {
            hook.hookedBaseVtable[i] = (*baseVtable)[i];
        }

        hook.createSwapChainOriginal = hook.hookedBaseVtable[10];
        hook.hookedBaseVtable[10] =
            reinterpret_cast<void*>(&frontier_factory_create_swap_chain);
        *baseVtable = hook.hookedBaseVtable.data();

        log_line("[FrontierD3D] IDXGIFactory shadow-vtable hook installed for returned factory instance");
    }

    if (hook.createSwapChainOriginal == nullptr &&
        hook.createSwapChainForHwndOriginal == nullptr &&
        hook.createSwapChainForCompositionOriginal == nullptr) {
        if (hook.factory2SharesBaseObject) {
            *baseVtable = hook.originalBaseVtable;
        } else {
            if (hook.originalBaseVtable != nullptr) *baseVtable = hook.originalBaseVtable;
            if (hook.factory2 != nullptr && hook.originalFactory2Vtable != nullptr) {
                void*** vtable2 = reinterpret_cast<void***>(hook.factory2.Get());
                if (vtable2 != nullptr && *vtable2 == hook.hookedFactory2Vtable.data()) {
                    *vtable2 = hook.originalFactory2Vtable;
                }
            }
        }
        state.factoryHooks.pop_back();
        return;
    }
}
void hook_swapchain(
    PresentHookState& state,
    IDXGISwapChain* swapChain,
    ID3D12CommandQueue* commandQueueHint) {
    if (swapChain == nullptr) return;

    std::lock_guard lock(state.mutex);
    if (state.swapchainHooked || state.swapchain1Hooked) return;

    DXGI_SWAP_CHAIN_DESC desc{};
    const bool haveDesc = SUCCEEDED(swapChain->GetDesc(&desc));
    const HWND currentRdrWindow = find_rdr_window();
    if (haveDesc &&
        currentRdrWindow != nullptr &&
        desc.OutputWindow != currentRdrWindow) {
        std::ostringstream message;
        message << "[FrontierD3D] ignoring non-RDR swapchain="
                << static_cast<const void*>(swapChain)
                << " hwnd=" << desc.OutputWindow
                << " rdrHwnd=" << currentRdrWindow;
        log_line(message.str());
        return;
    }

    if (!state.renderActivationAllowed.load(std::memory_order_acquire)) {
        static std::atomic<std::uint32_t> gatedHookTrace{0};
        const auto trace = gatedHookTrace.fetch_add(1, std::memory_order_relaxed);
        if (trace < 4) {
            log_line(
                "[FrontierD3D] swapchain hook deferred until RDR loading/shader preload completes");
        }
        return;
    }

    // Query the highest swap-chain interface exposed by the RDR object before
    // replacing its vtable. A D3D12 swap chain commonly exposes IDXGISwapChain4;
    // its complete COM table has 41 entries. Replacing that table with only the
    // IDXGISwapChain1 prefix leaves later methods reading past the shadow array
    // and can crash DXGI during swap-chain initialization.
    ComPtr<IDXGISwapChain1> swapChain1;
    const bool haveSwapChain1 =
        SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain1))) &&
        swapChain1 != nullptr;

    void*** baseVtable = reinterpret_cast<void***>(swapChain);
    if (baseVtable == nullptr || *baseVtable == nullptr) return;

    if (haveSwapChain1) {
        void*** vtable1 = reinterpret_cast<void***>(swapChain1.Get());
        if (vtable1 == nullptr || *vtable1 == nullptr) return;

        std::size_t minimumVtableEntries = 29; // IDXGISwapChain1: 0..28
        ComPtr<IDXGISwapChain2> swapChain2;
        ComPtr<IDXGISwapChain3> swapChain3;
        ComPtr<IDXGISwapChain4> swapChain4;
        if (SUCCEEDED(swapChain1.As(&swapChain4)) && swapChain4 != nullptr) {
            minimumVtableEntries = 41;
        } else if (SUCCEEDED(swapChain1.As(&swapChain3)) && swapChain3 != nullptr) {
            minimumVtableEntries = 40;
        } else if (SUCCEEDED(swapChain1.As(&swapChain2)) && swapChain2 != nullptr) {
            minimumVtableEntries = 36;
        }

        const std::size_t extendedVtableEntries =
            readable_vtable_entries(
                *vtable1,
                minimumVtableEntries,
                state.hookedVtable1.size());
        if (extendedVtableEntries < minimumVtableEntries) {
            log_line("[FrontierD3D] swapchain vtable is shorter than the public ABI; refusing vtable shadow hook");
            return;
        }

        for (std::size_t i = 0; i < extendedVtableEntries; ++i) {
            state.originalVtable1[i] = (*vtable1)[i];
            state.hookedVtable1[i] = (*vtable1)[i];
        }

        state.presentOriginal =
            reinterpret_cast<PresentHookState::PresentProc>(
                state.originalVtable1[8]);
        state.present1Original =
            reinterpret_cast<PresentHookState::Present1Proc>(
                state.originalVtable1[22]);

        if (state.presentOriginal == nullptr) {
            state.originalVtable1.fill(nullptr);
            state.hookedVtable1.fill(nullptr);
            return;
        }

        if (state.present1Original == nullptr) {
            log_line("[FrontierD3D] IDXGISwapChain1 Present1 slot=22 is null");
        } else {
            state.hookedVtable1[22] =
                reinterpret_cast<void*>(&frontier_present1);
        }
        state.extendedVtableEntryCount = extendedVtableEntries;
        state.hookedVtable1[8] =
            reinterpret_cast<void*>(&frontier_present);

        // Collect all COM metadata and ownership before changing the object's
        // vtable. After the swap, avoid AddRef/Release/GetDesc calls through
        // the freshly-installed table on the creation/activation boundary.
        DXGI_SWAP_CHAIN_DESC1 desc1{};
        HWND hwnd1 = nullptr;
        const bool haveDesc1 = SUCCEEDED(swapChain1->GetDesc1(&desc1));
        const bool haveHwnd1 = SUCCEEDED(swapChain1->GetHwnd(&hwnd1));
        ComPtr<IDXGISwapChain> heldSwapChain = swapChain;
        ComPtr<IDXGISwapChain1> heldSwapChain1 = swapChain1;

        state.originalVtable1Address = *vtable1;
        *vtable1 = state.hookedVtable1.data();

        state.hookedSwapChain = heldSwapChain.Get();
        state.hookedSwapChainRef = std::move(heldSwapChain);
        state.hookedSwapChain1Ref = std::move(heldSwapChain1);
        state.swapchainHooked = true;
        state.swapchain1Hooked = true;
        state.swapchainUsesExtendedVtable = true;
        state.capturedCommandQueue.Reset();
        if (commandQueueHint != nullptr) {
            state.capturedCommandQueue = commandQueueHint;
            log_line("[FrontierD3D] captured D3D12 command queue from swapchain creation");
        }

        std::ostringstream message;
        message << "[FrontierD3D] hooked IDXGISwapChain1 swapchain="
                << static_cast<const void*>(swapChain1.Get())
                << " sameObject="
                << (swapChain1.Get() == swapChain ? 1 : 0)
                << " hwnd=" << (haveHwnd1 ? hwnd1 : nullptr)
                << " size=" << (haveDesc1 ? desc1.Width : 0)
                << "x" << (haveDesc1 ? desc1.Height : 0)
                << " format="
                << (haveDesc1 ? static_cast<int>(desc1.Format) : -1)
                << " presentSlot=8"
                << " present1Slot=22"
                << " publicVtableEntries=" << minimumVtableEntries
                << " shadowVtableEntries=" << extendedVtableEntries;
        log_line(message.str());

        if (swapChain1.Get() == swapChain) {
            // The derived interface is the same COM object pointer, so the
            // extended vtable already services both Present and Present1.
            return;
        }

        // A separate interface pointer/object is unusual but valid. Preserve
        // the base interface independently in that case.
        const std::size_t baseVtableEntries = readable_vtable_entries(
            *baseVtable,
            18u,
            state.hookedVtable.size());
        if (baseVtableEntries < 18u) {
            log_line("[FrontierD3D] IDXGISwapChain base vtable is shorter than the public ABI; refusing vtable shadow hook");
            return;
        }

        for (std::size_t i = 0; i < baseVtableEntries; ++i) {
            state.originalVtable[i] = (*baseVtable)[i];
            state.hookedVtable[i] = (*baseVtable)[i];
        }

        state.hookedVtable[8] =
            reinterpret_cast<void*>(&frontier_present);
        state.originalVtableAddress = *baseVtable;
        *baseVtable = state.hookedVtable.data();
        state.swapchainUsesExtendedVtable = false;
        state.capturedCommandQueue.Reset();
        if (commandQueueHint != nullptr) {
            state.capturedCommandQueue = commandQueueHint;
            log_line("[FrontierD3D] captured D3D12 command queue from swapchain creation");
        }

        return;
    }

    // Legacy/fallback path: only IDXGISwapChain is available.
    const std::size_t baseVtableEntries = readable_vtable_entries(
        *baseVtable,
        18u,
        state.hookedVtable.size());
    if (baseVtableEntries < 18u) {
        log_line("[FrontierD3D] IDXGISwapChain base vtable is shorter than the public ABI; refusing legacy vtable shadow hook");
        return;
    }

    for (std::size_t i = 0; i < baseVtableEntries; ++i) {
        state.originalVtable[i] = (*baseVtable)[i];
        state.hookedVtable[i] = (*baseVtable)[i];
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

    state.originalVtableAddress = *baseVtable;
    *baseVtable = state.hookedVtable.data();
    state.hookedSwapChain = swapChain;
    state.hookedSwapChainRef = swapChain;
    state.swapchainHooked = true;
    state.swapchainUsesExtendedVtable = false;
    state.capturedCommandQueue.Reset();
    if (commandQueueHint != nullptr) {
        state.capturedCommandQueue = commandQueueHint;
        log_line("[FrontierD3D] captured D3D12 command queue from swapchain creation");
    }

    std::ostringstream message;
    message << "[FrontierD3D] hooked IDXGISwapChain swapchain="
            << static_cast<const void*>(swapChain)
            << " hwnd=" << (haveDesc ? desc.OutputWindow : nullptr)
            << " size=" << (haveDesc ? desc.BufferDesc.Width : 0)
            << "x" << (haveDesc ? desc.BufferDesc.Height : 0)
            << " format="
            << (haveDesc ? static_cast<int>(desc.BufferDesc.Format) : -1)
            << " presentSlot=8"
            << " vtableEntries=18"
            << " present1Unavailable=1";
    log_line(message.str());
}

HRESULT WINAPI frontier_create_dxgi_factory(
    REFIID riid,
    void** factory) {
    PresentHookState::CreateDXGIFactoryProc original = nullptr;
    {
        std::lock_guard lock(g_presentHook.mutex);
        original = g_presentHook.createFactoryOriginal;
    }
    if (original == nullptr) return E_FAIL;

    const HRESULT result = original(riid, factory);
    if (SUCCEEDED(result) && factory != nullptr && *factory != nullptr) {
        hook_factory(
            g_presentHook,
            reinterpret_cast<IDXGIFactory*>(*factory));
    }
    return result;
}

HRESULT WINAPI frontier_create_dxgi_factory1(
    REFIID riid,
    void** factory) {
    PresentHookState::CreateDXGIFactoryProc original = nullptr;
    {
        std::lock_guard lock(g_presentHook.mutex);
        original = g_presentHook.createFactory1Original;
    }
    if (original == nullptr) return E_FAIL;

    const HRESULT result = original(riid, factory);
    if (SUCCEEDED(result) && factory != nullptr && *factory != nullptr) {
        hook_factory(
            g_presentHook,
            reinterpret_cast<IDXGIFactory*>(*factory));
    }
    return result;
}

HRESULT WINAPI frontier_create_dxgi_factory2(
    UINT flags,
    REFIID riid,
    void** factory) {
    PresentHookState::CreateDXGIFactory2Proc original = nullptr;
    {
        std::lock_guard lock(g_presentHook.mutex);
        original = g_presentHook.createFactory2Original;
    }
    if (original == nullptr) return E_FAIL;

    {
        std::ostringstream message;
        message << "[FrontierD3D] CreateDXGIFactory2 intercepted riid=" 
                << static_cast<const void*>(&riid)
                << " flags=0x" << std::hex << flags;
        log_line(message.str());
    }

    const HRESULT result = original(flags, riid, factory);
    {
        std::ostringstream message;
        message << "[FrontierD3D] CreateDXGIFactory2 result=0x"
                << std::hex << static_cast<unsigned long>(result)
                << " factory=" << (factory != nullptr ? *factory : nullptr);
        log_line(message.str());
    }
    if (SUCCEEDED(result) && factory != nullptr && *factory != nullptr) {
        hook_factory(
            g_presentHook,
            reinterpret_cast<IDXGIFactory*>(*factory));
    }
    return result;
}

HRESULT STDMETHODCALLTYPE frontier_factory_create_swap_chain(
    IDXGIFactory* factory,
    IUnknown* device,
    const DXGI_SWAP_CHAIN_DESC* desc,
    IDXGISwapChain** swapChain) {
    PresentHookState::CreateSwapChainProc original = nullptr;
    {
        std::lock_guard lock(g_presentHook.mutex);
        for (const auto& hook : g_presentHook.factoryHooks) {
            if (hook.factory.Get() == factory) {
                original = reinterpret_cast<PresentHookState::CreateSwapChainProc>(
                    hook.createSwapChainOriginal);
                break;
            }
        }
    }
    if (original == nullptr) return E_FAIL;

    const HRESULT result = original(factory, device, desc, swapChain);
    if (SUCCEEDED(result) && swapChain != nullptr && *swapChain != nullptr) {
        log_line("[FrontierD3D] DXGI CreateSwapChain produced swapchain");
        ComPtr<ID3D12CommandQueue> commandQueueHint;
        if (device != nullptr) {
            device->QueryInterface(IID_PPV_ARGS(&commandQueueHint));
        }
        defer_swapchain_hook(
            g_presentHook,
            *swapChain,
            commandQueueHint.Get());
    }
    return result;
}

HRESULT STDMETHODCALLTYPE frontier_factory_create_swap_chain_for_hwnd(
    IDXGIFactory2* factory,
    IUnknown* device,
    HWND window,
    const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
    IDXGIOutput* restrictToOutput,
    IDXGISwapChain1** swapChain) {
    PresentHookState::CreateSwapChainForHwndProc original = nullptr;
    {
        std::lock_guard lock(g_presentHook.mutex);
        for (const auto& hook : g_presentHook.factoryHooks) {
            if (hook.factory2.Get() == factory) {
                original = reinterpret_cast<PresentHookState::CreateSwapChainForHwndProc>(
                    hook.createSwapChainForHwndOriginal);
                break;
            }
        }
    }
    if (original == nullptr) return E_FAIL;

    {
        std::ostringstream message;
        message << "[FrontierD3D] CreateSwapChainForHwnd intercepted factory="
                << static_cast<const void*>(factory)
                << " hwnd=" << window
                << " desc="
                << (desc != nullptr ? static_cast<int>(desc->Format) : -1)
                << "x"
                << (desc != nullptr ? static_cast<int>(desc->Width) : 0);
        log_line(message.str());
    }

    const HRESULT result = original(
        factory,
        device,
        window,
        desc,
        fullscreenDesc,
        restrictToOutput,
        swapChain);

    {
        std::ostringstream message;
        message << "[FrontierD3D] CreateSwapChainForHwnd result=0x"
                << std::hex << static_cast<unsigned long>(result)
                << " swapchain="
                << (swapChain != nullptr ? *swapChain : nullptr);
        log_line(message.str());
    }

    if (SUCCEEDED(result) && swapChain != nullptr && *swapChain != nullptr) {
        log_line("[FrontierD3D] DXGI CreateSwapChainForHwnd produced swapchain");
        ComPtr<ID3D12CommandQueue> commandQueueHint;
        if (device != nullptr) {
            device->QueryInterface(IID_PPV_ARGS(&commandQueueHint));
        }
        defer_swapchain_hook(
            g_presentHook,
            reinterpret_cast<IDXGISwapChain*>(*swapChain),
            commandQueueHint.Get());
    }
    return result;
}

HRESULT STDMETHODCALLTYPE frontier_factory_create_swap_chain_for_composition(
    IDXGIFactory2* factory,
    IUnknown* device,
    const DXGI_SWAP_CHAIN_DESC1* desc,
    IDXGIOutput* restrictToOutput,
    IDXGISwapChain1** swapChain) {
    PresentHookState::CreateSwapChainForCompositionProc original = nullptr;
    {
        std::lock_guard lock(g_presentHook.mutex);
        for (const auto& hook : g_presentHook.factoryHooks) {
            if (hook.factory2.Get() == factory) {
                original = reinterpret_cast<PresentHookState::CreateSwapChainForCompositionProc>(
                    hook.createSwapChainForCompositionOriginal);
                break;
            }
        }
    }
    if (original == nullptr) return E_FAIL;

    const HRESULT result = original(
        factory,
        device,
        desc,
        restrictToOutput,
        swapChain);

    if (SUCCEEDED(result) && swapChain != nullptr && *swapChain != nullptr) {
        log_line("[FrontierD3D] DXGI CreateSwapChainForComposition produced swapchain");
        ComPtr<ID3D12CommandQueue> commandQueueHint;
        if (device != nullptr) {
            device->QueryInterface(IID_PPV_ARGS(&commandQueueHint));
        }
        defer_swapchain_hook(
            g_presentHook,
            reinterpret_cast<IDXGISwapChain*>(*swapChain),
            commandQueueHint.Get());
    }
    return result;
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
    PresentHookState::CreateDeviceAndSwapChainProc original = nullptr;
    {
        std::lock_guard lock(g_presentHook.mutex);
        original = g_presentHook.createOriginal;
    }
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
        log_line("[FrontierD3D] RDR D3D11CreateDeviceAndSwapChain succeeded");
        defer_swapchain_hook(g_presentHook, *swapChain, nullptr);
    } else {
        log_line(
            "[FrontierD3D] RDR D3D11CreateDeviceAndSwapChain result=0x" +
            [] (HRESULT value) {
                std::ostringstream stream;
                stream << std::hex << static_cast<unsigned long>(value);
                return stream.str();
            }(result));
    }

    return result;
}

void unhook_render_path() {
    std::lock_guard lock(g_presentHook.mutex);

    g_presentHook.overlay.store(nullptr, std::memory_order_release);

    if (g_presentHook.sharedPresentPatched &&
        g_presentHook.sharedSwapChainVtable != nullptr &&
        g_presentHook.sharedSwapChainVtable[8] ==
            reinterpret_cast<void*>(&frontier_present)) {
        DWORD oldProtect = 0;
        if (VirtualProtect(
                &g_presentHook.sharedSwapChainVtable[8],
                sizeof(void*),
                PAGE_READWRITE,
                &oldProtect)) {
            g_presentHook.sharedSwapChainVtable[8] =
                g_presentHook.sharedPresentOriginal;
            DWORD ignored = 0;
            VirtualProtect(
                &g_presentHook.sharedSwapChainVtable[8],
                sizeof(void*),
                oldProtect,
                &ignored);
            FlushInstructionCache(
                GetCurrentProcess(),
                &g_presentHook.sharedSwapChainVtable[8],
                sizeof(void*));
        }
    }

    if (g_presentHook.sharedPresent1Patched &&
        g_presentHook.sharedSwapChain1Vtable != nullptr &&
        g_presentHook.sharedSwapChain1Vtable[22] ==
            reinterpret_cast<void*>(&frontier_present1)) {
        DWORD oldProtect = 0;
        if (VirtualProtect(
                &g_presentHook.sharedSwapChain1Vtable[22],
                sizeof(void*),
                PAGE_READWRITE,
                &oldProtect)) {
            g_presentHook.sharedSwapChain1Vtable[22] =
                g_presentHook.sharedPresent1Original;
            DWORD ignored = 0;
            VirtualProtect(
                &g_presentHook.sharedSwapChain1Vtable[22],
                sizeof(void*),
                oldProtect,
                &ignored);
            FlushInstructionCache(
                GetCurrentProcess(),
                &g_presentHook.sharedSwapChain1Vtable[22],
                sizeof(void*));
        }
    }

    if (g_presentHook.historicalPresentHook != nullptr) {
        g_presentHook.historicalPresentHook->uninstall();
        g_presentHook.historicalPresentHook.reset();
    }
    g_presentHook.historicalPresentDetourAttached = false;
    g_presentHook.sharedSwapChainVtable = nullptr;
    g_presentHook.sharedPresentOriginal = nullptr;
    g_presentHook.sharedPresentPatched = false;
    g_presentHook.sharedSwapChain1Vtable = nullptr;
    g_presentHook.sharedPresent1Original = nullptr;
    g_presentHook.sharedPresent1Patched = false;
    g_presentHook.commandQueueOffsets.fill(0);
    g_presentHook.commandQueueOffsetCount = 0;
    g_presentHook.activeCommandQueueOffset =
        static_cast<std::size_t>(-1);
    g_presentHook.capturedCommandQueue.Reset();
    g_presentHook.pendingSwapChain.Reset();
    g_presentHook.pendingSwapChainQueue.Reset();

    if (g_presentHook.swapchainUsesExtendedVtable &&
        g_presentHook.swapchain1Hooked &&
        g_presentHook.hookedSwapChain1Ref != nullptr) {
        void*** vtable1 =
            reinterpret_cast<void***>(g_presentHook.hookedSwapChain1Ref.Get());
        if (vtable1 != nullptr &&
            *vtable1 == g_presentHook.hookedVtable1.data() &&
            g_presentHook.originalVtable1Address != nullptr) {
            *vtable1 = g_presentHook.originalVtable1Address;
        }
    } else if (g_presentHook.swapchainHooked &&
               g_presentHook.hookedSwapChain != nullptr) {
        void*** vtable =
            reinterpret_cast<void***>(g_presentHook.hookedSwapChain);
        if (vtable != nullptr &&
            *vtable == g_presentHook.hookedVtable.data() &&
            g_presentHook.originalVtableAddress != nullptr) {
            *vtable = g_presentHook.originalVtableAddress;
        }
    }

    if (g_presentHook.swapchain1Hooked &&
        !g_presentHook.swapchainUsesExtendedVtable &&
        g_presentHook.hookedSwapChain1Ref != nullptr) {
        void*** vtable1 =
            reinterpret_cast<void***>(g_presentHook.hookedSwapChain1Ref.Get());
        if (vtable1 != nullptr &&
            *vtable1 == g_presentHook.hookedVtable1.data() &&
            g_presentHook.originalVtable1Address != nullptr) {
            *vtable1 = g_presentHook.originalVtable1Address;
        }
    }

    g_presentHook.hookedSwapChain = nullptr;
    g_presentHook.hookedSwapChainRef.Reset();
    g_presentHook.hookedSwapChain1Ref.Reset();
    g_presentHook.hookedVtable.fill(nullptr);
    g_presentHook.originalVtable.fill(nullptr);
    g_presentHook.hookedVtable1.fill(nullptr);
    g_presentHook.originalVtable1.fill(nullptr);
    g_presentHook.extendedVtableEntryCount = 0;
    g_presentHook.originalVtableAddress = nullptr;
    g_presentHook.originalVtable1Address = nullptr;
    g_presentHook.presentOriginal = nullptr;
    g_presentHook.present1Original = nullptr;
    g_presentHook.historicalPresentDetourAttached = false;
    g_presentHook.swapchainHooked = false;
    g_presentHook.swapchain1Hooked = false;
    g_presentHook.swapchainUsesExtendedVtable = false;

    for (auto& hook : g_presentHook.factoryHooks) {
        if (hook.factory != nullptr && hook.originalBaseVtable != nullptr) {
            void*** vtable = reinterpret_cast<void***>(hook.factory.Get());
            if (vtable != nullptr) {
                const void* expected =
                    hook.factory2SharesBaseObject
                        ? static_cast<const void*>(hook.hookedFactory2Vtable.data())
                        : static_cast<const void*>(hook.hookedBaseVtable.data());
                if (*vtable == expected) {
                    *vtable = hook.originalBaseVtable;
                }
            }
        }

        if (!hook.factory2SharesBaseObject &&
            hook.factory2 != nullptr &&
            hook.originalFactory2Vtable != nullptr) {
            void*** vtable2 = reinterpret_cast<void***>(hook.factory2.Get());
            if (vtable2 != nullptr &&
                *vtable2 == hook.hookedFactory2Vtable.data()) {
                *vtable2 = hook.originalFactory2Vtable;
            }
        }
    }
    g_presentHook.factoryHooks.clear();

    for (auto& patch : g_presentHook.factoryImports) {
        restore_import(patch);
    }
    g_presentHook.createFactoryOriginal = nullptr;
    g_presentHook.createFactory1Original = nullptr;
    g_presentHook.createFactory2Original = nullptr;
    g_presentHook.factoryImportCount = 0;

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
        log_line("[FrontierD3D] RDR.exe module unavailable while installing historical Present detour");
        return false;
    }

    g_presentHook.overlay.store(overlay, std::memory_order_release);

    std::string error;
    if (!install_historical_present_hook(g_presentHook, error)) {
        g_presentHook.overlay.store(nullptr, std::memory_order_release);
        log_line(
            "[FrontierD3D] historical Present detour failed: " +
            (error.empty() ? "unknown error" : error));
        return false;
    }

    g_presentHook.installed = true;
    log_line(
        "[FrontierD3D] RDRMP-compatible render path active: "
        "shared Present detour only; DXGI factories and RDR swapchain vtables untouched");
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
            g_presentHook.historicalPresentDetourAttached;
    }

    static std::atomic<std::uint32_t> traceCount{0};
    const std::uint32_t trace = traceCount.fetch_add(1, std::memory_order_relaxed);
    if (trace < 8) {
        std::ostringstream message;
        message << "[FrontierD3D] Present trace=" << trace
                << " swapchain=" << static_cast<const void*>(swapChain)
                << " hooked=" << (shouldRender ? 1 : 0)
                << " historicalDetour="
                << (g_presentHook.historicalPresentDetourAttached ? 1 : 0)
                << " sync=" << syncInterval
                << " flags=0x" << std::hex << flags;
        log_line(message.str());
    }

    if (shouldRender && overlay != nullptr) {
        overlay->on_present(swapChain);
    }

    return original != nullptr
        ? original(swapChain, syncInterval, flags)
        : E_FAIL;
}

HRESULT STDMETHODCALLTYPE frontier_present1(
    IDXGISwapChain1* swapChain,
    UINT syncInterval,
    UINT flags,
    const DXGI_PRESENT_PARAMETERS* parameters) {
    PresentHookState::Present1Proc original = nullptr;
    CefOverlay* overlay = nullptr;

    bool shouldRender = false;
    {
        std::lock_guard lock(g_presentHook.mutex);
        original = g_presentHook.present1Original;
        overlay = g_presentHook.overlay.load(std::memory_order_acquire);
        // Historical RDRMP detoured Present, not Present1.
        shouldRender = false;
    }

    static std::atomic<std::uint32_t> traceCount{0};
    const std::uint32_t trace = traceCount.fetch_add(1, std::memory_order_relaxed);
    if (trace < 8) {
        std::ostringstream message;
        message << "[FrontierD3D] Present1 trace=" << trace
                << " swapchain=" << static_cast<const void*>(swapChain)
                << " hooked=" << (shouldRender ? 1 : 0)
                << " sync=" << syncInterval
                << " flags=0x" << std::hex << flags
                << " params=" << static_cast<const void*>(parameters);
        log_line(message.str());
    }

    if (shouldRender && overlay != nullptr) {
        overlay->on_present(reinterpret_cast<IDXGISwapChain*>(swapChain));
    }

    return original != nullptr
        ? original(swapChain, syncInterval, flags, parameters)
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
    bool firstCompositeLogged{};

    std::uint32_t renderReadyStreak{};
    bool renderActivationLogged{};

    ComPtr<ID3D11Device> d3dDevice{};
    ComPtr<ID3D11DeviceContext> d3dContext{};
    ComPtr<ID3D11On12Device> d3d11On12Device{};
    ComPtr<ID3D12Device> d3d12Device{};
    ComPtr<ID3D12CommandQueue> d3d12Queue{};
    ComPtr<ID3D11Texture2D> uiTexture{};

    std::vector<ComPtr<ID3D12Resource>> sourceBackBuffers12{};
    std::vector<ComPtr<ID3D11Resource>> wrappedBackBuffers{};
    std::vector<ComPtr<ID3D11RenderTargetView>> wrappedBackBufferViews{};
    ComPtr<ID3D11ShaderResourceView> uiTextureView{};
    ComPtr<ID3D11Texture2D> backBuffer{};
    ComPtr<ID3D11RenderTargetView> backBufferView{};
    ComPtr<ID3D11Buffer> vertexBuffer{};
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

void CefOverlay::set_frontend_command_handler(
    FrontendCommandHandler handler) {
    std::lock_guard lock(frontendCommandMutex_);
    frontendCommandHandler_ = std::move(handler);
}

bool CefOverlay::handle_frontend_command(
    const std::string& command,
    const std::vector<std::string>& arguments) {
    if (command == "connect") {
        frontendConnectRequested_.store(true, std::memory_order_release);

        FrontendCommandHandler handler;
        {
            std::lock_guard lock(frontendCommandMutex_);
            handler = frontendCommandHandler_;
        }

        if (handler) {
            handler(command, arguments);
        }

        log_line("[FrontierCEF] frontend connect command received");
        return true;
    }

    if (command == "quit") {
        if (bridge_ == nullptr) {
            return false;
        }

        std::string queueError;
        const bool queued = bridge_->submit_game_thread(
            [this] {
                if (state_ != nullptr &&
                    state_->gameWindow != nullptr &&
                    IsWindow(state_->gameWindow)) {
                    PostMessageA(state_->gameWindow, WM_CLOSE, 0, 0);
                }
            },
            queueError);

        if (!queued) {
            log_line(
                "[FrontierCEF] frontend quit command queue failed: " +
                (queueError.empty() ? "unknown error" : queueError));
        } else {
            log_line("[FrontierCEF] frontend quit command queued");
        }
        return queued;
    }

    return false;
}

bool CefOverlay::start(frontier::game::RdrBridge& bridge) {
    if (thread_.joinable()) return true;

    bridge_ = &bridge;
    stopRequested_.store(false, std::memory_order_release);
    frontendConnectRequested_.store(false, std::memory_order_release);
    g_activeOverlay.store(this, std::memory_order_release);

    // Mirror the historical RDRMP startup path: CEF initializes early, while
    // graphics integration uses one shared Present detour only.
    if (!install_render_path(this)) {
        log_line("[FrontierCEF] D3D render path not installed; CEF will still initialize for diagnostics");
    }

    try {
        thread_ = std::thread([this] { thread_main(); });
    } catch (...) {
        g_activeOverlay.store(nullptr, std::memory_order_release);
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
    frontendConnectRequested_.store(false, std::memory_order_release);

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

    g_activeOverlay.store(nullptr, std::memory_order_release);
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
    log_line("[FrontierCEF] using historical RDRMP lifecycle: CEF early, graphics lazy on Present");

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
    // CEF 151+ enables transparent painting by default for windowless
    // browsers. The transparent page background lets the native RDR scene
    // remain visible beneath the HTML controls.
    windowInfo.SetAsWindowless(state.gameWindow);

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
    state.initialized = true;
    attach_window_input_on_game_thread();
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

    // Keep CEF's browser/message pump independent from graphics setup.

    // The historical shared Present detour is already installed. The first
    // real RDR Present performs the lazy D3D11On12 initialization.
    CefDoMessageLoopWork();

    if (frontendConnectRequested_.load(std::memory_order_acquire) &&
        bridge_ != nullptr &&
        bridge_->game_thread_dispatcher_attached()) {
        std::string bootstrapLog;
        const bool bootstrapComplete =
            bridge_->advance_historical_online_bootstrap(bootstrapLog);

        if (!bootstrapLog.empty()) {
            log_line(bootstrapLog);
        }

        if (bootstrapComplete ||
            bridge_->historical_online_bootstrap_complete()) {
            frontendConnectRequested_.store(false, std::memory_order_release);
            log_line("[FrontierCEF] frontend multiplayer transition complete");
        }
    }

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
        state.backBuffer.Reset();
        state.backBufferView.Reset();
        state.vertexBuffer.Reset();
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

    if (state.backBuffer.Get() != backBuffer.Get()) {
        state.backBuffer = backBuffer;
        state.backBufferView.Reset();

        if (FAILED(state.d3dDevice->CreateRenderTargetView(
                state.backBuffer.Get(),
                nullptr,
                &state.backBufferView))) {
            log_line("[FrontierD3D] CreateRenderTargetView failed");
            state.backBuffer.Reset();
            return false;
        }
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
        state.backBufferView.GetAddressOf(),
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

    if (state.vertexBuffer == nullptr) {
        D3D11_BUFFER_DESC bufferDesc{};
        bufferDesc.ByteWidth = sizeof(vertices);
        bufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
        bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

        D3D11_SUBRESOURCE_DATA bufferData{};
        bufferData.pSysMem = vertices;

        if (FAILED(state.d3dDevice->CreateBuffer(
                &bufferDesc,
                &bufferData,
                &state.vertexBuffer))) {
            log_line("[FrontierD3D] vertex buffer creation failed");
            return false;
        }
    }

    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    ID3D11Buffer* vertexBuffers[]{
        state.vertexBuffer.Get()
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

namespace {

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
        g_activeOverlay.load(std::memory_order_acquire);

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

        host->SetFocus(true);
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
        // Never swallow the native Windows close accelerator. The historical
        // CEF menu also exposed an explicit Quit action; this preserves Alt+F4
        // even while the CEF browser owns keyboard input.
        if ((message == WM_SYSKEYDOWN || message == WM_SYSKEYUP) &&
            wParam == VK_F4 &&
            (GetKeyState(VK_MENU) & 0x8000) != 0) {
            return false;
        }

        CefKeyEvent event{};
        event.windows_key_code = static_cast<int>(wParam);
        event.native_key_code =
            static_cast<int>(lParam);
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
        event.native_key_code =
            static_cast<int>(lParam);
        event.is_system_key = message == WM_SYSCHAR;
        host->SendKeyEvent(event);
        result = 0;
        return true;
    }
    case WM_KILLFOCUS:
        host->SetFocus(false);
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

    // RDRMP pumped CEF independently from its graphics bridge.
    CefDoMessageLoopWork();

    std::lock_guard frameLock(state_->frameMutex);
    if (state_->frame.empty() ||
        state_->frameWidth <= 0 ||
        state_->frameHeight <= 0 ||
        state_->frameGeneration == 0) {
        return;
    }

    bool initializedGraphicsBridgeThisPresent = false;
    ComPtr<ID3D12Device> d3d12Device;
    if (SUCCEEDED(swapChain->GetDevice(
            IID_PPV_ARGS(&d3d12Device))) &&
        d3d12Device != nullptr) {
        ComPtr<ID3D12CommandQueue> commandQueue;
        std::size_t queueOffset = static_cast<std::size_t>(-1);
        bool historicalQueueLookup = false;
        {
            std::lock_guard lock(g_presentHook.mutex);
            historicalQueueLookup = g_presentHook.sharedPresentPatched;
            if (!historicalQueueLookup &&
                g_presentHook.capturedCommandQueue != nullptr) {
                commandQueue = g_presentHook.capturedCommandQueue;
            }
        }

        if (historicalQueueLookup &&
            !locate_command_queue(
                g_presentHook,
                swapChain,
                commandQueue,
                queueOffset)) {
            static std::atomic<std::uint32_t> missingQueueTrace{0};
            const auto trace = missingQueueTrace.fetch_add(1, std::memory_order_relaxed);
            if (trace < 8) {
                std::ostringstream message;
                message << "[FrontierD3D] historical D3D12 Present detected but command queue field was not resolved"
                        << " swapchain=" << static_cast<const void*>(swapChain)
                        << " device=" << static_cast<const void*>(d3d12Device.Get())
                        << " knownOffsets=" << g_presentHook.commandQueueOffsetCount;
                log_line(message.str());
            }
            return;
        }

        if (commandQueue == nullptr) {
            static std::atomic<std::uint32_t> missingCapturedQueueTrace{0};
            const auto trace =
                missingCapturedQueueTrace.fetch_add(1, std::memory_order_relaxed);
            if (trace < 8) {
                log_line(
                    "[FrontierD3D] targeted D3D12 Present has no captured command queue; "
                    "skipping private swapchain memory scan");
            }
            return;
        }

        if (state_->d3d12Device.Get() != d3d12Device.Get() ||
            state_->d3d12Queue.Get() != commandQueue.Get()) {
            state_->d3d12Device.Reset();
            state_->d3d12Queue.Reset();
            state_->d3d11On12Device.Reset();
            state_->d3dContext.Reset();
            state_->d3dDevice.Reset();
            state_->sourceBackBuffers12.clear();
            state_->wrappedBackBuffers.clear();
            state_->wrappedBackBufferViews.clear();

            D3D_FEATURE_LEVEL levels[]{
                D3D_FEATURE_LEVEL_11_0,
                D3D_FEATURE_LEVEL_11_1
            };
            IUnknown* queues[]{commandQueue.Get()};
            D3D_FEATURE_LEVEL chosenLevel{};

            const HRESULT bridgeResult = D3D11On12CreateDevice(
                d3d12Device.Get(),
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                levels,
                static_cast<UINT>(std::size(levels)),
                queues,
                1,
                0,
                &state_->d3dDevice,
                &state_->d3dContext,
                &chosenLevel);

            if (FAILED(bridgeResult) ||
                state_->d3dDevice == nullptr ||
                state_->d3dContext == nullptr) {
                std::ostringstream message;
                message << "[FrontierD3D] D3D11On12CreateDevice failed hr=0x"
                        << std::hex
                        << static_cast<unsigned long>(bridgeResult);
                log_line(message.str());
                return;
            }

            if (FAILED(state_->d3dDevice.As(
                    &state_->d3d11On12Device)) ||
                state_->d3d11On12Device == nullptr) {
                log_line("[FrontierD3D] QI ID3D11On12Device failed");
                state_->d3dContext.Reset();
                state_->d3dDevice.Reset();
                return;
            }

            state_->d3d12Device = d3d12Device;
            state_->d3d12Queue = commandQueue;
            initializedGraphicsBridgeThisPresent = true;

            std::ostringstream message;
            message << "[FrontierD3D] D3D11On12 bridge initialized"
                    << " device=" << static_cast<const void*>(d3d12Device.Get())
                    << " queue=" << static_cast<const void*>(commandQueue.Get())
                    << " queueOffset=" << queueOffset
                    << " queueSource="
                    << (historicalQueueLookup ? "private-swapchain-scan" : "CreateSwapChain")
                    << " featureLevel=0x" << std::hex
                    << static_cast<unsigned long>(chosenLevel);
            log_line(message.str());
        }

        if (initializedGraphicsBridgeThisPresent) {
            log_line(
                "[FrontierD3D] first real RDR Present initialized the graphics bridge; "
                "CEF draw deferred to the next Present");
            return;
        }

        ComPtr<IDXGISwapChain3> swapChain3;
        if (FAILED(swapChain->QueryInterface(
                IID_PPV_ARGS(&swapChain3))) ||
            swapChain3 == nullptr) {
            log_line("[FrontierD3D] IDXGISwapChain3 unavailable");
            return;
        }

        const UINT bufferIndex =
            swapChain3->GetCurrentBackBufferIndex();
        const UINT bufferCount = std::max<UINT>(
            1u,
            std::min<UINT>(desc.BufferCount, 8u));

        if (state_->sourceBackBuffers12.size() != bufferCount) {
            state_->sourceBackBuffers12.resize(bufferCount);
            state_->wrappedBackBuffers.resize(bufferCount);
            state_->wrappedBackBufferViews.resize(bufferCount);
        }

        if (bufferIndex >= state_->sourceBackBuffers12.size()) {
            log_line("[FrontierD3D] current backbuffer index out of range");
            return;
        }

        ComPtr<ID3D12Resource> backBuffer12;
        if (FAILED(swapChain3->GetBuffer(
                bufferIndex,
                IID_PPV_ARGS(&backBuffer12))) ||
            backBuffer12 == nullptr) {
            log_line("[FrontierD3D] D3D12 GetBuffer(backbuffer) failed");
            return;
        }

        if (state_->sourceBackBuffers12[bufferIndex].Get() != backBuffer12.Get() ||
            state_->wrappedBackBuffers[bufferIndex] == nullptr ||
            state_->wrappedBackBufferViews[bufferIndex] == nullptr) {
            D3D11_RESOURCE_FLAGS flags{};
            flags.BindFlags = D3D11_BIND_RENDER_TARGET;

            ComPtr<ID3D11Resource> wrapped;
            const HRESULT wrapResult =
                state_->d3d11On12Device->CreateWrappedResource(
                    backBuffer12.Get(),
                    &flags,
                    D3D12_RESOURCE_STATE_RENDER_TARGET,
                    D3D12_RESOURCE_STATE_PRESENT,
                    IID_PPV_ARGS(&wrapped));

            if (FAILED(wrapResult) || wrapped == nullptr) {
                std::ostringstream message;
                message << "[FrontierD3D] CreateWrappedResource failed hr=0x"
                        << std::hex
                        << static_cast<unsigned long>(wrapResult)
                        << " bufferIndex=" << std::dec << bufferIndex;
                log_line(message.str());
                return;
            }

            ComPtr<ID3D11RenderTargetView> view;
            const HRESULT viewResult =
                state_->d3dDevice->CreateRenderTargetView(
                    wrapped.Get(),
                    nullptr,
                    &view);
            if (FAILED(viewResult) || view == nullptr) {
                std::ostringstream message;
                message << "[FrontierD3D] CreateRenderTargetView(wrapped) failed hr=0x"
                        << std::hex
                        << static_cast<unsigned long>(viewResult);
                log_line(message.str());
                return;
            }

            state_->sourceBackBuffers12[bufferIndex] = backBuffer12;
            state_->wrappedBackBuffers[bufferIndex] = wrapped;
            state_->wrappedBackBufferViews[bufferIndex] = view;
        }

        if (!create_d3d_resources(
                *state_,
                state_->d3dDevice.Get(),
                state_->frameWidth,
                state_->frameHeight)) {
            return;
        }

        if (state_->frameGeneration != state_->uploadedGeneration) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            const HRESULT mapResult =
                state_->d3dContext->Map(
                    state_->uiTexture.Get(),
                    0,
                    D3D11_MAP_WRITE_DISCARD,
                    0,
                    &mapped);
            if (FAILED(mapResult)) {
                std::ostringstream message;
                message << "[FrontierD3D] Map(CEF texture) failed hr=0x"
                        << std::hex
                        << static_cast<unsigned long>(mapResult);
                log_line(message.str());
                return;
            }

            const std::size_t rowBytes =
                static_cast<std::size_t>(state_->frameWidth) * 4u;
            for (int y = 0; y < state_->frameHeight; ++y) {
                std::memcpy(
                    static_cast<std::uint8_t*>(mapped.pData) +
                        static_cast<std::size_t>(y) * mapped.RowPitch,
                    state_->frame.data() +
                        static_cast<std::size_t>(y) * rowBytes,
                    rowBytes);
            }

            state_->d3dContext->Unmap(state_->uiTexture.Get(), 0);
            state_->uploadedGeneration = state_->frameGeneration;
        }

        ID3D11Resource* resources[]{
            state_->wrappedBackBuffers[bufferIndex].Get()
        };
        state_->d3d11On12Device->AcquireWrappedResources(
            resources,
            1);

        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = 0.0f;
        viewport.Width = static_cast<float>(desc.BufferDesc.Width);
        viewport.Height = static_cast<float>(desc.BufferDesc.Height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;

        state_->d3dContext->OMSetRenderTargets(
            1,
            state_->wrappedBackBufferViews[bufferIndex].GetAddressOf(),
            nullptr);
        state_->d3dContext->RSSetViewports(1, &viewport);
        state_->d3dContext->RSSetState(state_->rasterizerState.Get());
        state_->d3dContext->IASetInputLayout(state_->inputLayout.Get());
        state_->d3dContext->IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        const struct FrontierVertex final {
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

        if (state_->vertexBuffer == nullptr) {
            D3D11_BUFFER_DESC bufferDesc{};
            bufferDesc.ByteWidth = sizeof(vertices);
            bufferDesc.Usage = D3D11_USAGE_IMMUTABLE;
            bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

            D3D11_SUBRESOURCE_DATA bufferData{};
            bufferData.pSysMem = vertices;

            if (FAILED(state_->d3dDevice->CreateBuffer(
                    &bufferDesc,
                    &bufferData,
                    &state_->vertexBuffer))) {
                log_line("[FrontierD3D] vertex buffer creation failed");
                state_->d3d11On12Device->ReleaseWrappedResources(
                    resources,
                    1);
                state_->d3dContext->Flush();
                return;
            }
        }

        const UINT stride = sizeof(FrontierVertex);
        UINT offset = 0;
        ID3D11Buffer* vertexBuffers[]{
            state_->vertexBuffer.Get()
        };
        state_->d3dContext->IASetVertexBuffers(
            0,
            1,
            vertexBuffers,
            &stride,
            &offset);
        state_->d3dContext->VSSetShader(
            state_->vertexShader.Get(),
            nullptr,
            0);
        state_->d3dContext->PSSetShader(
            state_->pixelShader.Get(),
            nullptr,
            0);

        ID3D11ShaderResourceView* views[]{
            state_->uiTextureView.Get()
        };
        state_->d3dContext->PSSetShaderResources(0, 1, views);

        ID3D11SamplerState* samplers[]{
            state_->sampler.Get()
        };
        state_->d3dContext->PSSetSamplers(0, 1, samplers);

        const FLOAT blendFactor[4]{0.0f,0.0f,0.0f,0.0f};
        state_->d3dContext->OMSetBlendState(
            state_->blendState.Get(),
            blendFactor,
            0xffffffffu);

        state_->d3dContext->Draw(3, 0);

        ID3D11ShaderResourceView* nullViews[]{nullptr};
        state_->d3dContext->PSSetShaderResources(0, 1, nullViews);

        state_->d3d11On12Device->ReleaseWrappedResources(
            resources,
            1);
        state_->d3dContext->Flush();

        if (!state_->firstCompositeLogged) {
            state_->firstCompositeLogged = true;
            log_line(
                "[FrontierD3D] first CEF frame composited through D3D11On12 into RDR backbuffer");
        }
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

    if (!state_->firstCompositeLogged) {
        state_->firstCompositeLogged = true;
        log_line(
            "[FrontierD3D] first CEF frame composited through legacy D3D11 path");
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
    state_->d3d11On12Device.Reset();
    state_->d3d12Queue.Reset();
    state_->d3d12Device.Reset();
    state_->sourceBackBuffers12.clear();
    state_->wrappedBackBuffers.clear();
    state_->wrappedBackBufferViews.clear();
    state_->uiTexture.Reset();
    state_->uiTextureView.Reset();
    state_->backBuffer.Reset();
    state_->backBufferView.Reset();
    state_->vertexBuffer.Reset();
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
