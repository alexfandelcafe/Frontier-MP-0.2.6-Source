#pragma once

#include "frontier/types.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace frontier::rdrmp {

inline constexpr std::size_t kMaxStringLength = 0x0FFF;

enum class DeliveryType : std::uint8_t {
    Reliable = 0,
    Unreliable = 1,
};

enum class PacketId : std::uint16_t {
    ClientWelcome = 0,
    ClientEvent = 1,
    ClientPlayerState = 2,
    ServerEvent = 3,
    TimeOfDay = 5,
    PlayerRemove = 6,
    PlayerCreate = 7,
    PlayerProperty = 8,
    PlayerTransform = 9,
    TextChat = 10,
};

struct PlayerCreate final {
    std::uint16_t playerId{};
    std::uint32_t actorModel{};
    std::string name;
    Vec3 position{};
    Vec3 rotation{};
};

struct PlayerRemove final {
    std::uint16_t playerId{};
};

struct PlayerProperty final {
    std::uint16_t playerId{};
    std::uint32_t value{};
};

struct ClientPlayerState final {
    Vec3 position{};
    Vec3 rotation{};
};

struct PlayerTransform final {
    std::uint16_t playerId{};
    Vec3 position{};
    Vec3 rotation{};
};

struct ClientWelcome final {
    std::uint32_t actorModel{};
    std::string name;
    Vec3 position{};
    Vec3 rotation{};
};

using EventValue = std::variant<
    std::uint64_t,
    double,
    bool,
    std::string,
    Vec3>;

std::uint64_t fnv1a64(std::string_view value);

class Writer final {
public:
    void u8(std::uint8_t value);
    bool u16(std::uint16_t value);
    bool u32(std::uint32_t value);
    bool u64(std::uint64_t value);
    bool f32(float value);
    bool f64(double value);
    bool string(std::string_view value);

    const std::vector<std::uint8_t>& data() const { return bytes_; }

private:
    std::vector<std::uint8_t> bytes_;
};

class Reader final {
public:
    explicit Reader(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}

    bool u8(std::uint8_t& value);
    bool u16(std::uint16_t& value);
    bool u32(std::uint32_t& value);
    bool u64(std::uint64_t& value);
    bool f32(float& value);
    bool f64(double& value);
    bool string(std::string& value);

    std::size_t offset() const { return offset_; }
    std::size_t remaining() const { return bytes_.size() - offset_; }

private:
    bool take(void* destination, std::size_t size);

    const std::vector<std::uint8_t>& bytes_;
    std::size_t offset_{};
};

std::vector<std::uint8_t> encode_client_welcome(const ClientWelcome& value);
bool decode_client_welcome(const std::vector<std::uint8_t>& bytes, ClientWelcome& value);

std::vector<std::uint8_t> encode_player_create(const PlayerCreate& value);
bool decode_player_create(const std::vector<std::uint8_t>& bytes, PlayerCreate& value);

std::vector<std::uint8_t> encode_player_remove(const PlayerRemove& value);
bool decode_player_remove(const std::vector<std::uint8_t>& bytes, PlayerRemove& value);

std::vector<std::uint8_t> encode_player_property(const PlayerProperty& value);
bool decode_player_property(const std::vector<std::uint8_t>& bytes, PlayerProperty& value);

std::vector<std::uint8_t> encode_client_player_state(const ClientPlayerState& value);
bool decode_client_player_state(const std::vector<std::uint8_t>& bytes, ClientPlayerState& value);

std::vector<std::uint8_t> encode_player_transform(const PlayerTransform& value);
bool decode_player_transform(const std::vector<std::uint8_t>& bytes, PlayerTransform& value);

std::vector<std::uint8_t> encode_event(
    std::string_view eventName,
    const std::vector<EventValue>& arguments);
bool decode_event(
    const std::vector<std::uint8_t>& bytes,
    std::string& eventName,
    std::vector<EventValue>& arguments);

} // namespace frontier::rdrmp
