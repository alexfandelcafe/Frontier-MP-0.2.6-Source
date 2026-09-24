#include "frontier/client/remote_player_manager.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

namespace frontier::client {

namespace {
constexpr std::uint64_t kRemotePlayerGraceMs = 750;
constexpr float kPositionUpdateThreshold = 0.02f;
constexpr float kLocomotionTargetUpdateThreshold = 0.30f;
constexpr std::uint64_t kLocomotionTaskRetryMs = 150;

float distance_squared(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

float yaw_delta(float a, float b) {
    constexpr float kFullTurnDegrees = 360.0f;
    constexpr float kHalfTurnDegrees = 180.0f;
    float delta = std::fmod(a - b, kFullTurnDegrees);
    if (delta > kHalfTurnDegrees) delta -= kFullTurnDegrees;
    if (delta < -kHalfTurnDegrees) delta += kFullTurnDegrees;
    return std::fabs(delta);
}
} // namespace

void RemotePlayerManager::set_local_player_id(std::uint16_t playerId) {
    localPlayerId_ = playerId;
}

void RemotePlayerManager::on_snapshot(const protocol::Snapshot& snapshot, std::uint64_t nowMs) {
    latestServerTick_ = snapshot.serverTick;
    interpolator_.push_snapshot(snapshot);

    for (const auto& state : snapshot.players) {
        if (state.playerId == localPlayerId_ || state.playerId == 0) continue;

        auto [it, inserted] = players_.try_emplace(state.playerId);
        auto& player = it->second;
        if (inserted) {
            player.playerId = state.playerId;
            player.spawnPending = false;
            player.actor = {};
        }

        player.targetState = state;
        player.lastServerTick = snapshot.serverTick;
        player.lastSeenMs = nowMs;
    }
}

bool RemotePlayerManager::ensure_spawned(RemotePlayer& player, std::uint64_t nowMs,
                                         frontier::game::RdrBridge& bridge) {
    constexpr std::uint64_t kSpawnRetryMs = 250;
    if (player.actor.valid()) return true;
    if (player.spawnPending) return false;
    if (player.lastSpawnAttemptMs != 0 &&
        nowMs - player.lastSpawnAttemptMs < kSpawnRetryMs) {
        return false;
    }

    player.lastSpawnAttemptMs = nowMs;
    player.spawnPending = true;

    frontier::game::RemoteActorHandle created{};
    std::string error;
    const bool ok = bridge.spawn_remote_actor(player.playerId, player.targetState, created, error);

    player.spawnPending = false;

    if (!ok) {
        std::fprintf(stderr,
                     "[FrontierRemotePlayer] spawn failed playerId=%u error=%s\n",
                     static_cast<unsigned>(player.playerId),
                     error.c_str());
        return false;
    }

    player.actor = created;
    player.renderedState = player.targetState;

    // The synthetic 65000 entity is the controlled Actor-locomotion probe.
    // Its target is supplied by the network snapshot path; it must not follow
    // the local player or be overwritten by a network teleport.
    if (player.playerId == 65000u) {
        player.locomotionTaskActive = false;
        player.lastLocomotionTaskAttemptMs = 0;
    }

    std::fprintf(stderr,
                 "[FrontierRemotePlayer] spawned playerId=%u actorRef=0x%llX actorHandle=0x%08X position=(%.3f,%.3f,%.3f)\n",
                 static_cast<unsigned>(player.playerId),
                 static_cast<unsigned long long>(player.actor.actorRef),
                 player.actor.actorHandle,
                 player.renderedState.position.x,
                 player.renderedState.position.y,
                 player.renderedState.position.z);
    return true;
}

void RemotePlayerManager::update(std::uint64_t nowMs, frontier::game::RdrBridge& bridge) {
    const std::uint64_t effectiveNow = nowMs < lastUpdateMs_ ? lastUpdateMs_ : nowMs;

    for (auto it = players_.begin(); it != players_.end();) {
        auto& player = it->second;

        if (effectiveNow - player.lastSeenMs > kRemotePlayerGraceMs) {
            remove_player(it++, bridge);
            continue;
        }

        if (!ensure_spawned(player, effectiveNow, bridge)) {
            ++it;
            continue;
        }

        const auto sampled = interpolator_.sample(player.playerId, latestServerTick_);
        const PlayerState desired = sampled.has_value() ? sampled->state : player.targetState;

        // The synthetic locomotion probe is driven by the sampled network target.
        // Keep the native TASK_GO_TO_COORD task as the movement owner and only
        // retarget it when the destination has moved far enough.
        if (player.playerId == 65000u) {
            const bool firstTask = !player.locomotionTaskActive;
            const bool targetMoved =
                distance_squared(player.renderedState.position, desired.position) >
                kLocomotionTargetUpdateThreshold * kLocomotionTargetUpdateThreshold;
            const bool retryAllowed =
                player.lastLocomotionTaskAttemptMs == 0 ||
                effectiveNow - player.lastLocomotionTaskAttemptMs >= kLocomotionTaskRetryMs;

            if ((firstTask || targetMoved) && retryAllowed) {
                player.lastLocomotionTaskAttemptMs = effectiveNow;

                std::string taskError;
                if (bridge.task_go_to_remote_coord(
                        player.actor.actorHandle, desired.position, taskError)) {
                    player.locomotionTaskActive = true;
                    // renderedState is the last commanded network target for the
                    // locomotion path; the Actor itself remains engine-controlled.
                    player.renderedState = desired;

                    std::fprintf(stderr,
                                 "[FrontierRemotePlayer] locomotion target playerId=%u "
                                 "actor=0x%08X target=(%.3f,%.3f,%.3f)\n",
                                 static_cast<unsigned>(player.playerId),
                                 player.actor.actorHandle,
                                 desired.position.x,
                                 desired.position.y,
                                 desired.position.z);
                } else {
                    player.locomotionTaskActive = false;
                    std::fprintf(stderr,
                                 "[FrontierRemotePlayer] locomotion target deferred "
                                 "playerId=%u actor=0x%08X error=%s\n",
                                 static_cast<unsigned>(player.playerId),
                                 player.actor.actorHandle,
                                 taskError.c_str());
                }
            }

            ++it;
            continue;
        }

        const bool moved = distance_squared(player.renderedState.position, desired.position) >
                           kPositionUpdateThreshold * kPositionUpdateThreshold;
        const bool rotated = yaw_delta(player.renderedState.yaw, desired.yaw) > 1.0f;

        if (moved || rotated) {
            std::string error;
            if (bridge.update_remote_actor_transform(player.actor.actorHandle, desired, error)) {
                player.renderedState = desired;
            } else {
                std::fprintf(stderr,
                             "[FrontierRemotePlayer] update failed playerId=%u actor=0x%08X error=%s\n",
                             static_cast<unsigned>(player.playerId),
                             player.actor.actorHandle,
                             error.c_str());
            }
        }

        ++it;
    }

    lastUpdateMs_ = effectiveNow;
}

void RemotePlayerManager::remove_player(
    std::unordered_map<std::uint16_t, RemotePlayer>::iterator it,
    frontier::game::RdrBridge& bridge) {
    RemotePlayer player = it->second;
    if (player.actor.valid()) {
        std::string error;
        if (!bridge.destroy_remote_actor(player.actor.actorHandle, error)) {
            std::fprintf(stderr,
                         "[FrontierRemotePlayer] destroy failed playerId=%u actor=0x%08X error=%s\n",
                         static_cast<unsigned>(player.playerId),
                         player.actor.actorHandle,
                         error.c_str());
        }
    }

    interpolator_.remove_player(player.playerId);
    players_.erase(it);

    std::fprintf(stderr,
                 "[FrontierRemotePlayer] removed playerId=%u\n",
                 static_cast<unsigned>(player.playerId));
}

void RemotePlayerManager::clear(frontier::game::RdrBridge& bridge) {
    for (auto it = players_.begin(); it != players_.end();) {
        auto current = it++;
        remove_player(current, bridge);
    }
    interpolator_.clear();
    latestServerTick_ = 0;
    lastUpdateMs_ = 0;
}

} // namespace frontier::client
