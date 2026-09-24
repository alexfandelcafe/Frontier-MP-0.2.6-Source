#include "frontier/game/asset_overlay.hpp"

#include "frontier/game/pattern_scanner.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <array>
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
    std::array<unsigned char, 260> chars{};
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<const void*>(address),
                           chars.data(),
                           chars.size(),
                           &bytesRead) ||
        bytesRead == 0) {
        return false;
    }

    for (std::size_t i = 0; i < bytesRead; ++i) {
        const unsigned char c = chars[i];
        if (c == 0) {
            return !out.empty();
        }
        if (c < 32 || c > 126) {
            out.clear();
            return false;
        }
        out.push_back(static_cast<char>(c));
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
    std::array<wchar_t, 260> chars{};
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<const void*>(address),
                           chars.data(),
                           sizeof(chars),
                           &bytesRead) ||
        bytesRead < sizeof(wchar_t)) {
        return false;
    }

    const std::size_t charCount = bytesRead / sizeof(wchar_t);
    for (std::size_t i = 0; i < charCount; ++i) {
        const wchar_t c = chars[i];
        if (c == L'\0') {
            return !out.empty();
        }
        if (c < 32 || c > 126) {
            out.clear();
            return false;
        }
        out.push_back(static_cast<char>(c));
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

std::string memory_qword_probe(std::uintptr_t address) {
    std::ostringstream out;
    if (address < 0x10000u) return out.str();

#ifdef _WIN32
    std::array<std::uintptr_t, 4> words{};
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<const void*>(address),
                           words.data(),
                           sizeof(words),
                           &bytesRead) ||
        bytesRead != sizeof(words)) {
        return out.str();
    }

    for (std::size_t i = 0; i < words.size(); ++i) {
        const auto value = words[i];
        out << " q" << i << "=0x" << std::hex << value << std::dec;

        std::string ascii;
        std::string utf16;
        if (guarded_ascii(value, ascii) && !ascii.empty()) {
            out << ":ascii=" << ascii;
        } else if (guarded_utf16(value, utf16) && !utf16.empty()) {
            out << ":utf16=" << utf16;
        }
    }
#else
    (void)address;
#endif

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

        const auto bProbe = memory_qword_probe(b);
        if (!bProbe.empty()) before << " bProbe=" << bProbe;

        const auto cProbe = memory_qword_probe(c);
        if (!cProbe.empty()) before << " cProbe=" << cProbe;

        log_line(before.str());
    }

    const auto result = g_originalFullReadPath(a, b, c, d);

    if (callIndex < 128) {
        std::ostringstream after;
        after << "[FrontierAsset] fullReadPath return=0x" << std::hex << result << std::dec
              << pointer_candidates({result, a, b, c, d});

        const auto resultProbe = memory_qword_probe(result);
        if (!resultProbe.empty()) after << " resultProbe=" << resultProbe;

        log_line(after.str());
    }

    insideHook = false;
    return result;
}

} // namespace

AssetOverlay::~AssetOverlay() {
    g_originalFullReadPath = nullptr;
}

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
    if (!hook_.install(*target,
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
        reinterpret_cast<FullReadPathFn>(hook_.trampoline());

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
