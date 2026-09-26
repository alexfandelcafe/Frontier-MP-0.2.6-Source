#pragma once

#include <atomic>
#include <memory>
#include <thread>
#include <cstddef>
#include <cstdint>

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

    // Called by the CEF render handler. The buffer is BGRA and owned by CEF.
    void accept_paint(const void* buffer, int width, int height);

    // Called from the RDR D3D11 present hook. This only composites the last
    // CEF frame; it never calls into CEF.
    void on_present(::IDXGISwapChain* swapChain);

public:
    struct State;

    bool handle_window_message(
        unsigned int message,
        std::uintptr_t wParam,
        std::intptr_t lParam,
        std::intptr_t& result);

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
};

} // namespace frontier::client
