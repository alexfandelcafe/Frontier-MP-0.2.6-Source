#include "frontier/server/rdrmp_server.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    frontier::server::RdrmpServer::Config config{};

    if (argc > 1) {
        try {
            const auto value = std::stoul(argv[1]);
            if (value <= 65535U) config.port = static_cast<std::uint16_t>(value);
        } catch (...) {
            std::fprintf(stderr, "[rdrmp] invalid port, using %u\n", config.port);
        }
    }

    frontier::server::RdrmpServer server(config);
    if (!server.start()) {
        std::fprintf(stderr, "[rdrmp] failed to start\n");
        return 1;
    }

    server.run();
    return 0;
}
