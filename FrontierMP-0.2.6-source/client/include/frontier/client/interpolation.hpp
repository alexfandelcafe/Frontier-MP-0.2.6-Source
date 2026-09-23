#pragma once

#include "frontier/types.hpp"
#include "frontier/protocol.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>

namespace frontier::client {

struct InterpolatedState final {
    PlayerState state{};
    bool extrapolated{};
};

class RemoteEntityInterpolator final {
public:
    explicit RemoteEntityInterpolator(std::uint32_t interpolationDelayTicks = 2, std::uint32_t maxExtrapolationTicks = 4);

    void push_snapshot(const protocol::Snapshot& snapshot);
    std::optional<InterpolatedState> sample(std::uint16_t playerId, std::uint32_t renderTick) const;
    void remove_player(std::uint16_t playerId);
    void clear();

private:
    struct Sample final {
        std::uint32_t tick{};
        PlayerState state{};
    };

    std::unordered_map<std::uint16_t, std::deque<Sample>> buffers_;
    std::uint32_t interpolationDelayTicks_{};
    std::uint32_t maxExtrapolationTicks_{};
};

} // namespace frontier::client
