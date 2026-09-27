#include "frontier/rdrmp_enet.hpp"

#include <enet/enet.h>

#include <algorithm>
#include <cstring>

namespace frontier::rdrmp {

namespace {

std::vector<std::uint8_t> add_packet_id(PacketId packetId, const std::vector<std::uint8_t>& payload) {
    Writer writer;
    writer.u16(static_cast<std::uint16_t>(packetId));
    std::vector<std::uint8_t> result = writer.data();
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

bool split_packet(const std::uint8_t* data, std::size_t size,
                  PacketId& packetId, std::vector<std::uint8_t>& payload) {
    if (size < 2) return false;
    const std::uint16_t id = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[0]) << 8U) | data[1]);
    packetId = static_cast<PacketId>(id);
    payload.assign(data + 2, data + size);
    return true;
}

ENetPacket* make_packet(DeliveryType delivery, const std::vector<std::uint8_t>& bytes) {
    const enet_uint32 flags =
        delivery == DeliveryType::Reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED;
    return enet_packet_create(bytes.data(), bytes.size(), flags);
}

} // namespace

bool EnetClient::start(const std::string& hostName, std::uint16_t port, const std::string& playerName) {
    stop();

    if (enet_initialize() != 0) return false;
    initialized_ = true;

    host_ = enet_host_create(nullptr, 1, 2, 0, 0);
    if (host_ == nullptr) {
        stop();
        return false;
    }

    ENetAddress address{};
    if (enet_address_set_host(&address, hostName.c_str()) != 0) {
        stop();
        return false;
    }
    address.port = port;

    peer_ = enet_host_connect(host_, &address, 2, 0);
    if (peer_ == nullptr) {
        stop();
        return false;
    }

    playerName_ = playerName;
    port_ = port;
    localPlayerId_ = 0;
    welcomeSent_ = false;
    connected_ = false;
    players_.clear();
    return true;
}

void EnetClient::stop() {
    if (peer_ != nullptr) {
        enet_peer_disconnect(peer_, 0);
        ENetEvent event{};
        for (int i = 0; i < 4; ++i) {
            if (host_ == nullptr || enet_host_service(host_, &event, 50) <= 0) break;
            if (event.type == ENET_EVENT_TYPE_DISCONNECT) break;
        }
        peer_ = nullptr;
    }
    if (host_ != nullptr) {
        enet_host_destroy(host_);
        host_ = nullptr;
    }
    connected_ = false;
    welcomeSent_ = false;
    players_.clear();
    localPlayerId_ = 0;
    if (initialized_) {
        enet_deinitialize();
        initialized_ = false;
    }
}

void EnetClient::update() {
    if (host_ == nullptr) return;

    ENetEvent event{};
    while (enet_host_service(host_, &event, 0) > 0) {
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT:
            connected_ = true;
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            if (event.packet != nullptr) {
                handle_packet(event.packet->data, event.packet->dataLength);
                enet_packet_destroy(event.packet);
            }
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            connected_ = false;
            peer_ = nullptr;
            if (onDisconnect_) onDisconnect_("RDRMP ENet connection closed");
            break;
        default:
            break;
        }
    }

    enet_host_flush(host_);
}

bool EnetClient::send(
    DeliveryType delivery,
    PacketId packetId,
    const std::vector<std::uint8_t>& payload) {
    if (!connected_ || peer_ == nullptr) return false;

    const auto bytes = add_packet_id(packetId, payload);
    if (bytes.size() > 0xFFFFU) return false;

    ENetPacket* packet = make_packet(delivery, bytes);
    if (packet == nullptr) return false;

    const enet_uint8 channel = delivery == DeliveryType::Reliable ? 0U : 1U;
    if (enet_peer_send(peer_, channel, packet) != 0) {
        enet_packet_destroy(packet);
        return false;
    }
    return true;
}

bool EnetClient::send_welcome(const PlayerState& state, std::uint32_t actorModel) {
    ClientWelcome welcome{};
    welcome.actorModel = actorModel;
    welcome.name = playerName_;
    welcome.position = state.position;
    welcome.rotation = {0.0f, state.yaw, 0.0f};
    return send(DeliveryType::Reliable, PacketId::ClientWelcome,
                encode_client_welcome(welcome));
}

bool EnetClient::submit_player_state(const PlayerState& state, std::uint32_t actorModel) {
    if (!connected_) return false;
    if (!welcomeSent_) {
        if (!send_welcome(state, actorModel)) return false;
        welcomeSent_ = true;
    }

    ClientPlayerState transform{};
    transform.position = state.position;
    transform.rotation = {0.0f, state.yaw, 0.0f};
    return send(DeliveryType::Unreliable, PacketId::ClientPlayerState,
                encode_client_player_state(transform));
}

void EnetClient::handle_packet(const std::uint8_t* data, std::size_t size) {
    PacketId packetId{};
    std::vector<std::uint8_t> payload;
    if (!split_packet(data, size, packetId, payload)) return;

    switch (packetId) {
    case PacketId::PlayerCreate: {
        PlayerCreate player{};
        if (!decode_player_create(payload, player) || player.playerId == 0) return;
        players_[player.playerId] = player;
        if (localPlayerId_ == 0) {
            localPlayerId_ = player.playerId;
            if (onPlayerEvent_) onPlayerEvent_(player);
        } else if (player.playerId == localPlayerId_ && onPlayerEvent_) {
            onPlayerEvent_(player);
        }
        emit_snapshot();
        break;
    }
    case PacketId::PlayerRemove: {
        PlayerRemove player{};
        if (!decode_player_remove(payload, player)) return;
        players_.erase(player.playerId);
        emit_snapshot();
        break;
    }
    case PacketId::PlayerTransform: {
        PlayerTransform transform{};
        if (!decode_player_transform(payload, transform)) return;
        auto it = players_.find(transform.playerId);
        if (it == players_.end()) return;
        it->second.position = transform.position;
        it->second.rotation = transform.rotation;
        emit_snapshot();
        break;
    }
    case PacketId::PlayerProperty:
        break;
    case PacketId::ServerEvent:
    case PacketId::TimeOfDay:
    case PacketId::TextChat:
    default:
        break;
    }
}

void EnetClient::emit_snapshot() {
    if (!onSnapshot_) return;
    std::vector<PlayerCreate> snapshot;
    snapshot.reserve(players_.size());
    for (const auto& item : players_) snapshot.push_back(item.second);
    std::sort(snapshot.begin(), snapshot.end(),
              [](const PlayerCreate& a, const PlayerCreate& b) {
                  return a.playerId < b.playerId;
              });
    onSnapshot_(snapshot);
}

} // namespace frontier::rdrmp
