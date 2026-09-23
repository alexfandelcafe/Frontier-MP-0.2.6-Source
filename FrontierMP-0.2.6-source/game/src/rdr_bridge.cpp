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
#endif

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

std::string RdrBridge::local_player_chain_diagnostic() const {
    std::lock_guard lock(localPlayerDiagnosticMutex_);
    return localPlayerChainDiagnostic_;
}

bool RdrBridge::initialize(const ExecutableFingerprint& fingerprint, KnownBuild build) {
    initialized_ = false;
    build_ = build;
    localPlayerStorage_ = 0;
    actorManagerSlotsStorage_ = 0;
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
