#include "frontier/client/interpolation.hpp"

#include <algorithm>

namespace frontier::client {

namespace {
float lerp(float a, float b, float t) { return a + (b - a) * t; }
Vec3 lerp(const Vec3& a, const Vec3& b, float t) { return {lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t)}; }
}

RemoteEntityInterpolator::RemoteEntityInterpolator(std::uint32_t interpolationDelayTicks, std::uint32_t maxExtrapolationTicks)
    : interpolationDelayTicks_(interpolationDelayTicks), maxExtrapolationTicks_(maxExtrapolationTicks) {}

void RemoteEntityInterpolator::push_snapshot(const protocol::Snapshot& snapshot) {
    for (const auto& player : snapshot.players) {
        auto& buffer = buffers_[player.playerId];
        if (!buffer.empty() && snapshot.serverTick <= buffer.back().tick) continue;
        buffer.push_back({snapshot.serverTick, player});
        while (buffer.size() > 12) buffer.pop_front();
    }
}

std::optional<InterpolatedState> RemoteEntityInterpolator::sample(std::uint16_t playerId, std::uint32_t renderTick) const {
    const auto it = buffers_.find(playerId);
    if (it == buffers_.end() || it->second.empty()) return std::nullopt;

    const auto& buffer = it->second;
    const std::uint32_t targetTick = renderTick > interpolationDelayTicks_ ? renderTick - interpolationDelayTicks_ : 0;

    if (buffer.size() == 1) return InterpolatedState{buffer.front().state, false};

    for (std::size_t i = 0; i + 1 < buffer.size(); ++i) {
        const auto& a = buffer[i];
        const auto& b = buffer[i + 1];
        if (targetTick < a.tick || targetTick > b.tick) continue;
        const float span = static_cast<float>(b.tick - a.tick);
        const float t = span > 0.0f ? static_cast<float>(targetTick - a.tick) / span : 0.0f;
        PlayerState result = a.state;
        result.position = lerp(a.state.position, b.state.position, t);
        result.velocity = lerp(a.state.velocity, b.state.velocity, t);
        result.yaw = lerp(a.state.yaw, b.state.yaw, t);
        return InterpolatedState{result, false};
    }

    const auto& latest = buffer.back();
    if (targetTick > latest.tick) {
        const std::uint32_t ahead = targetTick - latest.tick;
        if (ahead <= maxExtrapolationTicks_) {
            PlayerState result = latest.state;
            const float seconds = static_cast<float>(ahead) / static_cast<float>(kServerTickRate);
            result.position.x += result.velocity.x * seconds;
            result.position.y += result.velocity.y * seconds;
            result.position.z += result.velocity.z * seconds;
            return InterpolatedState{result, true};
        }
    }

    return InterpolatedState{buffer.front().state, false};
}

void RemoteEntityInterpolator::clear() { buffers_.clear(); }

} // namespace frontier::client
