#include "frontier/client/interpolation.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>

int main() {
    frontier::client::RemoteEntityInterpolator interp(2, 4);
    frontier::protocol::Snapshot a{};
    a.serverTick = 10;
    a.players.push_back(frontier::PlayerState{1, 1, {0, 0, 0}, 0, {10, 0, 0}, 0, 0});
    frontier::protocol::Snapshot b{};
    b.serverTick = 12;
    b.players.push_back(frontier::PlayerState{1, 2, {2, 4, 6}, 1, {10, 0, 0}, 0, 0});
    interp.push_snapshot(a);
    interp.push_snapshot(b);

    auto mid = interp.sample(1, 13);
    assert(mid.has_value());
    assert(!mid->extrapolated);
    assert(std::fabs(mid->state.position.x - 1.0f) < 0.001f);
    assert(std::fabs(mid->state.position.y - 2.0f) < 0.001f);

    auto future = interp.sample(1, 16);
    assert(future.has_value());
    assert(future->extrapolated);

    std::printf("FrontierMP interpolation tests: OK\n");
    return 0;
}
