#pragma once

#include "frontier/game/inline_hook.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace frontier::game {

class ContentPathRedirector final {
public:
    ContentPathRedirector() = default;
    ~ContentPathRedirector();

    ContentPathRedirector(const ContentPathRedirector&) = delete;
    ContentPathRedirector& operator=(const ContentPathRedirector&) = delete;

    bool install(const std::uint8_t* text,
                 std::size_t textSize,
                 std::uintptr_t moduleBase,
                 std::size_t imageSize,
                 const std::filesystem::path& clientPackageRoot,
                 std::string& error);

    bool attached() const { return hook_.attached(); }

private:
    using FullReadPathFn = char (*)(std::uintptr_t self, char* path);

    static constexpr std::size_t kHistoricalRouteCount = 4;
    static constexpr std::size_t kPathStorageSize = 1024;
    static constexpr std::size_t kMaxRedirectPathBytes = 240;

    struct PreparedRoute final {
        const char* source{};
        std::size_t sourceLength{};
        char resolved[kPathStorageSize]{};
        std::size_t resolvedLength{};
        bool enabled{};
    };

    static char __fastcall full_read_path_hook(std::uintptr_t self, char* path);

    char invoke_and_redirect(std::uintptr_t self, char* path);

    bool prepare_routes(std::string& error);
    void log_message(const char* message) const;

    static ContentPathRedirector* active_;

    InlineHook hook_{};
    FullReadPathFn original_{};
    std::filesystem::path contentRoot_{};
    std::array<PreparedRoute, kHistoricalRouteCount> routes_{};
#ifdef _WIN32
    void* logFile_{};
#endif
    std::atomic<std::uint32_t> redirectLogCount_{0};
};

} // namespace frontier::game
