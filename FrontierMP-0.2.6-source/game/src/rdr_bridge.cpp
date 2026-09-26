#include "frontier/game/rdr_bridge.hpp"

#include "frontier/game/pattern_scanner.hpp"
#include "frontier/game/game_thread_dispatcher.hpp"
#include "frontier/game/content_path_redirector.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <cstring>
#include <sstream>
#include <memory>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <utility>

#ifdef _WIN32
#include <intrin.h>
#endif

namespace frontier::game {

namespace {

constexpr const char* kLocalPlayerPattern =
    "48 89 15 ? ? ? ? E9 ? ? ? ?";
constexpr const char* kHistoricalWaitPattern =
    "E8 ? ? ? ? 8D 56 10";
constexpr const char* kHistoricalRdrStartNewScriptPattern =
    "E8 ? ? ? ? 48 8B 0B 8B 44 24 40 89 01 48 83 C4 30 5B C3 CC 40 53";
constexpr const char* kHistoricalStartNewThreadOverridePattern =
    "E8 ? ? ? ? 48 8B D8 85 FF 74 16";
constexpr std::uint32_t kNativeGetScriptName = 0x0BC52445u;

constexpr std::uint32_t kNativeGetGameState = 0xDD9BD22B;
constexpr std::uint32_t kNativeUiSendEvent = 0xB58825F5;
constexpr std::uint32_t kNativeHudFadeToLoadingScreen = 0xB0B4296A;
constexpr std::uint32_t kNativeHudIsFading = 0xE5CC6F08;
constexpr std::uint32_t kNativeUiExit = 0x2DF89C2E;
constexpr std::uint32_t kNativeNetAuthenticateGamer = 0x8E0D7219;
constexpr std::uint32_t kNativeStreamingIsWorldLoaded = 0x87B74064;
constexpr std::uint32_t kNativeIsSimulateStartMultiplayer = 0x9A73C2CD;
constexpr std::uint32_t kNativeIsStartPosInCommandLine = 0x814D97E8;
constexpr std::uint32_t kNativeFindNamedLayout = 0x5699DE7E;
constexpr std::uint32_t kNativeCreateLayout = 0x6CA53214;
constexpr std::uint32_t kNativeIsLayoutRefValid = 0xFC8E55ED;
constexpr std::uint32_t kNativeCreateActorInLayout = 0x8D67F397;
constexpr std::uint32_t kNativeDestroyActor = 0x8BD21869;
constexpr std::uint32_t kNativeTeleportActorWithHeading = 0xE4DE507C;
constexpr std::uint32_t kNativeCreatePlayerActorInLayout = 0x6A307D5F;
constexpr std::uint32_t kNativeGetPlayerActor = 0xE8CFDD53;
constexpr std::uint32_t kNativeIsActorPlayer = 0xB27E91E7;
constexpr std::uint32_t kNativeIsActorLocalPlayer = 0x6542CF26;
constexpr std::uint32_t kNativeGetActorSlot = 0xAABF3356;
constexpr std::uint32_t kNativeGetActorUpdatePriority = 0x6D322CD3;
constexpr std::uint32_t kNativeIsActorValid = 0xBA6C3E92u;
constexpr std::uint32_t kNativeGetPosition = 0x99BD9D6Fu;
constexpr std::uint32_t kNativeIsActorenumInstalled = 0x9B903F45;
constexpr std::uint32_t kNativeGetActorEnum = 0x0B28E9EC;
constexpr std::uint32_t kNativeStreamingRequestActor = 0xB0A79FEE;
constexpr std::uint32_t kNativeStreamingIsActorLoaded = 0x7DF72579;
constexpr std::uint32_t kNativeGetLocalSlot = 0xAD68A22E;
constexpr std::uint32_t kNativeGetSlotActor = 0xDB9B49D8;
constexpr std::uint32_t kNativeEnableMover = 0xE29F0A39;
constexpr std::uint32_t kNativeSetMoverFrozen = 0x13E6B5EE;
constexpr std::uint32_t kNativeTaskGoToCoord = 0x8C574832;
constexpr std::uint32_t kNativeMakeActorReadyForAction = 0xF04335A6;
constexpr std::uint32_t kNativeSetActorStreamingHighPriority = 0x0911BA31;
constexpr std::uint32_t kNativeActorForceNextUpdate = 0x5C7F63E3;
constexpr std::uint32_t kNativeIsMoverFrozen = 0x9C12BD5A;
constexpr std::uintptr_t kTaggedLayoutRef = 0x100000000ull;
constexpr std::int32_t kRemoteTestActorEnum = 837; // ACTOR_MPPLAYER01

constexpr const char* kActorManagerPattern =
    "48 8B 05 ? ? ? ? 0F B7 CA 48 03 C9 C1 EA 10 66 39 54 C8 ? 75 03 B0 01 C3 32 C0 C3 CC 48 89 5C 24 ?";

struct MinimalSagPlayer final {
    std::byte padding0[0x5EC];
    std::uint32_t guid;
};

void write_bridge_log_line(const char* message) {
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

#ifdef _WIN32
std::filesystem::path frontier_client_package_root() {
    HMODULE module = GetModuleHandleW(L"FrontierClient.dll");
    if (!module) return {};

    wchar_t modulePath[32768]{};
    constexpr DWORD kModulePathCapacity =
        static_cast<DWORD>(sizeof(modulePath) / sizeof(modulePath[0]));
    const DWORD length = GetModuleFileNameW(module, modulePath, kModulePathCapacity);
    if (length == 0 || length >= kModulePathCapacity) return {};

    return std::filesystem::path(modulePath).parent_path();
}
#endif

struct MinimalSagActor final {
    std::byte padding0[0xB0];
    std::uintptr_t actorComponent;
};

struct MinimalSagActorComponent final {
    std::byte padding0[0x18];
    std::uintptr_t transform;
};

struct MinimalMatrix34 final {
    std::byte padding0[0x30];
    Vec3 position;
};

#ifdef _WIN32
bool guarded_read_u32(std::uintptr_t address, std::uint32_t& out) {
    __try {
        out = *reinterpret_cast<const std::uint32_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = 0;
        return false;
    }
}

bool guarded_read_pointer(std::uintptr_t address, std::uintptr_t& out) {
    __try {
        out = *reinterpret_cast<const std::uintptr_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = 0;
        return false;
    }
}

bool guarded_read_position(std::uintptr_t address, Vec3& out) {
    __try {
        out = reinterpret_cast<const MinimalMatrix34*>(address)->position;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = {};
        return false;
    }
}

bool guarded_read_c_string(std::uintptr_t address, char* out, std::size_t capacity) {
    if (out == nullptr || capacity == 0) return false;
    out[0] = '\0';
    if (address == 0) return false;

    __try {
        for (std::size_t i = 0; i + 1 < capacity; ++i) {
            const char ch = *reinterpret_cast<const char*>(address + i);
            out[i] = ch;
            if (ch == '\0') return true;
        }
        out[capacity - 1] = '\0';
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
        return false;
    }
}

bool guarded_read_vec3(std::uintptr_t address, Vec3& out) {
    __try {
        out = *reinterpret_cast<const Vec3*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = {};
        return false;
    }
}
#endif


} // namespace

RdrBridge* RdrBridge::activeHistoricalScriptTrace_ = nullptr;

namespace {

std::uintptr_t resolve_relative_call_target(
    std::uintptr_t callSite,
    std::uintptr_t moduleBase,
    std::size_t imageSize) {
#ifdef _WIN32
    if (callSite == 0 || moduleBase == 0 || imageSize == 0) return 0;
    if (callSite < moduleBase || callSite - moduleBase >= imageSize) return 0;

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(callSite), &mbi, sizeof(mbi)) != sizeof(mbi)) {
        return 0;
    }
    if (mbi.State != MEM_COMMIT ||
        (mbi.Protect & PAGE_GUARD) != 0 ||
        (mbi.Protect & PAGE_NOACCESS) != 0) {
        return 0;
    }

    const auto* bytes = reinterpret_cast<const std::uint8_t*>(callSite);
    __try {
        if (*bytes != 0xE8) return 0;
        std::int32_t displacement = 0;
        std::memcpy(&displacement, bytes + 1, sizeof(displacement));
        const auto target =
            callSite + 5 + static_cast<std::intptr_t>(displacement);
        if (target < moduleBase || target - moduleBase >= imageSize) return 0;
        return target;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
#else
    (void)callSite;
    (void)moduleBase;
    (void)imageSize;
    return 0;
#endif
}

void log_historical_script_resolution(
    const char* label,
    std::uintptr_t callSite,
    std::uintptr_t target,
    std::uintptr_t moduleBase) {
    char line[384]{};
    std::snprintf(
        line,
        sizeof(line),
        "[FrontierScript] historical %s callsite=0x%llX rva=0x%llX "
        "target=0x%llX targetRva=0x%llX",
        label,
        static_cast<unsigned long long>(callSite),
        static_cast<unsigned long long>(callSite - moduleBase),
        static_cast<unsigned long long>(target),
        static_cast<unsigned long long>(target - moduleBase));
    write_bridge_log_line(line);
}
#ifdef _WIN32
bool guarded_read_code_byte(std::uintptr_t address, std::uint8_t& out) {
    __try {
        out = *reinterpret_cast<const std::uint8_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = 0;
        return false;
    }
}
#endif



bool historical_wait_target_plausible(std::uintptr_t target) {
#ifdef _WIN32
    std::uint8_t bytes[4]{};
    for (std::size_t i = 0; i < sizeof(bytes); ++i) {
        if (!guarded_read_code_byte(target + i, bytes[i])) return false;
    }

    // Reject obvious tail/landing bytes. The current weak E8 signature resolved
    // to an address beginning with "add esp, imm32; ret", which is not a valid
    // x64 function entry and caused the hook to execute into a bogus stack frame.
    if (bytes[0] == 0xC3 || bytes[0] == 0xC2 || bytes[0] == 0xCC ||
        bytes[0] == 0xE9 || bytes[0] == 0xEB) {
        return false;
    }
    if (bytes[0] == 0x81 && bytes[1] == 0xC4) return false;
    if (bytes[0] == 0x83 && bytes[1] == 0xC4) return false;

    // Common MSVC/RAGE x64 function-entry families: push/mov/sub/REX prologues.
    switch (bytes[0]) {
        case 0x40:
        case 0x41:
        case 0x48:
        case 0x49:
        case 0x4C:
        case 0x55:
        case 0x56:
        case 0x57:
        case 0x53:
        case 0xF3:
            return true;
        default:
            return false;
    }
#else
    (void)target;
    return false;
#endif
}

std::vector<std::uintptr_t> find_pattern_hits(
    const std::uint8_t* data,
    std::size_t size,
    const BytePattern& pattern,
    std::size_t maxHits) {
    std::vector<std::uintptr_t> hits;
    if (data == nullptr || pattern.bytes.empty() || size < pattern.bytes.size()) {
        return hits;
    }

    for (std::size_t offset = 0;
         offset + pattern.bytes.size() <= size && hits.size() < maxHits;
         ++offset) {
        bool match = true;
        for (std::size_t i = 0; i < pattern.bytes.size(); ++i) {
            if (pattern.bytes[i].has_value() &&
                data[offset + i] != *pattern.bytes[i]) {
                match = false;
                break;
            }
        }
        if (match) {
            hits.push_back(reinterpret_cast<std::uintptr_t>(data + offset));
        }
    }
    return hits;
}

void log_code_window(
    const char* label,
    std::uintptr_t address,
    std::uintptr_t moduleBase,
    std::size_t before,
    std::size_t after) {
#ifdef _WIN32
    if (label == nullptr || address == 0 || moduleBase == 0 || address < before) {
        return;
    }

    std::ostringstream stream;
    stream << "[FrontierScript] code-window " << label
           << " address=0x" << std::hex << std::uppercase << address
           << " rva=0x" << (address - moduleBase) << " bytes=";

    const auto start = address - before;
    const auto count = before + after;
    for (std::size_t i = 0; i < count; ++i) {
        std::uint8_t byte = 0;
        const auto current = start + i;
        const bool ok = guarded_read_code_byte(current, byte);

        if (!ok) {
            stream << "??";
        } else {
            constexpr char hex[] = "0123456789ABCDEF";
            stream << hex[(byte >> 4u) & 0x0Fu]
                   << hex[byte & 0x0Fu];
        }
        if (i + 1 < count) stream << ' ';
    }

    write_bridge_log_line(stream.str().c_str());
#else
    (void)label;
    (void)address;
    (void)moduleBase;
    (void)before;
    (void)after;
#endif
}


} // namespace

std::uintptr_t RdrBridge::resolve_rip_target(std::uintptr_t instruction) const {
#ifdef _WIN32
    if (!readable(instruction + 3, sizeof(std::int32_t))) return 0;
    std::int32_t displacement = 0;
    std::memcpy(&displacement, reinterpret_cast<const void*>(instruction + 3), sizeof(displacement));
    return instruction + 7 + static_cast<std::intptr_t>(displacement);
#else
    (void)instruction;
    return 0;
#endif
}

bool RdrBridge::readable(std::uintptr_t address, std::size_t size) const {
#ifdef _WIN32
    if (address == 0 || size == 0) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if ((mbi.Protect & PAGE_GUARD) != 0 || (mbi.Protect & PAGE_NOACCESS) != 0) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    const auto end = begin + mbi.RegionSize;
    if (address < begin || address >= end || size > end - address) return false;
    const DWORD readableFlags = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                 PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & readableFlags) != 0;
#else
    (void)address; (void)size;
    return false;
#endif
}

