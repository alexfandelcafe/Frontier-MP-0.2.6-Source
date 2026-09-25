#pragma once

#include "frontier/game/inline_hook.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <atomic>

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
                 const std::filesystem::path& clientPackageRoot,
                 std::string& error);

    bool attached() const { return hook_.attached(); }

private:
    using FullReadPathFn = char (*)(std::uintptr_t self, char* path);

    static char __fastcall full_read_path_hook(std::uintptr_t self, char* path);

    char invoke_and_redirect(std::uintptr_t self, char* path);

    static ContentPathRedirector* active_;

    InlineHook hook_{};
    FullReadPathFn original_{};
    std::filesystem::path contentRoot_{};
    std::atomic<std::uint32_t> redirectLogCount_{0};
};

} // namespace frontier::game
