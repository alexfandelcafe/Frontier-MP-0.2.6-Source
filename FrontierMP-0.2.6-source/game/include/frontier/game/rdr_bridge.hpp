#pragma once

#include "frontier/game/build_fingerprint.hpp"
#include "frontier/game/native_invoker.hpp"
#include "frontier/game/game_thread_dispatcher.hpp"
#include "frontier/game/startup_native_tracer.hpp"
#include "frontier/game/content_path_redirector.hpp"
#include "frontier/types.hpp"

#include <cstdint>
#include <mutex>
#include <string>

namespace frontier::game {

struct RemoteActorHandle final {
    std::uintptr_t actorRef{};
    std::uint32_t actorHandle{};

    bool valid() const { return actorHandle != 0; }
};

class RdrBridge final {
public:
    bool initialize(const ExecutableFingerprint& fingerprint, KnownBuild build);
    bool read_local_player_state(PlayerState& outState, std::string& error) const;
    bool request_remote_actor_test(const PlayerState& origin, std::string& error) const;
    bool send_ui_event(const std::string& eventName, std::string& error) const;
    bool advance_historical_online_bootstrap(std::string& logLine);
    bool spawn_remote_actor(std::uint16_t playerId, const PlayerState& state,
                            RemoteActorHandle& outActor, std::string& error) const;
    bool update_remote_actor_transform(std::uint32_t actorHandle, const PlayerState& state,
                                       std::string& error) const;
    bool is_remote_actor_valid(std::uint32_t actorHandle, bool& outValid, std::string& error) const;
    bool read_remote_actor_position(std::uint32_t actorHandle, Vec3& outPosition, std::string& error) const;
    bool prepare_remote_actor_for_locomotion(std::uint32_t actorHandle, std::string& error) const;
    bool task_go_to_remote_coord(std::uint32_t actorHandle, const Vec3& target, std::string& error) const;
    bool destroy_remote_actor(std::uint32_t actorHandle, std::string& error) const;
    std::string local_player_chain_diagnostic() const;
    bool local_player_pointer_available() const;
    bool try_initialize_native_invoker();
    bool try_initialize_game_thread_dispatcher();
    bool game_thread_dispatcher_attached() const { return gameThreadDispatcher_.attached(); }
    const std::string& game_thread_dispatcher_error() const { return gameThreadDispatcherError_; }
    bool startup_native_tracer_attached() const { return startupNativeTracer_.attached(); }
    bool native_invoker_ready() const { return nativeInvoker_.ready(); }
    const std::string& native_invoker_error() const { return nativeInvoker_.last_error(); }
    bool read_game_runtime(std::int32_t& gameState, bool& worldLoaded, bool& worldLoadedKnown,
                           bool& simulateStartMultiplayer, bool& simulateStartMultiplayerKnown,
                           bool& startPosCommandLine, bool& startPosCommandLineKnown, std::string& error) const;

    bool initialized() const { return initialized_; }
    bool local_player_symbol_resolved() const { return localPlayerStorage_ != 0; }
    bool actor_manager_symbol_resolved() const { return actorManagerSlotsStorage_ != 0; }

private:
    std::uintptr_t resolve_rip_target(std::uintptr_t instruction) const;
    bool readable(std::uintptr_t address, std::size_t size) const;
    bool read_pointer(std::uintptr_t address, std::uintptr_t& out) const;

    struct RuntimeSnapshot final {
        bool gameStateKnown{};
        std::int32_t gameState{-1};
        bool worldLoadedKnown{};
        bool worldLoaded{};
        bool simulateStartMultiplayerKnown{};
        bool simulateStartMultiplayer{};
        bool startPosCommandLineKnown{};
        bool startPosCommandLine{};
    };

    bool initialized_{};
    KnownBuild build_{KnownBuild::Unknown};
    std::uintptr_t moduleBase_{};
    std::uintptr_t localPlayerStorage_{};
    std::uintptr_t actorManagerSlotsStorage_{};
    NativeInvoker nativeInvoker_{};
    mutable GameThreadDispatcher gameThreadDispatcher_{};
    std::string gameThreadDispatcherError_;
    StartupNativeTracer startupNativeTracer_{};
    ContentPathRedirector contentPathRedirector_{};
    enum class HistoricalOnlineBootstrapStage : std::uint8_t {
        NotStarted,
        WaitingForFade,
        WaitingForPlayerActor,
        Complete,
        Failed
    };

    HistoricalOnlineBootstrapStage historicalOnlineBootstrapStage_{
        HistoricalOnlineBootstrapStage::NotStarted};
    std::uint32_t historicalOnlineBootstrapAttempts_{};
    mutable std::mutex runtimeSnapshotMutex_;
    mutable RuntimeSnapshot runtimeSnapshot_{};
    mutable bool runtimeRefreshPending_{};
    mutable std::mutex localPlayerDiagnosticMutex_;
    mutable std::string localPlayerChainDiagnostic_;
    mutable std::mutex remoteActorTestMutex_;
    mutable bool remoteActorTestPending_{};
    mutable bool remoteActorTestSpawned_{};
    mutable std::uint32_t remoteActorTestLayout_{};
    mutable std::uintptr_t remoteActorTestActorRef_{};
    mutable std::uint32_t remoteActorLayout_{};
};

} // namespace frontier::game