bool RdrBridge::read_pointer(std::uintptr_t address, std::uintptr_t& out) const {
#ifdef _WIN32
    if (!readable(address, sizeof(out))) return false;
    __try {
        std::memcpy(&out, reinterpret_cast<const void*>(address), sizeof(out));
        return out != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = 0;
        return false;
    }
#else
    (void)address; (void)out;
    return false;
#endif
}

void RdrBridge::historical_wait_hook(void* context) {
    auto* bridge = activeHistoricalScriptTrace_;
    if (bridge == nullptr) return;

    bridge->on_historical_wait(context);

    using WaitFn = void (*)(void*);
    const auto original =
        reinterpret_cast<WaitFn>(bridge->historicalWaitHook_.trampoline());
    if (original != nullptr) {
        original(context);
    }
}

void RdrBridge::on_historical_wait(void* context) {
#ifdef _WIN32
    const auto traceIndex =
        historicalWaitTraceCount_.fetch_add(1, std::memory_order_relaxed);
    if (traceIndex >= 256) return;

    char scriptName[128]{};
    const char* scriptNameText = "<native-not-ready>";
    if (nativeInvoker_.ready()) {
        std::uintptr_t result = 0;
        if (nativeInvoker_.invoke_raw(
                kNativeGetScriptName, nullptr, 0u, result) &&
            result != 0 &&
            guarded_read_c_string(result, scriptName, sizeof(scriptName))) {
            scriptNameText = scriptName;
        } else {
            scriptNameText = "<unreadable>";
        }
    }

    char line[640]{};
    std::snprintf(
        line,
        sizeof(line),
        "[FrontierScript] historical scrThread::Wait trace=%u "
        "thread=%lu infoBase=0x%llX script=%s return=0x%llX",
        static_cast<unsigned>(traceIndex),
        static_cast<unsigned long>(GetCurrentThreadId()),
        static_cast<unsigned long long>(
            reinterpret_cast<std::uintptr_t>(context)),
        scriptNameText,
        static_cast<unsigned long long>(
            reinterpret_cast<std::uintptr_t>(_ReturnAddress())));
    write_bridge_log_line(line);
#else
    (void)context;
#endif
}

bool RdrBridge::local_player_pointer_available() const {
#ifdef _WIN32
    if (!initialized_ || !localPlayerStorage_) return false;
    std::uintptr_t localPlayer = 0;
    return read_pointer(localPlayerStorage_, localPlayer);
#else
    return false;
#endif
}

std::string RdrBridge::local_player_chain_diagnostic() const {
    std::lock_guard lock(localPlayerDiagnosticMutex_);
    return localPlayerChainDiagnostic_;
}

