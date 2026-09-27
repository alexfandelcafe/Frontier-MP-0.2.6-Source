#include "frontier/rdrmp_wire.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <variant>
#include <vector>

namespace {

void expect_bytes(const std::vector<std::uint8_t>& actual,
                  std::initializer_list<std::uint8_t> expected) {
    assert(actual == std::vector<std::uint8_t>(expected));
}

void test_client_welcome() {
    frontier::rdrmp::ClientWelcome message{};
    message.actorModel = 837;
    message.name = "John";
    message.position = {1.0f, 2.0f, 3.0f};
    message.rotation = {10.0f, 20.0f, 30.0f};

    const auto bytes = frontier::rdrmp::encode_client_welcome(message);
    expect_bytes(bytes, {
        0x00, 0x00, 0x03, 0x45,
        0x00, 0x04, 0x4A, 0x6F, 0x68, 0x6E,
        0x3F, 0x80, 0x00, 0x00,
        0x40, 0x00, 0x00, 0x00,
        0x40, 0x40, 0x00, 0x00,
        0x41, 0x20, 0x00, 0x00,
        0x41, 0xA0, 0x00, 0x00,
        0x41, 0xF0, 0x00, 0x00,
    });

    frontier::rdrmp::ClientWelcome decoded{};
    assert(frontier::rdrmp::decode_client_welcome(bytes, decoded));
    assert(decoded.actorModel == 837);
    assert(decoded.name == "John");
    assert(decoded.position.x == 1.0f && decoded.position.y == 2.0f && decoded.position.z == 3.0f);
    assert(decoded.rotation.x == 10.0f && decoded.rotation.y == 20.0f && decoded.rotation.z == 30.0f);
}

void test_player_packets() {
    frontier::rdrmp::PlayerCreate create{};
    create.playerId = 0x1234;
    create.actorModel = 837;
    create.name = "John";
    create.position = {1.0f, 2.0f, 3.0f};
    create.rotation = {10.0f, 20.0f, 30.0f};

    expect_bytes(frontier::rdrmp::encode_player_create(create), {
        0x12, 0x34,
        0x00, 0x00, 0x03, 0x45,
        0x00, 0x04, 0x4A, 0x6F, 0x68, 0x6E,
        0x3F, 0x80, 0x00, 0x00,
        0x40, 0x00, 0x00, 0x00,
        0x40, 0x40, 0x00, 0x00,
        0x41, 0x20, 0x00, 0x00,
        0x41, 0xA0, 0x00, 0x00,
        0x41, 0xF0, 0x00, 0x00,
    });

    frontier::rdrmp::PlayerTransform transform{};
    transform.playerId = 0x1234;
    transform.position = create.position;
    transform.rotation = create.rotation;
    const auto transformBytes = frontier::rdrmp::encode_player_transform(transform);
    assert(transformBytes.size() == 26);
    assert(transformBytes[0] == 0x12 && transformBytes[1] == 0x34);

    frontier::rdrmp::PlayerProperty property{};
    property.playerId = 0x1234;
    property.value = 0xCAFEBABE;
    expect_bytes(frontier::rdrmp::encode_player_property(property), {
        0x12, 0x34, 0xCA, 0xFE, 0xBA, 0xBE,
    });

    frontier::rdrmp::PlayerRemove remove{};
    remove.playerId = 0x1234;
    expect_bytes(frontier::rdrmp::encode_player_remove(remove), {0x12, 0x34});
}

void test_event_codec() {
    assert(frontier::rdrmp::fnv1a64("core:on_player_joined") == 0xC4D7C7B3F6C05466ULL);

    const std::vector<frontier::rdrmp::EventValue> values{
        std::uint64_t{7},
        2.5,
        true,
        std::string("hello"),
        frontier::Vec3{1.0f, 2.0f, 3.0f},
    };

    const auto bytes = frontier::rdrmp::encode_event("test:event", values);
    assert(!bytes.empty());

    std::string eventName;
    std::vector<frontier::rdrmp::EventValue> decoded;
    assert(frontier::rdrmp::decode_event(bytes, eventName, decoded));
    assert(eventName == "test:event");
    assert(decoded.size() == values.size());
    assert(std::get<std::uint64_t>(decoded[0]) == 7);
    assert(std::fabs(std::get<double>(decoded[1]) - 2.5) < 1e-12);
    assert(std::get<bool>(decoded[2]));
    assert(std::get<std::string>(decoded[3]) == "hello");
    const auto vector = std::get<frontier::Vec3>(decoded[4]);
    assert(vector.x == 1.0f && vector.y == 2.0f && vector.z == 3.0f);
}

void test_malformed_event() {
    std::vector<std::uint8_t> bytes{0x00, 0x01, 0x06};
    std::string eventName;
    std::vector<frontier::rdrmp::EventValue> arguments;
    assert(!frontier::rdrmp::decode_event(bytes, eventName, arguments));
}

} // namespace

int main() {
    test_client_welcome();
    test_player_packets();
    test_event_codec();
    test_malformed_event();
    return 0;
}
