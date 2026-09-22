#pragma once

#include "frontier/game/build_fingerprint.hpp"
#include "frontier/game/native_invoker.hpp"
#include "frontier/types.hpp"

#include <cstdint>
#include <string>

namespace frontier::game {

class RdrBridge final {
public:
    bool initialize(const ExecutableFingerprint& fingerprint, KnownBuild build);
    bool read_local_player_state(PlayerState& outState, std::string& error) const;
    bool local_player_pointer_available() const;
    bool try_initialize_native_invoker();
    bool native_invoker_ready() const { return nativeInvoker_.ready(); }
    const std::string& native_invoker_error() const { return nativeInvoker_.last_error(); }
    bool read_game_runtime(std::int32_t& gameState, bool& worldLoaded, bool& worldLoadedKnown,
                           bool& simulateStartMultiplayer, bool& simulateStartMultiplayerKnown,
                           bool& startPosCommandLine, bool& startPosCommandLineKnown, std::string& error) const;

    bool initialized() const { return initialized_; }
    bool local_player_symbol_resolved() const { return localPlayerStorage_ != 0; }
    bool actor_manager_symbol_resolved() const { return actorManagerSlotsStorage_ != 0; }

private:
    std::uintptr_t resolve_rip_target(std::uintptr_t instruction) const;
    bool readable(std::uintptr_t address, std::size_t size) const;
    bool read_pointer(std::uintptr_t address, std::uintptr_t& out) const;

    bool initialized_{};
    KnownBuild build_{KnownBuild::Unknown};
    std::uintptr_t moduleBase_{};
    std::uintptr_t localPlayerStorage_{};
    std::uintptr_t actorManagerSlotsStorage_{};
    NativeInvoker nativeInvoker_{};
};

} // namespace frontier::game
