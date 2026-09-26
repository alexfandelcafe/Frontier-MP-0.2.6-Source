#pragma once

#include <atomic>
#include <thread>

namespace frontier::game {
class RdrBridge;
}

namespace frontier::client {

class CefOverlay final {
public:
    CefOverlay() = default;
    ~CefOverlay();

    CefOverlay(const CefOverlay&) = delete;
    CefOverlay& operator=(const CefOverlay&) = delete;

    bool start(frontier::game::RdrBridge& bridge);
    void stop();

private:
    void thread_main();
    bool initialize_on_game_thread();
    void pump_on_game_thread();
    void shutdown_on_game_thread();

    std::atomic<bool> stopRequested_{false};
    std::thread thread_{};
    frontier::game::RdrBridge* bridge_{};
};

} // namespace frontier::client
