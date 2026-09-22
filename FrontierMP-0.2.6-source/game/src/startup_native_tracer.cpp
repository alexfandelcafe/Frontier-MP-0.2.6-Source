#include "frontier/game/startup_native_tracer.hpp"

#include "frontier/game/native_invoker.hpp"

#include <cstdio>
#include <fstream>
#include <filesystem>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace frontier::game {

namespace {
StartupNativeTracer* g_tracer = nullptr;

constexpr std::uint32_t kSetStartPos = 0x0CB93120u;
constexpr std::uint32_t kScriptDoneLoading = 0x5401F0CAu;
constexpr std::uint32_t kClearMissionInfo = 0x02092A6Eu;

struct NativeTraceContext final {
    void* returnBuffer{};
    std::uint32_t argumentCount{};
    void* argumentBuffer{};
    std::uint32_t dataCount{};
    void* outputVectors[4]{};
    std::uint8_t inputVectors[0x30]{};
};

bool read_i32_arg(void* context, std::uint32_t index, std::int32_t& out) {
    out = 0;
    if (!context) return false;

    const auto* call = reinterpret_cast<const NativeTraceContext*>(context);
    if (!call->argumentBuffer || index >= call->argumentCount) return false;

#ifdef _WIN32
    __try {
        const auto* values = reinterpret_cast<const std::uintptr_t*>(call->argumentBuffer);
        out = static_cast<std::int32_t>(values[index]);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    return false;
#endif
}

void write_log_line(const char* message) {
#ifdef _WIN32
    char localAppData[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH);
    if (n != 0 && n < MAX_PATH) {
        std::filesystem::path dir = std::filesystem::path(localAppData) / "FrontierMP" / "logs";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ofstream file(dir / "client.log", std::ios::app);
        if (file) file << message << "\n";
    }
#endif
    std::fprintf(stderr, "%s\n", message);
}

} // namespace

bool StartupNativeTracer::attach(NativeInvoker& invoker, std::string& error) {
    if (attached_) return true;
    if (g_tracer != nullptr) {
        error = "another startup native tracer is already attached";
        return false;
    }

    g_tracer = this;
    invoker_ = &invoker;

    if (!invoker.hook_native(kSetStartPos, &StartupNativeTracer::set_start_pos_hook,
                             originalSetStartPos_, &error)) {
        g_tracer = nullptr;
        invoker_ = nullptr;
        return false;
    }

    if (!invoker.hook_native(kScriptDoneLoading, &StartupNativeTracer::script_done_loading_hook,
                             originalScriptDoneLoading_, &error)) {
        invoker.unhook_native(kSetStartPos, &StartupNativeTracer::set_start_pos_hook,
                              originalSetStartPos_);
        g_tracer = nullptr;
        invoker_ = nullptr;
        originalSetStartPos_ = nullptr;
        return false;
    }

    if (!invoker.hook_native(kClearMissionInfo, &StartupNativeTracer::clear_mission_info_hook,
                             originalClearMissionInfo_, &error)) {
        invoker.unhook_native(kScriptDoneLoading, &StartupNativeTracer::script_done_loading_hook,
                              originalScriptDoneLoading_);
        invoker.unhook_native(kSetStartPos, &StartupNativeTracer::set_start_pos_hook,
                              originalSetStartPos_);
        g_tracer = nullptr;
        invoker_ = nullptr;
        originalSetStartPos_ = nullptr;
        originalScriptDoneLoading_ = nullptr;
        return false;
    }

    attached_ = true;
    log("[FrontierNativeTrace] startup native tracer attached");
    return true;
}

void StartupNativeTracer::log(const char* message) {
    write_log_line(message);
}

void StartupNativeTracer::set_start_pos_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    const auto* call = reinterpret_cast<const NativeTraceContext*>(context);
    std::int32_t a0=0,a1=0,a2=0,a3=0,a4=0;
    const bool ok = read_i32_arg(context,0,a0) && read_i32_arg(context,1,a1) &&
                    read_i32_arg(context,2,a2) && read_i32_arg(context,3,a3) &&
                    read_i32_arg(context,4,a4);
    if (ok) {
        char buffer[256]{};
        std::snprintf(buffer, sizeof(buffer),
                      "[FrontierNativeTrace] SET_START_POS(%d,%d,%d,%d,%d) argc=%u args=0x%llX",
                      a0,a1,a2,a3,a4,
                      call ? call->argumentCount : 0u,
                      call ? static_cast<unsigned long long>(
                          reinterpret_cast<std::uintptr_t>(call->argumentBuffer)) : 0ull);
        log(buffer);
    } else {
        char buffer[256]{};
        std::snprintf(buffer, sizeof(buffer),
                      "[FrontierNativeTrace] SET_START_POS(<args-unreadable>) argc=%u args=0x%llX data=%u",
                      call ? call->argumentCount : 0u,
                      call ? static_cast<unsigned long long>(
                          reinterpret_cast<std::uintptr_t>(call->argumentBuffer)) : 0ull,
                      call ? call->dataCount : 0u);
        log(buffer);
    }

    if (tracer->originalSetStartPos_) tracer->originalSetStartPos_(context);
}

void StartupNativeTracer::script_done_loading_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;
    log("[FrontierNativeTrace] SCRIPT_DONE_LOADING()");
    if (tracer->originalScriptDoneLoading_) tracer->originalScriptDoneLoading_(context);
}

void StartupNativeTracer::clear_mission_info_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;
    log("[FrontierNativeTrace] CLEAR_MISSION_INFO()");
    if (tracer->originalClearMissionInfo_) tracer->originalClearMissionInfo_(context);
}

} // namespace frontier::game
