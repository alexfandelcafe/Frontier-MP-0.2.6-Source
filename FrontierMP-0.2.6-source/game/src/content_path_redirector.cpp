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

constexpr std::array<HistoricalContentRoute, 4> kHistoricalRdrmpContentRoutes = {{
    {"content/ui/boot.sc", "content/ui/boot.sc.xml"},
    {"content/ui/pausemenu/pausemenuscene.sc",
     "content/ui/pausemenu/pausemenuscene.sc.xml"},
    {"content/ui/pausemenu/savegame.sc",
     "content/ui/pausemenu/savegame.sc.xml"},
    {"content/ui/net/profileeditor/main.sc",
     "content/ui/net/profileeditor/main.sc.xml"},
}};

bool path_contains_route(const char* path, std::string_view route) {
    if (path == nullptr || route.empty()) {
        return false;
    }

    const std::size_t pathLength = std::strlen(path);
    if (pathLength < route.size()) {
        return false;
    }

    for (std::size_t start = 0; start + route.size() <= pathLength; ++start) {
        if (start != 0 && path[start - 1] != '/' && path[start - 1] != '\\') {
            continue;
        }

        bool equal = true;
        for (std::size_t i = 0; i < route.size(); ++i) {
            char lhs = path[start + i];
            const char rhs = route[i];

            if (lhs == '\\') {
                lhs = '/';
            }

            if (lhs != rhs) {
                equal = false;
                break;
            }
        }

        if (!equal) {
            continue;
        }

        const std::size_t end = start + route.size();
        if (end == pathLength || path[end] == '/' || path[end] == '\\') {
            return true;
        }

        // The historical table maps the script path to its XML resource. The
        // same source entry can therefore be observed either as "...sc" or
        // "...sc.xml".
        if (route.size() >= 3 &&
            route.substr(route.size() - 3) == ".sc" &&
            path[end] == '.' &&
            end + 4 <= pathLength &&
            path[end + 1] == 'x' &&
            path[end + 2] == 'm' &&
            path[end + 3] == 'l') {
            return true;
        }
    }

    return false;
}

#ifdef _WIN32
void make_path_preview(const char* path, char* out, std::size_t capacity) {
    if (out == nullptr || capacity == 0) return;
    out[0] = '\0';
    if (path == nullptr) {
        std::snprintf(out, capacity, "<null>");
        return;
    }

    __try {
        std::size_t i = 0;
        for (; i + 1 < capacity; ++i) {
            const char ch = path[i];
            if (ch == '\0') break;
            out[i] = ch;
        }
        out[i < capacity ? i : capacity - 1] = '\0';
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::snprintf(out, capacity, "<unreadable>");
    }
}

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

std::string compact_path(const std::filesystem::path& path) {
    const std::string full = path.string();
    char shortPath[1024]{};

    const DWORD result = GetShortPathNameA(
        full.c_str(),
        shortPath,
        static_cast<DWORD>(std::size(shortPath)));

    if (result != 0 && result < std::size(shortPath)) {
        return std::string(shortPath, result);
    }

    return full;
}
#endif

} // namespace

ContentPathRedirector* ContentPathRedirector::active_ = nullptr;

ContentPathRedirector::~ContentPathRedirector() {
    if (active_ == this) {
        active_ = nullptr;
    }

#ifdef _WIN32
    if (logFile_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(logFile_));
        logFile_ = nullptr;
    }
#endif
}

void ContentPathRedirector::log_message(const char* message) const {
    if (message == nullptr) {
        return;
    }

#ifdef _WIN32
    if (logFile_ != nullptr) {
        const DWORD length =
            static_cast<DWORD>(std::strlen(message));
        DWORD written = 0;
        WriteFile(
            static_cast<HANDLE>(logFile_),
            message,
            length,
            &written,
            nullptr);
        const char newline = '\n';
        WriteFile(
            static_cast<HANDLE>(logFile_),
            &newline,
            1,
            &written,
            nullptr);
    }
#else
    (void)message;
#endif
}

