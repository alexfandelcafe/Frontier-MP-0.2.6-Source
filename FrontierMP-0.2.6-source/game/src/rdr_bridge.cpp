#include "frontier/game/rdr_bridge.hpp"

#include "frontier/game/pattern_scanner.hpp"
#include "frontier/game/game_thread_dispatcher.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <cstring>
#include <sstream>
#include <memory>

namespace frontier::game {

namespace {

constexpr const char* kLocalPlayerPattern =
    "48 89 15 ? ? ? ? E9 ? ? ? ?";

constexpr std::uint32_t kNativeGetGameState = 0xDD9BD22B;
constexpr std::uint32_t kNativeStreamingIsWorldLoaded = 0x87B74064;
constexpr std::uint32_t kNativeIsSimulateStartMultiplayer = 0x9A73C2CD;
constexpr std::uint32_t kNativeIsStartPosInCommandLine = 0x814D97E8;

constexpr const char* kActorManagerPattern =
    "48 8B 05 ? ? ? ? 0F B7 CA 48 03 C9 C1 EA 10 66 39 54 C8 ? 75 03 B0 01 C3 32 C0 C3 CC 48 89 5C 24 ?";

struct MinimalSagPlayer final {
    std::byte padding0[0x5EC];
    std::uint32_t guid;
};

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

bool RdrBridge::local_player_pointer_available() const {
#ifdef _WIN32
    if (!initialized_ || !localPlayerStorage_) return false;
    std::uintptr_t localPlayer = 0;
    return read_pointer(localPlayerStorage_, localPlayer);
#else
    return false;
#endif
}

bool RdrBridge::initialize(const ExecutableFingerprint& fingerprint, KnownBuild build) {
    initialized_ = false;
    build_ = build;
    localPlayerStorage_ = 0;
    actorManagerSlotsStorage_ = 0;

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
        if (!startupNativeTracer_.attach(nativeInvoker_, traceError)) {
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

    struct RuntimeNativeResult final {
        bool gameStateOk{};
        std::uint32_t gameState{};
        bool worldLoadedOk{};
        std::uint32_t worldLoaded{};
        bool simulateMpOk{};
        std::uint32_t simulateMp{};
        bool startPosOk{};
        std::uint32_t startPos{};
    };

    auto result = std::make_shared<RuntimeNativeResult>();
    std::string dispatchError;
    bool dispatched = gameThreadDispatcher_.submit_and_wait(
        [this, result]() {
            result->gameStateOk = nativeInvoker_.invoke_u32(kNativeGetGameState, result->gameState);
            result->worldLoadedOk = nativeInvoker_.invoke_u32(kNativeStreamingIsWorldLoaded, result->worldLoaded);
            result->simulateMpOk = nativeInvoker_.invoke_u32(kNativeIsSimulateStartMultiplayer, result->simulateMp);
            result->startPosOk = nativeInvoker_.invoke_u32(kNativeIsStartPosInCommandLine, result->startPos);
        },
        1000,
        dispatchError);

    if (!dispatched) {
        error = dispatchError.empty() ? "game-thread native dispatch failed" : dispatchError;
        return false;
    }

    if (!result->gameStateOk) {
        error += "GET_GAME_STATE invoke failed; ";
    } else {
        gameState = static_cast<std::int32_t>(result->gameState);
    }

    worldLoadedKnown = result->worldLoadedOk;
    if (result->worldLoadedOk) {
        worldLoaded = result->worldLoaded != 0;
    } else {
        error += "STREAMING_IS_WORLD_LOADED invoke failed; ";
    }

    simulateStartMultiplayerKnown = result->simulateMpOk;
    if (result->simulateMpOk) {
        simulateStartMultiplayer = result->simulateMp != 0;
    } else {
        error += "IS_SIMULATE_START_MULTIPLAYER invoke failed; ";
    }

    startPosCommandLineKnown = result->startPosOk;
    if (result->startPosOk) {
        startPosCommandLine = result->startPos != 0;
    } else {
        error += "IS_STARTPOS_IN_COMMANDLINE invoke failed; ";
    }

    return result->gameStateOk && result->worldLoadedOk &&
           result->simulateMpOk && result->startPosOk;
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
        error = "local player pointer unavailable";
        return false;
    }
    if (!read_pointer(actorManagerSlotsStorage_, managerSlots)) {
        error = "actor manager slots unavailable";
        return false;
    }

    if (!readable(localPlayer, sizeof(MinimalSagPlayer))) {
        error = "local player object unreadable";
        return false;
    }

    std::uint32_t guid = 0;
    __try {
        guid = reinterpret_cast<const MinimalSagPlayer*>(localPlayer)->guid;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        error = "local player GUID read failed";
        return false;
    }

    const auto actorSlotAddress = managerSlots + (static_cast<std::uintptr_t>(static_cast<std::uint16_t>(guid)) * 0x10u);
    std::uintptr_t actor = 0;
    if (!read_pointer(actorSlotAddress, actor)) {
        error = "local actor pointer unavailable";
        return false;
    }
    if (!readable(actor, sizeof(MinimalSagActor))) {
        error = "local actor unreadable";
        return false;
    }

    std::uintptr_t actorComponent = 0;
    __try {
        actorComponent = reinterpret_cast<const MinimalSagActor*>(actor)->actorComponent;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        error = "actor component read failed";
        return false;
    }
    if (!readable(actorComponent, sizeof(MinimalSagActorComponent))) {
        error = "actor component unreadable";
        return false;
    }

    std::uintptr_t transform = 0;
    __try {
        transform = reinterpret_cast<const MinimalSagActorComponent*>(actorComponent)->transform;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        error = "transform pointer read failed";
        return false;
    }
    if (!readable(transform, sizeof(MinimalMatrix34))) {
        error = "transform unreadable";
        return false;
    }

    __try {
        outState.position = reinterpret_cast<const MinimalMatrix34*>(transform)->position;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        error = "position read failed";
        return false;
    }
    return true;
#else
    error = "RDR bridge is Windows-only";
    return false;
#endif
}

} // namespace frontier::game
