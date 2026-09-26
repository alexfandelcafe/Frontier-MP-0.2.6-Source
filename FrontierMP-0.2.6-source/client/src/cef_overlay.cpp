#include "frontier/client/cef_overlay.hpp"
#include "frontier/game/rdr_bridge.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"

namespace frontier::client {

namespace {

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
    IMPLEMENT_REFCOUNTING(FrontierCefApp);
};

class FrontierCefClient final
    : public CefClient
    , public CefLifeSpanHandler
    , public CefLoadHandler {
public:
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
        return this;
    }

    CefRefPtr<CefLoadHandler> GetLoadHandler() override {
        return this;
    }

    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
        browser_ = browser;
        log_line("[FrontierCEF] browser created");
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
        CefRefPtr<CefBrowser> browser,
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
    CefRefPtr<CefBrowser> browser_{};

    IMPLEMENT_REFCOUNTING(FrontierCefClient);
};

RECT overlay_rect(HWND gameWindow) {
    RECT client{};
    GetClientRect(gameWindow, &client);

    const LONG clientWidth = client.right - client.left;
    const LONG clientHeight = client.bottom - client.top;

    constexpr LONG kOverlayWidth = 820;
    constexpr LONG kOverlayHeight = 500;

    RECT rect{};
    rect.left = std::max<LONG>(0, (clientWidth - kOverlayWidth) / 2);
    rect.top = std::max<LONG>(0, (clientHeight - kOverlayHeight) / 2);
    rect.right = rect.left + std::min<LONG>(kOverlayWidth, std::max<LONG>(0, clientWidth));
    rect.bottom = rect.top + std::min<LONG>(kOverlayHeight, std::max<LONG>(0, clientHeight));
    return rect;
}

void position_browser_window(
    HWND gameWindow,
    CefRefPtr<CefBrowser> browser) {
    if (gameWindow == nullptr || browser == nullptr) return;

    HWND browserWindow = browser->GetHost()->GetWindowHandle();
    if (browserWindow == nullptr) return;

    const RECT rect = overlay_rect(gameWindow);
    SetWindowPos(
        browserWindow,
        HWND_TOP,
        rect.left,
        rect.top,
        rect.right - rect.left,
        rect.bottom - rect.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

} // namespace

struct CefOverlay::State final {
    std::filesystem::path moduleDir;
    std::filesystem::path cefDir;
    std::filesystem::path subprocess;
    std::filesystem::path ui;
    HWND gameWindow{};
    CefRefPtr<FrontierCefApp> app{};
    CefRefPtr<FrontierCefClient> client{};
    CefRefPtr<CefBrowser> browser{};
    bool initialized{};
};

CefOverlay::~CefOverlay() {
    stop();
}

bool CefOverlay::start(frontier::game::RdrBridge& bridge) {
    if (thread_.joinable()) return true;

    bridge_ = &bridge;
    stopRequested_.store(false, std::memory_order_release);
    try {
        thread_ = std::thread([this] { thread_main(); });
    } catch (...) {
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
    settings.windowless_rendering_enabled = false;
    settings.log_severity = LOGSEVERITY_WARNING;

    CefString(&settings.browser_subprocess_path) =
        state.subprocess.string();
    CefString(&settings.resources_dir_path) =
        (state.cefDir / "Resources").string();
    CefString(&settings.locales_dir_path) =
        (state.cefDir / "locales").string();

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

    log_line("[FrontierCEF] initializing on RDR game thread");
    if (!CefInitialize(mainArgs, settings, state.app, nullptr)) {
        log_line("[FrontierCEF] CefInitialize failed");
        state.app = nullptr;
        return false;
    }

    state.gameWindow = find_rdr_window();
    if (state.gameWindow == nullptr) {
        log_line("[FrontierCEF] RDR window not found");
        CefShutdown();
        state.app = nullptr;
        return false;
    }

    const RECT rect = overlay_rect(state.gameWindow);
    const CefRect cefRect(
        rect.left,
        rect.top,
        rect.right - rect.left,
        rect.bottom - rect.top);

    CefWindowInfo windowInfo;
    windowInfo.SetAsChild(state.gameWindow, cefRect);

    CefBrowserSettings browserSettings;
    state.client = new FrontierCefClient();
    const std::string url = file_url(state.ui);

    log_line("[FrontierCEF] creating browser url=" + url);
    state.browser =
        CefBrowserHost::CreateBrowserSync(
            windowInfo,
            state.client,
            url,
            browserSettings,
            nullptr,
            nullptr);

    if (state.browser == nullptr) {
        log_line("[FrontierCEF] CreateBrowserSync failed");
        state.client = nullptr;
        state.app = nullptr;
        CefShutdown();
        return false;
    }

    const HWND browserWindow =
        state.browser->GetHost()->GetWindowHandle();
    if (browserWindow != nullptr) {
        ShowWindow(browserWindow, SW_SHOW);
    }

    state.initialized = true;
    pump_on_game_thread();
    return true;
}

void CefOverlay::pump_on_game_thread() {
    if (state_ == nullptr ||
        !state_->initialized ||
        stopRequested_.load(std::memory_order_acquire)) {
        return;
    }

    State& state = *state_;
    if (state.gameWindow == nullptr ||
        !IsWindow(state.gameWindow)) {
        log_line("[FrontierCEF] RDR window was destroyed");
        shutdown_on_game_thread();
        return;
    }

    CefDoMessageLoopWork();

    if (state.browser != nullptr) {
        position_browser_window(state.gameWindow, state.browser);
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

void CefOverlay::shutdown_on_game_thread() {
    if (state_ == nullptr || !state_->initialized) return;

    State& state = *state_;
    if (state.browser != nullptr) {
        state.browser->GetHost()->CloseBrowser(true);
        state.browser = nullptr;
    }

    state.client = nullptr;
    state.app = nullptr;
    state.gameWindow = nullptr;

    CefShutdown();
    state.initialized = false;
    log_line("[FrontierCEF] shut down on RDR game thread");
}

} // namespace frontier::client
