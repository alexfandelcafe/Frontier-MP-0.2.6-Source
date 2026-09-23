#include "frontier/game/startup_native_tracer.hpp"

#include "frontier/game/native_invoker.hpp"
#include "frontier/game/game_thread_dispatcher.hpp"

#include <cstdio>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#endif

namespace frontier::game {

namespace {
StartupNativeTracer* g_tracer = nullptr;
constexpr std::size_t kObservedScriptCapacity = 128;
std::int32_t g_scriptHandles[kObservedScriptCapacity]{};
char g_scriptHandlePaths[kObservedScriptCapacity][96]{};
std::uint32_t g_scriptHandleOwners[kObservedScriptCapacity]{};
bool g_scriptHandleOwnerKnown[kObservedScriptCapacity]{};
bool g_scriptHandleValid[kObservedScriptCapacity]{};
bool g_scriptHandleValidKnown[kObservedScriptCapacity]{};
std::size_t g_scriptHandleCount = 0;
std::uint32_t gCreatePlayerActorTraceCount = 0;
std::uint32_t gGetPlayerActorTraceCount = 0;
std::uint32_t gCreateActorInLayoutTraceCount = 0;
std::uint32_t gIsActorInitedTraceCount = 0;
std::uint32_t gIsActorValidTraceCount = 0;
std::uint32_t gIsActorPlayerTraceCount = 0;
std::uint32_t gIsActorLocalPlayerTraceCount = 0;
std::uint32_t gIsLocalPlayerValidTraceCount = 0;
std::uint32_t gRespawnPlayerActorTraceCount = 0;
std::uint32_t gSwitchPlayerToEnumTraceCount = 0;
std::uint32_t gInitNativeActorenumPlayerTraceCount = 0;
std::uint32_t gGetActorSlotTraceCount = 0;
std::uint32_t gGetSlotActorTraceCount = 0;
std::uint32_t gGetLocalSlotTraceCount = 0;
std::uint32_t gIsSlotValidTraceCount = 0;
bool gRemotePlayerProbeSpawned = false;
bool gRemotePlayerProbeRequested = false;
std::uint32_t gRemotePlayerProbePlayerLayoutId = 0u;
float gRemotePlayerProbeX = 0.0f;
float gRemotePlayerProbeY = 0.0f;
float gRemotePlayerProbeZ = 0.0f;

void append_execution_identity(char* buffer, std::size_t capacity, std::size_t& used, void* context) {
    if (!buffer || capacity == 0 || used >= capacity) return;
#ifdef _WIN32
    const DWORD tid = GetCurrentThreadId();
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto ctx = reinterpret_cast<std::uintptr_t>(context);
    const int n = std::snprintf(buffer + used, capacity - used,
                                " tid=%lu ctx=0x%llX ret=0x%llX",
                                static_cast<unsigned long>(tid),
                                static_cast<unsigned long long>(ctx),
                                static_cast<unsigned long long>(caller));
    if (n > 0) used += static_cast<std::size_t>(n);
#else
    (void)context;
#endif
}

int find_remembered_script_handle(std::int32_t handle) {
    if (handle <= 0) return -1;
    for (std::size_t i = 0; i < g_scriptHandleCount; ++i) {
        if (g_scriptHandles[i] == handle) return static_cast<int>(i);
    }
    return -1;
}

void remember_script_handle(const char* path,
                            std::int32_t handle,
                            bool ownerKnown,
                            std::uint32_t ownerScriptId) {
    if (handle <= 0) return;

    const auto capacity = sizeof(g_scriptHandles) / sizeof(g_scriptHandles[0]);
    for (std::size_t i = 0; i < g_scriptHandleCount; ++i) {
        if (g_scriptHandles[i] != handle) continue;

        if (path && *path) {
            std::snprintf(g_scriptHandlePaths[i], sizeof(g_scriptHandlePaths[i]), "%s", path);
        }
        g_scriptHandleOwnerKnown[i] = ownerKnown;
        g_scriptHandleOwners[i] = ownerKnown ? ownerScriptId : 0u;
        g_scriptHandleValidKnown[i] = false;
        return;
    }

    if (g_scriptHandleCount >= capacity) return;

    const auto slot = g_scriptHandleCount++;
    g_scriptHandles[slot] = handle;
    std::snprintf(g_scriptHandlePaths[slot], sizeof(g_scriptHandlePaths[slot]), "%s",
                  (path && *path) ? path : "<unknown>");
    g_scriptHandleOwnerKnown[slot] = ownerKnown;
    g_scriptHandleOwners[slot] = ownerKnown ? ownerScriptId : 0u;
    g_scriptHandleValid[slot] = false;
    g_scriptHandleValidKnown[slot] = false;
}

bool is_remembered_script_handle(std::int32_t handle) {
    return find_remembered_script_handle(handle) >= 0;
}

constexpr std::uint32_t kSetStartPos = 0x0CB93120u;
constexpr std::uint32_t kScriptDoneLoading = 0x5401F0CAu;
constexpr std::uint32_t kClearMissionInfo = 0x02092A6Eu;
constexpr std::uint32_t kLaunchNewScript = 0x85A30503u;
constexpr std::uint32_t kLaunchNewScriptWithArgs = 0xA602F586u;
constexpr std::uint32_t kSetMissionInfo = 0x3B417D4Eu;
constexpr std::uint32_t kWasLastResetForMultiplayer = 0x3B004817u;
constexpr std::uint32_t kIsLaunchRetail = 0x7CE2C2E1u;
constexpr std::uint32_t kIsSimulateStartPress = 0xD8E31D42u;
constexpr std::uint32_t kIsScriptValid = 0x45F7D589u;
constexpr std::uint32_t kTerminateScript = 0x60A7FF09u;
constexpr std::uint32_t kTerminateThisScript = 0x245B6AB6u;
constexpr std::uint32_t kNetEnableMultiplayer = 0x9180FF1Cu;
constexpr std::uint32_t kNetIsInSession = 0x8CA54980u;
constexpr std::uint32_t kNetIsSessionClient = 0xFF65A07Cu;
constexpr std::uint32_t kNetSessionQuickJoin = 0x8DF05A4Fu;
constexpr std::uint32_t kNetSessionStartGameplay = 0x86FF3A9Bu;
constexpr std::uint32_t kNetSessionEndGameplay = 0x81FD9851u;
constexpr std::uint32_t kNetSessionIsGameplayStarted = 0xDC88B308u;
constexpr std::uint32_t kCreatePlayerActorInLayout = 0x6A307D5Fu;
constexpr std::uint32_t kGetPlayerActor = 0xE8CFDD53u;
constexpr std::uint32_t kCreateActorInLayout = 0x8D67F397u;
constexpr std::uint32_t kGetActorEnum = 0x0B28E9ECu;
constexpr std::uint32_t kIsActorInited = 0x24F4DAB2u;
constexpr std::uint32_t kIsActorValid = 0xBA6C3E92u;
constexpr std::uint32_t kIsActorPlayer = 0xB27E91E7u;
constexpr std::uint32_t kIsActorLocalPlayer = 0x6542CF26u;
constexpr std::uint32_t kIsLocalPlayerValid = 0x0ADC17E9u;
constexpr std::uint32_t kRespawnPlayerActorInLayout = 0x637E446Bu;
constexpr std::uint32_t kSwitchPlayerToEnum = 0x95FBA0B0u;
constexpr std::uint32_t kInitNativeActorenumPlayer = 0xCBA75200u;
constexpr std::uint32_t kCreateLayout = 0x6CA53214u;
constexpr std::uint32_t kFindNamedLayout = 0x5699DE7Eu;
constexpr std::uint32_t kIsLayoutrefValid = 0xFC8E55EDu;
constexpr std::uint32_t kGetActorSlot = 0xAABF3356u;
constexpr std::uint32_t kGetSlotActor = 0xDB9B49D8u;
constexpr std::uint32_t kGetLocalSlot = 0xAD68A22Eu;
constexpr std::uint32_t kIsSlotValid = 0xD04480FEu;
constexpr std::uint32_t kStreamingRequestActor = 0xB0A79FEEu;
constexpr std::uint32_t kStreamingIsActorLoaded = 0x7DF72579u;

struct NativeTraceContext final {
    void* returnBuffer{};
    std::uint32_t argumentCount{};
    void* argumentBuffer{};
    std::uint32_t dataCount{};
    void* outputVectors[4]{};
    std::uint8_t inputVectors[0x30]{};
};

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
        out = 0;
        return false;
    }
}

