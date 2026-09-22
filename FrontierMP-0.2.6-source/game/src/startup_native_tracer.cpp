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
constexpr std::uint32_t kLaunchNewScriptWithArgs = 0xA602F586u;
constexpr std::uint32_t kSetMissionInfo = 0x3B417D4Eu;
constexpr std::uint32_t kWasLastResetForMultiplayer = 0x3B004817u;
constexpr std::uint32_t kIsLaunchRetail = 0x7CE2C2E1u;
constexpr std::uint32_t kIsSimulateStartPress = 0xD8E31D42u;

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


bool read_c_string(void* context, std::uint32_t index, char* out, std::size_t capacity) {
    if (!out || capacity == 0) return false;
    out[0] = '\0';

    std::uintptr_t pointer = 0;
    if (!read_u64_arg(context, index, pointer) || pointer == 0) return false;

#ifdef _WIN32
    __try {
        const auto* source = reinterpret_cast<const char*>(pointer);
        for (std::size_t i = 0; i + 1 < capacity; ++i) {
            const char ch = source[i];
            if (ch == '\0') return true;
            if (static_cast<unsigned char>(ch) < 0x20u || static_cast<unsigned char>(ch) > 0x7Eu) {
                return false;
            }
            out[i] = ch;
        }
        out[capacity - 1] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
        return false;
    }
#else
    (void)context;
    (void)index;
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

    if (!invoker.hook_native(kLaunchNewScriptWithArgs, &StartupNativeTracer::launch_new_script_with_args_hook,
                             originalLaunchNewScriptWithArgs_, &error)) {
        invoker.unhook_native(kLaunchNewScript, &StartupNativeTracer::launch_new_script_hook,
                              originalLaunchNewScript_);
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
        originalLaunchNewScript_ = nullptr;
        return false;
    }

    if (!invoker.hook_native(kSetMissionInfo, &StartupNativeTracer::set_mission_info_hook,
                             originalSetMissionInfo_, &error)) {
        invoker.unhook_native(kLaunchNewScriptWithArgs, &StartupNativeTracer::launch_new_script_with_args_hook,
                              originalLaunchNewScriptWithArgs_);
        invoker.unhook_native(kLaunchNewScript, &StartupNativeTracer::launch_new_script_hook,
                              originalLaunchNewScript_);
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
        originalLaunchNewScript_ = nullptr;
        originalLaunchNewScriptWithArgs_ = nullptr;
        return false;
    }

    if (!invoker.hook_native(kWasLastResetForMultiplayer, &StartupNativeTracer::was_last_reset_for_multiplayer_hook,
                             originalWasLastResetForMultiplayer_, &error)) {
        invoker.unhook_native(kSetMissionInfo, &StartupNativeTracer::set_mission_info_hook, originalSetMissionInfo_);
        invoker.unhook_native(kLaunchNewScriptWithArgs, &StartupNativeTracer::launch_new_script_with_args_hook, originalLaunchNewScriptWithArgs_);
        invoker.unhook_native(kLaunchNewScript, &StartupNativeTracer::launch_new_script_hook, originalLaunchNewScript_);
        invoker.unhook_native(kClearMissionInfo, &StartupNativeTracer::clear_mission_info_hook, originalClearMissionInfo_);
        invoker.unhook_native(kScriptDoneLoading, &StartupNativeTracer::script_done_loading_hook, originalScriptDoneLoading_);
        invoker.unhook_native(kSetStartPos, &StartupNativeTracer::set_start_pos_hook, originalSetStartPos_);
        g_tracer = nullptr; invoker_ = nullptr;
        originalSetStartPos_ = nullptr; originalScriptDoneLoading_ = nullptr; originalClearMissionInfo_ = nullptr;
        originalLaunchNewScript_ = nullptr; originalLaunchNewScriptWithArgs_ = nullptr; originalSetMissionInfo_ = nullptr;
        return false;
    }

    if (!invoker.hook_native(kIsLaunchRetail, &StartupNativeTracer::is_launch_retail_hook,
                             originalIsLaunchRetail_, &error)) {
        invoker.unhook_native(kWasLastResetForMultiplayer, &StartupNativeTracer::was_last_reset_for_multiplayer_hook, originalWasLastResetForMultiplayer_);
        invoker.unhook_native(kSetMissionInfo, &StartupNativeTracer::set_mission_info_hook, originalSetMissionInfo_);
        invoker.unhook_native(kLaunchNewScriptWithArgs, &StartupNativeTracer::launch_new_script_with_args_hook, originalLaunchNewScriptWithArgs_);
        invoker.unhook_native(kLaunchNewScript, &StartupNativeTracer::launch_new_script_hook, originalLaunchNewScript_);
        invoker.unhook_native(kClearMissionInfo, &StartupNativeTracer::clear_mission_info_hook, originalClearMissionInfo_);
        invoker.unhook_native(kScriptDoneLoading, &StartupNativeTracer::script_done_loading_hook, originalScriptDoneLoading_);
        invoker.unhook_native(kSetStartPos, &StartupNativeTracer::set_start_pos_hook, originalSetStartPos_);
        g_tracer = nullptr; invoker_ = nullptr;
        originalSetStartPos_ = nullptr; originalScriptDoneLoading_ = nullptr; originalClearMissionInfo_ = nullptr;
        originalLaunchNewScript_ = nullptr; originalLaunchNewScriptWithArgs_ = nullptr; originalSetMissionInfo_ = nullptr;
        originalWasLastResetForMultiplayer_ = nullptr;
        return false;
    }

    if (!invoker.hook_native(kIsSimulateStartPress, &StartupNativeTracer::is_simulate_start_press_hook,
                             originalIsSimulateStartPress_, &error)) {
        invoker.unhook_native(kIsLaunchRetail, &StartupNativeTracer::is_launch_retail_hook, originalIsLaunchRetail_);
        invoker.unhook_native(kWasLastResetForMultiplayer, &StartupNativeTracer::was_last_reset_for_multiplayer_hook, originalWasLastResetForMultiplayer_);
        invoker.unhook_native(kSetMissionInfo, &StartupNativeTracer::set_mission_info_hook, originalSetMissionInfo_);
        invoker.unhook_native(kLaunchNewScriptWithArgs, &StartupNativeTracer::launch_new_script_with_args_hook, originalLaunchNewScriptWithArgs_);
        invoker.unhook_native(kLaunchNewScript, &StartupNativeTracer::launch_new_script_hook, originalLaunchNewScript_);
        invoker.unhook_native(kClearMissionInfo, &StartupNativeTracer::clear_mission_info_hook, originalClearMissionInfo_);
        invoker.unhook_native(kScriptDoneLoading, &StartupNativeTracer::script_done_loading_hook, originalScriptDoneLoading_);
        invoker.unhook_native(kSetStartPos, &StartupNativeTracer::set_start_pos_hook, originalSetStartPos_);
        g_tracer = nullptr; invoker_ = nullptr;
        originalSetStartPos_ = nullptr; originalScriptDoneLoading_ = nullptr; originalClearMissionInfo_ = nullptr;
        originalLaunchNewScript_ = nullptr; originalLaunchNewScriptWithArgs_ = nullptr; originalSetMissionInfo_ = nullptr;
        originalWasLastResetForMultiplayer_ = nullptr; originalIsLaunchRetail_ = nullptr;
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
    const bool pathReadable = read_c_string(context, 0, path, sizeof(path));

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


void StartupNativeTracer::launch_new_script_with_args_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    char path[96]{};
    const bool pathReadable = read_c_string(context, 0, path, sizeof(path));
    std::int32_t argCount = 0;
    std::int32_t stackSize = 0;
    std::uintptr_t argsPtr = 0;
    const bool hasArgs = read_u64_arg(context, 1, argsPtr);
    const bool hasArgCount = read_i32_arg(context, 2, argCount);
    const bool hasStackSize = read_i32_arg(context, 3, stackSize);

    // LAUNCH_NEW_SCRIPT_WITH_ARGS declares its payload as int*, so the
    // pointed-to script arguments are 32-bit elements on this build.
    char buffer[1024]{};
    std::snprintf(buffer, sizeof(buffer),
                  "[FrontierNativeTrace] LAUNCH_NEW_SCRIPT_WITH_ARGS path=%s argsPtr=0x%llX argCount=%s%d stackSize=%s%d values=",
                  pathReadable ? path : "<unreadable>",
                  static_cast<unsigned long long>(argsPtr),
                  hasArgCount ? "" : "?", hasArgCount ? argCount : 0,
                  hasStackSize ? "" : "?", hasStackSize ? stackSize : 0);

    std::size_t used = std::strlen(buffer);
    if (hasArgs && argsPtr != 0 && hasArgCount && argCount > 0) {
        const auto count = (argCount < 8) ? static_cast<std::uint32_t>(argCount) : 8u;
        for (std::uint32_t index = 0; index < count && used + 80u < sizeof(buffer); ++index) {
            std::uint32_t raw = 0;
            bool ok = false;
#ifdef _WIN32
            __try {
                std::memcpy(&raw,
                            reinterpret_cast<const void*>(argsPtr + static_cast<std::uintptr_t>(index) * sizeof(raw)),
                            sizeof(raw));
                ok = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
            }
#endif
            if (ok) {
                float asFloat = 0.0f;
                std::memcpy(&asFloat, &raw, sizeof(asFloat));
                const int n = std::snprintf(buffer + used, sizeof(buffer) - used,
                                            "%s0x%08X i32=%d f32=%.6f",
                                            index == 0 ? "" : ",",
                                            raw,
                                            static_cast<std::int32_t>(raw),
                                            static_cast<double>(asFloat));
                if (n > 0) used += static_cast<std::size_t>(n);
            } else {
                const int n = std::snprintf(buffer + used, sizeof(buffer) - used,
                                            "%s<unreadable>",
                                            index == 0 ? "" : ",");
                if (n > 0) used += static_cast<std::size_t>(n);
            }
        }
        if (argCount > 8 && used + 8u < sizeof(buffer)) {
            std::snprintf(buffer + used, sizeof(buffer) - used, ",...");
        }
    } else if (hasArgs && argsPtr != 0 && hasArgCount && argCount > 0) {
        std::snprintf(buffer + used, sizeof(buffer) - used, "<payload-unreadable>");
    } else if (!hasArgs) {
        std::snprintf(buffer + used, sizeof(buffer) - used, "<args-pointer-unreadable>");
    } else {
        std::snprintf(buffer + used, sizeof(buffer) - used, "<no-args>");
    }

    // Preserve the original native call exactly; only inspect its return slot
    // after execution. The documented native returns int.
    if (tracer->originalLaunchNewScriptWithArgs_) {
        tracer->originalLaunchNewScriptWithArgs_(context);
    }

    std::int32_t result = 0;
    bool resultReadable = false;
#ifdef _WIN32
    if (context) {
        const auto* call = reinterpret_cast<const NativeTraceContext*>(context);
        if (call->returnBuffer) {
            __try {
                std::memcpy(&result, call->returnBuffer, sizeof(result));
                resultReadable = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                resultReadable = false;
            }
        }
    }
#endif

    if (used + 48u < sizeof(buffer)) {
        const int n = std::snprintf(buffer + used, sizeof(buffer) - used,
                                    " result=%s%d",
                                    resultReadable ? "" : "?",
                                    resultReadable ? result : 0);
        (void)n;
    }
    log(buffer);
}

void StartupNativeTracer::set_mission_info_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    std::int32_t a0 = 0;
    std::int32_t a1 = 0;
    const bool ok0 = read_i32_arg(context, 0, a0);
    const bool ok1 = read_i32_arg(context, 1, a1);

    char buffer[192]{};
    std::snprintf(buffer, sizeof(buffer),
                  "[FrontierNativeTrace] SET_MISSION_INFO(%s%d,%s%d)",
                  ok0 ? "" : "?", ok0 ? a0 : 0,
                  ok1 ? "" : "?", ok1 ? a1 : 0);
    log(buffer);

    if (tracer->originalSetMissionInfo_) tracer->originalSetMissionInfo_(context);
}

