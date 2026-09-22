#include "frontier/server/server.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <thread>

namespace frontier::server {

namespace {

std::uint64_t monotonic_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

float distance_squared(const Vec3& a, const Vec3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

} // namespace

Server::Server(Config config) : config_(config) {}

bool Server::start() {
    if (!socket_.open(config_.port)) {
        std::fprintf(stderr, "[server] failed to bind UDP port %u\n", config_.port);
        return false;
    }
    running_ = true;
    std::printf("[server] FrontierMP listening on UDP %u\n", config_.port);
    return true;
}

void Server::run() {
    using namespace std::chrono_literals;
    auto nextTick = std::chrono::steady_clock::now();
    while (running_) {
        const std::uint64_t now = now_ms();
        tick(now);
        nextTick += 50ms;
        std::this_thread::sleep_until(nextTick);
        if (std::chrono::steady_clock::now() > nextTick + 250ms) nextTick = std::chrono::steady_clock::now();
    }
}

void Server::stop() {
    if (!running_) return;
    running_ = false;
    socket_.close();
}

void Server::tick(std::uint64_t nowMs) {
    ++serverTick_;
    receive_packets(nowMs);

    if (nowMs - lastSnapshotMs_ >= 50) {
        broadcast_snapshots(nowMs);
        lastSnapshotMs_ = nowMs;
    }

    for (auto& [key, connection] : connections_) {
        for (auto item : connection.reliability.due_resends(nowMs)) {
            socket_.send(connection.endpoint, item.packet);
        }
        (void)key;
    }
    prune(nowMs);
}

void Server::receive_packets(std::uint64_t nowMs) {
    Endpoint endpoint;
    std::vector<std::uint8_t> bytes;
    while (socket_.receive(endpoint, bytes)) {
        Connection* connection = find_connection(endpoint);
        if (connection == nullptr) connection = &create_connection(endpoint, nowMs);
        handle_packet(connection, bytes, nowMs);
        bytes.clear();
    }
}

void Server::handle_packet(Connection* connection, const std::vector<std::uint8_t>& bytes, std::uint64_t nowMs) {
    protocol::PacketHeader header{};
    protocol::Message message{};
    if (!protocol::decode_packet(bytes, header, message)) return;

    connection->reliability.acknowledge(header.ack, header.ackBits);
    if (!connection->receiveHistory.accept(header.sequence)) return;
    connection->lastReceiveMs = nowMs;

    switch (message.type) {
    case protocol::MessageType::Hello:
        handle_hello(*connection, message, nowMs);
        break;
    case protocol::MessageType::PlayerState:
        if (connection->welcomed) handle_player_state(*connection, message, nowMs);
        break;
    case protocol::MessageType::Ping:
        send_message(*connection, protocol::MessageType::Pong, protocol::Channel::Control, false, {});
        break;
    case protocol::MessageType::Goodbye:
        connection->lastReceiveMs = nowMs > config_.clientTimeoutMs ? nowMs - config_.clientTimeoutMs - 1 : 1;
        break;
    default:
        break;
    }
}

void Server::handle_hello(Connection& connection, const protocol::Message& message, std::uint64_t nowMs) {
    protocol::Hello hello{};
    if (!protocol::decode_hello(message.payload, hello)) return;

    if (hello.playerName.empty()) hello.playerName = "Player";
    if (hello.playerName.size() > 24) hello.playerName.resize(24);

    auto resumeIt = resumes_.end();
    if (!hello.sessionToken.empty()) {
        resumeIt = std::find_if(resumes_.begin(), resumes_.end(), [&](const auto& entry) {
            return entry.second.sessionToken == hello.sessionToken && entry.second.expiresAtMs >= nowMs;
        });
    }

    if (!connection.welcomed) {
        if (resumeIt != resumes_.end()) {
            connection.playerId = resumeIt->second.playerId;
            connection.sessionToken = resumeIt->second.sessionToken;
            connection.state = resumeIt->second.state;
            connection.stateInitialized = true;
            resumes_.erase(resumeIt);
        } else {
            connection.playerId = allocate_player_id();
            connection.sessionToken = make_session_token();
            connection.state.playerId = connection.playerId;
            connection.state.position = {0.0f, 0.0f, 0.0f};
            connection.state.yaw = 0.0f;
            connection.stateInitialized = false;
        }
        connection.connectionId = make_u64_token();
        connection.welcomed = true;
    }

    protocol::Welcome welcome{};
    welcome.playerId = connection.playerId;
    welcome.connectionId = connection.connectionId;
    welcome.serverTick = serverTick_;
    welcome.spawn.position = connection.state.position;
    welcome.spawn.yaw = connection.state.yaw;
    welcome.sessionToken = connection.sessionToken;
    send_message(connection, protocol::MessageType::Welcome, protocol::Channel::Control, true, protocol::encode_welcome(welcome));

    std::printf("[server] client %s joined as player %u (build=%s hash=%016llX)\n",
        connection.endpoint.key().c_str(), connection.playerId, hello.buildId.c_str(), static_cast<unsigned long long>(hello.buildTextHash));
}

void Server::handle_player_state(Connection& connection, const protocol::Message& message, std::uint64_t nowMs) {
    PlayerState incoming{};
    if (!protocol::decode_player_state(message.payload, incoming)) return;
    if (incoming.playerId != connection.playerId) return;

    const auto finite = [](float value) { return std::isfinite(value); };
    if (!finite(incoming.position.x) || !finite(incoming.position.y) || !finite(incoming.position.z) ||
        !finite(incoming.yaw) || !finite(incoming.velocity.x) || !finite(incoming.velocity.y) || !finite(incoming.velocity.z)) {
        std::printf("[security] rejected non-finite movement for player %u\n", connection.playerId);
        return;
    }

    // The first real client transform cannot be speed-checked against the synthetic
    // pre-spawn state (0,0,0). Accept one authoritative bootstrap state, then
    // enforce the normal movement window on subsequent updates.
    if (!connection.stateInitialized) {
        connection.state = incoming;
        connection.state.playerId = connection.playerId;
        connection.stateInitialized = true;
        connection.lastReceiveMs = nowMs;
        std::printf("[server] player %u authoritative state initialized at (%.2f, %.2f, %.2f)\n",
            connection.playerId, incoming.position.x, incoming.position.y, incoming.position.z);
        return;
    }

    const float maxDeltaSeconds = 0.25f;
    const float distance = std::sqrt(distance_squared(connection.state.position, incoming.position));
    const float maxDistance = config_.maxAcceptedSpeedMps * maxDeltaSeconds;
    if (distance > maxDistance) {
        std::printf("[security] rejected movement for player %u: %.2fm > %.2fm window\n", connection.playerId, distance, maxDistance);
        return;
    }

    connection.state = incoming;
    connection.state.playerId = connection.playerId;
    connection.lastReceiveMs = nowMs;
}

void Server::send_message(Connection& connection, protocol::MessageType type, protocol::Channel channel, bool reliable, const std::vector<std::uint8_t>& payload) {
    protocol::Message message{};
    message.type = type;
    message.channel = channel;
    message.reliable = reliable;
    message.payload = payload;
    if (message.payload.size() > frontier::kMaxMessageSize) return;

    protocol::PacketHeader header{};
    header.sequence = connection.reliability.next_sequence();
    header.ack = connection.receiveHistory.highest();
    header.ackBits = connection.receiveHistory.ack_bits();
    header.connectionId = connection.connectionId;

    auto packet = protocol::encode_packet(header, message);
    if (packet.empty()) return;
    if (reliable) connection.reliability.remember_reliable(header.sequence, packet, now_ms());
    socket_.send(connection.endpoint, packet);
}

void Server::broadcast_snapshots(std::uint64_t nowMs) {
    (void)nowMs;
    for (auto& [viewerKey, viewer] : connections_) {
        if (!viewer.welcomed) continue;
        protocol::Snapshot snapshot{};
        snapshot.serverTick = serverTick_;
        for (const auto& [targetKey, target] : connections_) {
            if (!target.welcomed) continue;
            if (!relevant(viewer.state, target.state)) continue;
            snapshot.players.push_back(target.state);
            (void)targetKey;
        }
        auto payload = protocol::encode_snapshot(snapshot);
        if (!payload.empty()) send_message(viewer, protocol::MessageType::Snapshot, protocol::Channel::State, false, payload);
        (void)viewerKey;
    }
}

void Server::prune(std::uint64_t nowMs) {
    for (auto it = connections_.begin(); it != connections_.end();) {
        Connection& connection = it->second;
        if (connection.lastReceiveMs != 0 && nowMs - connection.lastReceiveMs > config_.clientTimeoutMs) {
            if (connection.welcomed) {
                resumes_[it->first] = ResumeRecord{connection.playerId, connection.sessionToken, connection.state, nowMs + config_.resumeGraceMs};
                std::printf("[server] timeout player %u from %s; resume window started\n", connection.playerId, connection.endpoint.key().c_str());
            }
            it = connections_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = resumes_.begin(); it != resumes_.end();) {
        if (it->second.expiresAtMs < nowMs) it = resumes_.erase(it);
        else ++it;
    }
}

std::uint64_t Server::now_ms() const { return monotonic_ms(); }

Server::Connection* Server::find_connection(const Endpoint& endpoint) {
    auto it = connections_.find(endpoint.key());
    return it == connections_.end() ? nullptr : &it->second;
}

Server::Connection& Server::create_connection(const Endpoint& endpoint, std::uint64_t nowMs) {
    auto [it, inserted] = connections_.try_emplace(endpoint.key());
    if (inserted) {
        it->second.endpoint = endpoint;
        it->second.lastReceiveMs = nowMs;
        it->second.connectionId = make_u64_token();
    }
    return it->second;
}

std::uint16_t Server::allocate_player_id() {
    for (std::uint32_t i = 0; i < 65535; ++i) {
        const std::uint16_t candidate = nextPlayerId_++;
        if (candidate == 0) continue;
        const bool used = std::any_of(connections_.begin(), connections_.end(), [candidate](const auto& item) {
            return item.second.welcomed && item.second.playerId == candidate;
        });
        if (!used) return candidate;
    }
    return 0;
}

std::uint64_t Server::make_u64_token() {
    std::random_device device;
    std::uint64_t a = static_cast<std::uint64_t>(device());
    std::uint64_t b = static_cast<std::uint64_t>(device());
    return (a << 32U) ^ b ^ monotonic_ms();
}

std::string Server::make_session_token() {
    char buffer[33]{};
    std::snprintf(buffer, sizeof(buffer), "%016llX%016llX",
        static_cast<unsigned long long>(make_u64_token()), static_cast<unsigned long long>(make_u64_token()));
    return buffer;
}

bool Server::relevant(const PlayerState& viewer, const PlayerState& target) const {
    if (viewer.playerId == target.playerId) return true;
    return distance_squared(viewer.position, target.position) <= config_.interestRadius * config_.interestRadius;
}

} // namespace frontier::server
