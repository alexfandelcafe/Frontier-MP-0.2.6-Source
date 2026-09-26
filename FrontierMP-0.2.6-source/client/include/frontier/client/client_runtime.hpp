#pragma once

#include <cstdint>
#include <string>
#include <atomic>
#include "frontier/game/rdr_bridge.hpp"
#include "frontier/game/frontier_session.hpp"
#include "frontier/client/remote_player_manager.hpp"

namespace frontier::client {

class ClientRuntime final {
public:
    bool initialize(const std::string& host, std::uint16_t port, const std::string& playerName);
    bool initialize_from_process_command_line();
    void run_loop();
    void shutdown();
    void update();
    bool connected() const { return connected_; }

private:
    bool connected_{};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> updateInProgress_{false};
    frontier::game::RdrBridge gameBridge_{};
    std::uint32_t clientTick_{};
    std::uint64_t lastStateSendMs_{};
    std::uint64_t lastBridgeLogMs_{};
    bool bridgeStateReady_{};
    frontier::game::FrontierSession session_{};
    std::uint64_t lastSessionUpdateMs_{};
    std::uint64_t lastFrontendBootstrapAttemptMs_{};
    bool historicalOnlineBootstrapLogged_{};
    bool nativeUiBootstrapEnabled_{};
    std::string sessionMode_{"freeroam"};
    RemotePlayerManager remotePlayers_{};
    std::uint16_t localPlayerId_{};
    frontier::SpawnPoint localSpawnPoint_{};
    std::uint32_t localPlayerActorModel_{frontier::kDefaultPlayerActorModel};
    std::uint64_t lastLocalPlayerSpawnAttemptMs_{};
    bool localPlayerSpawnReady_{};
    bool debugRemoteTestEnabled_{};
};

} // namespace frontier::client
