#pragma once

#include <atomic>
#include <thread>

namespace frontier::client {

class CefOverlay final {
public:
    CefOverlay() = default;
    ~CefOverlay();

    CefOverlay(const CefOverlay&) = delete;
    CefOverlay& operator=(const CefOverlay&) = delete;

    bool start();
    void stop();

private:
    void thread_main();

    std::atomic<bool> stopRequested_{false};
    std::thread thread_{};
};

} // namespace frontier::client
