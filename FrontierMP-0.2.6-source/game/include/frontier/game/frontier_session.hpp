#pragma once

#include <cstdint>
#include <string>

namespace frontier::game {

class RdrBridge;

enum class FrontierSessionState : std::uint8_t {
    Booting,
    WaitingForNativeInvoker,
    Frontend,
    WaitingForWorld,
    RuntimeQueryFailed,
    WaitingForLocalPlayer,
    Active
};

struct FrontierRuntimeState final {
    FrontierSessionState state{FrontierSessionState::Booting};
    std::int32_t gameState{-1};
    bool worldLoaded{};
    bool simulateStartMultiplayer{};
    bool startPositionFromCommandLine{};
    bool localPlayerReady{};
};

class FrontierSession final {
public:
    void reset();
    bool update(RdrBridge& bridge, std::string& logLine);
    const FrontierRuntimeState& runtime_state() const { return runtime_; }

private:
    FrontierRuntimeState runtime_{};
    std::uint64_t lastLogMs_{};
};

} // namespace frontier::game