bool read_u64_return(void* context, std::uintptr_t& out) noexcept {
    out = 0;
    if (!context) return false;
    const auto* call = reinterpret_cast<const NativeTraceContext*>(context);
    if (!call->returnBuffer) return false;
    __try {
        std::memcpy(&out, call->returnBuffer, sizeof(out));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = 0;
        return false;
    }
}
#endif

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

std::uintptr_t read_u64_arg_value(void* context, std::uint32_t index) {
    std::uintptr_t value = 0;
    (void)read_u64_arg(context, index, value);
    return value;
}

bool invoke_handler_in_existing_context(NativeInvoker::NativeHandler handler,
                                         void* context,
                                         std::uintptr_t* arguments,
                                         std::size_t argumentCount,
                                         std::uintptr_t& out) {
    out = 0;
    if (!handler || !context || argumentCount > 32u) return false;

#ifdef _WIN32
    auto* call = reinterpret_cast<NativeTraceContext*>(context);
    if (!call) return false;

    auto* savedArgumentBuffer = call->argumentBuffer;
    const auto savedArgumentCount = call->argumentCount;
    auto* savedReturnBuffer = call->returnBuffer;

    std::uintptr_t returnValue = 0;
    call->argumentBuffer = arguments;
    call->argumentCount = static_cast<std::uint32_t>(argumentCount);
    call->returnBuffer = &returnValue;

    bool ok = false;
    __try {
        handler(context);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }

    call->argumentBuffer = savedArgumentBuffer;
    call->argumentCount = savedArgumentCount;
    call->returnBuffer = savedReturnBuffer;

    if (!ok) return false;
    out = returnValue;
    return true;
#else
    (void)handler;
    (void)context;
    (void)arguments;
    (void)argumentCount;
    return false;
#endif
}