bool RdrBridge::initialize(const ExecutableFingerprint& fingerprint, KnownBuild build) {
    initialized_ = false;
    build_ = build;
    localPlayerStorage_ = 0;
    actorManagerSlotsStorage_ = 0;
    historicalWaitTarget_ = 0;
    historicalRdrStartNewScriptTarget_ = 0;
    historicalStartNewThreadOverrideTarget_ = 0;
    historicalWaitTraceCount_.store(0, std::memory_order_release);
    if (activeHistoricalScriptTrace_ == this) {
        activeHistoricalScriptTrace_ = nullptr;
    }
    historicalOnlineBootstrapStage_ = HistoricalOnlineBootstrapStage::NotStarted;
    historicalOnlineBootstrapTask_ = HistoricalOnlineBootstrapTask::None;
    historicalOnlineBootstrapEventCompleted_ = false;
    historicalOnlineBootstrapAttempts_ = 0;
    historicalOnlineBootstrapTaskPending_.store(false, std::memory_order_release);
    historicalOnlineBootstrapTaskDone_.store(false, std::memory_order_release);
    historicalOnlineBootstrapTaskFailed_.store(false, std::memory_order_release);
    historicalOnlineBootstrapTaskResult_.store(0u, std::memory_order_release);
    historicalOnlineBootstrapAuthResult_.store(0u, std::memory_order_release);
    {
        std::lock_guard lock(remoteActorTestMutex_);
        remoteActorTestPending_ = false;
        remoteActorTestSpawned_ = false;
        remoteActorTestLayout_ = 0;
        remoteActorTestActorRef_ = 0;
        remoteActorLayout_ = 0;
    }
    {
        std::lock_guard lock(runtimeSnapshotMutex_);
        runtimeSnapshot_ = {};
        runtimeRefreshPending_ = false;
    }

#ifdef _WIN32
    if (build == KnownBuild::Unknown) return false;
    if (!BuildDetector::descriptor(build).fingerprintComplete) return false;

    HMODULE module = GetModuleHandleW(nullptr);
    if (!module) return false;
    moduleBase_ = reinterpret_cast<std::uintptr_t>(module);

    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase_);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(moduleBase_ + static_cast<std::uintptr_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

    const auto descriptor = BuildDetector::descriptor(build);
    if (descriptor.expected.textHash != fingerprint.textHash ||
        descriptor.expected.imageSize != fingerprint.imageSize ||
        descriptor.expected.peTimestamp != fingerprint.peTimestamp) {
        return false;
    }

    const auto* text = reinterpret_cast<const std::uint8_t*>(moduleBase_ + fingerprint.textRva);
    const auto localPattern = BytePattern::parse(kLocalPlayerPattern);
    const auto actorPattern = BytePattern::parse(kActorManagerPattern);
    if (!localPattern || !actorPattern) return false;

    const auto localHit = PatternScanner::scan_buffer(text, fingerprint.textSize, *localPattern);
    const auto actorHit = PatternScanner::scan_buffer(text, fingerprint.textSize, *actorPattern);
    if (!localHit || !actorHit) return false;

    const auto localInstruction = *localHit;
    const auto actorInstruction = *actorHit;
    if (!PatternScanner::validate_in_module(localInstruction, moduleBase_, nt->OptionalHeader.SizeOfImage) ||
        !PatternScanner::validate_in_module(actorInstruction, moduleBase_, nt->OptionalHeader.SizeOfImage)) {
        return false;
    }

    localPlayerStorage_ = resolve_rip_target(localInstruction);
    actorManagerSlotsStorage_ = resolve_rip_target(actorInstruction);

    if (!PatternScanner::validate_in_module(localPlayerStorage_, moduleBase_, nt->OptionalHeader.SizeOfImage) ||
        !readable(actorManagerSlotsStorage_, sizeof(std::uintptr_t))) {
        localPlayerStorage_ = 0;
        actorManagerSlotsStorage_ = 0;
        return false;
    }

    // Historical game-core resolves the script lifecycle targets before waiting
    // for native registration. Do the same here so the first script waits are
    // observable even while the native invoker is still unavailable.
    const auto waitPattern = BytePattern::parse(kHistoricalWaitPattern);
    const auto rdrStartNewScriptPattern =
        BytePattern::parse(kHistoricalRdrStartNewScriptPattern);
    const auto startNewThreadOverridePattern =
        BytePattern::parse(kHistoricalStartNewThreadOverridePattern);

    if (waitPattern) {
        // The historical E8 signature is intentionally treated as a candidate
        // locator, not a proof of function identity. The first hit on this build
        // decoded to 0x1E6A60, whose bytes are an x64-invalid tail ("add esp; ret")
        // immediately followed by a different function at +0x10. Hook only when
        // exactly one decoded target has a plausible x64 function entry.
        const waitHits = find_pattern_hits(
            text,
            fingerprint.textSize,
            *waitPattern,
            16u);
        if (waitHits.empty()) {
            write_bridge_log_line(
                "[FrontierScript] historical scrThread::Wait pattern not found");
        } else {
            std::size_t plausibleCount = 0;
            std::uintptr_t plausibleTarget = 0;
            std::uintptr_t plausibleCallSite = 0;

            for (std::size_t i = 0; i < waitHits.size(); ++i) {
                const auto callSite = waitHits[i];
                const auto target = resolve_relative_call_target(
                    callSite,
                    moduleBase_,
                    nt->OptionalHeader.SizeOfImage);

                char candidate[384]{};
                std::snprintf(
                    candidate,
                    sizeof(candidate),
                    "[FrontierScript] historical scrThread::Wait candidate=%u callsite=0x%llX rva=0x%llX target=0x%llX targetRva=0x%llX plausible=%u",
                    static_cast<unsigned>(i),
                    static_cast<unsigned long long>(callSite),
                    static_cast<unsigned long long>(callSite - moduleBase_),
                    static_cast<unsigned long long>(target),
                    target != 0
                        ? static_cast<unsigned long long>(target - moduleBase_)
                        : 0ull,
                    target != 0 && historical_wait_target_plausible(target) ? 1u : 0u);
                write_bridge_log_line(candidate);

                log_code_window(
                    "scrThread::Wait candidate callsite",
                    callSite,
                    moduleBase_,
                    24u,
                    32u);
                if (target != 0 &&
                    PatternScanner::validate_in_module(
                        target,
                        moduleBase_,
                        nt->OptionalHeader.SizeOfImage)) {
                    log_code_window(
                        "scrThread::Wait candidate target",
                        target,
                        moduleBase_,
                        8u,
                        32u);
                }

                if (target != 0 &&
                    PatternScanner::validate_in_module(
                        target,
                        moduleBase_,
                        nt->OptionalHeader.SizeOfImage) &&
                    historical_wait_target_plausible(target)) {
                    ++plausibleCount;
                    plausibleTarget = target;
                    plausibleCallSite = callSite;
                }
            }

            if (plausibleCount == 1) {
                historicalWaitTarget_ = plausibleTarget;
                log_historical_script_resolution(
                    "scrThread::Wait",
                    plausibleCallSite,
                    historicalWaitTarget_,
                    moduleBase_);

                std::string hookError;
                activeHistoricalScriptTrace_ = this;
                // game-core's PostLoad pattern identifies a CALL site, but its
                // hooker resolves the E8 target and patches that function.
                // Keep the ABI probe disabled unless the target itself passes
                // the x64 entry-point sanity check above.
                if (!historicalWaitHook_.install(
                        historicalWaitTarget_,
                        reinterpret_cast<std::uintptr_t>(&RdrBridge::historical_wait_hook),
                        16u,
                        "historical scrThread::Wait",
                        hookError)) {
                    activeHistoricalScriptTrace_ = nullptr;
                    std::string message =
                        "[FrontierScript] historical scrThread::Wait target hook not attached: " +
                        hookError;
                    write_bridge_log_line(message.c_str());
                } else {
                    char message[256]{};
                    std::snprintf(
                        message,
                        sizeof(message),
                        "[FrontierScript] historical scrThread::Wait target hook attached target=0x%llX targetRva=0x%llX",
                        static_cast<unsigned long long>(historicalWaitTarget_),
                        static_cast<unsigned long long>(
                            historicalWaitTarget_ - moduleBase_));
                    write_bridge_log_line(message);
                }
            } else {
                activeHistoricalScriptTrace_ = nullptr;
                if (plausibleCount == 0) {
                    write_bridge_log_line(
                        "[FrontierScript] historical scrThread::Wait no plausible target; hook disabled");
                } else {
                    char message[256]{};
                    std::snprintf(
                        message,
                        sizeof(message),
                        "[FrontierScript] historical scrThread::Wait ambiguous plausible targets=%u; hook disabled",
                        static_cast<unsigned>(plausibleCount));
                    write_bridge_log_line(message);
                }
            }
        }
    }

    if (rdrStartNewScriptPattern) {
        const auto hit = PatternScanner::scan_buffer(
            text, fingerprint.textSize, *rdrStartNewScriptPattern);
        if (hit &&
            PatternScanner::validate_in_module(
                *hit, moduleBase_, nt->OptionalHeader.SizeOfImage)) {
            historicalRdrStartNewScriptTarget_ = resolve_relative_call_target(
                *hit, moduleBase_, nt->OptionalHeader.SizeOfImage);
            if (historicalRdrStartNewScriptTarget_ != 0) {
                log_historical_script_resolution(
                    "sagCoreScript::RDRStartNewScript",
                    *hit,
                    historicalRdrStartNewScriptTarget_,
                    moduleBase_);
                log_code_window("sagCoreScript::RDRStartNewScript callsite", *hit, moduleBase_, 24u, 64u);
                log_code_window("sagCoreScript::RDRStartNewScript target", historicalRdrStartNewScriptTarget_, moduleBase_, 16u, 64u);
            }
        }
        if (historicalRdrStartNewScriptTarget_ == 0) {
            write_bridge_log_line(
                "[FrontierScript] historical RDRStartNewScript target unresolved");
        }
    }

    if (startNewThreadOverridePattern) {
        const auto hit = PatternScanner::scan_buffer(
            text, fingerprint.textSize, *startNewThreadOverridePattern);
        if (hit &&
            PatternScanner::validate_in_module(
                *hit, moduleBase_, nt->OptionalHeader.SizeOfImage)) {
            historicalStartNewThreadOverrideTarget_ = resolve_relative_call_target(
                *hit, moduleBase_, nt->OptionalHeader.SizeOfImage);
            if (historicalStartNewThreadOverrideTarget_ != 0) {
                log_historical_script_resolution(
                    "rage::scrThread::StartNewThreadOverride",
                    *hit,
                    historicalStartNewThreadOverrideTarget_,
                    moduleBase_);
                log_code_window("rage::scrThread::StartNewThreadOverride callsite", *hit, moduleBase_, 24u, 64u);
                log_code_window("rage::scrThread::StartNewThreadOverride target", historicalStartNewThreadOverrideTarget_, moduleBase_, 16u, 64u);
            }
        }
        if (historicalStartNewThreadOverrideTarget_ == 0) {
            write_bridge_log_line(
                "[FrontierScript] historical StartNewThreadOverride target unresolved");
        }
    }

    const auto packageRoot = frontier_client_package_root();
    if (packageRoot.empty()) {
        write_bridge_log_line(
            "[FrontierContent] unable to resolve FrontierClient.dll package root; "
            "content hook disabled");
    } else {
        std::string contentHookError;
        if (!contentPathRedirector_.install(
                text,
                fingerprint.textSize,
                moduleBase_,
                nt->OptionalHeader.SizeOfImage,
                packageRoot,
                contentHookError)) {
            std::string message =
                "[FrontierContent] fullReadPath hook not attached: " +
                contentHookError;
            write_bridge_log_line(message.c_str());
        }
    }

    initialized_ = true;
    return true;
#else
    (void)fingerprint;
    (void)build;
    return false;
#endif
}

bool RdrBridge::try_initialize_native_invoker() {
#ifdef _WIN32
    if (!initialized_) return false;
    const auto descriptor = BuildDetector::descriptor(build_);
    return nativeInvoker_.initialize(moduleBase_, descriptor.expected.imageSize,
                                     descriptor.expected.textRva, descriptor.expected.textSize);
#else
    return false;
#endif
}

bool RdrBridge::try_initialize_game_thread_dispatcher() {
#ifdef _WIN32
    if (!initialized_ || !nativeInvoker_.ready()) {
        gameThreadDispatcherError_ = "native invoker not ready";
        return false;
    }
    if (gameThreadDispatcher_.attached()) return true;

    std::string error;
    if (!gameThreadDispatcher_.attach(nativeInvoker_, error)) {
        gameThreadDispatcherError_ = error;
        return false;
    }

    gameThreadDispatcherError_.clear();

    if (!startupNativeTracer_.attached()) {
        std::string traceError;
        if (!startupNativeTracer_.attach(nativeInvoker_, gameThreadDispatcher_, traceError)) {
            gameThreadDispatcherError_ = "dispatcher attached but startup tracer failed: " + traceError;
            return true;
        }
    }

    return true;
#else
    gameThreadDispatcherError_ = "game-thread dispatcher is Windows-only";
    return false;
#endif
}

