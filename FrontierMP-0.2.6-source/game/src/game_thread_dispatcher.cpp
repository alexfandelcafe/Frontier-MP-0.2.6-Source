#include "frontier/game/game_thread_dispatcher.hpp"

#include "frontier/game/native_invoker.hpp"

#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#include <intrin.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

namespace frontier::game {

namespace {
std::atomic<GameThreadDispatcher*> g_dispatcher{nullptr};
std::atomic<std::uint32_t> g_waitTraceCount{0};
constexpr std::uint32_t kNativeScrThreadWait = 0x7715C03Bu;
constexpr std::uint32_t kNativeGetThisScriptId = 0x9C424E0Du;

struct NativeTraceContext final {
    void* returnBuffer{};
    std::uint32_t argumentCount{};
    void* argumentBuffer{};
    std::uint32_t dataCount{};
    void* outputVectors[4]{};
    std::uint8_t inputVectors[0x30]{};
    std::uintptr_t stack[32]{};
};

void write_script_trace_line(const char* message) {
    std::fprintf(stderr, "%s\n", message);
}

#ifdef _WIN32
bool read_u32_return(void* context, std::uint32_t& out) noexcept {
    out = 0;
    if (!context) return false;
    const auto* call = reinterpret_cast<const NativeTraceContext*>(context);
    if (!call->returnBuffer) return false;
    __try {
        std::memcpy(&out, call->returnBuffer, sizeof(out));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#endif
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

    NativeInvoker::NativeHandler originalGetThisScriptId = nullptr;
    if (!invoker.current_handler(kNativeGetThisScriptId, originalGetThisScriptId)) {
        error = "GET_THIS_SCRIPT_ID handler not found";
        return false;
    }

    invoker_ = &invoker;
    originalWait_.store(original, std::memory_order_release);
    originalGetThisScriptId_.store(originalGetThisScriptId, std::memory_order_release);
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
        originalGetThisScriptId_.store(nullptr, std::memory_order_release);
        return false;
    }

    if (installedOriginal != original) {
        invoker.unhook_native(kNativeScrThreadWait, &GameThreadDispatcher::wait_hook, installedOriginal);
        g_dispatcher.store(nullptr, std::memory_order_release);
        attached_.store(false, std::memory_order_release);
        invoker_ = nullptr;
        originalWait_.store(nullptr, std::memory_order_release);
        originalGetThisScriptId_.store(nullptr, std::memory_order_release);
        error = "scrThread::Wait handler changed during hook installation";
        return false;
    }

    NativeInvoker::NativeHandler installedScriptId = nullptr;
    if (!invoker.hook_native(kNativeGetThisScriptId, &GameThreadDispatcher::get_this_script_id_hook,
                             installedScriptId, &error)) {
        invoker.unhook_native(kNativeScrThreadWait, &GameThreadDispatcher::wait_hook, original);
        g_dispatcher.store(nullptr, std::memory_order_release);
        attached_.store(false, std::memory_order_release);
        invoker_ = nullptr;
        originalWait_.store(nullptr, std::memory_order_release);
        originalGetThisScriptId_.store(nullptr, std::memory_order_release);
        return false;
    }

    if (installedScriptId != originalGetThisScriptId) {
        invoker.unhook_native(kNativeGetThisScriptId, &GameThreadDispatcher::get_this_script_id_hook, installedScriptId);
        invoker.unhook_native(kNativeScrThreadWait, &GameThreadDispatcher::wait_hook, original);
        g_dispatcher.store(nullptr, std::memory_order_release);
        attached_.store(false, std::memory_order_release);
        invoker_ = nullptr;
        originalWait_.store(nullptr, std::memory_order_release);
        originalGetThisScriptId_.store(nullptr, std::memory_order_release);
        error = "GET_THIS_SCRIPT_ID handler changed during hook installation";
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
        invoker_->unhook_native(kNativeGetThisScriptId, &GameThreadDispatcher::get_this_script_id_hook,
                                originalGetThisScriptId_.load(std::memory_order_acquire));
        invoker_->unhook_native(kNativeScrThreadWait, &GameThreadDispatcher::wait_hook,
                                originalWait_.load(std::memory_order_acquire));
    }

    if (g_dispatcher.load(std::memory_order_acquire) == this) {
        g_dispatcher.store(nullptr, std::memory_order_release);
    }

    invoker_ = nullptr;
    originalWait_.store(nullptr, std::memory_order_release);
    originalGetThisScriptId_.store(nullptr, std::memory_order_release);
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
#ifdef _WIN32
        std::fprintf(stderr, "[FrontierNative] dispatcher first pump thread=%lu\\n",
                     static_cast<unsigned long>(GetCurrentThreadId()));
#endif
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
#ifdef _WIN32
        const auto traceIndex = g_waitTraceCount.fetch_add(1, std::memory_order_relaxed);
        if (traceIndex < 32) {
            char line[192]{};
            std::snprintf(line, sizeof(line),
                          "[FrontierNative] scrThread::Wait trace=%lu context=0x%llX thread=%lu ret=0x%llX",
                          static_cast<unsigned long>(traceIndex),
                          static_cast<unsigned long long>(
                              reinterpret_cast<std::uintptr_t>(context)),
                          static_cast<unsigned long>(GetCurrentThreadId()),
                          static_cast<unsigned long long>(
                              reinterpret_cast<std::uintptr_t>(_ReturnAddress())));
            write_script_trace_line(line);
        }
#endif
        dispatcher->pump();
        const auto original = dispatcher->originalWait_.load(std::memory_order_acquire);
        if (original) {
            original(context);
        }
        return;
    }
}

void GameThreadDispatcher::get_this_script_id_hook(void* context) {
    auto* dispatcher = g_dispatcher.load(std::memory_order_acquire);
    if (!dispatcher) return;

    const auto original =
        dispatcher->originalGetThisScriptId_.load(std::memory_order_acquire);
    if (original) {
        original(context);
    }

#ifdef _WIN32
    const auto traceIndex = g_waitTraceCount.fetch_add(1, std::memory_order_relaxed);
    if (traceIndex < 32) {
        std::uint32_t scriptId = 0;
        const bool ok = read_u32_return(context, scriptId);

        char line[224]{};
        std::snprintf(line, sizeof(line),
                      "[FrontierNative] GET_THIS_SCRIPT_ID trace=%lu context=0x%llX "
                      "thread=%lu id=%s%u ret=0x%llX",
                      static_cast<unsigned long>(traceIndex),
                      static_cast<unsigned long long>(
                          reinterpret_cast<std::uintptr_t>(context)),
                      static_cast<unsigned long>(GetCurrentThreadId()),
                      ok ? "" : "?",
                      ok ? scriptId : 0u,
                      static_cast<unsigned long long>(
                          reinterpret_cast<std::uintptr_t>(_ReturnAddress())));
        write_script_trace_line(line);
    }
#endif
}

} // namespace frontier::game
