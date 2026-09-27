#include "frontier/rdrmp_wire.hpp"

#include <cstring>
#include <limits>

namespace frontier::rdrmp {

namespace {

void append_be16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

void append_be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((value >> static_cast<unsigned>(shift)) & 0xFFU));
    }
}

void append_be64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((value >> static_cast<unsigned>(shift)) & 0xFFU));
    }
}

std::uint16_t read_be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8U) | p[1]);
}

std::uint32_t read_be32(const std::uint8_t* p) {
    std::uint32_t value{};
    for (unsigned i = 0; i < 4; ++i) value = (value << 8U) | static_cast<std::uint32_t>(p[i]);
    return value;
}

std::uint64_t read_be64(const std::uint8_t* p) {
    std::uint64_t value{};
    for (unsigned i = 0; i < 8; ++i) value = (value << 8U) | static_cast<std::uint64_t>(p[i]);
    return value;
}

std::uint32_t float_bits(float value) {
    std::uint32_t bits{};
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float bits_float(std::uint32_t bits) {
    float value{};
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::uint64_t double_bits(double value) {
    std::uint64_t bits{};
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

double bits_double(std::uint64_t bits) {
    double value{};
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool write_vec3(Writer& writer, const Vec3& value) {
    return writer.f32(value.x) && writer.f32(value.y) && writer.f32(value.z);
}

bool read_vec3(Reader& reader, Vec3& value) {
    return reader.f32(value.x) && reader.f32(value.y) && reader.f32(value.z);
}

} // namespace

std::uint64_t fnv1a64(std::string_view value) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

void Writer::u8(std::uint8_t value) { bytes_.push_back(value); }
bool Writer::u16(std::uint16_t value) { append_be16(bytes_, value); return true; }
bool Writer::u32(std::uint32_t value) { append_be32(bytes_, value); return true; }
bool Writer::u64(std::uint64_t value) { append_be64(bytes_, value); return true; }
bool Writer::f32(float value) { return u32(float_bits(value)); }
bool Writer::f64(double value) { return u64(double_bits(value)); }

bool Writer::string(std::string_view value) {
    if (value.size() > kMaxStringLength || value.size() > std::numeric_limits<std::uint16_t>::max()) return false;
    if (!u16(static_cast<std::uint16_t>(value.size()))) return false;
    bytes_.insert(bytes_.end(), value.begin(), value.end());
    return true;
}

bool Reader::take(void* destination, std::size_t size) {
    if (size > remaining()) return false;
    std::memcpy(destination, bytes_.data() + offset_, size);
    offset_ += size;
    return true;
}

bool Reader::u8(std::uint8_t& value) { return take(&value, sizeof(value)); }

bool Reader::u16(std::uint16_t& value) {
    if (remaining() < 2) return false;
    value = read_be16(bytes_.data() + offset_); offset_ += 2; return true;
}

bool Reader::u32(std::uint32_t& value) {
    if (remaining() < 4) return false;
    value = read_be32(bytes_.data() + offset_); offset_ += 4; return true;
}

bool Reader::u64(std::uint64_t& value) {
    if (remaining() < 8) return false;
    value = read_be64(bytes_.data() + offset_); offset_ += 8; return true;
}

bool Reader::f32(float& value) {
    std::uint32_t bits{};
    if (!u32(bits)) return false;
    value = bits_float(bits);
    return true;
}

bool Reader::f64(double& value) {
    std::uint64_t bits{};
    if (!u64(bits)) return false;
    value = bits_double(bits);
    return true;
}

bool Reader::string(std::string& value) {
    std::uint16_t length{};
    if (!u16(length) || length > kMaxStringLength || length > remaining()) return false;
    value.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), length);
    offset_ += length;
    return true;
}

std::vector<std::uint8_t> encode_client_welcome(const ClientWelcome& value) {
    Writer writer;
    if (!writer.u32(value.actorModel) || !writer.string(value.name) ||
        !write_vec3(writer, value.position) || !write_vec3(writer, value.rotation)) return {};
    return writer.data();
}

bool decode_client_welcome(const std::vector<std::uint8_t>& bytes, ClientWelcome& value) {
    Reader reader(bytes);
    return reader.u32(value.actorModel) && reader.string(value.name) &&
        read_vec3(reader, value.position) && read_vec3(reader, value.rotation) && reader.remaining() == 0;
}

std::vector<std::uint8_t> encode_player_create(const PlayerCreate& value) {
    Writer writer;
    if (!writer.u16(value.playerId) || !writer.u32(value.actorModel) || !writer.string(value.name) ||
        !write_vec3(writer, value.position) || !write_vec3(writer, value.rotation)) return {};
    return writer.data();
}

bool decode_player_create(const std::vector<std::uint8_t>& bytes, PlayerCreate& value) {
    Reader reader(bytes);
    return reader.u16(value.playerId) && reader.u32(value.actorModel) && reader.string(value.name) &&
        read_vec3(reader, value.position) && read_vec3(reader, value.rotation) && reader.remaining() == 0;
}

std::vector<std::uint8_t> encode_player_remove(const PlayerRemove& value) {
    Writer writer;
    if (!writer.u16(value.playerId)) return {};
    return writer.data();
}

bool decode_player_remove(const std::vector<std::uint8_t>& bytes, PlayerRemove& value) {
    Reader reader(bytes);
    return reader.u16(value.playerId) && reader.remaining() == 0;
}

std::vector<std::uint8_t> encode_player_property(const PlayerProperty& value) {
    Writer writer;
    if (!writer.u16(value.playerId) || !writer.u32(value.value)) return {};
    return writer.data();
}

bool decode_player_property(const std::vector<std::uint8_t>& bytes, PlayerProperty& value) {
    Reader reader(bytes);
    return reader.u16(value.playerId) && reader.u32(value.value) && reader.remaining() == 0;
}

std::vector<std::uint8_t> encode_client_player_state(const ClientPlayerState& value) {
    Writer writer;
    if (!write_vec3(writer, value.position) || !write_vec3(writer, value.rotation)) return {};
    return writer.data();
}

bool decode_client_player_state(const std::vector<std::uint8_t>& bytes, ClientPlayerState& value) {
    Reader reader(bytes);
    return read_vec3(reader, value.position) &&
           read_vec3(reader, value.rotation) &&
           reader.remaining() == 0;
}

std::vector<std::uint8_t> encode_player_transform(const PlayerTransform& value) {
    Writer writer;
    if (!writer.u16(value.playerId) || !write_vec3(writer, value.position) || !write_vec3(writer, value.rotation)) return {};
    return writer.data();
}

bool decode_player_transform(const std::vector<std::uint8_t>& bytes, PlayerTransform& value) {
    Reader reader(bytes);
    return reader.u16(value.playerId) && read_vec3(reader, value.position) && read_vec3(reader, value.rotation) && reader.remaining() == 0;
}

std::vector<std::uint8_t> encode_event(std::string_view eventName, const std::vector<EventValue>& arguments) {
    Writer writer;
    if (arguments.size() > 0xFFU || !writer.string(eventName)) return {};
    writer.u8(static_cast<std::uint8_t>(arguments.size()));
    for (const EventValue& argument : arguments) {
        if (std::holds_alternative<std::uint64_t>(argument)) {
            writer.u8(1); if (!writer.u64(std::get<std::uint64_t>(argument))) return {};
        } else if (std::holds_alternative<double>(argument)) {
            writer.u8(2); if (!writer.f64(std::get<double>(argument))) return {};
        } else if (std::holds_alternative<bool>(argument)) {
            writer.u8(3); writer.u8(std::get<bool>(argument) ? 1U : 0U);
        } else if (std::holds_alternative<std::string>(argument)) {
            writer.u8(4); if (!writer.string(std::get<std::string>(argument))) return {};
        } else {
            writer.u8(5); if (!write_vec3(writer, std::get<Vec3>(argument))) return {};
        }
    }
    return writer.data();
}

bool decode_event(const std::vector<std::uint8_t>& bytes, std::string& eventName, std::vector<EventValue>& arguments) {
    Reader reader(bytes);
    if (!reader.string(eventName)) return false;
    std::uint8_t argumentCount{};
    if (!reader.u8(argumentCount)) return false;
    arguments.clear();
    arguments.reserve(argumentCount);
    for (std::uint8_t index = 0; index < argumentCount; ++index) {
        std::uint8_t type{};
        if (!reader.u8(type)) return false;
        switch (type) {
        case 1: { std::uint64_t v{}; if (!reader.u64(v)) return false; arguments.emplace_back(v); break; }
        case 2: { double v{}; if (!reader.f64(v)) return false; arguments.emplace_back(v); break; }
        case 3: { std::uint8_t v{}; if (!reader.u8(v)) return false; arguments.emplace_back(v != 0); break; }
        case 4: { std::string v; if (!reader.string(v)) return false; arguments.emplace_back(std::move(v)); break; }
        case 5: { Vec3 v{}; if (!read_vec3(reader, v)) return false; arguments.emplace_back(v); break; }
        default: return false;
        }
    }
    return reader.remaining() == 0;
}

} // namespace frontier::rdrmp