bool read_c_string_pointer(std::uintptr_t pointer, char* out, std::size_t capacity) {
    if (!out || capacity == 0 || pointer == 0) return false;
    out[0] = '\0';

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

    if (!invoker.hook_native(kIsScriptValid, &StartupNativeTracer::is_script_valid_hook,
                             originalIsScriptValid_, &error)) {
        invoker.unhook_native(kIsSimulateStartPress, &StartupNativeTracer::is_simulate_start_press_hook, originalIsSimulateStartPress_);
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
        originalIsSimulateStartPress_ = nullptr; originalWasLastResetForMultiplayer_ = nullptr;
        originalIsScriptValid_ = nullptr;
        return false;
    }

    if (!invoker.hook_native(kTerminateThisScript, &StartupNativeTracer::terminate_this_script_hook,
                             originalTerminateThisScript_, &error)) {
        invoker.unhook_native(kIsScriptValid, &StartupNativeTracer::is_script_valid_hook, originalIsScriptValid_);
        invoker.unhook_native(kIsSimulateStartPress, &StartupNativeTracer::is_simulate_start_press_hook, originalIsSimulateStartPress_);
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
        originalIsSimulateStartPress_ = nullptr; originalIsScriptValid_ = nullptr; originalTerminateThisScript_ = nullptr;
        return false;
    }

    if (!invoker.hook_native(kTerminateScript, &StartupNativeTracer::terminate_script_hook,
                             originalTerminateScript_, &error)) {
        invoker.unhook_native(kIsScriptValid, &StartupNativeTracer::is_script_valid_hook, originalIsScriptValid_);
        invoker.unhook_native(kIsSimulateStartPress, &StartupNativeTracer::is_simulate_start_press_hook, originalIsSimulateStartPress_);
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
        originalIsSimulateStartPress_ = nullptr; originalIsScriptValid_ = nullptr; originalTerminateScript_ = nullptr;
        return false;
    }

    // Auxiliary native tracing is optional so a missing native cannot prevent
    // the core startup tracer from attaching.
    const auto install_optional = [&](std::uint32_t hash,
                                       NativeInvoker::NativeHandler replacement,
                                       NativeInvoker::NativeHandler& original,
                                       const char* label) {
        if (!invoker.has_handler(hash)) {
            char buffer[176]{};
            std::snprintf(buffer, sizeof(buffer),
                          "[FrontierNativeTrace] optional hook unavailable %s hash=0x%08X",
                          label, hash);
            log(buffer);
            return;
        }

        std::string optionalError;
        if (!invoker.hook_native(hash, replacement, original, &optionalError)) {
            char buffer[272]{};
            std::snprintf(buffer, sizeof(buffer),
                          "[FrontierNativeTrace] optional hook failed %s hash=0x%08X error=%s",
                          label, hash, optionalError.empty() ? "<unknown>" : optionalError.c_str());
            log(buffer);
            original = nullptr;
        }
    };

    install_optional(kNetEnableMultiplayer,
                     &StartupNativeTracer::net_enable_multiplayer_hook,
                     originalNetEnableMultiplayer_,
                     "NET_ENABLE_MULTIPLAYER");
    install_optional(kNetIsInSession,
                     &StartupNativeTracer::net_is_in_session_hook,
                     originalNetIsInSession_,
                     "NET_IS_IN_SESSION");
    install_optional(kNetIsSessionClient,
                     &StartupNativeTracer::net_is_session_client_hook,
                     originalNetIsSessionClient_,
                     "NET_IS_SESSION_CLIENT");
    install_optional(kNetSessionQuickJoin,
                     &StartupNativeTracer::net_session_quick_join_hook,
                     originalNetSessionQuickJoin_,
                     "NET_SESSION_QUICK_JOIN_NATIVE");
    install_optional(kNetSessionStartGameplay,
                     &StartupNativeTracer::net_session_start_gameplay_hook,
                     originalNetSessionStartGameplay_,
                     "NET_SESSION_START_GAMEPLAY");
    install_optional(kNetSessionEndGameplay,
                     &StartupNativeTracer::net_session_end_gameplay_hook,
                     originalNetSessionEndGameplay_,
                     "NET_SESSION_END_GAMEPLAY");
    install_optional(kNetSessionIsGameplayStarted,
                     &StartupNativeTracer::net_session_is_gameplay_started_hook,
                     originalNetSessionIsGameplayStarted_,
                     "NET_SESSION_IS_GAMEPLAY_STARTED");

    install_optional(kCreatePlayerActorInLayout,
                     &StartupNativeTracer::create_player_actor_in_layout_hook,
                     originalCreatePlayerActorInLayout_,
                     "CREATE_PLAYER_ACTOR_IN_LAYOUT");
    install_optional(kGetPlayerActor,
                     &StartupNativeTracer::get_player_actor_hook,
                     originalGetPlayerActor_,
                     "GET_PLAYER_ACTOR");
    install_optional(kCreateActorInLayout,
                     &StartupNativeTracer::create_actor_in_layout_hook,
                     originalCreateActorInLayout_,
                     "CREATE_ACTOR_IN_LAYOUT");
    install_optional(kGetActorEnum,
                     &StartupNativeTracer::get_actor_enum_hook,
                     originalGetActorEnum_,
                     "GET_ACTOR_ENUM");
    install_optional(kIsActorInited,
                     &StartupNativeTracer::is_actor_inited_hook,
                     originalIsActorInited_,
                     "IS_ACTOR_INITED");
    install_optional(kIsActorValid,
                     &StartupNativeTracer::is_actor_valid_hook,
                     originalIsActorValid_,
                     "IS_ACTOR_VALID");
    install_optional(kIsActorPlayer,
                     &StartupNativeTracer::is_actor_player_hook,
                     originalIsActorPlayer_,
                     "IS_ACTOR_PLAYER");
    install_optional(kIsActorLocalPlayer,
                     &StartupNativeTracer::is_actor_local_player_hook,
                     originalIsActorLocalPlayer_,
                     "IS_ACTOR_LOCAL_PLAYER");
    install_optional(kIsLocalPlayerValid,
                     &StartupNativeTracer::is_local_player_valid_hook,
                     originalIsLocalPlayerValid_,
                     "IS_LOCAL_PLAYER_VALID");
    install_optional(kRespawnPlayerActorInLayout,
                     &StartupNativeTracer::respawn_player_actor_in_layout_hook,
                     originalRespawnPlayerActorInLayout_,
                     "RESPAWN_PLAYER_ACTOR_IN_LAYOUT");
    install_optional(kSwitchPlayerToEnum,
                     &StartupNativeTracer::switch_player_to_enum_hook,
                     originalSwitchPlayerToEnum_,
                     "SWITCH_PLAYER_TO_ENUM");
    install_optional(kInitNativeActorenumPlayer,
                     &StartupNativeTracer::init_native_actorenum_player_hook,
                     originalInitNativeActorenumPlayer_,
                     "INIT_NATIVE_ACTORENUM_PLAYER");
    install_optional(kGetActorSlot,
                     &StartupNativeTracer::get_actor_slot_hook,
                     originalGetActorSlot_,
                     "GET_ACTOR_SLOT");
    install_optional(kGetSlotActor,
                     &StartupNativeTracer::get_slot_actor_hook,
                     originalGetSlotActor_,
                     "GET_SLOT_ACTOR");
    install_optional(kGetLocalSlot,
                     &StartupNativeTracer::get_local_slot_hook,
                     originalGetLocalSlot_,
                     "GET_LOCAL_SLOT");
    install_optional(kIsSlotValid,
                     &StartupNativeTracer::is_slot_valid_hook,
                     originalIsSlotValid_,
                     "IS_SLOT_VALID");

    install_optional(kCreateLayout,
                     &StartupNativeTracer::create_layout_hook,
                     originalCreateLayout_,
                     "CREATE_LAYOUT");
    install_optional(kFindNamedLayout,
                     &StartupNativeTracer::find_named_layout_hook,
                     originalFindNamedLayout_,
                     "FIND_NAMED_LAYOUT");
    install_optional(kIsLayoutrefValid,
                     &StartupNativeTracer::is_layoutref_valid_hook,
                     originalIsLayoutrefValid_,
                     "IS_LAYOUTREF_VALID");

    g_scriptHandleCount = 0;
    std::memset(g_scriptHandlePaths, 0, sizeof(g_scriptHandlePaths));
    std::memset(g_scriptHandleOwners, 0, sizeof(g_scriptHandleOwners));
    std::memset(g_scriptHandleOwnerKnown, 0, sizeof(g_scriptHandleOwnerKnown));
    std::memset(g_scriptHandleValid, 0, sizeof(g_scriptHandleValid));
    std::memset(g_scriptHandleValidKnown, 0, sizeof(g_scriptHandleValidKnown));
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
        std::size_t usedIdentity = std::strlen(buffer);
        append_execution_identity(buffer, sizeof(buffer), usedIdentity, context);
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

    if (tracer->originalLaunchNewScript_) {
        tracer->originalLaunchNewScript_(context);
    }

    std::uint32_t result = 0;
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

    std::uint32_t ownerScriptId = 0;
    const bool hasOwnerScriptId = lookup_script_context_id(context, ownerScriptId);

    if (resultReadable) {
        remember_script_handle(pathReadable ? path : nullptr,
                               static_cast<std::int32_t>(result),
                               hasOwnerScriptId,
                               ownerScriptId);
    }

    char buffer[352]{};
    const std::uint32_t supplied = context
        ? reinterpret_cast<const NativeTraceContext*>(context)->argumentCount
        : 0u;
    std::snprintf(buffer, sizeof(buffer),
                  "[FrontierNativeTrace] LAUNCH_NEW_SCRIPT path=%s ptr=0x%llX arg1=%s%d supplied=%u result=%s%u ownerScriptId=%s%u",
                  pathReadable ? path : "<unreadable>",
                  static_cast<unsigned long long>(scriptPtr),
                  hasArg1 ? "" : "?", hasArg1 ? arg1 : 0,
                  supplied,
                  resultReadable ? "" : "?", resultReadable ? result : 0u,
                  hasOwnerScriptId ? "" : "?", hasOwnerScriptId ? ownerScriptId : 0u);
    std::size_t usedIdentity = std::strlen(buffer);
    append_execution_identity(buffer, sizeof(buffer), usedIdentity, context);
    log(buffer);
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

    std::uint32_t ownerScriptId = 0;
    const bool hasOwnerScriptId = lookup_script_context_id(context, ownerScriptId);

    if (resultReadable) {
        remember_script_handle(pathReadable ? path : nullptr,
                               result,
                               hasOwnerScriptId,
                               ownerScriptId);
    }

    if (used + 80u < sizeof(buffer)) {
        const int n = std::snprintf(buffer + used, sizeof(buffer) - used,
                                    " result=%s%d ownerScriptId=%s%u",
                                    resultReadable ? "" : "?", resultReadable ? result : 0,
                                    hasOwnerScriptId ? "" : "?", hasOwnerScriptId ? ownerScriptId : 0u);
        (void)n;
    }
    std::size_t usedIdentity = std::strlen(buffer);
    append_execution_identity(buffer, sizeof(buffer), usedIdentity, context);
    log(buffer);
}

void StartupNativeTracer::is_script_valid_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    std::int32_t scriptId = 0;
    const bool hasScriptId = read_i32_arg(context, 0, scriptId);

    bool shouldTrace = false;
    if (hasScriptId) {
        shouldTrace = is_remembered_script_handle(scriptId);
    }

    if (tracer->originalIsScriptValid_) {
        tracer->originalIsScriptValid_(context);
    }

    if (!shouldTrace) return;

    std::uint32_t result = 0;
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

    if (!resultReadable) return;

    const int slot = find_remembered_script_handle(scriptId);
    if (slot < 0) return;

    const bool valid = result != 0;
    if (g_scriptHandleValidKnown[slot] && g_scriptHandleValid[slot] == valid) {
        return;
    }
    g_scriptHandleValidKnown[slot] = true;
    g_scriptHandleValid[slot] = valid;

    const char* path = (slot >= 0)
        ? g_scriptHandlePaths[slot]
        : "";

    char buffer[320]{};
    const bool ownerKnown = g_scriptHandleOwnerKnown[slot];
    const std::uint32_t ownerScriptId = g_scriptHandleOwners[slot];

    std::snprintf(buffer, sizeof(buffer),
                  "[FrontierNativeTrace] IS_SCRIPT_VALID id=%d path=%s result=%u transition ownerScriptId=%s%u",
                  scriptId,
                  path && *path ? path : "<unknown>",
                  result,
                  ownerKnown ? "" : "?",
                  ownerKnown ? ownerScriptId : 0u);
    std::size_t usedIdentity = std::strlen(buffer);
    append_execution_identity(buffer, sizeof(buffer), usedIdentity, context);
    log(buffer);
}

