#pragma once

#include "frontier/types.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace frontier::protocol {

enum class PacketFlags : std::uint8_t {
    None = 0,
    Reliable = 1 << 0,
};

inline constexpr PacketFlags operator|(PacketFlags lhs, PacketFlags rhs) {
    return static_cast<PacketFlags>(static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

inline constexpr bool has_flag(PacketFlags value, PacketFlags flag) {
    return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(flag)) != 0;
}

enum class MessageType : std::uint8_t {
    Hello = 1,
    Welcome = 2,
    PlayerState = 3,
    Snapshot = 4,
    Goodbye = 5,
    Ping = 6,
    Pong = 7,
};

enum class Channel : std::uint8_t {
    Control = 0,
    State = 1,
};

#pragma pack(push, 1)
struct PacketHeader final {
    std::uint32_t magic{};
    std::uint8_t version{};
    std::uint8_t flags{};
    std::uint16_t payloadSize{};
    std::uint32_t sequence{};
    std::uint32_t ack{};
    std::uint32_t ackBits{};
    std::uint64_t connectionId{};
};

struct MessageHeader final {
    std::uint8_t type{};
    std::uint8_t channel{};
    std::uint16_t size{};
};
#pragma pack(pop)

inline constexpr std::uint32_t kMagic = 0x504D5246U; // ASCII "FRMP" in little-endian memory.
inline constexpr std::size_t kPacketHeaderSize = sizeof(PacketHeader);
inline constexpr std::size_t kMessageHeaderSize = sizeof(MessageHeader);
inline constexpr std::size_t kMaxDatagramSize = 1200; // Keep under common Internet MTU after headers.

struct Hello final {
    std::string playerName;
    std::string buildId;
    std::uint64_t buildTextHash{};
    std::string sessionToken;
};

struct Welcome final {
    std::uint16_t playerId{};
    std::uint64_t connectionId{};
    std::uint32_t serverTick{};
    SpawnPoint spawn{};
    std::string sessionToken;
};

struct Goodbye final {
    std::uint16_t reason{};
    std::string message;
};

struct Snapshot final {
    std::uint32_t serverTick{};
    std::vector<PlayerState> players;
};

struct Message final {
    MessageType type{MessageType::Ping};
    Channel channel{Channel::Control};
    bool reliable{};
    std::vector<std::uint8_t> payload;
};

class Writer final {
public:
    void u8(std::uint8_t value);
    void u16(std::uint16_t value);
    void u32(std::uint32_t value);
    void u64(std::uint64_t value);
    void f32(float value);
    bool string(const std::string& value, std::size_t maxLength = 1024);
    bool bytes(const std::vector<std::uint8_t>& value);
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
    bool string(std::string& value, std::size_t maxLength = 1024);
    bool bytes(std::vector<std::uint8_t>& value, std::size_t maxLength = kMaxMessageSize);
    std::size_t remaining() const { return bytes_.size() - offset_; }

private:
    bool take(void* destination, std::size_t size);

    const std::vector<std::uint8_t>& bytes_;
    std::size_t offset_{};
};

std::vector<std::uint8_t> encode_hello(const Hello& message);
bool decode_hello(const std::vector<std::uint8_t>& bytes, Hello& message);

std::vector<std::uint8_t> encode_welcome(const Welcome& message);
bool decode_welcome(const std::vector<std::uint8_t>& bytes, Welcome& message);

std::vector<std::uint8_t> encode_goodbye(const Goodbye& message);
bool decode_goodbye(const std::vector<std::uint8_t>& bytes, Goodbye& message);

std::vector<std::uint8_t> encode_player_state(const PlayerState& state);
bool decode_player_state(const std::vector<std::uint8_t>& bytes, PlayerState& state);

std::vector<std::uint8_t> encode_snapshot(const Snapshot& snapshot);
bool decode_snapshot(const std::vector<std::uint8_t>& bytes, Snapshot& snapshot);

std::vector<std::uint8_t> encode_packet(const PacketHeader& header, const Message& message);
bool decode_packet(const std::vector<std::uint8_t>& bytes, PacketHeader& header, Message& message);

class ReceiveHistory final {
public:
    bool accept(std::uint32_t sequence);
    std::uint32_t highest() const { return highest_; }
    std::uint32_t ack_bits() const { return ackBits_; }

private:
    std::uint32_t highest_{};
    std::uint32_t ackBits_{};
    bool initialized_{};
};

class ReliabilityTracker final {
public:
    std::uint32_t next_sequence() { return nextSequence_++; }
    void remember_reliable(std::uint32_t sequence, const std::vector<std::uint8_t>& packet, std::uint64_t sentAtMs);
    void acknowledge(std::uint32_t ack, std::uint32_t ackBits);

    struct ResendItem final {
        std::uint32_t sequence{};
        std::vector<std::uint8_t> packet;
        std::uint8_t retries{};
        std::uint64_t lastSentMs{};
    };

    std::vector<ResendItem> due_resends(std::uint64_t nowMs, std::uint64_t resendDelayMs = 120, std::uint8_t maxRetries = 10);

private:
    std::uint32_t nextSequence_{1};
    std::vector<ResendItem> reliablePackets_;
};

} // namespace frontier::protocol
