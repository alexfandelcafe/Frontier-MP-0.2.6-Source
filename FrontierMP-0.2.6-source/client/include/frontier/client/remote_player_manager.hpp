#pragma once

#include "frontier/client/interpolation.hpp"
#include "frontier/protocol.hpp"
#include "frontier/game/rdr_bridge.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

namespace frontier::client {

struct RemotePlayer final {
    std::uint16_t playerId{};
    frontier::game::RemoteActorHandle actor{};
    PlayerState targetState{};
    PlayerState renderedState{};
    std::uint32_t lastServerTick{};
    std::uint64_t lastSeenMs{};
    bool spawnPending{};
    std::uint64_t lastSpawnAttemptMs{};
    bool locomotionTaskActive{};
};

class RemotePlayerManager final {
public:
    void set_local_player_id(std::uint16_t playerId);

    void on_snapshot(const protocol::Snapshot& snapshot, std::uint64_t nowMs);

    void update(std::uint64_t nowMs, frontier::game::RdrBridge& bridge);

    void clear(frontier::game::RdrBridge& bridge);

    std::size_t size() const { return players_.size(); }

private:
    bool ensure_spawned(RemotePlayer& player, std::uint64_t nowMs,
                        frontier::game::RdrBridge& bridge);
    void remove_player(std::unordered_map<std::uint16_t, RemotePlayer>::iterator it,
                       frontier::game::RdrBridge& bridge);

    std::uint16_t localPlayerId_{};
    std::uint32_t latestServerTick_{};
    std::uint64_t lastUpdateMs_{};
    std::unordered_map<std::uint16_t, RemotePlayer> players_;
    RemoteEntityInterpolator interpolator_{2, 4};
};

} // namespace frontier::client
