#pragma once

#include "frontier/client/udp_socket.hpp"
#include "frontier/protocol.hpp"

#include <cstdint>
#include <functional>
#include <string>

namespace frontier::client {

enum class ConnectionState {
    Disconnected,
    Handshaking,
    Connected,
};

class NetworkClient final {
public:
    using SnapshotCallback = std::function<void(const protocol::Snapshot&)>;
    using WelcomeCallback = std::function<void(const protocol::Welcome&)>;
    using DisconnectCallback = std::function<void(const std::string&)>;

    bool start(const std::string& host, std::uint16_t port, const std::string& playerName, const std::string& buildId, std::uint64_t buildHash, const std::string& sessionToken = {});
    void stop();
    void update(std::uint64_t nowMs);
    void submit_player_state(PlayerState state);
    ConnectionState state() const { return state_; }
    std::uint16_t player_id() const { return playerId_; }
    const std::string& session_token() const { return sessionToken_; }

    void set_on_welcome(WelcomeCallback callback) { onWelcome_ = std::move(callback); }
    void set_on_snapshot(SnapshotCallback callback) { onSnapshot_ = std::move(callback); }
    void set_on_disconnect(DisconnectCallback callback) { onDisconnect_ = std::move(callback); }

private:
    void send_hello(std::uint64_t nowMs);
    void send_message(protocol::MessageType type, protocol::Channel channel, bool reliable, const std::vector<std::uint8_t>& payload);
    void handle_packet(const std::vector<std::uint8_t>& bytes, std::uint64_t nowMs);
    void reconnect_or_timeout(std::uint64_t nowMs);
    std::uint64_t make_connection_id() const;

    UdpSocket socket_;
    Endpoint server_;
    std::string playerName_;
    std::string buildId_;
    std::uint64_t buildHash_{};
    std::string sessionToken_;
    ConnectionState state_{ConnectionState::Disconnected};
    std::uint16_t playerId_{};
    std::uint64_t connectionId_{};
    std::uint64_t lastReceiveMs_{};
    std::uint64_t lastHelloMs_{};
    std::uint64_t lastHeartbeatMs_{};
    std::uint32_t nextSequence_{1};
    protocol::ReceiveHistory receiveHistory_;
    protocol::ReliabilityTracker reliability_;
    WelcomeCallback onWelcome_;
    SnapshotCallback onSnapshot_;
    DisconnectCallback onDisconnect_;
};

} // namespace frontier::client
