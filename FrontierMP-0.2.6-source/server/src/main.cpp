#include "frontier/server/server.hpp"

#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    frontier::server::Server::Config config{};
    if (argc > 1) config.port = static_cast<std::uint16_t>(std::strtoul(argv[1], nullptr, 10));

    frontier::server::Server server(config);
    if (!server.start()) return 1;
    server.run();
    return 0;
}
