#include "frontier/client/udp_socket.hpp"

#include <array>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace frontier::client {

UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::open() {
#ifdef _WIN32
    static bool wsaStarted = false;
    if (!wsaStarted) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
        wsaStarted = true;
    }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    handle_ = static_cast<std::uintptr_t>(s);
#else
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return false;
    const int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
    handle_ = s;
#endif
    opened_ = true;
    return true;
}

void UdpSocket::close() {
    if (!opened_) return;
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(handle_));
#else
    ::close(handle_);
#endif
    opened_ = false;
}

bool UdpSocket::send(const Endpoint& endpoint, const std::vector<std::uint8_t>& bytes) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(endpoint.port);
    if (inet_pton(AF_INET, endpoint.host.c_str(), &addr.sin_addr) != 1) return false;
#ifdef _WIN32
    const int sent = sendto(static_cast<SOCKET>(handle_), reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
#else
    const int sent = static_cast<int>(sendto(handle_, bytes.data(), bytes.size(), 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
#endif
    return sent == static_cast<int>(bytes.size());
}

bool UdpSocket::receive(Endpoint& endpoint, std::vector<std::uint8_t>& bytes) {
    if (!opened_) return false;
    std::array<std::uint8_t, 2048> buffer{};
    sockaddr_in addr{};
#ifdef _WIN32
    int addrLen = sizeof(addr);
    const int received = recvfrom(static_cast<SOCKET>(handle_), reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&addr), &addrLen);
    if (received == SOCKET_ERROR) return false;
#else
    socklen_t addrLen = sizeof(addr);
    const int received = static_cast<int>(recvfrom(handle_, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr*>(&addr), &addrLen));
    if (received < 0) return false;
#endif
    char host[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &addr.sin_addr, host, sizeof(host)) == nullptr) return false;
    endpoint.host = host;
    endpoint.port = ntohs(addr.sin_port);
    bytes.assign(buffer.begin(), buffer.begin() + received);
    return true;
}

} // namespace frontier::client
