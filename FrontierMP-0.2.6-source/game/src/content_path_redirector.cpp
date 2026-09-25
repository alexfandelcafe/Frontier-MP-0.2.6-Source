#include "frontier/game/content_path_redirector.hpp"

#include "frontier/game/pattern_scanner.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <array>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace frontier::game {

namespace {

constexpr const char* kFullReadPathPattern =
    "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? "
    "57 41 54 41 55 41 56 41 57 48 83 EC 30 "
    "48 8B 05 ? ? ? ? 33 ED";

constexpr std::size_t kFullReadPathPatchSize = 15;

struct HistoricalContentRoute final {
    std::string_view source;
    std::string_view destination;
};

// RDRMP's historical table contains four source/destination pairs.
// The source is searched inside the normalized requested path. A request for
// either ".../boot.sc" or ".../boot.sc.xml" therefore resolves to boot.sc.xml.
constexpr std::array<HistoricalContentRoute, 4> kHistoricalRdrmpContentRoutes = {{
    {"content/ui/boot.sc", "content/ui/boot.sc.xml"},
    {"content/ui/pausemenu/pausemenuscene.sc",
     "content/ui/pausemenu/pausemenuscene.sc.xml"},
    {"content/ui/pausemenu/savegame.sc",
     "content/ui/pausemenu/savegame.sc.xml"},
    {"content/ui/net/profileeditor/main.sc",
     "content/ui/net/profileeditor/main.sc.xml"},
}};

void log_redirector(const char* message) {
    std::fprintf(stderr, "%s\n", message);

#ifdef _WIN32
    char localAppData[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH);
    if (n != 0 && n < MAX_PATH) {
        std::filesystem::path dir =
            std::filesystem::path(localAppData) / "FrontierMP" / "logs";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ofstream file(dir / "client.log", std::ios::app);
        if (file) {
            file << message << "\n";
        }
    }
#endif
}

#ifdef _WIN32
bool writable_memory_range(std::uintptr_t address, std::size_t size) {
    if (address == 0 || size == 0) {
        return false;
    }

    auto current = address;
    std::size_t remaining = size;

    while (remaining != 0) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(
                reinterpret_cast<const void*>(current),
                &mbi,
                sizeof(mbi)) != sizeof(mbi)) {
            return false;
        }

        if (mbi.State != MEM_COMMIT ||
            (mbi.Protect & PAGE_GUARD) != 0 ||
            (mbi.Protect & PAGE_NOACCESS) != 0) {
            return false;
        }

        const DWORD writableFlags =
            PAGE_READWRITE |
            PAGE_WRITECOPY |
            PAGE_EXECUTE_READWRITE |
            PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & writableFlags) == 0) {
            return false;
        }

        const auto begin =
            reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto end = begin + mbi.RegionSize;

        if (current < begin || current >= end) {
            return false;
        }

        const auto available =
            static_cast<std::size_t>(end - current);
        if (available >= remaining) {
            return true;
        }

        current = end;
        remaining -= available;
    }

    return true;
}
#endif

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
    std::size_t imageSize,
    const std::filesystem::path& clientPackageRoot,
    std::string& error) {
    error.clear();
    original_ = nullptr;
    contentRoot_.clear();
    redirectLogCount_.store(0);

    if (text == nullptr || textSize == 0 || moduleBase == 0 || imageSize == 0) {
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
            *hit, moduleBase, imageSize)) {
        error = "fullReadPath hook: target outside RDR image";
        return false;
    }

    contentRoot_ = clientPackageRoot / "game";

    std::error_code ec;
    if (!std::filesystem::is_directory(contentRoot_, ec) || ec) {
        error = "fullReadPath hook: Frontier game directory is missing: " +
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
    // Historical RDRMP invokes the original first and only applies its
    // redirect when fullReadPath reports success.
    const char originalResult = original_(self, path);

    if (originalResult == 0 || path == nullptr || *path == '\0') {
        return originalResult;
    }

    std::string requested(path);
    for (char& ch : requested) {
        if (ch == '\\') {
            ch = '/';
        }
    }

    const HistoricalContentRoute* matchedRoute = nullptr;
    for (const auto& route : kHistoricalRdrmpContentRoutes) {
        if (requested.find(route.source) != std::string::npos) {
            matchedRoute = &route;
            break;
        }
    }

    if (matchedRoute == nullptr) {
        return originalResult;
    }

    const std::filesystem::path candidate =
        contentRoot_ / std::filesystem::path(matchedRoute->destination);

    std::error_code ec;
    if (!std::filesystem::is_regular_file(candidate, ec) || ec) {
        return originalResult;
    }

    const std::string resolved = candidate.string();

#ifdef _WIN32
    if (!writable_memory_range(
            reinterpret_cast<std::uintptr_t>(path),
            resolved.size() + 1)) {
        const auto logIndex = redirectLogCount_.fetch_add(1);
        if (logIndex < 16u) {
            char message[640]{};
            std::snprintf(
                message,
                sizeof(message),
                "[FrontierContent] redirect skipped: destination buffer unsafe "
                "requested=%s bytes=%zu resolved=%s",
                requested.c_str(),
                resolved.size() + 1,
                resolved.c_str());
            log_redirector(message);
        }
        return originalResult;
    }
#endif

    std::memcpy(path, resolved.c_str(), resolved.size() + 1);

    const auto logIndex = redirectLogCount_.fetch_add(1);
    if (logIndex < 16u) {
        char message[640]{};
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
