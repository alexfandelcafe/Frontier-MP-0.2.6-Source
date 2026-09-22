#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace frontier::server {

struct Endpoint final {
    std::string host;
    std::uint16_t port{};

    std::string key() const;
};

class UdpSocket final {
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool open(std::uint16_t port);
    void close();
    bool send(const Endpoint& endpoint, const std::vector<std::uint8_t>& bytes);
    bool receive(Endpoint& endpoint, std::vector<std::uint8_t>& bytes);

private:
#ifdef _WIN32
    using Handle = std::uintptr_t;
#else
    using Handle = int;
#endif
    Handle handle_{};
    bool opened_{};
};

} // namespace frontier::server
