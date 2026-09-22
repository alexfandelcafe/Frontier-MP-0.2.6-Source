#include "frontier/protocol.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>

int main() {
    using namespace frontier;
    using namespace frontier::protocol;

    Hello hello{"John", "test-build", 0x123456789ABCDEF0ULL, "session"};
    auto helloBytes = encode_hello(hello);
    Hello helloOut{};
    assert(decode_hello(helloBytes, helloOut));
    assert(helloOut.playerName == hello.playerName);
    assert(helloOut.buildTextHash == hello.buildTextHash);

    PlayerState state{};
    state.playerId = 7;
    state.clientTick = 42;
    state.position = {1.0f, 2.0f, 3.0f};
    state.yaw = 1.5f;
    state.velocity = {4.0f, 5.0f, 6.0f};
    state.gait = 2;
    state.flags = 3;
    auto stateBytes = encode_player_state(state);
    PlayerState stateOut{};
    assert(decode_player_state(stateBytes, stateOut));
    assert(stateOut.playerId == 7 && std::fabs(stateOut.position.z - 3.0f) < 0.001f);

    Snapshot snap{};
    snap.serverTick = 900;
    snap.players = {state, PlayerState{8, 43, {9, 8, 7}, 0.5f, {1, 1, 1}, 3, 0}};
    auto snapBytes = encode_snapshot(snap);
    Snapshot snapOut{};
    assert(decode_snapshot(snapBytes, snapOut));
    assert(snapOut.players.size() == 2);
    assert(snapOut.players[1].playerId == 8);

    ReceiveHistory history;
    assert(history.accept(1));
    assert(history.accept(2));
    assert(!history.accept(1));
    assert(history.highest() == 2);

    std::printf("FrontierMP protocol tests: OK\n");
    return 0;
}