bool RdrBridge::advance_historical_online_bootstrap(std::string& logLine) {
    logLine.clear();

    if (!initialized_ || !nativeInvoker_.ready() ||
        !gameThreadDispatcher_.attached()) {
        return false;
    }

    auto submitTask = [this](HistoricalOnlineBootstrapTask task,
                             std::string& error) {
        error.clear();

        if (historicalOnlineBootstrapTaskPending_.load(
                std::memory_order_acquire)) {
            return true;
        }

        historicalOnlineBootstrapTask_ = task;
        historicalOnlineBootstrapTaskDone_.store(false, std::memory_order_release);
        historicalOnlineBootstrapTaskFailed_.store(false, std::memory_order_release);
        historicalOnlineBootstrapTaskPending_.store(true, std::memory_order_release);

        const bool submitted = gameThreadDispatcher_.submit(
            [this, task]() {
                bool ok = true;
                std::uintptr_t result = 0u;

                switch (task) {
                case HistoricalOnlineBootstrapTask::SendEnterOnlineForInvite: {
                    std::uintptr_t args[1]{};
                    args[0] = reinterpret_cast<std::uintptr_t>(
                        "net.EnterOnlineForInvite");
                    ok = nativeInvoker_.invoke_raw(
                        kNativeUiSendEvent, args, 1u, result);
                    historicalOnlineBootstrapTaskResult_.store(
                        static_cast<std::uint32_t>(result),
                        std::memory_order_release);
                    break;
                }

                case HistoricalOnlineBootstrapTask::FadeToLoadingScreen:
                    ok = nativeInvoker_.invoke_raw(
                        kNativeHudFadeToLoadingScreen, nullptr, 0u, result);
                    break;

                case HistoricalOnlineBootstrapTask::QueryFade:
                    ok = nativeInvoker_.invoke_raw(
                        kNativeHudIsFading, nullptr, 0u, result);
                    historicalOnlineBootstrapTaskResult_.store(
                        static_cast<std::uint32_t>(result),
                        std::memory_order_release);
                    break;

                case HistoricalOnlineBootstrapTask::FinishLoadOnline: {
                    std::uintptr_t exitArgs[1]{};
                    exitArgs[0] =
                        reinterpret_cast<std::uintptr_t>("StartScreen1");
                    ok = nativeInvoker_.invoke_raw(
                        kNativeUiExit, exitArgs, 1u, result);

                    if (ok) {
                        std::uintptr_t args[2]{};
                        args[0] = reinterpret_cast<std::uintptr_t>(
                            "fileSetForMPLoad");
                        ok = nativeInvoker_.invoke_raw(
                            kNativeUiSendEvent, args, 1u, result);
                        if (ok) {
                            args[0] = reinterpret_cast<std::uintptr_t>(
                                "fileStartupChecksComplete");
                            ok = nativeInvoker_.invoke_raw(
                                kNativeUiSendEvent, args, 1u, result);
                        }
                        if (ok) {
                            args[0] = 0u;
                            args[1] =
                                reinterpret_cast<std::uintptr_t>("Online");
                            ok = nativeInvoker_.invoke_raw(
                                kNativeNetAuthenticateGamer,
                                args,
                                2u,
                                result);
                            historicalOnlineBootstrapAuthResult_.store(
                                result,
                                std::memory_order_release);
                        }
                    }
                    break;
                }

                case HistoricalOnlineBootstrapTask::QueryPlayerActor: {
                    std::uintptr_t args[1]{};
                    args[0] = static_cast<std::uintptr_t>(0xFFFFFFFFu);
                    ok = nativeInvoker_.invoke_raw(
                        kNativeGetPlayerActor, args, 1u, result);
                    historicalOnlineBootstrapTaskResult_.store(
                        static_cast<std::uint32_t>(result),
                        std::memory_order_release);
                    break;
                }

                case HistoricalOnlineBootstrapTask::None:
                    ok = false;
                    break;
                }

                historicalOnlineBootstrapTaskFailed_.store(
                    !ok, std::memory_order_release);
                historicalOnlineBootstrapTaskPending_.store(
                    false, std::memory_order_release);
                historicalOnlineBootstrapTaskDone_.store(
                    true, std::memory_order_release);
            },
            error);

        if (!submitted) {
            historicalOnlineBootstrapTaskPending_.store(
                false, std::memory_order_release);
            historicalOnlineBootstrapTaskFailed_.store(
                true, std::memory_order_release);
            historicalOnlineBootstrapTaskDone_.store(
                true, std::memory_order_release);
        }

        return submitted;
    };

    switch (historicalOnlineBootstrapStage_) {
    case HistoricalOnlineBootstrapStage::NotStarted: {
        ++historicalOnlineBootstrapAttempts_;
        std::string error;
        if (!submitTask(
                HistoricalOnlineBootstrapTask::SendStartScreenExit,
                error)) {
            historicalOnlineBootstrapStage_ =
                HistoricalOnlineBootstrapStage::Failed;
            logLine =
                "[FrontierSession] historical boot.sc "
                "startScreenExit queue failed: " + error;
            return false;
        }

        historicalOnlineBootstrapStage_ =
            HistoricalOnlineBootstrapStage::WaitingForStartScreenExit;
        logLine =
            "[FrontierSession] historical boot.sc "
            "startScreenExit queued";
        return false;
    }

    case HistoricalOnlineBootstrapStage::WaitingForStartScreenExit: {
        if (historicalOnlineBootstrapTaskPending_.load(
                std::memory_order_acquire)) {
            return false;
        }

        if (historicalOnlineBootstrapTaskDone_.exchange(
                false, std::memory_order_acq_rel)) {
            if (historicalOnlineBootstrapTaskFailed_.load(
                    std::memory_order_acquire)) {
                ++historicalOnlineBootstrapAttempts_;
                if (historicalOnlineBootstrapAttempts_ == 1 ||
                    (historicalOnlineBootstrapAttempts_ % 8u) == 0u) {
                    logLine =
                        "[FrontierSession] historical boot.sc "
                        "startScreenExit invoke failed; retrying";
                }
                std::string retryError;
                if (!submitTask(
                        HistoricalOnlineBootstrapTask::SendStartScreenExit,
                        retryError) &&
                    logLine.empty()) {
                    logLine =
                        "[FrontierSession] historical boot.sc "
                        "startScreenExit retry queue failed: " + retryError;
                }
                return false;
            }

            std::string error;
            if (!submitTask(
                    HistoricalOnlineBootstrapTask::SendEnterOnlineForInvite,
                    error)) {
                historicalOnlineBootstrapStage_ =
                    HistoricalOnlineBootstrapStage::Failed;
                logLine =
                    "[FrontierSession] historical boot.sc "
                    "net.EnterOnlineForInvite queue failed: " + error;
                return false;
            }

            historicalOnlineBootstrapStage_ =
                HistoricalOnlineBootstrapStage::WaitingForEnterOnlineForInvite;
            logLine =
                "[FrontierSession] historical boot.sc "
                "net.EnterOnlineForInvite queued after StartScreen1 exit";
            return false;
        }

        return false;
    }

    case HistoricalOnlineBootstrapStage::WaitingForEnterOnlineForInvite: {
        if (historicalOnlineBootstrapTaskPending_.load(
                std::memory_order_acquire)) {
            return false;
        }

        if (historicalOnlineBootstrapTaskDone_.exchange(
                false, std::memory_order_acq_rel)) {
            if (historicalOnlineBootstrapTaskFailed_.load(
                    std::memory_order_acquire)) {
                ++historicalOnlineBootstrapAttempts_;
                if (historicalOnlineBootstrapAttempts_ == 1 ||
                    (historicalOnlineBootstrapAttempts_ % 8u) == 0u) {
                    logLine =
                        "[FrontierSession] historical boot.sc "
                        "net.EnterOnlineForInvite invoke failed; retrying";
                }
                std::string retryError;
                if (!submitTask(
                        HistoricalOnlineBootstrapTask::SendEnterOnlineForInvite,
                        retryError) &&
                    logLine.empty()) {
                    logLine =
                        "[FrontierSession] historical boot.sc "
                        "net.EnterOnlineForInvite retry queue failed: " +
                        retryError;
                }
                return false;
            }

            std::string error;
            if (!submitTask(
                    HistoricalOnlineBootstrapTask::FadeToLoadingScreen,
                    error)) {
                historicalOnlineBootstrapStage_ =
                    HistoricalOnlineBootstrapStage::Failed;
                logLine =
                    "[FrontierSession] historical LoadOnline stage=1 "
                    "HUD_FADE_TO_LOADING_SCREEN queue failed: " + error;
                return false;
            }

            historicalOnlineBootstrapStage_ =
                HistoricalOnlineBootstrapStage::WaitingForFade;
            logLine =
                "[FrontierSession] historical boot.sc "
                "StartScreen2 online transition complete; "
                "HUD_FADE_TO_LOADING_SCREEN queued";
            return false;
        }

        return false;
    }

    case HistoricalOnlineBootstrapStage::WaitingForFade: {
        if (historicalOnlineBootstrapTaskPending_.load(
                std::memory_order_acquire)) {
            return false;
        }

        if (historicalOnlineBootstrapTaskDone_.exchange(
                false, std::memory_order_acq_rel)) {
            if (historicalOnlineBootstrapTaskFailed_.load(
                    std::memory_order_acquire)) {
                if (historicalOnlineBootstrapTask_ ==
                    HistoricalOnlineBootstrapTask::FadeToLoadingScreen) {
                    historicalOnlineBootstrapStage_ =
                        HistoricalOnlineBootstrapStage::Failed;
                    logLine =
                        "[FrontierSession] historical LoadOnline "
                        "HUD_FADE_TO_LOADING_SCREEN invoke failed";
                } else if (
                    historicalOnlineBootstrapTask_ ==
                    HistoricalOnlineBootstrapTask::FinishLoadOnline) {
                    if (historicalOnlineBootstrapAttempts_ == 1 ||
                        (historicalOnlineBootstrapAttempts_ % 8u) == 0u) {
                        logLine =
                            "[FrontierSession] historical LoadOnline "
                            "finish step invoke failed; retrying";
                    }
                    std::string retryError;
                    if (!submitTask(
                            HistoricalOnlineBootstrapTask::FinishLoadOnline,
                            retryError) &&
                        logLine.empty()) {
                        logLine =
                            "[FrontierSession] historical LoadOnline "
                            "finish retry queue failed: " + retryError;
                    }
                } else {
                    logLine =
                        "[FrontierSession] historical LoadOnline "
                        "HUD_IS_FADING invoke failed; retrying";
                }
                return false;
            }

            if (historicalOnlineBootstrapTask_ ==
                HistoricalOnlineBootstrapTask::FinishLoadOnline) {
                historicalOnlineBootstrapEventCompleted_ = true;
                historicalOnlineBootstrapStage_ =
                    HistoricalOnlineBootstrapStage::WaitingForPlayerActor;

                char message[256]{};
                std::snprintf(
                    message,
                    sizeof(message),
                    "[FrontierSession] historical LoadOnline stage=2 complete "
                    "StartScreen2 transition active, file events sent, "
                    "NET_AUTHENTICATE_GAMER result=0x%llX",
                    static_cast<unsigned long long>(
                        historicalOnlineBootstrapAuthResult_.load(
                            std::memory_order_acquire)));
                logLine = message;
                return false;
            }

            if (historicalOnlineBootstrapTask_ ==
                HistoricalOnlineBootstrapTask::QueryFade) {
                const auto fading =
                    historicalOnlineBootstrapTaskResult_.load(
                        std::memory_order_acquire);

                if (fading != 0u) {
                    ++historicalOnlineBootstrapAttempts_;
                    if (historicalOnlineBootstrapAttempts_ == 1 ||
                        (historicalOnlineBootstrapAttempts_ % 8u) == 0u) {
                        char message[256]{};
                        std::snprintf(
                            message,
                            sizeof(message),
                            "[FrontierSession] historical LoadOnline waiting-for-fade "
                            "HUD_IS_FADING=1 attempt=%u",
                            historicalOnlineBootstrapAttempts_);
                        logLine = message;
                    }
                    return false;
                }

                std::string error;
                if (!submitTask(
                        HistoricalOnlineBootstrapTask::FinishLoadOnline,
                        error)) {
                    historicalOnlineBootstrapStage_ =
                        HistoricalOnlineBootstrapStage::Failed;
                    logLine =
                        "[FrontierSession] historical LoadOnline stage=2 queue failed: " +
                        error;
                }
                return false;
            }
        }

        if (!historicalOnlineBootstrapTaskPending_.load(
                std::memory_order_acquire) &&
            historicalOnlineBootstrapTask_ ==
                HistoricalOnlineBootstrapTask::FadeToLoadingScreen) {
            std::string error;
            if (!submitTask(
                    HistoricalOnlineBootstrapTask::QueryFade,
                    error)) {
                logLine =
                    "[FrontierSession] historical LoadOnline HUD_IS_FADING "
                    "queue delayed: " + error;
            }
        } else if (!historicalOnlineBootstrapTaskPending_.load(
                       std::memory_order_acquire) &&
                   historicalOnlineBootstrapTask_ ==
                       HistoricalOnlineBootstrapTask::QueryFade) {
            const auto fading =
                historicalOnlineBootstrapTaskResult_.load(
                    std::memory_order_acquire);
            if (fading != 0u) {
                std::string error;
                if (!submitTask(
                        HistoricalOnlineBootstrapTask::QueryFade,
                        error)) {
                    if (historicalOnlineBootstrapAttempts_ == 1 ||
                        (historicalOnlineBootstrapAttempts_ % 8u) == 0u) {
                        logLine =
                            "[FrontierSession] historical LoadOnline HUD_IS_FADING "
                            "queue delayed: " + error;
                    }
                }
            }
        }

        return false;
    }

    case HistoricalOnlineBootstrapStage::WaitingForPlayerActor: {
        if (historicalOnlineBootstrapTaskPending_.load(
                std::memory_order_acquire)) {
            return false;
        }

        if (historicalOnlineBootstrapTaskDone_.exchange(
                false, std::memory_order_acq_rel)) {
            const auto completedTask = historicalOnlineBootstrapTask_;
            const bool failed =
                historicalOnlineBootstrapTaskFailed_.load(
                    std::memory_order_acquire);

            if (completedTask ==
                       HistoricalOnlineBootstrapTask::QueryPlayerActor) {
                if (failed) {
                    if (historicalOnlineBootstrapAttempts_ == 1 ||
                        (historicalOnlineBootstrapAttempts_ % 8u) == 0u) {
                        logLine =
                            "[FrontierSession] historical boot.sc "
                            "GET_PLAYER_ACTOR invoke failed; retrying";
                    }
                } else {
                    const auto actor =
                        historicalOnlineBootstrapTaskResult_.load(
                            std::memory_order_acquire);

                    if (actor != 0u) {
                        historicalOnlineBootstrapStage_ =
                            HistoricalOnlineBootstrapStage::Complete;

                        char message[256]{};
                        std::snprintf(
                            message,
                            sizeof(message),
                            "[FrontierSession] historical InitSpawn "
                            "player actor ready actor=0x%08X",
                            static_cast<unsigned>(actor));
                        logLine = message;
                        return true;
                    }
                }
            }
        }

        if (!historicalOnlineBootstrapEventCompleted_) {
            return false;
        }

        if (!historicalOnlineBootstrapTaskPending_.load(
                std::memory_order_acquire)) {
            ++historicalOnlineBootstrapAttempts_;
            std::string error;
            if (!submitTask(
                    HistoricalOnlineBootstrapTask::QueryPlayerActor,
                    error) &&
                (historicalOnlineBootstrapAttempts_ == 1 ||
                 (historicalOnlineBootstrapAttempts_ % 8u) == 0u)) {
                logLine =
                    "[FrontierSession] historical boot.sc "
                    "GET_PLAYER_ACTOR queue delayed: " + error;
            }
        }

        return false;
    }

    case HistoricalOnlineBootstrapStage::Complete:
        return true;

    case HistoricalOnlineBootstrapStage::Failed:
        return false;
    }

    return false;
}