bool ContentPathRedirector::prepare_routes(std::string& error) {
    error.clear();

    std::size_t enabledCount = 0;

    for (std::size_t i = 0; i < kHistoricalRouteCount; ++i) {
        const auto& source = kHistoricalRdrmpContentRoutes[i];
        auto& route = routes_[i];

        route.source = source.source.data();
        route.sourceLength = source.source.size();
        route.resolved[0] = '\0';
        route.resolvedLength = 0;
        route.enabled = false;

        const std::filesystem::path candidate =
            contentRoot_ / std::filesystem::path(source.destination);

        std::error_code ec;
        if (!std::filesystem::is_regular_file(candidate, ec) || ec) {
            continue;
        }

#ifdef _WIN32
        const std::string resolved = compact_path(candidate);
#else
        const std::string resolved = candidate.string();
#endif

        if (resolved.empty()) {
            continue;
        }

        if (resolved.size() + 1 > sizeof(route.resolved)) {
            continue;
        }

        // The RDR asset path call was designed around legacy path buffers.
        // Keep a conservative bound so a deep Frontier checkout can never
        // turn the historical redirect into an oversized write.
        if (resolved.size() + 1 > kMaxRedirectPathBytes) {
            continue;
        }

        std::memcpy(
            route.resolved,
            resolved.c_str(),
            resolved.size() + 1);
        route.resolvedLength = resolved.size();
        route.enabled = true;
        ++enabledCount;
    }

    if (enabledCount == 0) {
        error =
            "fullReadPath hook: no historical content routes are available "
            "within the safe path-length limit";
        return false;
    }

    return true;
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
    routes_ = {};
    redirectLogCount_.store(0);
    callLogCount_.store(0);

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

    if (!prepare_routes(error)) {
        return false;
    }

#ifdef _WIN32
    char localAppData[MAX_PATH]{};
    const DWORD localAppDataLength =
        GetEnvironmentVariableA(
            "LOCALAPPDATA",
            localAppData,
            static_cast<DWORD>(std::size(localAppData)));

    if (localAppDataLength != 0 &&
        localAppDataLength < std::size(localAppData)) {
        const std::filesystem::path logDir =
            std::filesystem::path(localAppData) / "FrontierMP" / "logs";

        std::error_code logError;
        std::filesystem::create_directories(logDir, logError);

        const std::filesystem::path logPath = logDir / "client.log";
        const HANDLE file = CreateFileA(
            logPath.string().c_str(),
            FILE_APPEND_DATA,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (file != INVALID_HANDLE_VALUE) {
            logFile_ = file;
        }
    }
#endif

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

    char message[640]{};
    std::snprintf(
        message,
        sizeof(message),
        "[FrontierContent] fullReadPath hook attached target=0x%llX rva=0x%llX "
        "root=%s safeMax=%zu prologue=%02X %02X %02X %02X %02X %02X %02X %02X "
        "%02X %02X %02X %02X %02X %02X %02X",
        static_cast<unsigned long long>(*hit),
        static_cast<unsigned long long>(*hit - moduleBase),
        contentRoot_.string().c_str(),
        kMaxRedirectPathBytes,
        static_cast<unsigned int>(reinterpret_cast<const std::uint8_t*>(*hit)[0]),
        static_cast<unsigned int>(reinterpret_cast<const std::uint8_t*>(*hit)[1]),
        static_cast<unsigned int>(reinterpret_cast<const std::uint8_t*>(*hit)[2]),
        static_cast<unsigned int>(reinterpret_cast<const std::uint8_t*>(*hit)[3]),
        static_cast<unsigned int>(reinterpret_cast<const std::uint8_t*>(*hit)[4]),
        static_cast<unsigned int>(reinterpret_cast<const std::uint8_t*>(*hit)[5]),
        static_cast<unsigned int>(reinterpret_cast<const std::uint8_t*>(*hit)[6]),
        static_cast<unsigned int>(reinterpret_cast<const std::uint8_t*>(*hit)[7]),
        static_cast<unsigned int>(reinterpret_cast<const std::uint8_t*>(*hit)[8]),
        static_cast<unsigned int>(reinterpret_cast<const std::uint8_t*>(*hit)[9]),
        static_cast<unsigned int>(reinterpret_cast<const std::uint8_t*>(*hit)[10]),
        static_cast<unsigned int>(reinterpret_cast<const std::uint8_t*>(*hit)[11]),
        static_cast<unsigned int>(reinterpret_cast<const std::uint8_t*>(*hit)[12]),
        static_cast<unsigned int>(reinterpret_cast<const std::uint8_t*>(*hit)[13]),
        static_cast<unsigned int>(reinterpret_cast<const std::uint8_t*>(*hit)[14]));
    log_message(message);

    for (std::size_t i = 0; i < kHistoricalRouteCount; ++i) {
        if (!routes_[i].enabled) {
            continue;
        }

        char routeMessage[640]{};
        std::snprintf(
            routeMessage,
            sizeof(routeMessage),
            "[FrontierContent] route prepared source=%s resolved=%s bytes=%zu",
            routes_[i].source,
            routes_[i].resolved,
            routes_[i].resolvedLength + 1);
        log_message(routeMessage);
    }

    return true;
}

char __fastcall ContentPathRedirector::full_read_path_hook(
    std::uintptr_t self,
    char* path,
    std::uintptr_t arg3,
    std::uintptr_t arg4,
    std::uintptr_t arg5,
    std::uintptr_t arg6) {
    if (active_ == nullptr || active_->original_ == nullptr) {
        return 0;
    }

    return active_->invoke_and_redirect(
        self, path, arg3, arg4, arg5, arg6);
}

char ContentPathRedirector::invoke_and_redirect(
    std::uintptr_t self,
    char* path,
    std::uintptr_t arg3,
    std::uintptr_t arg4,
    std::uintptr_t arg5,
    std::uintptr_t arg6) {
    // The historical RDRMP hook rewrites the requested script path before
    // calling the original resolver. Calling the original first is too late:
    // RDR may already have resolved the stock content.rpf entry.
    const PreparedRoute* matchedRoute = nullptr;
    if (path != nullptr && *path != '\0') {
        for (std::size_t i = 0; i < routes_.size(); ++i) {
            const auto& route = routes_[i];
            if (route.enabled && path_contains_route(path, route.source)) {
                matchedRoute = &route;
                break;
            }
        }
    }

    char originalPath[256]{};
    if (matchedRoute != nullptr) {
        make_path_preview(path, originalPath, sizeof(originalPath));

#ifdef _WIN32
        if (!writable_memory_range(
                reinterpret_cast<std::uintptr_t>(path),
                matchedRoute->resolvedLength + 1)) {
            const auto logIndex = redirectLogCount_.fetch_add(1);
            if (logIndex < 16u) {
                char message[640]{};
                std::snprintf(
                    message,
                    sizeof(message),
                    "[FrontierContent] redirect skipped: destination buffer unsafe "
                    "requested=%s bytes=%zu resolved=%s",
                    originalPath,
                    matchedRoute->resolvedLength + 1,
                    matchedRoute->resolved);
                log_message(message);
            }
            matchedRoute = nullptr;
        } else {
            std::memcpy(
                path,
                matchedRoute->resolved,
                matchedRoute->resolvedLength + 1);
        }
#else
        std::memcpy(
            path,
            matchedRoute->resolved,
            matchedRoute->resolvedLength + 1);
#endif
    }

    const char originalResult = original_(
        self, path, arg3, arg4, arg5, arg6);

    const auto callIndex = callLogCount_.fetch_add(1);
    if (callIndex < 32u) {
        char message[640]{};
        if (matchedRoute != nullptr && originalPath[0] != '\0') {
            std::snprintf(
                message,
                sizeof(message),
                "[FrontierContent] fullReadPath call=%u result=%d "
                "requested=%s redirected=%s",
                callIndex,
                static_cast<int>(originalResult),
                originalPath,
                matchedRoute->resolved);
        } else {
            char preview[256]{};
            make_path_preview(path, preview, sizeof(preview));
            std::snprintf(
                message,
                sizeof(message),
                "[FrontierContent] fullReadPath call=%u result=%d path=%s",
                callIndex,
                static_cast<int>(originalResult),
                preview);
        }
        log_message(message);
    }

    return originalResult;
}
}

} // namespace frontier::game