void StartupNativeTracer::terminate_this_script_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    if (tracer->originalTerminateThisScript_) {
        tracer->originalTerminateThisScript_(context);
    }

    std::uint32_t scriptId = 0;
    const bool hasScriptId = lookup_script_context_id(context, scriptId);

    char buffer[320]{};
    std::snprintf(buffer, sizeof(buffer),
                  "[FrontierNativeTrace] TERMINATE_THIS_SCRIPT scriptId=%s%u",
                  hasScriptId ? "" : "?",
                  hasScriptId ? scriptId : 0u);
    std::size_t usedIdentity = std::strlen(buffer);
    append_execution_identity(buffer, sizeof(buffer), usedIdentity, context);
    log(buffer);
}

void StartupNativeTracer::terminate_script_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    std::int32_t scriptId = 0;
    const bool hasScriptId = read_i32_arg(context, 0, scriptId);

    const int slot = hasScriptId ? find_remembered_script_handle(scriptId) : -1;
    if (tracer->originalTerminateScript_) {
        tracer->originalTerminateScript_(context);
    }

    if (slot < 0) return;

    g_scriptHandleValidKnown[slot] = true;
    g_scriptHandleValid[slot] = false;

    const char* path = g_scriptHandlePaths[slot];
    const bool ownerKnown = g_scriptHandleOwnerKnown[slot];
    const std::uint32_t ownerScriptId = g_scriptHandleOwners[slot];

    char buffer[384]{};
    std::snprintf(buffer, sizeof(buffer),
                  "[FrontierNativeTrace] TERMINATE_SCRIPT id=%d path=%s launchOwnerScriptId=%s%u",
                  scriptId,
                  path && *path ? path : "<unknown>",
                  ownerKnown ? "" : "?",
                  ownerKnown ? ownerScriptId : 0u);
    std::size_t usedIdentity = std::strlen(buffer);
    append_execution_identity(buffer, sizeof(buffer), usedIdentity, context);
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


void StartupNativeTracer::net_enable_multiplayer_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    std::int32_t mode = 0;
    const bool modeOk = read_i32_arg(context, 0, mode);
    if (tracer->originalNetEnableMultiplayer_) {
        tracer->originalNetEnableMultiplayer_(context);
    }

    std::uint32_t result = 0;
    const bool resultOk =
#ifdef _WIN32
        read_u32_return(context, result);
#else
        false;
#endif

    char buffer[256]{};
    std::size_t used = static_cast<std::size_t>(std::snprintf(
        buffer, sizeof(buffer),
        "[FrontierNativeTrace] NET_ENABLE_MULTIPLAYER arg0=%s%d result=%s%u",
        modeOk ? "" : "?",
        modeOk ? mode : 0,
        resultOk ? "" : "?",
        resultOk ? result : 0u));
    append_execution_identity(buffer, sizeof(buffer), used, context);
    log(buffer);
}

void StartupNativeTracer::net_is_in_session_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    if (tracer->originalNetIsInSession_) {
        tracer->originalNetIsInSession_(context);
    }

    std::uint32_t result = 0;
    const bool resultOk =
#ifdef _WIN32
        read_u32_return(context, result);
#else
        false;
#endif

    char buffer[224]{};
    std::size_t used = static_cast<std::size_t>(std::snprintf(
        buffer, sizeof(buffer),
        "[FrontierNativeTrace] NET_IS_IN_SESSION result=%s%u",
        resultOk ? "" : "?",
        resultOk ? result : 0u));
    append_execution_identity(buffer, sizeof(buffer), used, context);
    log(buffer);
}

void StartupNativeTracer::net_is_session_client_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    std::int32_t arg0 = 0;
    const bool argOk = read_i32_arg(context, 0, arg0);

    if (tracer->originalNetIsSessionClient_) {
        tracer->originalNetIsSessionClient_(context);
    }

    std::uint32_t result = 0;
    const bool resultOk =
#ifdef _WIN32
        read_u32_return(context, result);
#else
        false;
#endif

    char buffer[256]{};
    std::size_t used = static_cast<std::size_t>(std::snprintf(
        buffer, sizeof(buffer),
        "[FrontierNativeTrace] NET_IS_SESSION_CLIENT arg0=%s%d result=%s%u",
        argOk ? "" : "?",
        argOk ? arg0 : 0,
        resultOk ? "" : "?",
        resultOk ? result : 0u));
    append_execution_identity(buffer, sizeof(buffer), used, context);
    log(buffer);
}


void StartupNativeTracer::net_session_quick_join_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    std::int32_t arg0 = 0;
    const bool argOk = read_i32_arg(context, 0, arg0);

    if (tracer->originalNetSessionQuickJoin_) {
        tracer->originalNetSessionQuickJoin_(context);
    }

    std::uint32_t result = 0;
    const bool resultOk =
#ifdef _WIN32
        read_u32_return(context, result);
#else
        false;
#endif

    char buffer[280]{};
    std::size_t used = static_cast<std::size_t>(std::snprintf(
        buffer, sizeof(buffer),
        "[FrontierNativeTrace] NET_SESSION_QUICK_JOIN_NATIVE arg0=%s%d result=%s%u",
        argOk ? "" : "?",
        argOk ? arg0 : 0,
        resultOk ? "" : "?",
        resultOk ? result : 0u));
    append_execution_identity(buffer, sizeof(buffer), used, context);
    log(buffer);
}

void StartupNativeTracer::net_session_start_gameplay_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    if (tracer->originalNetSessionStartGameplay_) {
        tracer->originalNetSessionStartGameplay_(context);
    }

    std::uint32_t result = 0;
    const bool resultOk =
#ifdef _WIN32
        read_u32_return(context, result);
#else
        false;
#endif

    char buffer[240]{};
    std::size_t used = static_cast<std::size_t>(std::snprintf(
        buffer, sizeof(buffer),
        "[FrontierNativeTrace] NET_SESSION_START_GAMEPLAY result=%s%u",
        resultOk ? "" : "?",
        resultOk ? result : 0u));
    append_execution_identity(buffer, sizeof(buffer), used, context);
    log(buffer);
}

void StartupNativeTracer::net_session_end_gameplay_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    if (tracer->originalNetSessionEndGameplay_) {
        tracer->originalNetSessionEndGameplay_(context);
    }

    std::uint32_t result = 0;
    const bool resultOk =
#ifdef _WIN32
        read_u32_return(context, result);
#else
        false;
#endif

    char buffer[240]{};
    std::size_t used = static_cast<std::size_t>(std::snprintf(
        buffer, sizeof(buffer),
        "[FrontierNativeTrace] NET_SESSION_END_GAMEPLAY result=%s%u",
        resultOk ? "" : "?",
        resultOk ? result : 0u));
    append_execution_identity(buffer, sizeof(buffer), used, context);
    log(buffer);
}

void StartupNativeTracer::net_session_is_gameplay_started_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    if (tracer->originalNetSessionIsGameplayStarted_) {
        tracer->originalNetSessionIsGameplayStarted_(context);
    }

    std::uint32_t result = 0;
    const bool resultOk =
#ifdef _WIN32
        read_u32_return(context, result);
#else
        false;
#endif

    char buffer[256]{};
    std::size_t used = static_cast<std::size_t>(std::snprintf(
        buffer, sizeof(buffer),
        "[FrontierNativeTrace] NET_SESSION_IS_GAMEPLAY_STARTED result=%s%u",
        resultOk ? "" : "?",
        resultOk ? result : 0u));
    append_execution_identity(buffer, sizeof(buffer), used, context);
    log(buffer);
}


void StartupNativeTracer::create_layout_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    char layoutName[160]{};
    const bool nameOk = read_c_string(context, 0, layoutName, sizeof(layoutName));

    if (tracer->originalCreateLayout_) {
        tracer->originalCreateLayout_(context);
    }

    std::uint32_t result = 0;
#ifdef _WIN32
    const bool resultOk = read_u32_return(context, result);
#else
    const bool resultOk = false;
