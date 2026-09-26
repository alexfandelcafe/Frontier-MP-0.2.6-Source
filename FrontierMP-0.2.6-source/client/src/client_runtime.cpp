#include "frontier/client/client_runtime.hpp"
#include "frontier/client/network_client.hpp"
#include "frontier/game/build_fingerprint.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <iomanip>
#include <cmath>

namespace frontier::client {

namespace {
std::unique_ptr<NetworkClient> g_network;

std::uint64_t monotonic_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void log_line(const std::string& line) {
    char localAppData[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;

    std::filesystem::path dir = std::filesystem::path(localAppData) / "FrontierMP" / "logs";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream file(dir / "client.log", std::ios::app);
    if (file) file << line << "\n";
}

std::string environment_value(const char* key) {
    char buffer[512]{};
    const DWORD n = GetEnvironmentVariableA(key, buffer, sizeof(buffer));
    if (n == 0 || n >= sizeof(buffer)) return {};
    return std::string(buffer, buffer + n);
}

} // namespace

bool ClientRuntime::initialize(const std::string& host, std::uint16_t port, const std::string& playerName) {
    stopRequested_.store(false, std::memory_order_release);
    lastFrontendBootstrapAttemptMs_ = 0;
    historicalOnlineBootstrapLogged_ = false;
    nativeUiBootstrapEnabled_ = environment_value("FRONTIER_NATIVE_UI_BOOTSTRAP") == "1";
    localSpawnPoint_ = {};
    lastLocalPlayerSpawnAttemptMs_ = 0;
    localPlayerSpawnReady_ = false;
    frontier::game::ExecutableFingerprint fingerprint{};
    if (!frontier::game::BuildDetector::inspect_loaded_module(fingerprint)) {
        log_line("[FrontierClient] build inspection failed");
        return false;
    }

    const auto build = frontier::game::BuildDetector::match(fingerprint);
    if (build == frontier::game::KnownBuild::Unknown) {
        std::ostringstream message;
        message << "[FrontierClient] unsupported build timestamp=0x" << std::hex << fingerprint.peTimestamp
                << " image=0x" << fingerprint.imageSize
                << " textHash=0x" << fingerprint.textHash
                << " version=" << fingerprint.fileVersion;
        log_line(message.str());
        return false;
    }

    debugRemoteTestEnabled_ = environment_value("FRONTIER_REMOTE_TEST") == "1";

    if (debugRemoteTestEnabled_) {
        log_line("[FrontierClient] solo remote-player test enabled");
    }
    log_line(std::string("[FrontierClient] native UI bootstrap ") +
             (nativeUiBootstrapEnabled_ ? "enabled" : "disabled (CEF/frontend-owned mode)"));

    if (!gameBridge_.initialize(fingerprint, build)) {
        log_line("[FrontierClient] game bridge initialization failed; networking will continue without local state replication");
    } else {
        log_line("[FrontierClient] game bridge initialized; local-player symbol and actor-manager paths resolved");
    }

    g_network = std::make_unique<NetworkClient>();
    g_network->set_on_welcome([this](const auto& welcome) {
        localPlayerId_ = welcome.playerId;
        localSpawnPoint_ = welcome.spawn;
        lastLocalPlayerSpawnAttemptMs_ = 0;
        localPlayerSpawnReady_ = false;
        gameBridge_.reset_local_player_spawn();
        remotePlayers_.set_local_player_id(localPlayerId_);

        std::ostringstream message;
        message << "[FrontierClient] connected as player " << welcome.playerId
                << " serverTick=" << welcome.serverTick;
        log_line(message.str());
    });
    g_network->set_on_snapshot([this](const auto& snapshot) {
        protocol::Snapshot effectiveSnapshot = snapshot;

        if (debugRemoteTestEnabled_ && localPlayerId_ != 0) {
            constexpr std::uint16_t kSoloRemotePlayerId = 65000;
            constexpr float kTestSpeed = 2.4f; // m/s
            constexpr float kPi = 3.14159265358979323846f;

            // Controlled locomotion test cycle:
            //   0-6s  : walk +X
            //   6-8s  : stop
            //   8-14s : walk -X
            //  14-16s : stop
            //  16-22s : walk +Z
            //  22-24s : stop
            //  24-30s : walk -Z
            //  30-32s : stop, then repeat.
            constexpr float kMoveDurationSeconds = 6.0f;
            constexpr float kStopDurationSeconds = 2.0f;
            constexpr float kCycleSeconds =
                (kMoveDurationSeconds + kStopDurationSeconds) * 4.0f;

            const auto localIt = std::find_if(
                effectiveSnapshot.players.begin(),
                effectiveSnapshot.players.end(),
                [this](const auto& player) {
                    return player.playerId == localPlayerId_;
                });

            if (localIt != effectiveSnapshot.players.end()) {
                const float seconds =
                    static_cast<float>(effectiveSnapshot.serverTick) /
                    static_cast<float>(frontier::kServerTickRate);
                const float cycleTime = std::fmod(seconds, kCycleSeconds);

                PlayerState fake{};
                fake.playerId = kSoloRemotePlayerId;
                fake.clientTick = effectiveSnapshot.serverTick;
                fake.position = localIt->position;

                float offsetX = 0.0f;
                float offsetZ = 0.0f;
                float velocityX = 0.0f;
                float velocityZ = 0.0f;
                float yaw = 0.0f;
                const char* phase = "stop";

                const float move0End = kMoveDurationSeconds;
                const float stop0End = move0End + kStopDurationSeconds;
                const float move1End = stop0End + kMoveDurationSeconds;
                const float stop1End = move1End + kStopDurationSeconds;
                const float move2End = stop1End + kMoveDurationSeconds;
                const float stop2End = move2End + kStopDurationSeconds;
                const float move3End = stop2End + kMoveDurationSeconds;

                if (cycleTime < move0End) {
                    offsetX = kTestSpeed * cycleTime;
                    velocityX = kTestSpeed;
                    yaw = 90.0f;
                    phase = "walk+X";
                } else if (cycleTime < stop0End) {
                    offsetX = kTestSpeed * kMoveDurationSeconds;
                    yaw = 90.0f;
                    phase = "stop@+X";
                } else if (cycleTime < move1End) {
                    const float t = cycleTime - stop0End;
                    offsetX = kTestSpeed * (kMoveDurationSeconds - t);
                    velocityX = -kTestSpeed;
                    yaw = 270.0f;
                    phase = "walk-X";
                } else if (cycleTime < stop1End) {
                    yaw = 270.0f;
                    phase = "stop@-X";
                } else if (cycleTime < move2End) {
                    const float t = cycleTime - stop1End;
                    offsetZ = kTestSpeed * t;
                    velocityZ = kTestSpeed;
                    yaw = 0.0f;
                    phase = "walk+Z";
                } else if (cycleTime < stop2End) {
                    offsetZ = kTestSpeed * kMoveDurationSeconds;
                    yaw = 0.0f;
                    phase = "stop@+Z";
                } else if (cycleTime < move3End) {
                    const float t = cycleTime - stop2End;
                    offsetZ = kTestSpeed * (kMoveDurationSeconds - t);
                    velocityZ = -kTestSpeed;
                    yaw = 180.0f;
                    phase = "walk-Z";
                } else {
                    yaw = 180.0f;
                    phase = "stop@-Z";
                }

                fake.position.x += offsetX;
                fake.position.z += offsetZ;
                fake.velocity = {velocityX, 0.0f, velocityZ};
                fake.yaw = yaw;

                effectiveSnapshot.players.push_back(fake);

                static const char* lastPhase = nullptr;
                if (lastPhase != phase) {
                    std::ostringstream phaseMessage;
                    phaseMessage << "[FrontierClient] locomotion test phase=" << phase
                                 << " cycleTime=" << cycleTime
                                 << " playerId=" << kSoloRemotePlayerId;
                    log_line(phaseMessage.str());
                    lastPhase = phase;
                }
            }
        }

        remotePlayers_.on_snapshot(effectiveSnapshot, monotonic_ms());

        std::ostringstream message;
        message << "[FrontierClient] snapshot tick=" << effectiveSnapshot.serverTick
                << " players=" << effectiveSnapshot.players.size()
                << " remoteEntities=" << remotePlayers_.size();
        log_line(message.str());
    });
    g_network->set_on_disconnect([this](const std::string& reason) {
        if (gameBridge_.initialized()) {
            remotePlayers_.clear(gameBridge_);
        }
        localPlayerId_ = 0;
        localSpawnPoint_ = {};
        lastLocalPlayerSpawnAttemptMs_ = 0;
        localPlayerSpawnReady_ = false;
        gameBridge_.reset_local_player_spawn();
        log_line("[FrontierClient] " + reason);
    });

    connected_ = g_network->start(host, port, playerName, frontier::game::build_label(build), fingerprint.textHash);
    if (!connected_) {
        log_line("[FrontierClient] network initialization failed");
        return false;
    }
    log_line("[FrontierClient] initialized sessionMode=" + sessionMode_);
    return true;
}

bool ClientRuntime::initialize_from_process_command_line() {
    sessionMode_ = environment_value("FRONTIER_SESSION_MODE");
    if (sessionMode_.empty()) sessionMode_ = "freeroam";

    const std::string host = environment_value("FRONTIER_SERVER");
    const std::string portText = environment_value("FRONTIER_PORT");
    const std::string playerName = environment_value("FRONTIER_NAME");

    std::uint32_t parsedPort = 30120;
    if (!portText.empty()) {
        try { parsedPort = static_cast<std::uint32_t>(std::stoul(portText)); }
        catch (...) { parsedPort = 30120; }
    }
    if (parsedPort > 65535u) parsedPort = 30120;

    localPlayerActorModel_ = frontier::kDefaultPlayerActorModel;
    const std::string modelText = environment_value("FRONTIER_PLAYER_ACTOR_MODEL");
    if (!modelText.empty()) {
        try {
            const auto parsedModel = std::stoul(modelText, nullptr, 0);
            if (parsedModel <= 0xFFFFFFFFu) {
                localPlayerActorModel_ = static_cast<std::uint32_t>(parsedModel);
            }
        } catch (...) {
            localPlayerActorModel_ = frontier::kDefaultPlayerActorModel;
        }
    }

    return initialize(host.empty() ? "127.0.0.1" : host,
                      static_cast<std::uint16_t>(parsedPort),
                      playerName.empty() ? "Player" : playerName);
}

void ClientRuntime::run_loop() {
    while (!stopRequested_.load(std::memory_order_relaxed)) {
        update();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void ClientRuntime::shutdown() {
    stopRequested_.store(true, std::memory_order_relaxed);
    if (g_network) g_network->stop();
    if (gameBridge_.initialized()) {
        remotePlayers_.clear(gameBridge_);
    }
    localPlayerId_ = 0;
    g_network.reset();
    connected_ = false;
    clientTick_ = 0;
    lastStateSendMs_ = 0;
    lastBridgeLogMs_ = 0;
    bridgeStateReady_ = false;
    session_.reset();
    lastSessionUpdateMs_ = 0;
    lastFrontendBootstrapAttemptMs_ = 0;
}

void ClientRuntime::update() {
    // The runtime has both an internal worker and an exported Update() entry
    // point. Serialize them so RemotePlayerManager state cannot race.
    if (updateInProgress_.exchange(true, std::memory_order_acquire)) return;
    struct UpdateGuard final {
        std::atomic<bool>& flag;
        ~UpdateGuard() { flag.store(false, std::memory_order_release); }
    } updateGuard{updateInProgress_};

    const auto now = monotonic_ms();
    if (g_network) {
        g_network->update(now);

        if (gameBridge_.initialized() && (lastSessionUpdateMs_ == 0 || now - lastSessionUpdateMs_ >= 250)) {
            std::string sessionLog;
            session_.update(gameBridge_, sessionLog);
            if (!sessionLog.empty()) log_line(sessionLog);
            lastSessionUpdateMs_ = now;

            // The custom historical boot.sc flow is driven from StartScreen1.
            // Do not emit net.EnterOnlineForInvite while the native frontend is
            // still being constructed; the stock event dispatcher can otherwise
            // consume the event before the UI state machine exists.
            if (nativeUiBootstrapEnabled_ &&
                sessionMode_ == "freeroam" &&
                !historicalOnlineBootstrapLogged_ &&
                session_.runtime_state().state !=
                    frontier::game::FrontierSessionState::RuntimeQueryFailed) {
                std::string bootstrapLog;
                const bool bootstrapComplete =
                    gameBridge_.advance_historical_online_bootstrap(bootstrapLog);

                if (!bootstrapLog.empty()) {
                    log_line(bootstrapLog);
                }
                if (bootstrapComplete) {
                    historicalOnlineBootstrapLogged_ = true;
                }
            }
        }

        if (g_network->state() == ConnectionState::Connected &&
            gameBridge_.initialized() &&
            session_.runtime_state().worldLoadedStable &&
            localPlayerId_ != 0 &&
            !localPlayerSpawnReady_ &&
            (lastLocalPlayerSpawnAttemptMs_ == 0 ||
             now - lastLocalPlayerSpawnAttemptMs_ >= 250)) {
            lastLocalPlayerSpawnAttemptMs_ = now;

            frontier::PlayerState spawnState{};
            spawnState.playerId = localPlayerId_;
            spawnState.position = localSpawnPoint_.position;
            spawnState.yaw = localSpawnPoint_.yaw;

            bool spawnReady = false;
            std::string spawnError;
            const bool spawnCallCompleted = gameBridge_.ensure_local_player(
                localPlayerId_,
                spawnState,
                localPlayerActorModel_,
                spawnReady,
                spawnError);

            if (spawnReady) {
                localPlayerSpawnReady_ = true;
                log_line("[FrontierClient] historical local-player spawn chain reached actor");
            } else if (!spawnCallCompleted &&
                       (lastBridgeLogMs_ == 0 || now - lastBridgeLogMs_ >= 1000)) {
                log_line("[FrontierClient] local-player spawn pending: " +
                         (spawnError.empty() ? std::string("waiting for RAGE player actor")
                                             : spawnError));
                lastBridgeLogMs_ = now;
            }
        }

        if (g_network->state() == ConnectionState::Connected && gameBridge_.initialized() &&
            (lastStateSendMs_ == 0 || now - lastStateSendMs_ >= 50)) {
            PlayerState state{};
            std::string bridgeError;
            if (gameBridge_.read_local_player_state(state, bridgeError)) {
                state.clientTick = ++clientTick_;
                g_network->submit_player_state(state);
                lastStateSendMs_ = now;
                if (!bridgeStateReady_) {
                    std::ostringstream message;
                    message << "[FrontierClient] local-player state replication active position=("
                            << state.position.x << ", " << state.position.y << ", " << state.position.z << ")";
                    log_line(message.str());

                    const auto chain = gameBridge_.local_player_chain_diagnostic();
                    if (!chain.empty()) {
                        log_line("[FrontierClient] local-player ready chain " + chain);
                    }

                    bridgeStateReady_ = true;
                }
            } else if (lastBridgeLogMs_ == 0 || now - lastBridgeLogMs_ >= 1000) {
                log_line("[FrontierClient] local-player read pending: " + bridgeError);
                lastBridgeLogMs_ = now;
            }

        }
        if (g_network->state() == ConnectionState::Connected && gameBridge_.initialized()) {
            const bool sessionActive =
                session_.runtime_state().state == frontier::game::FrontierSessionState::Active;
            remotePlayers_.update(now, gameBridge_, sessionActive);
        }
    }
}

} // namespace frontier::client