bool RdrBridge::read_game_runtime(std::int32_t& gameState, bool& worldLoaded, bool& worldLoadedKnown,
                                   bool& simulateStartMultiplayer, bool& simulateStartMultiplayerKnown,
                                   bool& startPosCommandLine, bool& startPosCommandLineKnown,
                                   std::string& error) const {
    gameState = -1;
    worldLoaded = false;
    worldLoadedKnown = false;
    simulateStartMultiplayer = false;
    simulateStartMultiplayerKnown = false;
    startPosCommandLine = false;
    startPosCommandLineKnown = false;
    error.clear();

    if (!nativeInvoker_.ready()) {
        error = "native invoker not ready";
        return false;
    }
    if (!gameThreadDispatcher_.attached()) {
        error = gameThreadDispatcherError_.empty()
            ? "game-thread dispatcher not attached"
            : gameThreadDispatcherError_;
        return false;
    }

    bool enqueueRefresh = false;
    RuntimeSnapshot snapshot{};
    {
        std::lock_guard lock(runtimeSnapshotMutex_);
        snapshot = runtimeSnapshot_;
        if (!runtimeRefreshPending_) {
            runtimeRefreshPending_ = true;
            enqueueRefresh = true;
        }
    }

    if (enqueueRefresh) {
        std::string dispatchError;
        const bool submitted = gameThreadDispatcher_.submit(
            [this]() {
                RuntimeSnapshot refreshed{};
                std::uint32_t value = 0;

                if (nativeInvoker_.invoke_u32(kNativeGetGameState, value)) {
                    refreshed.gameStateKnown = true;
                    refreshed.gameState = static_cast<std::int32_t>(value);
                }
                if (nativeInvoker_.invoke_u32(kNativeStreamingIsWorldLoaded, value)) {
                    refreshed.worldLoadedKnown = true;
                    refreshed.worldLoaded = value != 0;
                }
                if (nativeInvoker_.invoke_u32(kNativeIsSimulateStartMultiplayer, value)) {
                    refreshed.simulateStartMultiplayerKnown = true;
                    refreshed.simulateStartMultiplayer = value != 0;
                }
                if (nativeInvoker_.invoke_u32(kNativeIsStartPosInCommandLine, value)) {
                    refreshed.startPosCommandLineKnown = true;
                    refreshed.startPosCommandLine = value != 0;
                }

                std::lock_guard lock(runtimeSnapshotMutex_);
                if (refreshed.gameStateKnown) {
                    runtimeSnapshot_.gameStateKnown = true;
                    runtimeSnapshot_.gameState = refreshed.gameState;
                }
                if (refreshed.worldLoadedKnown) {
                    runtimeSnapshot_.worldLoadedKnown = true;
                    runtimeSnapshot_.worldLoaded = refreshed.worldLoaded;
                }
                if (refreshed.simulateStartMultiplayerKnown) {
                    runtimeSnapshot_.simulateStartMultiplayerKnown = true;
                    runtimeSnapshot_.simulateStartMultiplayer = refreshed.simulateStartMultiplayer;
                }
                if (refreshed.startPosCommandLineKnown) {
                    runtimeSnapshot_.startPosCommandLineKnown = true;
                    runtimeSnapshot_.startPosCommandLine = refreshed.startPosCommandLine;
                }
                runtimeRefreshPending_ = false;
            },
            dispatchError);

        if (!submitted) {
            std::lock_guard lock(runtimeSnapshotMutex_);
            runtimeRefreshPending_ = false;
            error = dispatchError.empty() ? "game-thread runtime refresh enqueue failed" : dispatchError;
        }
    }

    {
        std::lock_guard lock(runtimeSnapshotMutex_);
        snapshot = runtimeSnapshot_;
    }

    gameState = snapshot.gameState;
    worldLoaded = snapshot.worldLoaded;
    worldLoadedKnown = snapshot.worldLoadedKnown;
    simulateStartMultiplayer = snapshot.simulateStartMultiplayer;
    simulateStartMultiplayerKnown = snapshot.simulateStartMultiplayerKnown;
    startPosCommandLine = snapshot.startPosCommandLine;
    startPosCommandLineKnown = snapshot.startPosCommandLineKnown;

    if (!snapshot.gameStateKnown || !snapshot.worldLoadedKnown ||
        !snapshot.simulateStartMultiplayerKnown || !snapshot.startPosCommandLineKnown) {
        if (error.empty()) error = "runtime snapshot pending";
        return false;
    }

    return true;
}

namespace {

std::uintptr_t float_bits(float value) {
    std::uintptr_t bits = 0;
    static_assert(sizeof(float) == sizeof(std::uint32_t));
    std::uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    bits = raw;
    return bits;
}

float float_from_bits(std::uintptr_t value) {
    std::uint32_t raw = static_cast<std::uint32_t>(value);
    float out = 0.0f;
    std::memcpy(&out, &raw, sizeof(out));
    return out;
}


std::uintptr_t pack_vec2(float x, float y) {
    return float_bits(x) | (float_bits(y) << 32u);
}

} // namespace

bool RdrBridge::request_remote_actor_test(const PlayerState& origin, std::string& error) const {
    error.clear();
    if (!initialized_) {
        error = "bridge not initialized";
        return false;
    }
    if (!nativeInvoker_.ready()) {
        error = "native invoker not ready";
        return false;
    }
    if (!gameThreadDispatcher_.attached()) {
        error = gameThreadDispatcherError_.empty()
            ? "game-thread dispatcher not attached"
            : gameThreadDispatcherError_;
        return false;
    }

    {
        std::lock_guard lock(remoteActorTestMutex_);
        if (remoteActorTestSpawned_ || remoteActorTestPending_) return true;
        remoteActorTestPending_ = true;
    }

    const PlayerState spawnOrigin = origin;
    std::string dispatchError;
    const bool submitted = gameThreadDispatcher_.submit(
        [this, spawnOrigin]() {
            std::uint32_t layoutId = 0u;
            char layoutName[] = "FrontierRemoteLayout";
            char actorName[] = "FrontierRemoteMPActor";

            std::uintptr_t args[7]{};
            std::uintptr_t result = 0u;

            // Use a dedicated generic-Actor layout, separate from PlayerLayout.
            args[0] = reinterpret_cast<std::uintptr_t>(layoutName);

            if (nativeInvoker_.invoke_raw(kNativeFindNamedLayout, args, 1u, result)) {
                layoutId = static_cast<std::uint32_t>(result);
            }

            bool layoutValid = false;
            if (layoutId != 0u) {
                args[0] = static_cast<std::uintptr_t>(layoutId);
                result = 0u;
                layoutValid = nativeInvoker_.invoke_raw(
                    kNativeIsLayoutRefValid, args, 1u, result) && result != 0u;
            }

            if (!layoutValid) {
                args[0] = reinterpret_cast<std::uintptr_t>(layoutName);
                result = 0u;
                if (nativeInvoker_.invoke_raw(kNativeCreateLayout, args, 1u, result)) {
                    layoutId = static_cast<std::uint32_t>(result);
                    args[0] = static_cast<std::uintptr_t>(layoutId);
                    result = 0u;
                    layoutValid = nativeInvoker_.invoke_raw(
                        kNativeIsLayoutRefValid, args, 1u, result) && result != 0u;
                }
            }

            if (!layoutValid) {
                std::lock_guard lock(remoteActorTestMutex_);
                remoteActorTestPending_ = false;
                std::fprintf(stderr,
                             "[FrontierRemotePlayer] generic actor test layout invalid "
                             "name=%s id=0x%08X\n",
                             layoutName, layoutId);
                return;
            }

            const float x = spawnOrigin.position.x + 2.0f;
            const float y = spawnOrigin.position.y;
            const float z = spawnOrigin.position.z;

            // Request ACTOR_MPPLAYER01 and then create it as a normal Actor.
            args[0] = static_cast<std::uintptr_t>(kRemoteTestActorEnum);
            args[1] = 1u;
            args[2] = 0u;
            std::uintptr_t streamResult = 0u;
            const bool streamOk = nativeInvoker_.invoke_raw(
                kNativeStreamingRequestActor, args, 3u, streamResult);

            args[0] = kTaggedLayoutRef | static_cast<std::uintptr_t>(layoutId);
            args[1] = reinterpret_cast<std::uintptr_t>(actorName);
            args[2] = static_cast<std::uintptr_t>(
                static_cast<std::uint32_t>(kRemoteTestActorEnum));
            args[3] = pack_vec2(x, y);
            args[4] = float_bits(z);
            args[5] = pack_vec2(0.0f, 0.0f);
            args[6] = float_bits(0.0f);

            result = 0u;
            const bool createOk = nativeInvoker_.invoke_raw(
                kNativeCreateActorInLayout, args, 7u, result);

            // For the generic Actor native, result is the ActorRef. This is the same
            // return convention already observed for normal game-created Actors.
            const std::uintptr_t actorRef = result;
            const std::uint32_t actorHandle = static_cast<std::uint32_t>(actorRef);

            std::uintptr_t actorArgs[1]{
                static_cast<std::uintptr_t>(actorHandle)};
            std::uintptr_t validResult = 0u;
            const bool validOk =
                actorHandle != 0u &&
                nativeInvoker_.invoke_raw(kNativeIsActorValid, actorArgs, 1u, validResult);

            actorArgs[0] = static_cast<std::uintptr_t>(actorHandle);
            std::uintptr_t enumResult = 0u;
            const bool enumOk =
                actorHandle != 0u &&
                nativeInvoker_.invoke_raw(kNativeGetActorEnum, actorArgs, 1u, enumResult);

            actorArgs[0] = static_cast<std::uintptr_t>(actorHandle);
            std::uintptr_t isPlayerResult = 0u;
            const bool isPlayerOk =
                actorHandle != 0u &&
                nativeInvoker_.invoke_raw(kNativeIsActorPlayer, actorArgs, 1u, isPlayerResult);

            std::uint32_t actorSlot = 0xFFFFFFFFu;
            bool actorSlotOk = false;
            bool localPlayerActor = false;
            bool localPlayerActorOk = false;
            std::uint32_t updatePriority = 0xFFFFFFFFu;
            bool updatePriorityOk = false;

            if (actorHandle != 0u) {
                std::uintptr_t diagnosticArgs[1]{
                    static_cast<std::uintptr_t>(actorHandle)};
                std::uintptr_t diagnosticResult = 0u;

                actorSlotOk = nativeInvoker_.invoke_raw(
                    kNativeGetActorSlot, diagnosticArgs, 1u, diagnosticResult);
                if (actorSlotOk) {
                    actorSlot = static_cast<std::uint32_t>(diagnosticResult);
                }

                diagnosticResult = 0u;
                localPlayerActorOk = nativeInvoker_.invoke_raw(
                    kNativeIsActorLocalPlayer, diagnosticArgs, 1u, diagnosticResult);
                if (localPlayerActorOk) {
                    localPlayerActor = diagnosticResult != 0u;
                }

                diagnosticResult = 0u;
                updatePriorityOk = nativeInvoker_.invoke_raw(
                    kNativeGetActorUpdatePriority, diagnosticArgs, 1u, diagnosticResult);
                if (updatePriorityOk) {
                    updatePriority = static_cast<std::uint32_t>(diagnosticResult);
                }
            }

            const bool spawned =
                createOk && actorHandle != 0u && validOk && validResult != 0u;

            {
                std::lock_guard lock(remoteActorTestMutex_);
                remoteActorTestLayout_ = layoutId;
                remoteActorTestActorRef_ = actorRef;
                remoteActorTestSpawned_ = spawned;
                remoteActorTestPending_ = false;
            }

            char buffer[900]{};
            std::snprintf(
                buffer, sizeof(buffer),
                "[FrontierRemotePlayer] generic actor test layout=0x%08X "
                "streamOk=%u streamResult=0x%llX createOk=%u "
                "actorRef=0x%llX actorHandle=0x%08X actorValid=%u "
                "actorEnum=%u isActorPlayer=%u actorSlot=%s%u "
                "isLocalPlayer=%s%u updatePriority=%s%u "
                "position=(%.3f,%.3f,%.3f)",
                layoutId,
                streamOk ? 1u : 0u,
                static_cast<unsigned long long>(streamResult),
                createOk ? 1u : 0u,
                static_cast<unsigned long long>(actorRef),
                actorHandle,
                validOk ? static_cast<unsigned>(validResult) : 0u,
                enumOk ? static_cast<unsigned>(enumResult) : 0u,
                isPlayerOk ? static_cast<unsigned>(isPlayerResult) : 0u,
                actorSlotOk ? "" : "<unreadable>",
                actorSlotOk ? actorSlot : 0u,
                localPlayerActorOk ? "" : "<unreadable>",
                localPlayerActorOk ? static_cast<unsigned>(localPlayerActor) : 0u,
                updatePriorityOk ? "" : "<unreadable>",
                updatePriorityOk ? updatePriority : 0u,
                x, y, z);
            write_bridge_log_line(buffer);
        },
        dispatchError);

    if (!submitted) {
        std::lock_guard lock(remoteActorTestMutex_);
        remoteActorTestPending_ = false;
        error = dispatchError.empty() ? "remote actor test enqueue failed" : dispatchError;
        return false;
    }

    return true;
}


