#include "frontier/game/frontier_session.hpp"

#include "frontier/game/rdr_bridge.hpp"

namespace frontier::game {

namespace {
const char* state_name(FrontierSessionState state) {
    switch (state) {
    case FrontierSessionState::Booting: return "booting";
    case FrontierSessionState::WaitingForNativeInvoker: return "waiting-native-invoker";
    case FrontierSessionState::Frontend: return "frontend";
    case FrontierSessionState::WaitingForWorld: return "waiting-world";
    case FrontierSessionState::WaitingForLocalPlayer: return "waiting-local-player";
    case FrontierSessionState::Active: return "active";
    }
    return "unknown";
}
} // namespace

void FrontierSession::reset() {
    runtime_ = {};
    lastLogMs_ = 0;
}

bool FrontierSession::update(RdrBridge& bridge, std::string& logLine) {
    logLine.clear();

    if (!bridge.native_invoker_ready()) {
        const auto previous = runtime_.state;
        runtime_.state = FrontierSessionState::WaitingForNativeInvoker;
        if (bridge.try_initialize_native_invoker()) {
            logLine = "[FrontierSession] native invoker initialized";
        } else if (previous != runtime_.state) {
            logLine = "[FrontierSession] waiting for native invoker registration table";
            if (!bridge.native_invoker_error().empty()) logLine += " error=" + bridge.native_invoker_error();
        }
        return false;
    }

    FrontierRuntimeState next = runtime_;
    std::int32_t gameState = -1;
    bool worldLoaded = false;
    bool simulateMp = false;
    bool startPosCommandLine = false;
    std::string runtimeError;
    const bool gotGameState = bridge.read_game_runtime(gameState, worldLoaded, simulateMp, startPosCommandLine, runtimeError);

    next.gameState = gameState;
    next.worldLoaded = worldLoaded;
    next.simulateStartMultiplayer = simulateMp;
    next.startPositionFromCommandLine = startPosCommandLine;

    if (!gotGameState) {
        next.state = FrontierSessionState::WaitingForNativeInvoker;
    } else if (!worldLoaded) {
        next.state = FrontierSessionState::Frontend;
    } else {
        next.state = FrontierSessionState::WaitingForLocalPlayer;
        frontier::PlayerState state{};
        std::string error;
        if (bridge.read_local_player_state(state, error)) {
            next.localPlayerReady = true;
            next.state = FrontierSessionState::Active;
        } else {
            next.localPlayerReady = false;
        }
    }

    const bool changed = next.state != runtime_.state ||
                         next.gameState != runtime_.gameState ||
                         next.worldLoaded != runtime_.worldLoaded ||
                         next.simulateStartMultiplayer != runtime_.simulateStartMultiplayer ||
                         next.startPositionFromCommandLine != runtime_.startPositionFromCommandLine ||
                         next.localPlayerReady != runtime_.localPlayerReady;

    runtime_ = next;

    if (changed) {
        logLine = "[FrontierSession] state=";
        logLine += state_name(runtime_.state);
        logLine += " gameState=" + std::to_string(runtime_.gameState);
        logLine += " worldLoaded=" + std::to_string(runtime_.worldLoaded ? 1 : 0);
        logLine += " simulateStartMultiplayer=" + std::to_string(runtime_.simulateStartMultiplayer ? 1 : 0);
        logLine += " startPosCommandLine=" + std::to_string(runtime_.startPositionFromCommandLine ? 1 : 0);
        logLine += " localPlayer=" + std::to_string(runtime_.localPlayerReady ? 1 : 0);
        if (!runtimeError.empty()) {
            logLine += " nativeError=" + runtimeError;
        }
    } else if (!runtimeError.empty()) {
        logLine = "[FrontierSession] native diagnostics: " + runtimeError;
    }

    return runtime_.state == FrontierSessionState::Active;
}

} // namespace frontier::game
