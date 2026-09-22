#include "frontier/protocol.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace frontier::protocol {

namespace {

void append_le16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

void append_le32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((value >> (8U * i)) & 0xFFU));
}

void append_le64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>((value >> (8U * i)) & 0xFFU));
}

std::uint16_t read_le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) | (static_cast<std::uint16_t>(p[1]) << 8U);
}

std::uint32_t read_le32(const std::uint8_t* p) {
    std::uint32_t v{};
    for (unsigned i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(p[i]) << (8U * i);
    return v;
}

std::uint64_t read_le64(const std::uint8_t* p) {
    std::uint64_t v{};
    for (unsigned i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (8U * i);
    return v;
}

} // namespace

void Writer::u8(std::uint8_t value) { bytes_.push_back(value); }
void Writer::u16(std::uint16_t value) { append_le16(bytes_, value); }
void Writer::u32(std::uint32_t value) { append_le32(bytes_, value); }
void Writer::u64(std::uint64_t value) { append_le64(bytes_, value); }
void Writer::f32(float value) {
    static_assert(sizeof(float) == sizeof(std::uint32_t));
    std::uint32_t raw{};
    std::memcpy(&raw, &value, sizeof(raw));
    u32(raw);
}
bool Writer::string(const std::string& value, std::size_t maxLength) {
    if (value.size() > maxLength || value.size() > std::numeric_limits<std::uint16_t>::max()) return false;
    u16(static_cast<std::uint16_t>(value.size()));
    bytes_.insert(bytes_.end(), value.begin(), value.end());
    return true;
}
bool Writer::bytes(const std::vector<std::uint8_t>& value) {
    if (value.size() > std::numeric_limits<std::uint16_t>::max()) return false;
    u16(static_cast<std::uint16_t>(value.size()));
    bytes_.insert(bytes_.end(), value.begin(), value.end());
    return true;
}

bool Reader::take(void* destination, std::size_t size) {
    if (offset_ + size > bytes_.size()) return false;
    std::memcpy(destination, bytes_.data() + offset_, size);
    offset_ += size;
    return true;
}
bool Reader::u8(std::uint8_t& value) { return take(&value, 1); }
bool Reader::u16(std::uint16_t& value) {
    if (offset_ + 2 > bytes_.size()) return false;
    value = read_le16(bytes_.data() + offset_); offset_ += 2; return true;
}
bool Reader::u32(std::uint32_t& value) {
    if (offset_ + 4 > bytes_.size()) return false;
    value = read_le32(bytes_.data() + offset_); offset_ += 4; return true;
}
bool Reader::u64(std::uint64_t& value) {
    if (offset_ + 8 > bytes_.size()) return false;
    value = read_le64(bytes_.data() + offset_); offset_ += 8; return true;
}
bool Reader::f32(float& value) {
    std::uint32_t raw{};
    if (!u32(raw)) return false;
    std::memcpy(&value, &raw, sizeof(value));
    return true;
}
bool Reader::string(std::string& value, std::size_t maxLength) {
    std::uint16_t length{};
    if (!u16(length) || length > maxLength || offset_ + length > bytes_.size()) return false;
    value.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), length);
    offset_ += length;
    return true;
}
bool Reader::bytes(std::vector<std::uint8_t>& value, std::size_t maxLength) {
    std::uint16_t length{};
    if (!u16(length) || length > maxLength || offset_ + length > bytes_.size()) return false;
    value.assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_), bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + length));
    offset_ += length;
    return true;
}

std::vector<std::uint8_t> encode_hello(const Hello& message) {
    Writer w;
    const bool ok = w.string(message.playerName, 32) && w.string(message.buildId, 64);
    if (!ok) return {};
    w.u64(message.buildTextHash);
    if (!w.string(message.sessionToken, 128)) return {};
    return w.data();
}

bool decode_hello(const std::vector<std::uint8_t>& bytes, Hello& message) {
    Reader r(bytes);
    return r.string(message.playerName, 32) && r.string(message.buildId, 64) && r.u64(message.buildTextHash) && r.string(message.sessionToken, 128) && r.remaining() == 0;
}