bool RdrBridge::spawn_remote_actor(
    std::uint16_t playerId,
    const PlayerState& state,
    RemoteActorHandle& outActor,
    std::string& error) const {
    outActor = {};
    error.clear();

    if (!initialized_) {
        error = "bridge not initialized";
        return false;
    }
    if (!nativeInvoker_.ready()) {
        error = "native invoker not ready";
        return false;
    }
    if (!gameThreadDispatcher_.attached()) {
        error = gameThreadDispatcherError_.empty()
            ? "game-thread dispatcher not attached"
            : gameThreadDispatcherError_;
        return false;
    }

    const PlayerState spawnState = state;
    const std::uint16_t remotePlayerId = playerId;
    std::string dispatchError;
    const bool completed = gameThreadDispatcher_.submit_and_wait(
        [this, remotePlayerId, spawnState, &outActor, &error]() {
            char ambientLayoutName[] = "AmbientMissions_Layout";
            char fallbackLayoutName[] = "FrontierRemoteLayout";

            char actorName[64]{};
            std::snprintf(actorName, sizeof(actorName), "FrontierRemote_%u",
                          static_cast<unsigned>(remotePlayerId));

            // The Lua RDRMP resource validates that the actor enum exists before
            // requesting its asset, so mirror that sequence here.
            std::uintptr_t enumArgs[1]{
                static_cast<std::uintptr_t>(kRemoteTestActorEnum)};
            std::uintptr_t enumInstalledResult = 0u;
            if (!nativeInvoker_.invoke_raw(
                    kNativeIsActorenumInstalled, enumArgs, 1u, enumInstalledResult)) {
                error = "IS_ACTORENUM_INSTALLED invoke failed";
                return;
            }
            if (enumInstalledResult == 0u) {
                char message[192]{};
                std::snprintf(
                    message, sizeof(message),
                    "remote actor enum %u is not installed",
                    static_cast<unsigned>(kRemoteTestActorEnum));
                error = message;
                return;
            }

            // Match the proven RDRMP resource flow: request the actor first, then
            // only create it once the streamed asset reports loaded.
            std::uintptr_t streamArgs[3]{};
            streamArgs[0] = static_cast<std::uintptr_t>(kRemoteTestActorEnum);
            streamArgs[1] = 1u;
            streamArgs[2] = 0u;
            std::uintptr_t streamResult = 0u;
            if (!nativeInvoker_.invoke_raw(
                    kNativeStreamingRequestActor, streamArgs, 3u, streamResult)) {
                error = "STREAMING_REQUEST_ACTOR invoke failed";
                return;
            }

            std::uintptr_t loadedArgs[2]{};
            loadedArgs[0] = static_cast<std::uintptr_t>(kRemoteTestActorEnum);
            loadedArgs[1] =
                static_cast<std::uintptr_t>(static_cast<std::intptr_t>(-1));
            std::uintptr_t loadedResult = 0u;
            if (!nativeInvoker_.invoke_raw(
                    kNativeStreamingIsActorLoaded, loadedArgs, 2u, loadedResult)) {
                error = "STREAMING_IS_ACTOR_LOADED invoke failed";
                return;
            }
            if (loadedResult == 0u) {
                error = "remote actor asset still loading";
                return;
            }

            std::uint32_t layoutId = remoteActorLayout_;
            bool layoutValid = false;

            if (layoutId != 0u) {
                std::uintptr_t layoutArgs[1]{
                    static_cast<std::uintptr_t>(layoutId)};
                std::uintptr_t layoutResult = 0u;
                layoutValid = nativeInvoker_.invoke_raw(
                    kNativeIsLayoutRefValid, layoutArgs, 1u, layoutResult) &&
                    layoutResult != 0u;
            }

            if (!layoutValid) {
                // The Lua resource uses get_ambient_layout(); its trace resolves
                // that layout as AmbientMissions_Layout. Prefer the same proven
                // layout before falling back to a dedicated layout.
                std::uintptr_t args[1]{
                    reinterpret_cast<std::uintptr_t>(ambientLayoutName)};
                std::uintptr_t result = 0u;
                if (nativeInvoker_.invoke_raw(
                        kNativeFindNamedLayout, args, 1u, result)) {
                    layoutId = static_cast<std::uint32_t>(result);
                    if (layoutId != 0u) {
                        args[0] = static_cast<std::uintptr_t>(layoutId);
                        result = 0u;
                        layoutValid = nativeInvoker_.invoke_raw(
                            kNativeIsLayoutRefValid, args, 1u, result) &&
                            result != 0u;
                    }
                }
            }

            if (!layoutValid) {
                std::uintptr_t args[1]{
                    reinterpret_cast<std::uintptr_t>(fallbackLayoutName)};
                std::uintptr_t result = 0u;
                if (!nativeInvoker_.invoke_raw(
                        kNativeFindNamedLayout, args, 1u, result)) {
                    error = "FIND_NAMED_LAYOUT invoke failed";
                    return;
                }

                layoutId = static_cast<std::uint32_t>(result);
                if (layoutId != 0u) {
                    args[0] = static_cast<std::uintptr_t>(layoutId);
                    result = 0u;
                    layoutValid = nativeInvoker_.invoke_raw(
                        kNativeIsLayoutRefValid, args, 1u, result) &&
                        result != 0u;
                }
            }

            if (!layoutValid) {
                std::uintptr_t args[1]{
                    reinterpret_cast<std::uintptr_t>(fallbackLayoutName)};
                std::uintptr_t result = 0u;
                if (!nativeInvoker_.invoke_raw(
                        kNativeCreateLayout, args, 1u, result)) {
                    error = "CREATE_LAYOUT invoke failed";
                    return;
                }

                layoutId = static_cast<std::uint32_t>(result);
                if (layoutId != 0u) {
                    args[0] = static_cast<std::uintptr_t>(layoutId);
                    result = 0u;
                    layoutValid = nativeInvoker_.invoke_raw(
                        kNativeIsLayoutRefValid, args, 1u, result) &&
                        result != 0u;
                }
            }

            if (!layoutValid) {
                error = "actor layout is invalid";
                return;
            }

            remoteActorLayout_ = layoutId;

            std::uintptr_t args[7]{};
            args[0] = kTaggedLayoutRef | static_cast<std::uintptr_t>(layoutId);
            args[1] = reinterpret_cast<std::uintptr_t>(actorName);
            args[2] = static_cast<std::uintptr_t>(
                static_cast<std::uint32_t>(kRemoteTestActorEnum));
            args[3] = pack_vec2(spawnState.position.x, spawnState.position.y);
            args[4] = float_bits(spawnState.position.z);

            // CREATE_ACTOR_IN_LAYOUT receives rotation as Vector3. The native
            // trace for the Lua resource shows vector3(0, heading, 0) packed
            // as args[5]=pack_vec2(heading, 0), args[6]=0.
            args[5] = pack_vec2(spawnState.yaw, 0.0f);
            args[6] = float_bits(0.0f);

            std::uintptr_t result = 0u;
            if (!nativeInvoker_.invoke_raw(
                    kNativeCreateActorInLayout, args, 7u, result)) {
                error = "CREATE_ACTOR_IN_LAYOUT invoke failed";
                return;
            }

            const std::uintptr_t actorRef = result;
            const std::uint32_t actorHandle =
                static_cast<std::uint32_t>(actorRef);
            if (actorHandle == 0u) {
                error = "CREATE_ACTOR_IN_LAYOUT returned null ActorRef";
                return;
            }

            std::uintptr_t actorArgs[1]{
                static_cast<std::uintptr_t>(actorHandle)};
            std::uintptr_t validResult = 0u;
            if (!nativeInvoker_.invoke_raw(
                    kNativeIsActorValid, actorArgs, 1u, validResult) ||
                validResult == 0u) {
                error = "created Actor failed IS_ACTOR_VALID";
                return;
            }

            actorArgs[0] = static_cast<std::uintptr_t>(actorHandle);
            std::uintptr_t enumResult = 0u;
            if (!nativeInvoker_.invoke_raw(
                    kNativeGetActorEnum, actorArgs, 1u, enumResult) ||
                static_cast<std::uint32_t>(enumResult) !=
                    static_cast<std::uint32_t>(kRemoteTestActorEnum)) {
                char message[192]{};
                std::snprintf(
                    message, sizeof(message),
                    "remote Actor enum mismatch expected=%u actual=%u",
                    static_cast<unsigned>(kRemoteTestActorEnum),
                    static_cast<unsigned>(enumResult));
                error = message;
                return;
            }

            outActor.actorRef = actorRef;
            outActor.actorHandle = actorHandle;
        },
        500u,
        dispatchError);

    if (!completed) {
        error = dispatchError.empty()
            ? "remote actor spawn task did not complete"
            : dispatchError;
        return false;
    }
    return error.empty();
}

