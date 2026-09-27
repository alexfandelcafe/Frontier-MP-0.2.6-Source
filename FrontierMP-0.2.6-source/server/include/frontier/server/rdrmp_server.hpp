#pragma once

#include "frontier/rdrmp_wire.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct _ENetHost;
struct _ENetPeer;

namespace frontier::server {

class RdrmpServer final {
public:
    struct Config final {
        std::uint16_t port{4674};
        std::uint32_t maxPlayers{32};
    };

    explicit RdrmpServer(Config config);
    ~RdrmpServer();

    bool start();
    void stop();
    void run();

private:
    struct Player final {
        std::uint16_t id{};
        _ENetPeer* peer{};
        frontier::rdrmp::ClientWelcome welcome{};
        frontier::rdrmp::PlayerCreate state{};
        bool active{};
    };

    void tick();
    void handle_receive(_ENetPeer* peer, const std::uint8_t* data, std::size_t size);
    void handle_connect(_ENetPeer* peer);
    void handle_disconnect(_ENetPeer* peer);
    Player* find_player(_ENetPeer* peer);
    Player* find_player(std::uint16_t id);
    std::uint16_t allocate_player_id() const;

    bool send_player(_ENetPeer* peer, const Player& player);
    bool send_remove(_ENetPeer* peer, std::uint16_t playerId);
    bool send_transform(_ENetPeer* peer, const Player& player);
    bool broadcast_create(const Player& player, _ENetPeer* except);
    bool broadcast_remove(std::uint16_t playerId, _ENetPeer* except);
    bool broadcast_transform(const Player& player, _ENetPeer* except);
    bool broadcast_event(const std::vector<std::uint8_t>& eventPayload);

    Config config_{};
    _ENetHost* host_{};
    std::unordered_map<std::uint16_t, Player> players_;
};

} // namespace frontier::server
