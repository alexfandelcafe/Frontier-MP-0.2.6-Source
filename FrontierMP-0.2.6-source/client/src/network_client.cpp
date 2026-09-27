#include "frontier/client/network_client.hpp"

#include <chrono>
#include <cstdio>
#include <random>
#include <cstdlib>

namespace frontier::client {
namespace {
bool env_enabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && std::string(value) == "1";
}
}

bool NetworkClient::start(const std::string& host, std::uint16_t port, const std::string& playerName, const std::string& buildId, std::uint64_t buildHash, const std::string& sessionToken) {
#ifdef FRONTIER_RDRMP_ENET_AVAILABLE
    useRdrmpCompat_ = env_enabled("FRONTIER_RDRMP_COMPAT");
    if (useRdrmpCompat_) {
        playerName_ = playerName;
        playerId_ = 0;
        state_ = ConnectionState::Handshaking;
        rdrmpServerTick_ = 0;
        rdrmp_.set_on_player([this](const rdrmp::PlayerCreate& player) {
            if (playerId_ == 0) {
                playerId_ = player.playerId;
                state_ = ConnectionState::Connected;
            }
            if (onWelcome_ && player.playerId == playerId_) {
                protocol::Welcome welcome{};
                welcome.playerId = player.playerId;
                welcome.serverTick = ++rdrmpServerTick_;
                welcome.spawn.position = player.position;
                welcome.spawn.yaw = player.rotation.y;
                onWelcome_(welcome);
            }
        });
        rdrmp_.set_on_snapshot([this](const std::vector<rdrmp::PlayerCreate>& players) {
            protocol::Snapshot snapshot{};
            snapshot.serverTick = ++rdrmpServerTick_;
            snapshot.players.reserve(players.size());
            for (const auto& player : players) {
                PlayerState state{};
                state.playerId = player.playerId;
                state.clientTick = snapshot.serverTick;
                state.position = player.position;
                state.yaw = player.rotation.y;
                snapshot.players.push_back(state);
            }
            if (onSnapshot_) onSnapshot_(snapshot);
        });
        rdrmp_.set_on_disconnect([this](const std::string& reason) {
            state_ = ConnectionState::Disconnected;
            playerId_ = 0;
            if (onDisconnect_) onDisconnect_(reason);
        });
        if (!rdrmp_.start(host, port, playerName)) return false;
        state_ = ConnectionState::Handshaking;
        return true;
    }
#endif
    if (!socket_.open()) return false;
    server_ = {host, port};
    playerName_ = playerName;
    buildId_ = buildId;
    buildHash_ = buildHash;
    sessionToken_ = sessionToken;
    connectionId_ = make_connection_id();
    playerId_ = 0;
    state_ = ConnectionState::Handshaking;
    lastReceiveMs_ = 0;
    lastHelloMs_ = 0;
    lastHeartbeatMs_ = 0;
    nextSequence_ = 1;
    receiveHistory_ = {};
    reliability_ = {};
    const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    send_hello(now);
    return true;
}

void NetworkClient::stop() {
#ifdef FRONTIER_RDRMP_ENET_AVAILABLE
    if (useRdrmpCompat_) {
        rdrmp_.stop();
        useRdrmpCompat_ = false;
        playerId_ = 0;
        state_ = ConnectionState::Disconnected;
        return;
    }
#endif
    if (state_ == ConnectionState::Disconnected) return;
    send_message(protocol::MessageType::Goodbye, protocol::Channel::Control, true, protocol::encode_goodbye({0, "client shutdown"}));
    socket_.close();
    state_ = ConnectionState::Disconnected;
}

void NetworkClient::update(std::uint64_t nowMs) {
#ifdef FRONTIER_RDRMP_ENET_AVAILABLE
    if (useRdrmpCompat_) {
        rdrmp_.update();
        if (rdrmp_.connected()) state_ = ConnectionState::Connected;
        return;
    }
#endif
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
#ifdef FRONTIER_RDRMP_ENET_AVAILABLE
    if (useRdrmpCompat_) {
        if (!rdrmp_.connected()) return;
        (void)rdrmp_.submit_player_state(state, frontier::kDefaultPlayerActorModel);
        return;
    }
#endif
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
    // Before Welcome the server owns the connection id in its response. Once
    // connected, every non-Welcome packet must carry the server-issued id.
    if (state_ == ConnectionState::Connected &&
        message.type != protocol::MessageType::Welcome &&
        header.connectionId != connectionId_) {
        return;
    }

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
