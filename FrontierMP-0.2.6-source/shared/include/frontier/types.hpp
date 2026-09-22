#pragma once

#include <cstdint>
#include <string>

namespace frontier {

struct Vec3 final {
    float x{};
    float y{};
    float z{};
};

struct PlayerState final {
    std::uint16_t playerId{};
    std::uint32_t clientTick{};
    Vec3 position{};
    float yaw{};
    Vec3 velocity{};
    std::uint8_t gait{};
    std::uint8_t flags{};
};

struct SpawnPoint final {
    Vec3 position{};
    float yaw{};
};

inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr std::uint16_t kMaxMessageSize = 4096;
inline constexpr std::uint16_t kServerTickRate = 20;
inline constexpr float kDefaultInterestRadiusMeters = 400.0f;

} // namespace frontier
