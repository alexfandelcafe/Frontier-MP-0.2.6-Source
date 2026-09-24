#include "frontier/game/asset_overlay.hpp"

#include "frontier/game/pattern_scanner.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

namespace frontier::game {

namespace {

constexpr const char* kFullReadPathPattern =
    "48 83 EC 28 E8 ? ? ? ? C6 80 ? ? ? ? ?";
constexpr std::size_t kFullReadPathPatchSize = 16;

using FullReadPathFn = std::uintptr_t (*)(
    std::uintptr_t,
    std::uintptr_t,
    std::uintptr_t,
    std::uintptr_t);

InlineHook g_fullReadPathHook{};
FullReadPathFn g_originalFullReadPath = nullptr;
std::atomic<std::uint32_t> g_observedCalls{0};

void log_line(const std::string& line) {
#ifdef _WIN32
    char localAppData[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH);
    if (n != 0 && n < MAX_PATH) {
        std::filesystem::path dir =
            std::filesystem::path(localAppData) / "FrontierMP" / "logs";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ofstream file(dir / "client.log", std::ios::app);
        if (file) file << line << "\n";
    }
#else
    std::fprintf(stderr, "%s\n", line.c_str());
#endif
}

bool guarded_ascii(std::uintptr_t address, std::string& out) {
    out.clear();
    if (address < 0x10000u) return false;

#ifdef _WIN32
    __try {
        const auto* chars = reinterpret_cast<const unsigned char*>(address);
        for (std::size_t i = 0; i < 260; ++i) {
            const unsigned char c = chars[i];
            if (c == 0) {
                if (out.empty()) return false;
                return true;
            }
            if (c < 32 || c > 126) return false;
            out.push_back(static_cast<char>(c));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.clear();
        return false;
    }
#else
    (void)address;
#endif

    return false;
}

bool guarded_utf16(std::uintptr_t address, std::string& out) {
    out.clear();
    if (address < 0x10000u) return false;

#ifdef _WIN32
    __try {
        const auto* chars = reinterpret_cast<const wchar_t*>(address);
        for (std::size_t i = 0; i < 260; ++i) {
            const wchar_t c = chars[i];
            if (c == L'\0') {
                if (out.empty()) return false;
                return true;
            }
            if (c < 32 || c > 126) return false;
            out.push_back(static_cast<char>(c));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.clear();
        return false;
    }
#else
    (void)address;
#endif

    return false;
}

std::string pointer_candidates(std::initializer_list<std::uintptr_t> values) {
    std::ostringstream out;
    std::size_t index = 0;
    for (const auto value : values) {
        std::string ascii;
        std::string utf16;
        bool emitted = false;

        if (guarded_ascii(value, ascii) &&
            (ascii.find("content") != std::string::npos ||
             ascii.find("boot") != std::string::npos ||
             ascii.find(".rpf") != std::string::npos ||
             ascii.find("\\") != std::string::npos ||
             ascii.find("/") != std::string::npos)) {
            out << " arg" << index << "=ascii:" << ascii;
            emitted = true;
        }

        if (!emitted && guarded_utf16(value, utf16) &&
            (utf16.find("content") != std::string::npos ||
             utf16.find("boot") != std::string::npos ||
             utf16.find(".rpf") != std::string::npos ||
             utf16.find("\\") != std::string::npos ||
             utf16.find("/") != std::string::npos)) {
            out << " arg" << index << "=utf16:" << utf16;
        }

        ++index;
    }
    return out.str();
}

std::uintptr_t hooked_full_read_path(std::uintptr_t a,
                                     std::uintptr_t b,
                                     std::uintptr_t c,
                                     std::uintptr_t d) {
    if (!g_originalFullReadPath) return 0;

    thread_local bool insideHook = false;
    if (insideHook) {
        return g_originalFullReadPath(a, b, c, d);
    }

    insideHook = true;
    const auto callIndex = g_observedCalls.fetch_add(1, std::memory_order_relaxed);

    if (callIndex < 128) {
        std::ostringstream before;
        before << "[FrontierAsset] fullReadPath call=" << callIndex
               << " a=0x" << std::hex << a
               << " b=0x" << b
               << " c=0x" << c
               << " d=0x" << d
               << std::dec
               << pointer_candidates({a, b, c, d});
        log_line(before.str());
    }

    const auto result = g_originalFullReadPath(a, b, c, d);

    if (callIndex < 128) {
        std::ostringstream after;
        after << "[FrontierAsset] fullReadPath return=" << std::hex << result << std::dec
              << pointer_candidates({result, a, b, c, d});
        log_line(after.str());
    }

    insideHook = false;
    return result;
}

} // namespace

bool AssetOverlay::initialize(std::uintptr_t moduleBase,
                              std::uint32_t textRva,
                              std::uint32_t textSize,
                              std::filesystem::path overlayRoot,
                              std::string& error) {
    error.clear();
    lastError_.clear();
    overlayRoot_ = std::move(overlayRoot);
    bootOverridePath_ = overlayRoot_ / "content" / "ui" / "boot.sc.xml";

#ifdef _WIN32
    const auto* text = reinterpret_cast<const std::uint8_t*>(moduleBase + textRva);
    const auto pattern = BytePattern::parse(kFullReadPathPattern);
    if (!pattern) {
        error = "fullReadPath pattern parse failed";
        lastError_ = error;
        return false;
    }

    const auto target = PatternScanner::scan_buffer(text, textSize, *pattern);
    if (!target) {
        error = "fullReadPath pattern not found in RDR .text";
        lastError_ = error;
        log_line("[FrontierAsset] " + error);
        return false;
    }

    std::ostringstream found;
    found << "[FrontierAsset] fullReadPath target=0x"
          << std::hex << *target << std::dec
          << " overlayRoot=" << overlayRoot_.string()
          << " bootOverride=" << bootOverridePath_.string();
    log_line(found.str());

    std::string hookError;
    if (!g_fullReadPathHook.install(*target,
                                    reinterpret_cast<std::uintptr_t>(&hooked_full_read_path),
                                    kFullReadPathPatchSize,
                                    "fiAssetManager::fullReadPath",
                                    hookError)) {
        error = hookError;
        lastError_ = error;
        log_line("[FrontierAsset] hook failed: " + error);
        return false;
    }

    g_originalFullReadPath =
        reinterpret_cast<FullReadPathFn>(g_fullReadPathHook.trampoline());

    if (std::filesystem::exists(bootOverridePath_)) {
        log_line("[FrontierAsset] boot override file present");
    } else {
        log_line("[FrontierAsset] boot override file missing; hook is diagnostic-only");
    }

    log_line("[FrontierAsset] fiAssetManager::fullReadPath hook attached");
    return true;
#else
    (void)moduleBase;
    (void)textRva;
    (void)textSize;
    error = "asset overlay is Windows-only";
    lastError_ = error;
    return false;
#endif
}

} // namespace frontier::game
