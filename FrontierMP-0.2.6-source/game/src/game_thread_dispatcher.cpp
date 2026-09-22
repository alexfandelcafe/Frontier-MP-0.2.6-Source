#include "frontier/game/game_thread_dispatcher.hpp"

#include "frontier/game/native_invoker.hpp"

#include <chrono>

namespace frontier::game {

namespace {
std::atomic<GameThreadDispatcher*> g_dispatcher{nullptr};
constexpr std::uint32_t kNativeScrThreadWait = 0x7715C03Bu;
}

bool GameThreadDispatcher::attach(NativeInvoker& invoker, std::string& error) {
    if (attached_) return true;
    if (g_dispatcher.load(std::memory_order_acquire) != nullptr) {
        error = "another game-thread dispatcher is already attached";
        return false;
    }

    NativeInvoker::NativeHandler original = nullptr;
    if (!invoker.current_handler(kNativeScrThreadWait, original)) {
        error = "scrThread::Wait handler not found";
        return false;
    }

    invoker_ = &invoker;
    originalWait_.store(original, std::memory_order_release);
    attached_.store(true, std::memory_order_release);
    gameThreadKnown_.store(false, std::memory_order_release);
    g_dispatcher.store(this, std::memory_order_release);

    NativeInvoker::NativeHandler installedOriginal = nullptr;
    if (!invoker.hook_native(kNativeScrThreadWait, &GameThreadDispatcher::wait_hook,
                             installedOriginal, &error)) {
        g_dispatcher.store(nullptr, std::memory_order_release);
        attached_.store(false, std::memory_order_release);
        invoker_ = nullptr;
        originalWait_.store(nullptr, std::memory_order_release);
        return false;
    }

    if (installedOriginal != original) {
        invoker.unhook_native(kNativeScrThreadWait, &GameThreadDispatcher::wait_hook, installedOriginal);
        g_dispatcher.store(nullptr, std::memory_order_release);
        attached_.store(false, std::memory_order_release);
        invoker_ = nullptr;
        originalWait_.store(nullptr, std::memory_order_release);
        error = "scrThread::Wait handler changed during hook installation";
        return false;
    }

    return true;
}

void GameThreadDispatcher::detach() {
    if (!attached_.load(std::memory_order_acquire)) return;

    // Keep the dispatcher visible until the table entry has been restored so a
    // concurrent call already entering wait_hook can still reach originalWait_.

    std::deque<std::shared_ptr<PendingTask>> cancelled;
    {
        std::lock_guard lock(queueMutex_);
        cancelled.swap(queue_);
    }

    for (const auto& task : cancelled) {
        {
            std::lock_guard taskLock(task->mutex);
            task->cancelled = true;
            task->completed = true;
        }
        task->cv.notify_one();
    }

    if (invoker_) {
        invoker_->unhook_native(kNativeScrThreadWait, &GameThreadDispatcher::wait_hook,
                                originalWait_.load(std::memory_order_acquire));
    }

    if (g_dispatcher.load(std::memory_order_acquire) == this) {
        g_dispatcher.store(nullptr, std::memory_order_release);
    }

    invoker_ = nullptr;
    originalWait_.store(nullptr, std::memory_order_release);
    attached_.store(false, std::memory_order_release);
    gameThreadKnown_.store(false, std::memory_order_release);
    gameThreadId_ = {};
}

bool GameThreadDispatcher::submit_and_wait(std::function<void()> task,
                                           std::uint32_t timeoutMs,
                                           std::string& error) {
    error.clear();
    if (!attached_.load(std::memory_order_acquire)) {
        error = "game-thread dispatcher not attached";
        return false;
    }
    if (!task) {
        error = "empty game-thread task";
        return false;
    }
    if (is_game_thread()) {
        task();
        return true;
    }

    auto pending = std::make_shared<PendingTask>();
    pending->fn = std::move(task);

    {
        std::lock_guard lock(queueMutex_);
        if (!attached_) {
            error = "game-thread dispatcher detached";
            return false;
        }
        queue_.push_back(pending);
    }

    std::unique_lock taskLock(pending->mutex);
    const bool signalled = pending->cv.wait_for(
        taskLock,
        std::chrono::milliseconds(timeoutMs),
        [&pending] { return pending->completed; });

    if (!signalled) {
        {
            std::lock_guard queueLock(queueMutex_);
            for (auto it = queue_.begin(); it != queue_.end(); ++it) {
                if (it->get() == pending.get()) {
                    queue_.erase(it);
                    break;
                }
            }
        }

        if (!pending->completed) {
            pending->cancelled = true;
            error = "game-thread task timed out";
            return false;
        }
    }

    if (pending->cancelled) {
        error = "game-thread task cancelled";
        return false;
    }

    return true;
}

std::size_t GameThreadDispatcher::pump(std::size_t maxTasks) {
    if (!attached_ || maxTasks == 0) return 0;

    if (!gameThreadKnown_.load(std::memory_order_acquire)) {
        gameThreadId_ = std::this_thread::get_id();
        gameThreadKnown_.store(true, std::memory_order_release);
    }

    std::size_t processed = 0;
    while (processed < maxTasks) {
        std::shared_ptr<PendingTask> task;
        {
            std::lock_guard lock(queueMutex_);
            if (queue_.empty()) break;
            task = std::move(queue_.front());
            queue_.pop_front();
        }

        bool cancelled = false;
        {
            std::lock_guard taskLock(task->mutex);
            cancelled = task->cancelled;
        }

        if (!cancelled) {
            try {
                task->fn();
            } catch (...) {
                // The task owns its result/error channel. A thrown exception must
                // not escape into the game's native dispatch path.
            }
        }

        {
            std::lock_guard taskLock(task->mutex);
            task->completed = true;
        }
        task->cv.notify_one();
        ++processed;
    }

    return processed;
}

bool GameThreadDispatcher::is_game_thread() const {
    return gameThreadKnown_.load(std::memory_order_acquire) && std::this_thread::get_id() == gameThreadId_;
}

void GameThreadDispatcher::wait_hook(void* context) {
    auto* dispatcher = g_dispatcher.load(std::memory_order_acquire);
    if (dispatcher) {
        dispatcher->pump();
        const auto original = dispatcher->originalWait_.load(std::memory_order_acquire);
        if (original) {
            original(context);
        }
        return;
    }
}

} // namespace frontier::game
