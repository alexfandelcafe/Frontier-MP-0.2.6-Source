#pragma once

#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace frontier::game {

class NativeInvoker;

// Best-effort correlation between a live native context and the most recently
// observed GET_THIS_SCRIPT_ID result for that context.
bool record_script_context_id(void* context, std::uint32_t scriptId);
bool lookup_script_context_id(void* context, std::uint32_t& scriptId);

class GameThreadDispatcher final {
public:
    using Handler = void(*)(void* context);

    bool attach(NativeInvoker& invoker, std::string& error);
    void detach();
    bool attached() const { return attached_; }

    bool submit(std::function<void()> task, std::string& error);
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
    static void get_this_script_id_hook(void* context);
    static void get_script_name_hook(void* context);

    NativeInvoker* invoker_{};
    std::atomic<Handler> originalWait_{};
    std::atomic<Handler> originalGetThisScriptId_{};
    std::atomic<Handler> originalGetScriptName_{};
    std::atomic<bool> attached_{false};
    mutable std::mutex queueMutex_;
    std::deque<std::shared_ptr<PendingTask>> queue_;
    std::thread::id gameThreadId_{};
    std::atomic<bool> gameThreadKnown_{false};
};

} // namespace frontier::game