std::vector<std::uint8_t> encode_welcome(const Welcome& message) {
    Writer w;
    w.u16(message.playerId);
    w.u64(message.connectionId);
    w.u32(message.serverTick);
    w.f32(message.spawn.position.x);
    w.f32(message.spawn.position.y);
    w.f32(message.spawn.position.z);
    w.f32(message.spawn.yaw);
    if (!w.string(message.sessionToken, 128)) return {};
    return w.data();
}

bool decode_welcome(const std::vector<std::uint8_t>& bytes, Welcome& message) {
    Reader r(bytes);
    return r.u16(message.playerId) && r.u64(message.connectionId) && r.u32(message.serverTick) &&
        r.f32(message.spawn.position.x) && r.f32(message.spawn.position.y) && r.f32(message.spawn.position.z) &&
        r.f32(message.spawn.yaw) && r.string(message.sessionToken, 128) && r.remaining() == 0;
}

std::vector<std::uint8_t> encode_goodbye(const Goodbye& message) {
    Writer w; w.u16(message.reason); if (!w.string(message.message, 512)) return {}; return w.data();
}

bool decode_goodbye(const std::vector<std::uint8_t>& bytes, Goodbye& message) {
    Reader r(bytes); return r.u16(message.reason) && r.string(message.message, 512) && r.remaining() == 0;
}

std::vector<std::uint8_t> encode_player_state(const PlayerState& state) {
    Writer w;
    w.u16(state.playerId);
    w.u32(state.clientTick);
    w.f32(state.position.x); w.f32(state.position.y); w.f32(state.position.z);
    w.f32(state.yaw);
    w.f32(state.velocity.x); w.f32(state.velocity.y); w.f32(state.velocity.z);
    w.u8(state.gait); w.u8(state.flags);
    return w.data();
}

bool decode_player_state(const std::vector<std::uint8_t>& bytes, PlayerState& state) {
    Reader r(bytes);
    return r.u16(state.playerId) && r.u32(state.clientTick) &&
        r.f32(state.position.x) && r.f32(state.position.y) && r.f32(state.position.z) && r.f32(state.yaw) &&
        r.f32(state.velocity.x) && r.f32(state.velocity.y) && r.f32(state.velocity.z) &&
        r.u8(state.gait) && r.u8(state.flags) && r.remaining() == 0;
}

std::vector<std::uint8_t> encode_snapshot(const Snapshot& snapshot) {
    if (snapshot.players.size() > 64) return {};
    Writer w;
    w.u32(snapshot.serverTick);
    w.u16(static_cast<std::uint16_t>(snapshot.players.size()));
    for (const auto& player : snapshot.players) {
        const auto bytes = encode_player_state(player);
        if (bytes.size() > std::numeric_limits<std::uint16_t>::max()) return {};
        if (!w.bytes(bytes)) return {};
    }
    return w.data();
}

bool decode_snapshot(const std::vector<std::uint8_t>& bytes, Snapshot& snapshot) {
    Reader r(bytes);
    std::uint16_t count{};
    if (!r.u32(snapshot.serverTick) || !r.u16(count) || count > 64) return false;
    snapshot.players.clear();
    snapshot.players.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        std::vector<std::uint8_t> stateBytes;
        if (!r.bytes(stateBytes, 512)) return false;
        PlayerState player{};
        if (!decode_player_state(stateBytes, player)) return false;
        snapshot.players.push_back(player);
    }
    return r.remaining() == 0;
}

std::vector<std::uint8_t> encode_packet(const PacketHeader& header, const Message& message) {
    if (message.payload.size() > kMaxMessageSize || message.payload.size() > std::numeric_limits<std::uint16_t>::max()) return {};
    PacketHeader actual = header;
    actual.magic = kMagic;
    actual.version = kProtocolVersion;
    actual.flags = static_cast<std::uint8_t>(message.reliable ? PacketFlags::Reliable : PacketFlags::None);
    actual.payloadSize = static_cast<std::uint16_t>(kMessageHeaderSize + message.payload.size());

    std::vector<std::uint8_t> out;
    out.reserve(sizeof(PacketHeader) + actual.payloadSize);
    const auto append_u32 = [&out](std::uint32_t v) { append_le32(out, v); };
    const auto append_u64 = [&out](std::uint64_t v) { append_le64(out, v); };
    append_u32(actual.magic);
    out.push_back(actual.version);
    out.push_back(actual.flags);
    append_le16(out, actual.payloadSize);
    append_u32(actual.sequence);
    append_u32(actual.ack);
    append_u32(actual.ackBits);
    append_u64(actual.connectionId);
    out.push_back(static_cast<std::uint8_t>(message.type));
    out.push_back(static_cast<std::uint8_t>(message.channel));
    append_le16(out, static_cast<std::uint16_t>(message.payload.size()));
    out.insert(out.end(), message.payload.begin(), message.payload.end());
    return out;
}

