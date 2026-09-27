#include "frontier/server/rdrmp_server.hpp"

#include <enet/enet.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace frontier::server {

namespace {

std::vector<std::uint8_t> add_packet_id(frontier::rdrmp::PacketId packetId,
                                         const std::vector<std::uint8_t>& payload) {
    frontier::rdrmp::Writer writer;
    writer.u16(static_cast<std::uint16_t>(packetId));
    std::vector<std::uint8_t> result = writer.data();
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

bool split_packet(const std::uint8_t* data, std::size_t size,
                  frontier::rdrmp::PacketId& packetId,
                  std::vector<std::uint8_t>& payload) {
    if (size < 2) return false;
    packetId = static_cast<frontier::rdrmp::PacketId>(
        (static_cast<std::uint16_t>(data[0]) << 8U) | data[1]);
    payload.assign(data + 2, data + size);
    return true;
}

bool send_packet(_ENetPeer* peer, frontier::rdrmp::DeliveryType delivery,
                 frontier::rdrmp::PacketId packetId,
                 const std::vector<std::uint8_t>& payload) {
    if (peer == nullptr) return false;
    const auto bytes = add_packet_id(packetId, payload);
    const enet_uint32 flags =
        delivery == frontier::rdrmp::DeliveryType::Reliable
            ? ENET_PACKET_FLAG_RELIABLE
            : ENET_PACKET_FLAG_UNSEQUENCED;
    ENetPacket* packet = enet_packet_create(bytes.data(), bytes.size(), flags);
    if (packet == nullptr) return false;
    const enet_uint8 channel =
        delivery == frontier::rdrmp::DeliveryType::Reliable ? 0U : 1U;
    return enet_peer_send(peer, channel, packet) == 0;
}

} // namespace

RdrmpServer::RdrmpServer(Config config) : config_(config) {}

RdrmpServer::~RdrmpServer() {
    stop();
}

bool RdrmpServer::start() {
    if (enet_initialize() != 0) return false;

    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = config_.port;

    host_ = enet_host_create(&address, config_.maxPlayers, 2, 0, 0);
    if (host_ == nullptr) {
        enet_deinitialize();
        return false;
    }

    std::printf("[rdrmp] ENet server listening on UDP %u\n", config_.port);
    return true;
}

void RdrmpServer::stop() {
    if (host_ == nullptr) return;

    for (auto& entry : players_) {
        if (entry.second.peer != nullptr) enet_peer_disconnect_now(entry.second.peer, 0);
    }
    players_.clear();
    enet_host_destroy(host_);
    host_ = nullptr;
    enet_deinitialize();
}

void RdrmpServer::run() {
    using namespace std::chrono_literals;
    while (host_ != nullptr) {
        tick();
        std::this_thread::sleep_for(5ms);
    }
}

void RdrmpServer::tick() {
    if (host_ == nullptr) return;

    ENetEvent event{};
    while (enet_host_service(host_, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            handle_connect(event.peer);
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            if (event.packet != nullptr) {
                handle_receive(event.peer, event.packet->data, event.packet->dataLength);
                enet_packet_destroy(event.packet);
            }
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            handle_disconnect(event.peer);
            break;
        default:
            break;
        }
    }
    enet_host_flush(host_);
}

void RdrmpServer::handle_connect(_ENetPeer* peer) {
    std::printf("[rdrmp] peer connected\n");
    (void)peer;
}

RdrmpServer::Player* RdrmpServer::find_player(_ENetPeer* peer) {
    for (auto& entry : players_) {
        if (entry.second.peer == peer) return &entry.second;
    }
    return nullptr;
}

RdrmpServer::Player* RdrmpServer::find_player(std::uint16_t id) {
    auto it = players_.find(id);
    return it == players_.end() ? nullptr : &it->second;
}

std::uint16_t RdrmpServer::allocate_player_id() const {
    for (std::uint32_t candidate = 1; candidate <= 0xFFFFU; ++candidate) {
        if (players_.find(static_cast<std::uint16_t>(candidate)) == players_.end()) {
            return static_cast<std::uint16_t>(candidate);
        }
    }
    return 0;
}

bool RdrmpServer::send_player(_ENetPeer* peer, const Player& player) {
    return send_packet(peer, frontier::rdrmp::DeliveryType::Reliable,
                       frontier::rdrmp::PacketId::PlayerCreate,
                       frontier::rdrmp::encode_player_create(player.state));
}

bool RdrmpServer::send_remove(_ENetPeer* peer, std::uint16_t playerId) {
    return send_packet(peer, frontier::rdrmp::DeliveryType::Reliable,
                       frontier::rdrmp::PacketId::PlayerRemove,
                       frontier::rdrmp::encode_player_remove({playerId}));
}

bool RdrmpServer::send_transform(_ENetPeer* peer, const Player& player) {
    frontier::rdrmp::PlayerTransform transform{};
    transform.playerId = player.id;
    transform.position = player.state.position;
    transform.rotation = player.state.rotation;
    return send_packet(peer, frontier::rdrmp::DeliveryType::Unreliable,
                       frontier::rdrmp::PacketId::PlayerTransform,
                       frontier::rdrmp::encode_player_transform(transform));
}

bool RdrmpServer::broadcast_create(const Player& player, _ENetPeer* except) {
    for (const auto& entry : players_) {
        if (entry.second.active && entry.second.peer != except) {
            if (!send_player(entry.second.peer, player)) return false;
        }
    }
    return true;
}

bool RdrmpServer::broadcast_remove(std::uint16_t playerId, _ENetPeer* except) {
    for (const auto& entry : players_) {
        if (entry.second.active && entry.second.peer != except) {
            if (!send_remove(entry.second.peer, playerId)) return false;
        }
    }
    return true;
}

bool RdrmpServer::broadcast_transform(const Player& player, _ENetPeer* except) {
    for (const auto& entry : players_) {
        if (entry.second.active && entry.second.peer != except) {
            if (!send_transform(entry.second.peer, player)) return false;
        }
    }
    return true;
}

bool RdrmpServer::broadcast_event(const std::vector<std::uint8_t>& eventPayload) {
    bool ok = true;
    for (const auto& entry : players_) {
        if (entry.second.active) {
            ok = send_packet(entry.second.peer,
                             frontier::rdrmp::DeliveryType::Reliable,
                             frontier::rdrmp::PacketId::ServerEvent,
                             eventPayload) && ok;
        }
    }
    return ok;
}

void RdrmpServer::handle_receive(
    _ENetPeer* peer, const std::uint8_t* data, std::size_t size) {
    frontier::rdrmp::PacketId packetId{};
    std::vector<std::uint8_t> payload;
    if (!split_packet(data, size, packetId, payload)) return;

    switch (packetId) {
    case frontier::rdrmp::PacketId::ClientWelcome: {
        if (find_player(peer) != nullptr) return;
        if (players_.size() >= config_.maxPlayers) {
            enet_peer_disconnect(peer, 1);
            return;
        }

        frontier::rdrmp::ClientWelcome welcome{};
        if (!frontier::rdrmp::decode_client_welcome(payload, welcome)) return;

        const std::uint16_t id = allocate_player_id();
        if (id == 0) {
            enet_peer_disconnect(peer, 2);
            return;
        }

        Player player{};
        player.id = id;
        player.peer = peer;
        player.welcome = welcome;
        player.active = true;
        player.state.playerId = id;
        player.state.actorModel = welcome.actorModel;
        player.state.name = welcome.name;
        player.state.position = welcome.position;
        player.state.rotation = welcome.rotation;

        players_.emplace(id, player);
        Player& stored = players_.at(id);

        // The historical client receives packet 7 as the player-entry creation
        // message. Send the newly assigned local player first so a compatible
        // client can associate the first create packet with its own identity.
        send_player(peer, stored);
        for (const auto& existing : players_) {
            if (existing.second.active && existing.first != id) {
                send_player(peer, existing.second);
            }
        }
        broadcast_create(stored, peer);

        std::printf("[rdrmp] player %u joined name=%s model=%u\n",
                    static_cast<unsigned>(id),
                    stored.state.name.c_str(),
                    stored.state.actorModel);
        break;
    }
    case frontier::rdrmp::PacketId::ClientPlayerState: {
        Player* player = find_player(peer);
        if (player == nullptr) return;

        frontier::rdrmp::ClientPlayerState transform{};
        if (!frontier::rdrmp::decode_client_player_state(payload, transform)) return;
        player->state.position = transform.position;
        player->state.rotation = transform.rotation;
        broadcast_transform(*player, peer);
        break;
    }
    case frontier::rdrmp::PacketId::ClientEvent:
        if (find_player(peer) != nullptr) broadcast_event(payload);
        break;
    default:
        break;
    }
}

void RdrmpServer::handle_disconnect(_ENetPeer* peer) {
    Player* player = find_player(peer);
    if (player == nullptr) return;

    const std::uint16_t id = player->id;
    players_.erase(id);
    broadcast_remove(id, peer);
    std::printf("[rdrmp] player %u left\n", static_cast<unsigned>(id));
}

} // namespace frontier::server
