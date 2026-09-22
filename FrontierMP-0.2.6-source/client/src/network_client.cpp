#include "frontier/client/network_client.hpp"

#include <chrono>
#include <cstdio>
#include <random>

namespace frontier::client {

bool NetworkClient::start(const std::string& host, std::uint16_t port, const std::string& playerName, const std::string& buildId, std::uint64_t buildHash, const std::string& sessionToken) {
    if (!socket_.open()) return false;
    server_ = {host, port};
    playerName_ = playerName;
    buildId_ = buildId;
    buildHash_ = buildHash;
    sessionToken_ = sessionToken;
    connectionId_ = make_connection_id();
    state_ = ConnectionState::Handshaking;
    lastReceiveMs_ = 0;
    lastHelloMs_ = 0;
    lastHeartbeatMs_ = 0;
    const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    send_hello(now);
    return true;
}

void NetworkClient::stop() {
    if (state_ == ConnectionState::Disconnected) return;
    send_message(protocol::MessageType::Goodbye, protocol::Channel::Control, true, protocol::encode_goodbye({0, "client shutdown"}));
    socket_.close();
    state_ = ConnectionState::Disconnected;
}

void NetworkClient::update(std::uint64_t nowMs) {
    if (state_ == ConnectionState::Disconnected) return;
    Endpoint from;
    std::vector<std::uint8_t> packet;
    while (socket_.receive(from, packet)) {
        if (from.host == server_.host && from.port == server_.port) handle_packet(packet, nowMs);
        packet.clear();
    }

    if (state_ == ConnectionState::Handshaking && (lastHelloMs_ == 0 || nowMs - lastHelloMs_ >= 500)) send_hello(nowMs);
    if (state_ == ConnectionState::Connected && (lastHeartbeatMs_ == 0 || nowMs - lastHeartbeatMs_ >= 2000)) {
        send_message(protocol::MessageType::Ping, protocol::Channel::Control, false, {});
        lastHeartbeatMs_ = nowMs;
    }

    for (auto item : reliability_.due_resends(nowMs)) socket_.send(server_, item.packet);
    reconnect_or_timeout(nowMs);
}

void NetworkClient::submit_player_state(PlayerState state) {
    if (state_ != ConnectionState::Connected) return;
    state.playerId = playerId_;
    send_message(protocol::MessageType::PlayerState, protocol::Channel::State, false, protocol::encode_player_state(state));
}

void NetworkClient::send_hello(std::uint64_t nowMs) {
    protocol::Hello hello{};
    hello.playerName = playerName_;
    hello.buildId = buildId_;
    hello.buildTextHash = buildHash_;
    hello.sessionToken = sessionToken_;
    send_message(protocol::MessageType::Hello, protocol::Channel::Control, true, protocol::encode_hello(hello));
    lastHelloMs_ = nowMs;
}

void NetworkClient::send_message(protocol::MessageType type, protocol::Channel channel, bool reliable, const std::vector<std::uint8_t>& payload) {
    protocol::Message message{};
    message.type = type;
    message.channel = channel;
    message.reliable = reliable;
    message.payload = payload;

    protocol::PacketHeader header{};
    header.sequence = nextSequence_++;
    header.ack = receiveHistory_.highest();
    header.ackBits = receiveHistory_.ack_bits();
    header.connectionId = connectionId_;
    auto packet = protocol::encode_packet(header, message);
    if (packet.empty()) return;
    if (reliable) {
        const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        reliability_.remember_reliable(header.sequence, packet, now);
    }
    socket_.send(server_, packet);
}

void NetworkClient::handle_packet(const std::vector<std::uint8_t>& bytes, std::uint64_t nowMs) {
    protocol::PacketHeader header{};
    protocol::Message message{};
    if (!protocol::decode_packet(bytes, header, message)) return;
    reliability_.acknowledge(header.ack, header.ackBits);
    if (!receiveHistory_.accept(header.sequence)) return;
    lastReceiveMs_ = nowMs;

    switch (message.type) {
    case protocol::MessageType::Welcome: {
        protocol::Welcome welcome{};
        if (!protocol::decode_welcome(message.payload, welcome)) return;
        playerId_ = welcome.playerId;
        connectionId_ = welcome.connectionId;
        sessionToken_ = welcome.sessionToken;
        state_ = ConnectionState::Connected;
        if (onWelcome_) onWelcome_(welcome);
        break;
    }
    case protocol::MessageType::Snapshot: {
        protocol::Snapshot snapshot{};
        if (!protocol::decode_snapshot(message.payload, snapshot)) return;
        if (onSnapshot_) onSnapshot_(snapshot);
        break;
    }
    case protocol::MessageType::Pong:
        break;
    default:
        break;
    }
}

void NetworkClient::reconnect_or_timeout(std::uint64_t nowMs) {
    if (state_ == ConnectionState::Connected && lastReceiveMs_ != 0 && nowMs - lastReceiveMs_ > 10000) {
        state_ = ConnectionState::Handshaking;
        playerId_ = 0;
        if (onDisconnect_) onDisconnect_("server timeout; reconnecting");
        lastReceiveMs_ = nowMs;
    }
}

std::uint64_t NetworkClient::make_connection_id() const {
    std::random_device device;
    return (static_cast<std::uint64_t>(device()) << 32U) ^ static_cast<std::uint64_t>(device());
}

} // namespace frontier::client
