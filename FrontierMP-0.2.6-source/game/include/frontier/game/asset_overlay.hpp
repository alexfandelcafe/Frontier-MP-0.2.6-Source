#pragma once

#include "frontier/game/inline_hook.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace frontier::game {

class AssetOverlay final {
public:
    ~AssetOverlay();

    bool initialize(std::uintptr_t moduleBase,
                    std::uint32_t textRva,
                    std::uint32_t textSize,
                    std::filesystem::path overlayRoot,
                    std::string& error);

    bool attached() const { return hook_.attached(); }
    const std::filesystem::path& root() const { return overlayRoot_; }
    const std::filesystem::path& boot_override_path() const { return bootOverridePath_; }
    const std::string& last_error() const { return lastError_; }

private:
    InlineHook hook_{};
    std::filesystem::path overlayRoot_{};
    std::filesystem::path bootOverridePath_{};
    std::string lastError_{};
};

} // namespace frontier::game
