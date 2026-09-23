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
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cmath>

namespace frontier::game {

namespace {

constexpr const char* kLocalPlayerPattern =
    "48 89 15 ? ? ? ? E9 ? ? ? ?";

constexpr std::uint32_t kNativeGetGameState = 0xDD9BD22B;
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
constexpr std::uint32_t kNativeIsActorValid = 0xBA6C3E92u;
constexpr std::uint32_t kNativeIsActorenumInstalled = 0x9B903F45;
constexpr std::uint32_t kNativeGetActorEnum = 0x0B28E9EC;
constexpr std::uint32_t kNativeStreamingRequestActor = 0xB0A79FEE;
constexpr std::uint32_t kNativeStreamingIsActorLoaded = 0x7DF72579;
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
                "actorEnum=%u isActorPlayer=%u position=(%.3f,%.3f,%.3f)",
                layoutId,
                streamOk ? 1u : 0u,
                static_cast<unsigned long long>(streamResult),
                createOk ? 1u : 0u,
                static_cast<unsigned long long>(actorRef),
                actorHandle,
                validOk ? static_cast<unsigned>(validResult) : 0u,
                enumOk ? static_cast<unsigned>(enumResult) : 0u,
                isPlayerOk ? static_cast<unsigned>(isPlayerResult) : 0u,
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
            constexpr const char* kAmbientLayoutName = "AmbientMissions_Layout";
            constexpr const char* kFallbackLayoutName = "FrontierRemoteLayout";

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