#endif

    char buffer[320]{};
    std::size_t used = static_cast<std::size_t>(std::snprintf(
        buffer, sizeof(buffer),
        "[FrontierNativeTrace] CREATE_LAYOUT name=%s%s result=%s0x%08X",
        nameOk ? "" : "?",
        nameOk ? layoutName : "<unreadable>",
        resultOk ? "" : "?",
        resultOk ? result : 0u));
    append_execution_identity(buffer, sizeof(buffer), used, context);
    log(buffer);
}

void StartupNativeTracer::find_named_layout_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    char layoutName[160]{};
    const bool nameOk = read_c_string(context, 0, layoutName, sizeof(layoutName));

    if (tracer->originalFindNamedLayout_) {
        tracer->originalFindNamedLayout_(context);
    }

    std::uint32_t result = 0;
#ifdef _WIN32
    const bool resultOk = read_u32_return(context, result);
#else
    const bool resultOk = false;
#endif

    char buffer[320]{};
    std::size_t used = static_cast<std::size_t>(std::snprintf(
        buffer, sizeof(buffer),
        "[FrontierNativeTrace] FIND_NAMED_LAYOUT name=%s%s result=%s0x%08X",
        nameOk ? "" : "?",
        nameOk ? layoutName : "<unreadable>",
        resultOk ? "" : "?",
        resultOk ? result : 0u));
    append_execution_identity(buffer, sizeof(buffer), used, context);
    log(buffer);
}

void StartupNativeTracer::is_layoutref_valid_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    std::int32_t layout = 0;
    const bool layoutOk = read_i32_arg(context, 0, layout);

    if (tracer->originalIsLayoutrefValid_) {
        tracer->originalIsLayoutrefValid_(context);
    }

    std::uint32_t result = 0;
#ifdef _WIN32
    const bool resultOk = read_u32_return(context, result);
#else
    const bool resultOk = false;
#endif

    char buffer[320]{};
    std::size_t used = static_cast<std::size_t>(std::snprintf(
        buffer, sizeof(buffer),
        "[FrontierNativeTrace] IS_LAYOUTREF_VALID layout=%s0x%08X result=%s%u",
        layoutOk ? "" : "?",
        layoutOk ? static_cast<std::uint32_t>(layout) : 0u,
        resultOk ? "" : "?",
        resultOk ? result : 0u));
    append_execution_identity(buffer, sizeof(buffer), used, context);
    log(buffer);
}

void StartupNativeTracer::create_player_actor_in_layout_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    const auto traceIndex = gCreatePlayerActorTraceCount++;
#ifdef _WIN32
    const auto* call = reinterpret_cast<const NativeTraceContext*>(context);
    const auto argc = call ? call->argumentCount : 0u;
#else
    const auto argc = 0u;
#endif

    // Capture arguments before the engine executes the native. Some RDR natives
    // rewrite argument slots during execution; post-call logging can therefore
    // mistake an output/reference for the original input.
    const auto preA0 = read_u64_arg_value(context, 0);
    const auto preA1 = read_u64_arg_value(context, 1);
    const auto preA2 = read_u64_arg_value(context, 2);
    const auto preA3 = read_u64_arg_value(context, 3);
    const auto preA4 = read_u64_arg_value(context, 4);
    const auto preA5 = read_u64_arg_value(context, 5);
    const auto preA6 = read_u64_arg_value(context, 6);
    const auto preA7 = read_u64_arg_value(context, 7);
    const auto preA8 = read_u64_arg_value(context, 8);
    const auto preA9 = read_u64_arg_value(context, 9);

    if (tracer->originalCreatePlayerActorInLayout_) {
        tracer->originalCreatePlayerActorInLayout_(context);
    }

    char actorName[160]{};
    const bool actorNameOk = read_c_string_pointer(preA1, actorName, sizeof(actorName));
    if (actorNameOk &&
        std::strcmp(actorName, "player") == 0 &&
        static_cast<std::uint32_t>(preA2) == 0u) {
        gRemotePlayerProbePlayerLayoutId = static_cast<std::uint32_t>(preA0);
        const auto posXYLow = static_cast<std::uint32_t>(preA3);
        const auto posXYHigh = static_cast<std::uint32_t>(preA3 >> 32u);
        const auto posZRaw = static_cast<std::uint32_t>(preA4);
        std::memcpy(&gRemotePlayerProbeX, &posXYLow, sizeof(float));
        std::memcpy(&gRemotePlayerProbeY, &posXYHigh, sizeof(float));
        std::memcpy(&gRemotePlayerProbeZ, &posZRaw, sizeof(float));
    }

    if (traceIndex >= 64) return;

    const auto postA0 = read_u64_arg_value(context, 0);

    std::uint32_t result = 0;
#ifdef _WIN32
    const bool resultOk = read_u32_return(context, result);
#else
    const bool resultOk = false;
#endif

    const auto posXYLow = static_cast<std::uint32_t>(preA3);
    const auto posXYHigh = static_cast<std::uint32_t>(preA3 >> 32u);
    const auto posZRaw = static_cast<std::uint32_t>(preA4);
    const auto rotationXYLow = static_cast<std::uint32_t>(preA5);
    const auto rotationXYHigh = static_cast<std::uint32_t>(preA5 >> 32u);
    const auto rotationZRaw = static_cast<std::uint32_t>(preA6);

    float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
    float rotationX = 0.0f, rotationY = 0.0f, rotationZ = 0.0f;
    std::memcpy(&posX, &posXYLow, sizeof(posX));
    std::memcpy(&posY, &posXYHigh, sizeof(posY));
    std::memcpy(&posZ, &posZRaw, sizeof(posZ));
    std::memcpy(&rotationX, &rotationXYLow, sizeof(rotationX));
    std::memcpy(&rotationY, &rotationXYHigh, sizeof(rotationY));
    std::memcpy(&rotationZ, &rotationZRaw, sizeof(rotationZ));

    char buffer[1100]{};
    std::snprintf(buffer, sizeof(buffer),
                  "[FrontierNativeTrace] CREATE_PLAYER_ACTOR_IN_LAYOUT trace=%u argc=%u "
                  "layout=0x%llX actorName=%s%s model=%u "
                  "pos=(%.6f,%.6f,%.6f) rotation=(%.6f,%.6f,%.6f) outfitVariation=%u "
                  "pre={a0=0x%llX a1=0x%llX a2=0x%llX a3=0x%llX a4=0x%llX "
                  "a5=0x%llX a6=0x%llX a7=0x%llX} postA0=0x%llX result=%s%u",
                  traceIndex, argc,
                  static_cast<unsigned long long>(preA0),
                  actorNameOk ? "" : "?",
                  actorNameOk ? actorName : "<unreadable>",
                  static_cast<unsigned>(static_cast<std::uint32_t>(preA2)),
                  posX, posY, posZ,
                  rotationX, rotationY, rotationZ,
                  static_cast<unsigned>(static_cast<std::uint32_t>(preA7)),
                  static_cast<unsigned long long>(preA0),
                  static_cast<unsigned long long>(preA1),
                  static_cast<unsigned long long>(preA2),
                  static_cast<unsigned long long>(preA3),
                  static_cast<unsigned long long>(preA4),
                  static_cast<unsigned long long>(preA5),
                  static_cast<unsigned long long>(preA6),
                  static_cast<unsigned long long>(preA7),
                  static_cast<unsigned long long>(postA0),
                  resultOk ? "" : "?",
                  resultOk ? result : 0u);
    std::size_t used = std::strlen(buffer);
    append_execution_identity(buffer, sizeof(buffer), used, context);
    log(buffer);
}

namespace {
std::uintptr_t float_bits_for_tracer(float value);
std::uintptr_t pack_vec2_for_tracer(float x, float y);
}

void StartupNativeTracer::get_player_actor_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    const auto traceIndex = gGetPlayerActorTraceCount++;
    if (tracer->originalGetPlayerActor_) {
        tracer->originalGetPlayerActor_(context);
    }
    if (traceIndex >= 64) return;

    std::uintptr_t result = 0;