void StartupNativeTracer::was_last_reset_for_multiplayer_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    if (tracer->originalWasLastResetForMultiplayer_) {
        tracer->originalWasLastResetForMultiplayer_(context);
    }

    std::uint32_t result = 0;
    bool ok = false;
#ifdef _WIN32
    if (context) {
        const auto* call = reinterpret_cast<const NativeTraceContext*>(context);
        if (call->returnBuffer) {
            __try {
                std::memcpy(&result, call->returnBuffer, sizeof(result));
                ok = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
            }
        }
    }
#endif
    char buffer[160]{};
    std::snprintf(buffer, sizeof(buffer),
                  "[FrontierNativeTrace] WAS_LAST_RESET_FOR_MULTIPLAYER=%s%u",
                  ok ? "" : "?", ok ? result : 0u);
    log(buffer);
}

void StartupNativeTracer::is_launch_retail_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    if (tracer->originalIsLaunchRetail_) {
        tracer->originalIsLaunchRetail_(context);
    }

    std::uint32_t result = 0;
    bool ok = false;
#ifdef _WIN32
    if (context) {
        const auto* call = reinterpret_cast<const NativeTraceContext*>(context);
        if (call->returnBuffer) {
            __try {
                std::memcpy(&result, call->returnBuffer, sizeof(result));
                ok = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
            }
        }
    }
#endif
    char buffer[128]{};
    std::snprintf(buffer, sizeof(buffer),
                  "[FrontierNativeTrace] IS_LAUNCH_RETAIL=%s%u",
                  ok ? "" : "?", ok ? result : 0u);
    log(buffer);
}

void StartupNativeTracer::is_simulate_start_press_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    if (tracer->originalIsSimulateStartPress_) {
        tracer->originalIsSimulateStartPress_(context);
    }

    std::uint32_t result = 0;
    bool ok = false;
#ifdef _WIN32
    if (context) {
        const auto* call = reinterpret_cast<const NativeTraceContext*>(context);
        if (call->returnBuffer) {
            __try {
                std::memcpy(&result, call->returnBuffer, sizeof(result));
                ok = true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                ok = false;
            }
        }
    }
#endif
    char buffer[160]{};
    std::snprintf(buffer, sizeof(buffer),
                  "[FrontierNativeTrace] IS_SIMULATE_START_PRESS=%s%u",
                  ok ? "" : "?", ok ? result : 0u);
    log(buffer);
}


} // namespace frontier::game
