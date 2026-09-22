#include "frontier/client/network_client.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    const std::string name = argc > 1 ? argv[1] : "SimPlayer";
    const std::string host = argc > 2 ? argv[2] : "127.0.0.1";
    const std::uint16_t port = argc > 3 ? static_cast<std::uint16_t>(std::strtoul(argv[3], nullptr, 10)) : 30120;

    frontier::client::NetworkClient client;
    client.set_on_welcome([](const auto& welcome) {
        std::printf("[client] connected: playerId=%u serverTick=%u spawn=(%.1f, %.1f, %.1f)\n",
            welcome.playerId, welcome.serverTick, welcome.spawn.position.x, welcome.spawn.position.y, welcome.spawn.position.z);
    });
    client.set_on_snapshot([](const auto& snapshot) {
        std::printf("[client] snapshot tick=%u players=%zu", snapshot.serverTick, snapshot.players.size());
        for (const auto& player : snapshot.players) std::printf(" [id=%u x=%.1f y=%.1f]", player.playerId, player.position.x, player.position.y);
        std::printf("\n");
    });
    client.set_on_disconnect([](const std::string& reason) { std::printf("[client] %s\n", reason.c_str()); });

    if (!client.start(host, port, name, "simulator", 0)) return 1;

    auto start = std::chrono::steady_clock::now();
    auto nextState = start;
    while (true) {
        const auto now = std::chrono::steady_clock::now();
        const auto ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
        client.update(ms);

        if (now >= nextState && client.state() == frontier::client::ConnectionState::Connected) {
            const float t = std::chrono::duration<float>(now - start).count();
            frontier::PlayerState state{};
            state.position.x = std::cos(t * 0.4f) * 10.0f + (name == "Player2" ? 15.0f : 0.0f);
            state.position.y = std::sin(t * 0.4f) * 10.0f;
            state.position.z = 0.0f;
            state.yaw = t * 0.4f;
            state.velocity = {-std::sin(t * 0.4f) * 4.0f, std::cos(t * 0.4f) * 4.0f, 0.0f};
            state.gait = 2;
            client.submit_player_state(state);
            nextState += std::chrono::milliseconds(50);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