#ifdef _WIN32
    const bool resultOk = read_u64_return(context, result);
#else
    const bool resultOk = false;
#endif
    std::int32_t player = 0;
    const bool playerOk = read_i32_arg(context, 0, player);

    char buffer[288]{};
    std::size_t used = static_cast<std::size_t>(std::snprintf(
        buffer, sizeof(buffer),
        "[FrontierNativeTrace] GET_PLAYER_ACTOR trace=%u player=%s%d result=%s0x%llX",
        traceIndex,
        playerOk ? "" : "?",
        playerOk ? player : 0,
        resultOk ? "" : "?",
        static_cast<unsigned long long>(result)));
    append_execution_identity(buffer, sizeof(buffer), used, context);
    log(buffer);

    if (!gRemotePlayerProbeSpawned && playerOk && player > 0 && tracer->originalCreatePlayerActorInLayout_) {
        char actorName[] = "FrontierRemoteTest";
        std::uintptr_t args[8]{};
        std::uintptr_t nativeResult = 0;

        // Reuse the exact PlayerLayout handle supplied by the game's own
        // CREATE_PLAYER_ACTOR_IN_LAYOUT call. Do not call FIND/CREATE_LAYOUT
        // through the synthetic argument buffer here; that path previously
        // returned 0 and caused a duplicate layout to be created.
        const std::uint32_t layoutId = gRemotePlayerProbePlayerLayoutId;

        bool layoutValid = false;
        if (layoutId != 0u && tracer->originalIsLayoutrefValid_) {
            args[0] = static_cast<std::uintptr_t>(layoutId);
            std::uintptr_t validResult = 0;
            if (invoke_handler_in_existing_context(
                    tracer->originalIsLayoutrefValid_, context, args, 1u, validResult)) {
                layoutValid = validResult != 0u;
            }
        }

        bool streamRequested = false;
        NativeInvoker::NativeHandler streamingRequestActor{};
        NativeInvoker::NativeHandler streamingIsActorLoaded{};
        if (tracer->invoker_) {
            (void)tracer->invoker_->current_handler(kStreamingRequestActor, streamingRequestActor);
            (void)tracer->invoker_->current_handler(kStreamingIsActorLoaded, streamingIsActorLoaded);
        }

        if (!gRemotePlayerProbeRequested && streamingRequestActor) {
            args[0] = 837u;
            args[1] = 1u;
            args[2] = 0u;
            std::uintptr_t streamRequestResult = 0;
            streamRequested = invoke_handler_in_existing_context(
                streamingRequestActor, context, args, 3u, streamRequestResult);
            gRemotePlayerProbeRequested = true;
        }

        bool loaded = false;
        std::uint32_t loadedArg = 0u;
        if (streamingIsActorLoaded) {
            // Public RDR1 native headers expose STREAMING_IS_ACTOR_LOADED
            // as a single ActorModel argument. Keep this probe to the
            // canonical one-argument call and do not gate player creation
            // on it; CREATE_PLAYER_ACTOR_IN_LAYOUT is the actual test.
            args[0] = 837u;
            std::uintptr_t loadedResult = 0;
            if (invoke_handler_in_existing_context(
                    streamingIsActorLoaded, context, args, 1u, loadedResult) &&
                loadedResult != 0u) {
                loaded = true;
                loadedArg = 0u;
            }
        }

        if (layoutValid) {
            const float x = gRemotePlayerProbeX + 2.0f;
            const float y = gRemotePlayerProbeY;
            const float z = gRemotePlayerProbeZ;

            args[0] = 0x100000000ull | static_cast<std::uintptr_t>(layoutId);
            args[1] = reinterpret_cast<std::uintptr_t>(actorName);
            args[2] = 837u;
            args[3] = pack_vec2_for_tracer(x, y);
            args[4] = float_bits_for_tracer(z);
            args[5] = pack_vec2_for_tracer(0.0f, 0.0f);
            args[6] = float_bits_for_tracer(0.0f);
            args[7] = 0u;

            nativeResult = 0;
            const bool createOk = invoke_handler_in_existing_context(
                tracer->originalCreatePlayerActorInLayout_, context, args, 8u, nativeResult);

            const std::uint32_t playerId = static_cast<std::uint32_t>(nativeResult);
            const std::uintptr_t actorRef = args[0];
            const std::uint32_t actorHandle = static_cast<std::uint32_t>(actorRef);

            std::uintptr_t playerCheckArgs[1]{static_cast<std::uintptr_t>(actorHandle)};
            std::uintptr_t playerCheckResult = 0;
            const bool playerCheckOk =
                actorHandle != 0u && tracer->originalIsActorPlayer_ &&
                invoke_handler_in_existing_context(
                    tracer->originalIsActorPlayer_, context, playerCheckArgs, 1u, playerCheckResult);

            char probeLog[760]{};
            std::snprintf(
                probeLog, sizeof(probeLog),
                "[FrontierRemotePlayerProbe] createOk=%u layout=0x%08X loaded=%u loadedArg=0x%08X "
                "streamRequested=%u playerId=0x%08X actorRef=0x%llX actorHandle=0x%08X "
                "isActorPlayer=%u position=(%.3f,%.3f,%.3f)",
                createOk ? 1u : 0u,
                layoutId,
                loaded ? 1u : 0u,
                loadedArg,
                streamRequested ? 1u : 0u,
                playerId,
                static_cast<unsigned long long>(actorRef),
                actorHandle,
                playerCheckOk ? static_cast<unsigned>(playerCheckResult) : 0u,
                x, y, z);
            log(probeLog);

            if (createOk && playerId != 0u && actorHandle != 0u) {
                gRemotePlayerProbeSpawned = true;
            }
        } else {
            char waitLog[420]{};
            std::snprintf(
                waitLog, sizeof(waitLog),
                "[FrontierRemotePlayerProbe] waiting layoutValid=%u streamLoaded=%u "
                "streamRequested=%u layout=0x%08X",
                layoutValid ? 1u : 0u,
                loaded ? 1u : 0u,
                streamRequested ? 1u : 0u,
                layoutId);
            log(waitLog);
        }
    }
}

void StartupNativeTracer::get_actor_enum_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    const auto actorRef = read_u64_arg_value(context, 0);
    if (tracer->originalGetActorEnum_) {
        tracer->originalGetActorEnum_(context);
    }

    std::uintptr_t result = 0;
#ifdef _WIN32
    const bool resultOk = read_u64_return(context, result);
#else
    const bool resultOk = false;
#endif

    char buffer[320]{};
    std::size_t used = static_cast<std::size_t>(std::snprintf(
        buffer, sizeof(buffer),
        "[FrontierNativeTrace] GET_ACTOR_ENUM actor=0x%llX result=%s0x%llX enum=%s%u",
        static_cast<unsigned long long>(actorRef),
        resultOk ? "" : "?",
        static_cast<unsigned long long>(result),
        resultOk ? "" : "?",
        resultOk ? static_cast<unsigned int>(static_cast<std::uint32_t>(result)) : 0u));
    append_execution_identity(buffer, sizeof(buffer), used, context);
    log(buffer);
}


