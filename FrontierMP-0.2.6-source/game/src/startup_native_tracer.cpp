#include "frontier/game/startup_native_tracer.hpp"

#include "frontier/game/native_invoker.hpp"

#include <cstdio>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <cstring>

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
constexpr std::uint32_t kLaunchNewScript = 0x85A30503u;

struct NativeTraceContext final {
    void* returnBuffer{};
    std::uint32_t argumentCount{};
    void* argumentBuffer{};
    std::uint32_t dataCount{};
    void* outputVectors[4]{};
    std::uint8_t inputVectors[0x30]{};
};

bool read_u64_arg(void* context, std::uint32_t index, std::uintptr_t& out) {
    out = 0;
    if (!context) return false;

    const auto* call = reinterpret_cast<const NativeTraceContext*>(context);
    if (!call->argumentBuffer || index >= call->argumentCount) return false;

#ifdef _WIN32
    __try {
        const auto* values = reinterpret_cast<const std::uintptr_t*>(call->argumentBuffer);
        out = values[index];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = 0;
        return false;
    }
#else
    return false;
#endif
}

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

    if (!invoker.hook_native(kLaunchNewScript, &StartupNativeTracer::launch_new_script_hook,
                             originalLaunchNewScript_, &error)) {
        invoker.unhook_native(kClearMissionInfo, &StartupNativeTracer::clear_mission_info_hook,
                              originalClearMissionInfo_);
        invoker.unhook_native(kScriptDoneLoading, &StartupNativeTracer::script_done_loading_hook,
                              originalScriptDoneLoading_);
        invoker.unhook_native(kSetStartPos, &StartupNativeTracer::set_start_pos_hook,
                              originalSetStartPos_);
        g_tracer = nullptr;
        invoker_ = nullptr;
        originalSetStartPos_ = nullptr;
        originalScriptDoneLoading_ = nullptr;
        originalClearMissionInfo_ = nullptr;
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
    const auto argc = call ? call->argumentCount : 0u;
    const auto argsAddress = call
        ? static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(call->argumentBuffer))
        : 0ull;
    const auto dataCount = call ? call->dataCount : 0u;

    // The public RDR1 native declaration lists five parameters for SET_START_POS,
    // but the live call site currently reports argc=4. Trace exactly what the
    // engine supplied instead of requiring the documented arity.
    char buffer[768]{};
    int written = std::snprintf(buffer, sizeof(buffer),
                                "[FrontierNativeTrace] SET_START_POS argc=%u args=0x%llX data=%u values=",
                                argc, argsAddress, dataCount);
    if (written < 0) {
        log("[FrontierNativeTrace] SET_START_POS(<format-failed>)");
    } else {
        std::size_t used = static_cast<std::size_t>(written);
        const auto count = (argc < 8u) ? argc : 8u;
        for (std::uint32_t index = 0; index < count && used + 96u < sizeof(buffer); ++index) {
            std::uintptr_t rawSlot = 0;
            const bool ok = read_u64_arg(context, index, rawSlot);
            if (ok) {
                float asFloat = 0.0f;
                const auto raw32 = static_cast<std::uint32_t>(rawSlot);
                std::memcpy(&asFloat, &raw32, sizeof(asFloat));
                const int n = std::snprintf(buffer + used, sizeof(buffer) - used,
                                            "%s0x%08X i32=%d f32=%.6f",
                                            index == 0 ? "" : ",",
                                            raw32,
                                            static_cast<std::int32_t>(raw32),
                                            static_cast<double>(asFloat));
                if (n > 0) used += static_cast<std::size_t>(n);
            } else {
                const int n = std::snprintf(buffer + used, sizeof(buffer) - used,
                                            "%s<unreadable>",
                                            index == 0 ? "" : ",");
                if (n > 0) used += static_cast<std::size_t>(n);
            }
        }
        if (argc > count && used + 8u < sizeof(buffer)) {
            std::snprintf(buffer + used, sizeof(buffer) - used, ",...");
        }
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

void StartupNativeTracer::launch_new_script_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    std::uintptr_t scriptPtr = 0;
    std::int32_t arg1 = 0;
    const bool hasScript = read_u64_arg(context, 0, scriptPtr);
    const bool hasArg1 = read_i32_arg(context, 1, arg1);

    char path[96]{};
    bool pathReadable = false;
#ifdef _WIN32
    if (hasScript && scriptPtr != 0) {
        __try {
            const auto* source = reinterpret_cast<const char*>(scriptPtr);
            std::size_t i = 0;
            for (; i + 1 < sizeof(path); ++i) {
                const char ch = source[i];
                if (ch == '\0') {
                    pathReadable = true;
                    break;
                }
                if (static_cast<unsigned char>(ch) < 0x20u || static_cast<unsigned char>(ch) > 0x7Eu) {
                    break;
                }
                path[i] = ch;
            }
            if (i + 1 == sizeof(path)) path[sizeof(path) - 1] = '\0';
            if (!pathReadable && path[0] != '\0') pathReadable = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            pathReadable = false;
        }
    }
#endif

    char buffer[256]{};
    std::snprintf(buffer, sizeof(buffer),
                  "[FrontierNativeTrace] LAUNCH_NEW_SCRIPT path=%s ptr=0x%llX arg1=%s%d argc=%u",
                  pathReadable ? path : "<unreadable>",
                  static_cast<unsigned long long>(scriptPtr),
                  hasArg1 ? "" : "?",
                  hasArg1 ? arg1 : 0,
                  0u);
    if (context) {
        const auto* call = reinterpret_cast<const NativeTraceContext*>(context);
        const auto pos = std::strlen(buffer);
        if (pos + 16 < sizeof(buffer)) {
            std::snprintf(buffer + pos, sizeof(buffer) - pos, " supplied=%u", call->argumentCount);
        }
    }
    log(buffer);

    if (tracer->originalLaunchNewScript_) tracer->originalLaunchNewScript_(context);
}


} // namespace frontier::game