bool RdrBridge::update_remote_actor_transform(
    std::uint32_t actorHandle,
    const PlayerState& state,
    std::string& error) const {
    error.clear();

    if (actorHandle == 0u) {
        error = "invalid remote actor handle";
        return false;
    }
    if (!initialized_) {
        error = "bridge not initialized";
        return false;
    }
    if (!nativeInvoker_.ready()) {
        error = "native invoker not ready";
        return false;
    }
    if (!gameThreadDispatcher_.attached()) {
        error = gameThreadDispatcherError_.empty()
            ? "game-thread dispatcher not attached"
            : gameThreadDispatcherError_;
        return false;
    }

    const PlayerState target = state;
    std::string dispatchError;
    const bool completed = gameThreadDispatcher_.submit_and_wait(
        [this, actorHandle, target, &error]() {
            std::uintptr_t args[7]{};
            args[0] = static_cast<std::uintptr_t>(actorHandle);
            args[1] = pack_vec2(target.position.x, target.position.y);
            args[2] = float_bits(target.position.z);
            args[3] = float_bits(target.yaw);
            args[4] = 0u;
            args[5] = 0u;
            args[6] = 0u;

            std::uintptr_t result = 0u;
            if (!nativeInvoker_.invoke_raw(
                    kNativeTeleportActorWithHeading, args, 7u, result)) {
                error = "TELEPORT_ACTOR_WITH_HEADING invoke failed";
            }
        },
        250u,
        dispatchError);

    if (!completed) {
        error = dispatchError.empty()
            ? "remote actor transform task did not complete"
            : dispatchError;
        return false;
    }
    return error.empty();
}

bool RdrBridge::is_remote_actor_valid(
    std::uint32_t actorHandle,
    bool& outValid,
    std::string& error) const {
    outValid = false;
    error.clear();

    if (actorHandle == 0u) {
        error = "invalid remote actor handle";
        return false;
    }
    if (!initialized_) {
        error = "bridge not initialized";
        return false;
    }
    if (!nativeInvoker_.ready()) {
        error = "native invoker not ready";
        return false;
    }
    if (!gameThreadDispatcher_.attached()) {
        error = gameThreadDispatcherError_.empty()
            ? "game-thread dispatcher not attached"
            : gameThreadDispatcherError_;
        return false;
    }

    std::string dispatchError;
    const bool completed = gameThreadDispatcher_.submit_and_wait(
        [this, actorHandle, &outValid, &error]() {
            std::uintptr_t args[1]{};
            args[0] = static_cast<std::uintptr_t>(actorHandle);
            std::uintptr_t result = 0u;
            if (!nativeInvoker_.invoke_raw(kNativeIsActorValid, args, 1u, result)) {
                error = "IS_ACTOR_VALID invoke failed";
                return;
            }
            outValid = result != 0u;
        },
        250u,
        dispatchError);

    if (!completed) {
        error = dispatchError.empty()
            ? "remote actor validity task did not complete"
            : dispatchError;
        return false;
    }
    return error.empty();
}

bool RdrBridge::read_remote_actor_position(
    std::uint32_t actorHandle,
    Vec3& outPosition,
    std::string& error) const {
    outPosition = {};
    error.clear();

    if (actorHandle == 0u) {
        error = "invalid remote actor handle";
        return false;
    }
    if (!initialized_) {
        error = "bridge not initialized";
        return false;
    }
    if (!nativeInvoker_.ready()) {
        error = "native invoker not ready";
        return false;
    }
    if (!gameThreadDispatcher_.attached()) {
        error = gameThreadDispatcherError_.empty()
            ? "game-thread dispatcher not attached"
            : gameThreadDispatcherError_;
        return false;
    }

    Vec3 position{};
    std::string dispatchError;
    const bool completed = gameThreadDispatcher_.submit_and_wait(
        [this, actorHandle, &position, &error]() {
            // GET_POSITION(Actor, float*, float*, float*).
            std::uintptr_t args[4]{};
            args[0] = static_cast<std::uintptr_t>(actorHandle);
            args[1] = reinterpret_cast<std::uintptr_t>(&position.x);
            args[2] = reinterpret_cast<std::uintptr_t>(&position.y);
            args[3] = reinterpret_cast<std::uintptr_t>(&position.z);
            std::uintptr_t result = 0u;
            if (!nativeInvoker_.invoke_raw(kNativeGetPosition, args, 4u, result)) {
                error = "GET_POSITION invoke failed";
            }
        },
        250u,
        dispatchError);

    if (!completed) {
        error = dispatchError.empty()
            ? "remote actor position task did not complete"
            : dispatchError;
        return false;
    }
    if (!error.empty()) return false;

    const auto finite = [](float value) { return std::isfinite(value); };
    if (!finite(position.x) || !finite(position.y) || !finite(position.z)) {
        error = "GET_POSITION returned non-finite coordinates";
        return false;
    }

    outPosition = position;
    return true;
}

bool RdrBridge::prepare_remote_actor_for_locomotion(
    std::uint32_t actorHandle,
    std::string& error) const {
    error.clear();

    if (actorHandle == 0u) {
        error = "invalid remote actor handle";
        return false;
    }
    if (!initialized_) {
        error = "bridge not initialized";
        return false;
    }
    if (!nativeInvoker_.ready()) {
        error = "native invoker not ready";
        return false;
    }
    if (!gameThreadDispatcher_.attached()) {
        error = gameThreadDispatcherError_.empty()
            ? "game-thread dispatcher not attached"
            : gameThreadDispatcherError_;
        return false;
    }

    std::string dispatchError;
    const bool completed = gameThreadDispatcher_.submit_and_wait(
        [this, actorHandle, &error]() {
            std::uintptr_t result = 0u;

            // One-time locomotion prerequisites for this Actor. These must not
            // be repeated for every TASK_GO_TO_COORD retarget.
            std::uintptr_t moverArgs[2]{};
            moverArgs[0] = static_cast<std::uintptr_t>(actorHandle);
            moverArgs[1] = 0u; // false
            if (!nativeInvoker_.invoke_raw(
                    kNativeEnableMover, moverArgs, 1u, result)) {
                error = "ENABLE_MOVER invoke failed";
                return;
            }

            if (!nativeInvoker_.invoke_raw(
                    kNativeSetMoverFrozen, moverArgs, 2u, result)) {
                error = "SET_MOVER_FROZEN(false) invoke failed";
                return;
            }

            std::uintptr_t actorReadyArgs[2]{};
            actorReadyArgs[0] = static_cast<std::uintptr_t>(actorHandle);
            actorReadyArgs[1] = 1u; // true
            if (!nativeInvoker_.invoke_raw(
                    kNativeMakeActorReadyForAction, actorReadyArgs, 2u, result)) {
                error = "MAKE_ACTOR_READY_FOR_ACTION(true) invoke failed";
                return;
            }

            std::uintptr_t streamingArgs[2]{};
            streamingArgs[0] = static_cast<std::uintptr_t>(actorHandle);
            streamingArgs[1] = 1u; // true
            if (!nativeInvoker_.invoke_raw(
                    kNativeSetActorStreamingHighPriority, streamingArgs, 2u, result)) {
                error = "SET_ACTOR_STREAMING_HIGH_PRIORITY(true) invoke failed";
                return;
            }

            std::uintptr_t forceUpdateArgs[1]{
                static_cast<std::uintptr_t>(actorHandle)};
            if (!nativeInvoker_.invoke_raw(
                    kNativeActorForceNextUpdate, forceUpdateArgs, 1u, result)) {
                error = "ACTOR_FORCE_NEXT_UPDATE invoke failed";
                return;
            }

            std::uintptr_t frozenCheckArgs[1]{
                static_cast<std::uintptr_t>(actorHandle)};
            std::uintptr_t frozenResult = 0u;
            const bool frozenReadOk = nativeInvoker_.invoke_raw(
                kNativeIsMoverFrozen, frozenCheckArgs, 1u, frozenResult);

            char message[360]{};
            std::snprintf(
                message, sizeof(message),
                "[FrontierRemoteTask] prepare actor=0x%08X moverFrozen=%s%u",
                actorHandle,
                frozenReadOk ? "" : "<unreadable>",
                frozenReadOk ? static_cast<unsigned>(frozenResult != 0u) : 0u);
            write_bridge_log_line(message);
        },
        500u,
        dispatchError);

    if (!completed) {
        error = dispatchError.empty()
            ? "remote actor locomotion preparation did not complete"
            : dispatchError;
        return false;
    }
    return error.empty();
}