bool decode_packet(const std::vector<std::uint8_t>& bytes, PacketHeader& header, Message& message) {
    if (bytes.size() < sizeof(PacketHeader) + sizeof(MessageHeader) || bytes.size() > kMaxDatagramSize) return false;
    std::size_t offset = 0;
    header.magic = read_le32(bytes.data() + offset); offset += 4;
    header.version = bytes[offset++];
    header.flags = bytes[offset++];
    header.payloadSize = read_le16(bytes.data() + offset); offset += 2;
    header.sequence = read_le32(bytes.data() + offset); offset += 4;
    header.ack = read_le32(bytes.data() + offset); offset += 4;
    header.ackBits = read_le32(bytes.data() + offset); offset += 4;
    header.connectionId = read_le64(bytes.data() + offset); offset += 8;
    if (header.magic != kMagic || header.version != kProtocolVersion) return false;
    if (header.payloadSize != bytes.size() - sizeof(PacketHeader)) return false;

    message.type = static_cast<MessageType>(bytes[offset++]);
    message.channel = static_cast<Channel>(bytes[offset++]);
    const std::uint16_t size = read_le16(bytes.data() + offset); offset += 2;
    if (size != bytes.size() - sizeof(PacketHeader) - sizeof(MessageHeader) || size > kMaxMessageSize) return false;
    message.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end());
    message.reliable = (header.flags & static_cast<std::uint8_t>(PacketFlags::Reliable)) != 0;
    return true;
}

bool ReceiveHistory::accept(std::uint32_t sequence) {
    if (!initialized_) {
        initialized_ = true;
        highest_ = sequence;
        ackBits_ = 0;
        return true;
    }
    if (sequence > highest_) {
        const std::uint32_t shift = sequence - highest_;
        if (shift >= 32) ackBits_ = 0;
        else ackBits_ = (ackBits_ << shift) | (1U << (shift - 1U));
        highest_ = sequence;
        return true;
    }
    const std::uint32_t diff = highest_ - sequence;
    if (diff == 0 || diff > 32) return false;
    const std::uint32_t mask = 1U << (diff - 1U);
    const bool wasSeen = (ackBits_ & mask) != 0;
    ackBits_ |= mask;
    return !wasSeen;
}

void ReliabilityTracker::remember_reliable(std::uint32_t sequence, const std::vector<std::uint8_t>& packet, std::uint64_t sentAtMs) {
    reliablePackets_.push_back(ResendItem{sequence, packet, 0, sentAtMs});
}

void ReliabilityTracker::acknowledge(std::uint32_t ack, std::uint32_t ackBits) {
    reliablePackets_.erase(std::remove_if(reliablePackets_.begin(), reliablePackets_.end(), [ack, ackBits](const ResendItem& item) {
        if (item.sequence == ack) return true;
        if (item.sequence > ack) return false;
        const std::uint32_t diff = ack - item.sequence;
        if (diff == 0 || diff > 32) return false;
        return (ackBits & (1U << (diff - 1U))) != 0;
    }), reliablePackets_.end());
}

std::vector<ReliabilityTracker::ResendItem> ReliabilityTracker::due_resends(std::uint64_t nowMs, std::uint64_t resendDelayMs, std::uint8_t maxRetries) {
    std::vector<ResendItem> due;
    for (auto& item : reliablePackets_) {
        if (item.retries >= maxRetries) continue;
        if (item.lastSentMs == 0 || nowMs - item.lastSentMs >= resendDelayMs) {
            ++item.retries;
            item.lastSentMs = nowMs;
            due.push_back(item);
        }
    }
    return due;
}

} // namespace frontier::protocol
