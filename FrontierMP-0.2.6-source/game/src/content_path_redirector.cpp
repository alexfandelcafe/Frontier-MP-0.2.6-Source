#include "frontier/game/content_path_redirector.hpp"

#include "frontier/game/pattern_scanner.hpp"

#include <array>
#include <cstring>
#include <cstdio>
#include <string_view>
#include <system_error>

namespace frontier::game {

namespace {

constexpr const char* kFullReadPathPattern =
    "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? "
    "57 41 54 41 55 41 56 41 57 48 83 EC 30 "
    "48 8B 05 ? ? ? ? 33 ED";

constexpr std::size_t kFullReadPathPatchSize = 15;

constexpr std::array<std::string_view, 8> kHistoricalRdrmpContentPaths = {
    "content/ui/boot.sc",
    "content/ui/boot.sc.xml",
    "content/ui/pausemenu/pausemenuscene.sc",
    "content/ui/pausemenu/pausemenuscene.sc.xml",
    "content/ui/pausemenu/savegame.sc",
    "content/ui/pausemenu/savegame.sc.xml",
    "content/ui/net/profileeditor/main.sc",
    "content/ui/net/profileeditor/main.sc.xml",
};

void log_redirector(const char* message) {
    std::fprintf(stderr, "%s\n", message);
}

} // namespace

ContentPathRedirector* ContentPathRedirector::active_ = nullptr;

ContentPathRedirector::~ContentPathRedirector() {
    if (active_ == this) {
        active_ = nullptr;
    }
}

bool ContentPathRedirector::install(
    const std::uint8_t* text,
    std::size_t textSize,
    std::uintptr_t moduleBase,
    const std::filesystem::path& clientPackageRoot,
    std::string& error) {
    error.clear();
    original_ = nullptr;
    contentRoot_.clear();
    redirectLogCount_.store(0);

    if (text == nullptr || textSize == 0 || moduleBase == 0) {
        error = "fullReadPath hook: invalid game text/module";
        return false;
    }

    const auto pattern = BytePattern::parse(kFullReadPathPattern);
    if (!pattern) {
        error = "fullReadPath hook: invalid signature";
        return false;
    }

    const auto hit = PatternScanner::scan_buffer(text, textSize, *pattern);
    if (!hit) {
        error = "fullReadPath hook: target signature not found";
        return false;
    }

    if (!PatternScanner::validate_in_module(
            *hit, moduleBase, 0x5A5EC600u)) {
        error = "fullReadPath hook: target outside RDR image";
        return false;
    }

    contentRoot_ = clientPackageRoot / "game";

    std::error_code ec;
    if (!std::filesystem::is_directory(contentRoot_, ec) || ec) {
        error = "fullReadPath hook: Frontier game/content directory is missing: " +
                contentRoot_.string();
        return false;
    }

    std::string hookError;
    if (!hook_.install(
            *hit,
            reinterpret_cast<std::uintptr_t>(&ContentPathRedirector::full_read_path_hook),
            kFullReadPathPatchSize,
            "rage::fiAssetManager::fullReadPath",
            hookError)) {
        error = hookError;
        return false;
    }

    original_ = reinterpret_cast<FullReadPathFn>(hook_.trampoline());
    if (original_ == nullptr) {
        error = "fullReadPath hook: trampoline is null";
        return false;
    }

    active_ = this;

    char message[512]{};
    std::snprintf(
        message,
        sizeof(message),
        "[FrontierContent] fullReadPath hook attached target=0x%llX rva=0x%llX root=%s",
        static_cast<unsigned long long>(*hit),
        static_cast<unsigned long long>(*hit - moduleBase),
        contentRoot_.string().c_str());
    log_redirector(message);

    return true;
}

char __fastcall ContentPathRedirector::full_read_path_hook(
    std::uintptr_t self,
    char* path) {
    if (active_ == nullptr || active_->original_ == nullptr) {
        return 0;
    }

    return active_->invoke_and_redirect(self, path);
}

char ContentPathRedirector::invoke_and_redirect(
    std::uintptr_t self,
    char* path) {
    const char originalResult = original_(self, path);

    if (path == nullptr || *path == '\0') {
        return originalResult;
    }

    std::string requested(path);
    for (char& ch : requested) {
        if (ch == '\\') ch = '/';
    }

    bool matched = false;
    for (const auto historicalPath : kHistoricalRdrmpContentPaths) {
        if (requested == historicalPath) {
            matched = true;
            break;
        }
    }

    if (!matched) {
        return originalResult;
    }

    const std::filesystem::path candidate = contentRoot_ / std::filesystem::path(requested);

    std::error_code ec;
    if (!std::filesystem::is_regular_file(candidate, ec) || ec) {
        return originalResult;
    }

    const std::string resolved = candidate.string();
    std::memcpy(path, resolved.c_str(), resolved.size() + 1);

    const auto logIndex = redirectLogCount_.fetch_add(1);
    if (logIndex < 16u) {
        char message[512]{};
        std::snprintf(
            message,
            sizeof(message),
            "[FrontierContent] redirect %s -> %s",
            requested.c_str(),
            resolved.c_str());
        log_redirector(message);
    }

    return originalResult;
}

} // namespace frontier::game
