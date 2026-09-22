#pragma once

#include "frontier/protocol.hpp"
#include "frontier/server/udp_socket.hpp"
#include "frontier/types.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace frontier::server {

class Server final {
public:
    struct Config final {
        std::uint16_t port{30120};
        float interestRadius{400.0f};
        float maxAcceptedSpeedMps{100.0f};
        std::uint64_t clientTimeoutMs{10000};
        std::uint64_t resumeGraceMs{15000};
    };

    explicit Server(Config config);
    bool start();
    void run();
    void stop();

private:
    struct Connection final {
        Endpoint endpoint;
        std::uint16_t playerId{};
        std::uint64_t connectionId{};
        std::string sessionToken;
        std::uint64_t lastReceiveMs{};
        protocol::ReceiveHistory receiveHistory;
        protocol::ReliabilityTracker reliability;
        std::uint32_t lastSentSequence{};
        PlayerState state{};
        bool stateInitialized{};
        bool welcomed{};
    };

    struct ResumeRecord final {
        std::uint16_t playerId{};
        std::string sessionToken;
        PlayerState state{};
        std::uint64_t expiresAtMs{};
    };

    void tick(std::uint64_t nowMs);
    void receive_packets(std::uint64_t nowMs);
    void handle_packet(Connection* connection, const std::vector<std::uint8_t>& bytes, std::uint64_t nowMs);
    void handle_hello(Connection& connection, const protocol::Message& message, std::uint64_t nowMs);
    void handle_player_state(Connection& connection, const protocol::Message& message, std::uint64_t nowMs);
    void send_message(Connection& connection, protocol::MessageType type, protocol::Channel channel, bool reliable, const std::vector<std::uint8_t>& payload);
    void broadcast_snapshots(std::uint64_t nowMs);
    void prune(std::uint64_t nowMs);
    std::uint64_t now_ms() const;
    Connection* find_connection(const Endpoint& endpoint);
    Connection& create_connection(const Endpoint& endpoint, std::uint64_t nowMs);
    std::uint16_t allocate_player_id();
    std::uint64_t make_u64_token();
    std::string make_session_token();
    bool relevant(const PlayerState& viewer, const PlayerState& target) const;

    Config config_;
    UdpSocket socket_;
    bool running_{};
    std::uint32_t serverTick_{};
    std::uint16_t nextPlayerId_{1};
    std::uint64_t lastSnapshotMs_{};
    std::unordered_map<std::string, Connection> connections_;
    std::unordered_map<std::string, ResumeRecord> resumes_;
};

} // namespace frontier::server
