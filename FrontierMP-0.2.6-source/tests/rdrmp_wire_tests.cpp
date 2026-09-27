#include "frontier/rdrmp_wire.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <variant>
#include <vector>
#include <cstdio>
#include <cstdlib>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "RDRMP wire test failed: %s\n", message);
        std::exit(1);
    }
}

void expect_bytes(const std::vector<std::uint8_t>& actual,
                  std::initializer_list<std::uint8_t> expected) {
    const std::vector<std::uint8_t> expectedBytes(expected);
    require(actual == expectedBytes, "unexpected byte sequence");
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
    require(frontier::rdrmp::decode_client_welcome(bytes, decoded), "client welcome decode");
    require(decoded.actorModel == 837, "client welcome model");
    require(decoded.name == "John", "client welcome name");
    require(decoded.position.x == 1.0f && decoded.position.y == 2.0f && decoded.position.z == 3.0f, "client welcome position");
    require(decoded.rotation.x == 10.0f && decoded.rotation.y == 20.0f && decoded.rotation.z == 30.0f, "client welcome rotation");
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
    require(transformBytes.size() == 26, "player transform size");
    require(transformBytes[0] == 0x12 && transformBytes[1] == 0x34, "player transform id");

    frontier::rdrmp::ClientPlayerState clientState{};
    clientState.position = {4.0f, 5.0f, 6.0f};
    clientState.rotation = {0.0f, 45.0f, 0.0f};
    const auto clientStateBytes = frontier::rdrmp::encode_client_player_state(clientState);
    assert(clientStateBytes.size() == 24);
    expect_bytes(clientStateBytes, {
        0x40, 0x80, 0x00, 0x00,
        0x40, 0xA0, 0x00, 0x00,
        0x40, 0xC0, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x42, 0x34, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    });

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
    require(frontier::rdrmp::fnv1a64("core:on_player_joined") == 0xC4D7C7B3F6C05466ULL, "FNV-1a hash");

    const std::vector<frontier::rdrmp::EventValue> values{
        std::uint64_t{7},
        2.5,
        true,
        std::string("hello"),
        frontier::Vec3{1.0f, 2.0f, 3.0f},
    };

    const auto bytes = frontier::rdrmp::encode_event("test:event", values);
    require(!bytes.empty(), "event encoding");

    std::string eventName;
    std::vector<frontier::rdrmp::EventValue> decoded;
    require(frontier::rdrmp::decode_event(bytes, eventName, decoded), "event decode");
    require(eventName == "test:event", "event name");
    require(decoded.size() == values.size(), "event arg count");
    require(std::get<std::uint64_t>(decoded[0]) == 7, "event uint64");
    require(std::fabs(std::get<double>(decoded[1]) - 2.5) < 1e-12, "event double");
    require(std::get<bool>(decoded[2]), "event bool");
    require(std::get<std::string>(decoded[3]) == "hello", "event string");
    const auto vector = std::get<frontier::Vec3>(decoded[4]);
    require(vector.x == 1.0f && vector.y == 2.0f && vector.z == 3.0f, "event Vector3");
}

void test_malformed_event() {
    std::vector<std::uint8_t> bytes{0x00, 0x01, 0x06};
    std::string eventName;
    std::vector<frontier::rdrmp::EventValue> arguments;
    require(!frontier::rdrmp::decode_event(bytes, eventName, arguments), "malformed event rejected");
}

} // namespace

int main() {
    test_client_welcome();
    test_player_packets();
    test_event_codec();
    test_malformed_event();
    return 0;
}