namespace {
std::uintptr_t float_bits_for_tracer(float value) {
    std::uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    return static_cast<std::uintptr_t>(raw);
}

std::uintptr_t pack_vec2_for_tracer(float x, float y) {
    return float_bits_for_tracer(x) | (float_bits_for_tracer(y) << 32u);
}

void trace_actor_bool_native(void* context,
                             NativeInvoker::NativeHandler original,
                             std::uint32_t traceIndex,
                             std::uint32_t maxTraces,
                             const char* label,
                             bool includeActor) {
    const auto actorRaw = includeActor ? read_u64_arg_value(context, 0) : 0;
    if (original) original(context);
    if (traceIndex >= maxTraces) return;

    std::uint32_t resultRaw = 0;
    const bool resultOk =
#ifdef _WIN32
        read_u32_return(context, resultRaw);
#else
        false;
#endif

    char buffer[320]{};
    std::size_t used = static_cast<std::size_t>(std::snprintf(
        buffer, sizeof(buffer),
        includeActor
            ? "[FrontierNativeTrace] %s actor=0x%08X result=%s%u"
            : "[FrontierNativeTrace] %s result=%s%u",
        label,
        includeActor ? static_cast<unsigned>(static_cast<std::uint32_t>(actorRaw)) : 0u,
        resultOk ? "" : "?",
        resultOk ? resultRaw : 0u));
    append_execution_identity(buffer, sizeof(buffer), used, context);
    write_log_line(buffer);
}
}

void StartupNativeTracer::is_actor_inited_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;
    const auto traceIndex = gIsActorInitedTraceCount++;
    trace_actor_bool_native(context, tracer->originalIsActorInited_, traceIndex, 256u, "IS_ACTOR_INITED", true);
}

void StartupNativeTracer::is_actor_valid_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;
    const auto traceIndex = gIsActorValidTraceCount++;
    trace_actor_bool_native(context, tracer->originalIsActorValid_, traceIndex, 256u, "IS_ACTOR_VALID", true);
}

void StartupNativeTracer::is_actor_player_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;
    const auto traceIndex = gIsActorPlayerTraceCount++;
    trace_actor_bool_native(context, tracer->originalIsActorPlayer_, traceIndex, 256u, "IS_ACTOR_PLAYER", true);
}

void StartupNativeTracer::is_actor_local_player_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;
    const auto traceIndex = gIsActorLocalPlayerTraceCount++;
    trace_actor_bool_native(context, tracer->originalIsActorLocalPlayer_, traceIndex, 256u, "IS_ACTOR_LOCAL_PLAYER", true);
}

void StartupNativeTracer::is_local_player_valid_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;
    const auto traceIndex = gIsLocalPlayerValidTraceCount++;
    trace_actor_bool_native(context, tracer->originalIsLocalPlayerValid_, traceIndex, 128u, "IS_LOCAL_PLAYER_VALID", true);
}

void StartupNativeTracer::respawn_player_actor_in_layout_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    const auto traceIndex = gRespawnPlayerActorTraceCount++;
    const auto a0 = read_u64_arg_value(context, 0);
    const auto a1 = read_u64_arg_value(context, 1);
    const auto a2 = read_u64_arg_value(context, 2);
    const auto a3 = read_u64_arg_value(context, 3);
    const auto a4 = read_u64_arg_value(context, 4);
    const auto a5 = read_u64_arg_value(context, 5);
    const auto a6 = read_u64_arg_value(context, 6);
    const auto a7 = read_u64_arg_value(context, 7);
    const auto a8 = read_u64_arg_value(context, 8);

    if (tracer->originalRespawnPlayerActorInLayout_) {
        tracer->originalRespawnPlayerActorInLayout_(context);
    }
    if (traceIndex >= 32u) return;

    const auto postA0 = read_u64_arg_value(context, 0);
    std::uintptr_t result = 0;
#ifdef _WIN32
    const bool resultOk = read_u64_return(context, result);
#else
    const bool resultOk = false;
#endif

    char actorName[160]{};
    const bool nameOk = read_c_string_pointer(a2, actorName, sizeof(actorName));

    char buffer[760]{};
    std::size_t used = static_cast<std::size_t>(std::snprintf(
        buffer, sizeof(buffer),
        "[FrontierNativeTrace] RESPAWN_PLAYER_ACTOR_IN_LAYOUT trace=%u "
        "args={a0=0x%llX a1=0x%llX a2=%s%s a3=0x%llX a4=0x%llX a5=0x%llX a6=0x%llX a7=0x%llX a8=0x%llX} "
        "postA0=0x%llX result=%s0x%llX",
        traceIndex,
        static_cast<unsigned long long>(a0),
        static_cast<unsigned long long>(a1),
        nameOk ? "" : "?",
        nameOk ? actorName : "<unreadable>",
        static_cast<unsigned long long>(a3),
        static_cast<unsigned long long>(a4),
        static_cast<unsigned long long>(a5),
        static_cast<unsigned long long>(a6),
        static_cast<unsigned long long>(a7),
        static_cast<unsigned long long>(a8),
        static_cast<unsigned long long>(postA0),
        resultOk ? "" : "?",
        static_cast<unsigned long long>(result)));
    append_execution_identity(buffer, sizeof(buffer), used, context);
    log(buffer);
}

void StartupNativeTracer::switch_player_to_enum_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    const auto traceIndex = gSwitchPlayerToEnumTraceCount++;
    const auto model = read_u64_arg_value(context, 0);
    const auto variation = read_u64_arg_value(context, 1);

    if (tracer->originalSwitchPlayerToEnum_) {
        tracer->originalSwitchPlayerToEnum_(context);
    }
    if (traceIndex >= 32u) return;

    char buffer[320]{};
    std::size_t used = static_cast<std::size_t>(std::snprintf(
        buffer, sizeof(buffer),
        "[FrontierNativeTrace] SWITCH_PLAYER_TO_ENUM trace=%u model=%u variation=%u",
        traceIndex,
        static_cast<unsigned>(static_cast<std::uint32_t>(model)),
        static_cast<unsigned>(static_cast<std::uint32_t>(variation))));
    append_execution_identity(buffer, sizeof(buffer), used, context);
    log(buffer);
}

void StartupNativeTracer::init_native_actorenum_player_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    const auto traceIndex = gInitNativeActorenumPlayerTraceCount++;
    const auto a0 = read_u64_arg_value(context, 0);
    const auto a1 = read_u64_arg_value(context, 1);
    const auto a2 = read_u64_arg_value(context, 2);
    const auto a3 = read_u64_arg_value(context, 3);

    if (tracer->originalInitNativeActorenumPlayer_) {
        tracer->originalInitNativeActorenumPlayer_(context);
    }
    if (traceIndex >= 64u) return;

    char buffer[360]{};
    std::size_t used = static_cast<std::size_t>(std::snprintf(
        buffer, sizeof(buffer),
        "[FrontierNativeTrace] INIT_NATIVE_ACTORENUM_PLAYER trace=%u args={0x%08X,0x%08X,0x%08X,0x%08X}",
        traceIndex,
        static_cast<unsigned>(static_cast<std::uint32_t>(a0)),
        static_cast<unsigned>(static_cast<std::uint32_t>(a1)),
        static_cast<unsigned>(static_cast<std::uint32_t>(a2)),
        static_cast<unsigned>(static_cast<std::uint32_t>(a3))));
    append_execution_identity(buffer, sizeof(buffer), used, context);
    log(buffer);
}

void StartupNativeTracer::get_actor_slot_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    const auto traceIndex = gGetActorSlotTraceCount++;
    const auto actorRaw = read_u64_arg_value(context, 0);

    if (tracer->originalGetActorSlot_) {
        tracer->originalGetActorSlot_(context);
    }
    if (traceIndex >= 256u) return;

    std::uint32_t resultRaw = 0;
    const bool resultOk =
#ifdef _WIN32
        read_u32_return(context, resultRaw);
#else
        false;
#endif

    char buffer[320]{};
    std::size_t used = static_cast<std::size_t>(std::snprintf(
        buffer, sizeof(buffer),
        "[FrontierNativeTrace] GET_ACTOR_SLOT trace=%u actorRaw=0x%llX actor=0x%08X result=%s0x%08X",
        traceIndex,
        static_cast<unsigned long long>(actorRaw),
        static_cast<unsigned>(static_cast<std::uint32_t>(actorRaw)),
        resultOk ? "" : "?",
        resultRaw));
    append_execution_identity(buffer, sizeof(buffer), used, context);
    log(buffer);
}