bool RdrBridge::task_go_to_remote_coord(
    std::uint32_t actorHandle,
    const Vec3& target,
    std::string& error) const {
    error.clear();

    if (actorHandle == 0u) {
        error = "invalid remote actor handle";
        return false;
    }
    if (!initialized_) {
        error = "bridge not initialized";
        return false;
    }
    if (!nativeInvoker_.ready()) {
        error = "native invoker not ready";
        return false;
    }
    if (!gameThreadDispatcher_.attached()) {
        error = gameThreadDispatcherError_.empty()
            ? "game-thread dispatcher not attached"
            : gameThreadDispatcherError_;
        return false;
    }

    const Vec3 destination = target;
    std::string dispatchError;
    const bool completed = gameThreadDispatcher_.submit_and_wait(
        [this, actorHandle, destination, &error]() {
            std::uintptr_t result = 0u;

            // G.R.E-Lab's RDR1 native declaration identifies the ABI as:
            // TASK_GO_TO_COORD(Actor, const Vector3*, MoveType).
            // The Vector3 is therefore passed by pointer, not packed into native
            // argument slots as XY/Z scalar values.
            const Vec3 taskPosition = destination;
            std::uintptr_t taskArgs[3]{};
            taskArgs[0] = static_cast<std::uintptr_t>(actorHandle);
            taskArgs[1] = reinterpret_cast<std::uintptr_t>(&taskPosition);

            // MoveType is not yet resolved to a named enum in this build.
            // Keep the conservative zero value for the first runtime probe.
            taskArgs[2] = 0u;

            if (!nativeInvoker_.invoke_raw(
                    kNativeTaskGoToCoord, taskArgs, 3u, result)) {
                error = "TASK_GO_TO_COORD invoke failed";
                return;
            }

            char message[360]{};
            std::snprintf(
                message, sizeof(message),
                "[FrontierRemoteTask] go-to actor=0x%08X target=(%.3f,%.3f,%.3f)",
                actorHandle,
                destination.x,
                destination.y,
                destination.z);
            write_bridge_log_line(message);
        },
        500u,
        dispatchError);

    if (!completed) {
        error = dispatchError.empty()
            ? "remote actor go-to task did not complete"
            : dispatchError;
        return false;
    }
    return error.empty();
}

bool RdrBridge::destroy_remote_actor(
    std::uint32_t actorHandle,
    std::string& error) const {
    error.clear();

    if (actorHandle == 0u) return true;
    if (!initialized_) {
        error = "bridge not initialized";
        return false;
    }
    if (!nativeInvoker_.ready()) {
        error = "native invoker not ready";
        return false;
    }
    if (!gameThreadDispatcher_.attached()) {
        error = gameThreadDispatcherError_.empty()
            ? "game-thread dispatcher not attached"
            : gameThreadDispatcherError_;
        return false;
    }

    std::string dispatchError;
    const bool completed = gameThreadDispatcher_.submit_and_wait(
        [this, actorHandle, &error]() {
            std::uintptr_t args[1]{
                static_cast<std::uintptr_t>(actorHandle)};
            std::uintptr_t result = 0u;
            if (!nativeInvoker_.invoke_raw(
                    kNativeDestroyActor, args, 1u, result)) {
                error = "DESTROY_ACTOR invoke failed";
            }
        },
        250u,
        dispatchError);

    if (!completed) {
        error = dispatchError.empty()
            ? "remote actor destroy task did not complete"
            : dispatchError;
        return false;
    }
    return error.empty();
}

bool RdrBridge::send_ui_event(const std::string& eventName, std::string& error) const {
    error.clear();

    if (eventName.empty()) {
        error = "UI event name is empty";
        return false;
    }
    if (!initialized_) {
        error = "bridge not initialized";
        return false;
    }
    if (!nativeInvoker_.ready()) {
        error = "native invoker not ready";
        return false;
    }
    if (!gameThreadDispatcher_.attached()) {
        error = gameThreadDispatcherError_.empty()
            ? "game-thread dispatcher not attached"
            : gameThreadDispatcherError_;
        return false;
    }

    const std::string event = eventName;
    std::string dispatchError;
    const bool completed = gameThreadDispatcher_.submit_and_wait(
        [this, &event, &error]() {
            std::uintptr_t args[1]{};
            args[0] = reinterpret_cast<std::uintptr_t>(event.c_str());
            std::uintptr_t result = 0u;
            if (!nativeInvoker_.invoke_raw(kNativeUiSendEvent, args, 1u, result)) {
                error = "UI_SEND_EVENT invoke failed";
            }
        },
        500u,
        dispatchError);

    if (!completed) {
        error = dispatchError.empty()
            ? "UI_SEND_EVENT task did not complete"
            : dispatchError;
        return false;
    }
    return error.empty();
}

bool RdrBridge::read_local_player_state(PlayerState& outState, std::string& error) const {
    outState = {};
    error.clear();
    if (!initialized_) {
        error = "bridge not initialized";
        return false;
    }

#ifdef _WIN32
    std::uintptr_t localPlayer = 0;
    std::uintptr_t managerSlots = 0;
    if (!read_pointer(localPlayerStorage_, localPlayer)) {
        char buffer[192]{};
        std::snprintf(buffer, sizeof(buffer),
                      "local player pointer unavailable storage=0x%llX",
                      static_cast<unsigned long long>(localPlayerStorage_));
        error = buffer;
        return false;
    }
    if (!read_pointer(actorManagerSlotsStorage_, managerSlots)) {
        char buffer[192]{};
        std::snprintf(buffer, sizeof(buffer),
                      "actor manager slots unavailable storage=0x%llX",
                      static_cast<unsigned long long>(actorManagerSlotsStorage_));
        error = buffer;
        return false;
    }

    if (!readable(localPlayer, sizeof(MinimalSagPlayer))) {
        char buffer[224]{};
        std::snprintf(buffer, sizeof(buffer),
                      "local player object unreadable ptr=0x%llX",
                      static_cast<unsigned long long>(localPlayer));
        error = buffer;
        return false;
    }

    std::uint32_t guid = 0;
    if (!guarded_read_u32(localPlayer + offsetof(MinimalSagPlayer, guid), guid)) {
        char buffer[224]{};
        std::snprintf(buffer, sizeof(buffer),
                      "local player GUID read failed ptr=0x%llX",
                      static_cast<unsigned long long>(localPlayer));
        error = buffer;
        return false;
    }

    if (guid == 0) {
        char buffer[288]{};
        std::snprintf(buffer, sizeof(buffer),
                      "local player identity pending local=0x%llX manager=0x%llX guid=0x00000000",
                      static_cast<unsigned long long>(localPlayer),
                      static_cast<unsigned long long>(managerSlots));
        error = buffer;
        return false;
    }

    const auto actorSlotAddress = managerSlots +
        (static_cast<std::uintptr_t>(static_cast<std::uint16_t>(guid)) * 0x10u);
    std::uintptr_t actor = 0;
    if (!read_pointer(actorSlotAddress, actor)) {
        char buffer[320]{};
        std::snprintf(buffer, sizeof(buffer),
                      "local actor pointer unavailable manager=0x%llX guid=0x%08X slot=0x%llX",
                      static_cast<unsigned long long>(managerSlots),
                      guid,
                      static_cast<unsigned long long>(actorSlotAddress));
        error = buffer;
        return false;
    }
    if (!readable(actor, sizeof(MinimalSagActor))) {
        char buffer[240]{};
        std::snprintf(buffer, sizeof(buffer),
                      "local actor unreadable ptr=0x%llX guid=0x%08X slot=0x%llX",
                      static_cast<unsigned long long>(actor),
                      guid,
                      static_cast<unsigned long long>(actorSlotAddress));
        error = buffer;
        return false;
    }

    std::uintptr_t actorComponent = 0;
    const bool actorComponentRead =
        guarded_read_pointer(actor + offsetof(MinimalSagActor, actorComponent), actorComponent);
    if (!actorComponentRead) {
        char buffer[240]{};
        std::snprintf(buffer, sizeof(buffer),
                      "actor component read failed actor=0x%llX guid=0x%08X",
                      static_cast<unsigned long long>(actor),
                      guid);
        error = buffer;
        return false;
    }
    if (actorComponent == 0) {
        char buffer[384]{};
        std::snprintf(buffer, sizeof(buffer),
                      "actor component pending ptr=0x0 local=0x%llX manager=0x%llX guid=0x%08X slot=0x%llX actor=0x%llX",
                      static_cast<unsigned long long>(localPlayer),
                      static_cast<unsigned long long>(managerSlots),
                      guid,
                      static_cast<unsigned long long>(actorSlotAddress),
                      static_cast<unsigned long long>(actor));
        error = buffer;
        return false;
    }
    if (!readable(actorComponent, sizeof(MinimalSagActorComponent))) {
        char buffer[320]{};
        std::snprintf(buffer, sizeof(buffer),
                      "actor component unreadable ptr=0x%llX local=0x%llX manager=0x%llX guid=0x%08X actor=0x%llX",
                      static_cast<unsigned long long>(actorComponent),
                      static_cast<unsigned long long>(localPlayer),
                      static_cast<unsigned long long>(managerSlots),
                      guid,
                      static_cast<unsigned long long>(actor));
        error = buffer;
        return false;
    }

    std::uintptr_t transform = 0;
    const bool transformRead =
        guarded_read_pointer(actorComponent + offsetof(MinimalSagActorComponent, transform), transform);
    if (!transformRead) {
        char buffer[256]{};
        std::snprintf(buffer, sizeof(buffer),
                      "transform pointer read failed component=0x%llX actor=0x%llX guid=0x%08X",
                      static_cast<unsigned long long>(actorComponent),
                      static_cast<unsigned long long>(actor),
                      guid);
        error = buffer;
        return false;
    }
    if (transform == 0) {
        char buffer[352]{};
        std::snprintf(buffer, sizeof(buffer),
                      "transform pending ptr=0x0 component=0x%llX actor=0x%llX local=0x%llX guid=0x%08X",
                      static_cast<unsigned long long>(actorComponent),
                      static_cast<unsigned long long>(actor),
                      static_cast<unsigned long long>(localPlayer),
                      guid);
        error = buffer;
        return false;
    }
    if (!readable(transform, sizeof(MinimalMatrix34))) {
        char buffer[320]{};
        std::snprintf(buffer, sizeof(buffer),
                      "transform unreadable ptr=0x%llX component=0x%llX actor=0x%llX local=0x%llX guid=0x%08X",
                      static_cast<unsigned long long>(transform),
                      static_cast<unsigned long long>(actorComponent),
                      static_cast<unsigned long long>(actor),
                      static_cast<unsigned long long>(localPlayer),
                      guid);
        error = buffer;
        return false;
    }

    if (!guarded_read_position(transform, outState.position)) {
        error = "position read failed";
        return false;
    }

    {
        char buffer[448]{};
        std::snprintf(buffer, sizeof(buffer),
                      "local=0x%llX manager=0x%llX guid=0x%08X slot=0x%llX actor=0x%llX component=0x%llX transform=0x%llX",
                      static_cast<unsigned long long>(localPlayer),
                      static_cast<unsigned long long>(managerSlots),
                      guid,
                      static_cast<unsigned long long>(actorSlotAddress),
                      static_cast<unsigned long long>(actor),
                      static_cast<unsigned long long>(actorComponent),
                      static_cast<unsigned long long>(transform));
        std::lock_guard lock(localPlayerDiagnosticMutex_);
        localPlayerChainDiagnostic_ = buffer;
    }
    return true;
#else
    error = "RDR bridge is Windows-only";
    return false;
#endif
}

} // namespace frontier::game
