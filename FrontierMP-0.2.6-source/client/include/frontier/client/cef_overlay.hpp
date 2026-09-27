#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <mutex>

struct IDXGISwapChain;

namespace frontier::game {
class RdrBridge;
}

namespace frontier::client {

class CefOverlay final {
public:
    CefOverlay();
    ~CefOverlay();

    CefOverlay(const CefOverlay&) = delete;
    CefOverlay& operator=(const CefOverlay&) = delete;

    bool start(frontier::game::RdrBridge& bridge);
    void stop();

    using FrontendCommandHandler =
        std::function<void(const std::string&, const std::vector<std::string>&)>;

    void set_frontend_command_handler(FrontendCommandHandler handler);

    // Called by the CEF render handler. The buffer is BGRA and owned by CEF.
    void accept_paint(const void* buffer, int width, int height);

    // Called from the historical RDR render/present path. This pumps CEF,
    // then composites the last CEF OSR frame through D3D11On12 when RDR is
    // running its native D3D12 renderer. A D3D11 swapchain remains supported
    // as a fallback.
    void on_present(::IDXGISwapChain* swapChain);

public:
    struct State;

    bool handle_window_message(
        unsigned int message,
        std::uintptr_t wParam,
        std::intptr_t lParam,
        std::intptr_t& result);

    bool handle_frontend_command(
        const std::string& command,
        const std::vector<std::string>& arguments);

private:
    void thread_main();
    bool initialize_on_game_thread();
    void pump_on_game_thread();
    void shutdown_on_game_thread();

    void attach_window_input_on_game_thread();
    void detach_window_input_on_game_thread();

    std::atomic<bool> stopRequested_{false};
    std::thread thread_{};
    frontier::game::RdrBridge* bridge_{};
    std::unique_ptr<State> state_{};
    std::atomic<bool> frontendConnectRequested_{false};
    mutable std::mutex frontendCommandMutex_;
    FrontendCommandHandler frontendCommandHandler_{};
};

} // namespace frontier::client