void StartupNativeTracer::get_slot_actor_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    const auto traceIndex = gGetSlotActorTraceCount++;
    std::int32_t slot = 0;
    const bool slotOk = read_i32_arg(context, 0, slot);

    if (tracer->originalGetSlotActor_) {
        tracer->originalGetSlotActor_(context);
    }
    if (traceIndex >= 256u) return;

    std::uintptr_t result = 0;
    const bool resultOk =
#ifdef _WIN32
        read_u64_return(context, result);
#else
        false;
#endif

    char buffer[320]{};
    std::size_t used = static_cast<std::size_t>(std::snprintf(
        buffer, sizeof(buffer),
        "[FrontierNativeTrace] GET_SLOT_ACTOR trace=%u slot=%s%d result=%s0x%llX actor=0x%08X",
        traceIndex,
        slotOk ? "" : "?",
        slotOk ? slot : 0,
        resultOk ? "" : "?",
        static_cast<unsigned long long>(result),
        static_cast<unsigned>(static_cast<std::uint32_t>(result))));
    append_execution_identity(buffer, sizeof(buffer), used, context);
    log(buffer);
}

void StartupNativeTracer::get_local_slot_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    const auto traceIndex = gGetLocalSlotTraceCount++;

    if (tracer->originalGetLocalSlot_) {
        tracer->originalGetLocalSlot_(context);
    }
    if (traceIndex >= 64u) return;

    std::uint32_t resultRaw = 0;
    const bool resultOk =
#ifdef _WIN32
        read_u32_return(context, resultRaw);
#else
        false;
#endif

    char buffer[288]{};
    std::size_t used = static_cast<std::size_t>(std::snprintf(
        buffer, sizeof(buffer),
        "[FrontierNativeTrace] GET_LOCAL_SLOT trace=%u result=%s0x%08X slot=%s%d",
        traceIndex,
        resultOk ? "" : "?",
        resultRaw,
        resultOk ? "" : "?",
        resultOk ? static_cast<std::int32_t>(resultRaw) : 0));
    append_execution_identity(buffer, sizeof(buffer), used, context);
    log(buffer);
}

void StartupNativeTracer::is_slot_valid_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    const auto traceIndex = gIsSlotValidTraceCount++;
    std::int32_t slot = 0;
    const bool slotOk = read_i32_arg(context, 0, slot);

    if (tracer->originalIsSlotValid_) {
        tracer->originalIsSlotValid_(context);
    }
    if (traceIndex >= 256u) return;

    std::uint32_t resultRaw = 0;
    const bool resultOk =
#ifdef _WIN32
        read_u32_return(context, resultRaw);
#else
        false;
#endif

    char buffer[320]{};
    std::size_t used = static_cast<std::size_t>(std::snprintf(
        buffer, sizeof(buffer),
        "[FrontierNativeTrace] IS_SLOT_VALID trace=%u slot=%s%d result=%s%u",
        traceIndex,
        slotOk ? "" : "?",
        slotOk ? slot : 0,
        resultOk ? "" : "?",
        resultOk ? resultRaw : 0u));
    append_execution_identity(buffer, sizeof(buffer), used, context);
    log(buffer);
}

void StartupNativeTracer::create_actor_in_layout_hook(void* context) {
    auto* tracer = g_tracer;
    if (!tracer) return;

    const auto traceIndex = gCreateActorInLayoutTraceCount++;
#ifdef _WIN32
    const auto* call = reinterpret_cast<const NativeTraceContext*>(context);
    const auto argc = call ? call->argumentCount : 0u;
#else
    const auto argc = 0u;
#endif

    // Capture the caller-supplied arguments before the native executes.
    // CREATE_ACTOR_IN_LAYOUT may rewrite slot 0 with the resulting Actor handle.
    const auto preA0 = read_u64_arg_value(context, 0);
    const auto preA1 = read_u64_arg_value(context, 1);
    const auto preA2 = read_u64_arg_value(context, 2);
    const auto preA3 = read_u64_arg_value(context, 3);
    const auto preA4 = read_u64_arg_value(context, 4);
    const auto preA5 = read_u64_arg_value(context, 5);
    const auto preA6 = read_u64_arg_value(context, 6);

    if (tracer->originalCreateActorInLayout_) {
        tracer->originalCreateActorInLayout_(context);
    }
    if (traceIndex >= 64) return;

    char actorName[160]{};
    const bool actorNameOk = read_c_string_pointer(preA1, actorName, sizeof(actorName));

    const auto postA0 = read_u64_arg_value(context, 0);

    std::uintptr_t result = 0;
#ifdef _WIN32
    const bool resultOk = read_u64_return(context, result);
#else
    const bool resultOk = false;
#endif

    // Decode the Vector2 slots from the pre-call snapshot.
    std::uint32_t posXYLow = static_cast<std::uint32_t>(preA3);
    std::uint32_t posXYHigh = static_cast<std::uint32_t>(preA3 >> 32u);
    std::uint32_t posZRaw = static_cast<std::uint32_t>(preA4);
    std::uint32_t orientationXYLow = static_cast<std::uint32_t>(preA5);
    std::uint32_t orientationXYHigh = static_cast<std::uint32_t>(preA5 >> 32u);
    std::uint32_t orientationZRaw = static_cast<std::uint32_t>(preA6);

    float posX = 0.0f;
    float posY = 0.0f;
    float posZ = 0.0f;
    float orientationX = 0.0f;
    float orientationY = 0.0f;
    float orientationZ = 0.0f;
    std::memcpy(&posX, &posXYLow, sizeof(posX));
    std::memcpy(&posY, &posXYHigh, sizeof(posY));
    std::memcpy(&posZ, &posZRaw, sizeof(posZ));
    std::memcpy(&orientationX, &orientationXYLow, sizeof(orientationX));
    std::memcpy(&orientationY, &orientationXYHigh, sizeof(orientationY));
    std::memcpy(&orientationZ, &orientationZRaw, sizeof(orientationZ));

    std::int32_t actorEnum = 0;
    const auto actorEnumOk = argc > 2u;
#ifdef _WIN32
    if (actorEnumOk) {
        actorEnum = static_cast<std::int32_t>(static_cast<std::uint32_t>(preA2));
    }
#else
    (void)actorEnumOk;
#endif

    char buffer[960]{};
    std::size_t used = static_cast<std::size_t>(std::snprintf(
        buffer, sizeof(buffer),
        "[FrontierNativeTrace] CREATE_ACTOR_IN_LAYOUT trace=%u argc=%u "
        "preLayout=0x%llX actorName=%s%s actorEnum=%s%d "
        "pos=(%.6f,%.6f,%.6f) orient=(%.6f,%.6f,%.6f) "
        "preRaw={a0=0x%llX a1=0x%llX a2=0x%llX a3=0x%llX a4=0x%llX a5=0x%llX a6=0x%llX} "
        "postA0=0x%llX result=%s0x%llX",
        traceIndex,
        argc,
        static_cast<unsigned long long>(preA0),
        actorNameOk ? "" : "?",
        actorNameOk ? actorName : "<unreadable>",
        actorEnumOk ? "" : "?",
        actorEnum,
        posX, posY, posZ,
        orientationX, orientationY, orientationZ,
        static_cast<unsigned long long>(preA0),
        static_cast<unsigned long long>(preA1),
        static_cast<unsigned long long>(preA2),
        static_cast<unsigned long long>(preA3),
        static_cast<unsigned long long>(preA4),
        static_cast<unsigned long long>(preA5),
        static_cast<unsigned long long>(preA6),
        static_cast<unsigned long long>(postA0),
        resultOk ? "" : "?",
        static_cast<unsigned long long>(result)));

    append_execution_identity(buffer, sizeof(buffer), used, context);
    log(buffer);
}

} // namespace frontier::game
