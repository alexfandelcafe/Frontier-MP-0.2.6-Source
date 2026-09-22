#pragma once

#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace frontier::game {

class NativeInvoker;

class GameThreadDispatcher final {
public:
    bool attach(NativeInvoker& invoker, std::string& error);
    void detach();
    bool attached() const { return attached_; }

    bool submit_and_wait(std::function<void()> task, std::uint32_t timeoutMs, std::string& error);
    std::size_t pump(std::size_t maxTasks = 32);

    bool is_game_thread() const;

private:
    struct PendingTask final {
        std::function<void()> fn;
        std::mutex mutex;
        std::condition_variable cv;
        bool completed{};
        bool cancelled{};
    };

    static void wait_hook(void* context);

    NativeInvoker* invoker_{};
    void (*originalWait_)(void* context){};
    bool attached_{};
    mutable std::mutex queueMutex_;
    std::deque<PendingTask*> queue_;
    std::thread::id gameThreadId_{};
    bool gameThreadKnown_{};
};

} // namespace frontier::game
