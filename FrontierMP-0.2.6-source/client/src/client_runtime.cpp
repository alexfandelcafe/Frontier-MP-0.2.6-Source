#include "frontier/client/client_runtime.hpp"
#include "frontier/client/network_client.hpp"
#include "frontier/game/build_fingerprint.hpp"

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <iomanip>

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

    if (!gameBridge_.initialize(fingerprint, build)) {
        log_line("[FrontierClient] game bridge initialization failed; networking will continue without local state replication");
    } else {
        log_line("[FrontierClient] game bridge initialized; local-player symbol and actor-manager paths resolved");
    }

    g_network = std::make_unique<NetworkClient>();
    g_network->set_on_welcome([](const auto& welcome) {
        std::ostringstream message;
        message << "[FrontierClient] connected as player " << welcome.playerId
                << " serverTick=" << welcome.serverTick;
        log_line(message.str());
    });
    g_network->set_on_snapshot([](const auto& snapshot) {
        std::ostringstream message;
        message << "[FrontierClient] snapshot tick=" << snapshot.serverTick
                << " players=" << snapshot.players.size();
        log_line(message.str());
    });
    g_network->set_on_disconnect([](const std::string& reason) { log_line("[FrontierClient] " + reason); });

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
    g_network.reset();
    connected_ = false;
    clientTick_ = 0;
    lastStateSendMs_ = 0;
    lastBridgeLogMs_ = 0;
    bridgeStateReady_ = false;
    session_.reset();
    lastSessionUpdateMs_ = 0;
}

void ClientRuntime::update() {
    const auto now = monotonic_ms();
    if (g_network) {
        g_network->update(now);

        if (gameBridge_.initialized() && (lastSessionUpdateMs_ == 0 || now - lastSessionUpdateMs_ >= 250)) {
            std::string sessionLog;
            session_.update(gameBridge_, sessionLog);
            if (!sessionLog.empty()) log_line(sessionLog);
            lastSessionUpdateMs_ = now;
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
    }
}

} // namespace frontier::client
