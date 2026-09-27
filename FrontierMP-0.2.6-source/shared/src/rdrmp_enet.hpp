#pragma once

#include "frontier/rdrmp_wire.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct _ENetHost;
struct _ENetPeer;

namespace frontier::rdrmp {

class EnetClient final {
public:
    using WelcomeCallback = std::function<void(const PlayerCreate&)>;
    using SnapshotCallback = std::function<void(const std::vector<PlayerCreate>&)>;
    using DisconnectCallback = std::function<void(const std::string&)>;

    bool start(const std::string& host, std::uint16_t port, const std::string& playerName);
    void stop();
    void update();
    bool connected() const { return connected_; }
    bool send(DeliveryType delivery, PacketId packetId, const std::vector<std::uint8_t>& payload);
    bool submit_player_state(const PlayerState& state, std::uint32_t actorModel);

    void set_on_player_event(WelcomeCallback callback) { onPlayerEvent_ = std::move(callback); }
    void set_on_snapshot(SnapshotCallback callback) { onSnapshot_ = std::move(callback); }
    void set_on_disconnect(DisconnectCallback callback) { onDisconnect_ = std::move(callback); }

private:
    void handle_packet(const std::uint8_t* data, std::size_t size);
    void emit_snapshot();
    bool send_welcome(const PlayerState& state, std::uint32_t actorModel);

    _ENetHost* host_{};
    _ENetPeer* peer_{};
    bool initialized_{};
    bool connected_{};
    bool welcomeSent_{};
    std::string playerName_;
    std::uint16_t port_{};
    std::uint16_t localPlayerId_{};
    std::unordered_map<std::uint16_t, PlayerCreate> players_;
    WelcomeCallback onPlayerEvent_;
    SnapshotCallback onSnapshot_;
    DisconnectCallback onDisconnect_;
};

} // namespace frontier::rdrmp
