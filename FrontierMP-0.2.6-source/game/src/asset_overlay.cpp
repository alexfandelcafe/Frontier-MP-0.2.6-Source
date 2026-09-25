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
#ifdef _MSC_VER
#include <intrin.h>
#endif
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
        if (c == 0) return !out.empty();
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
        if (c == L'\0') return !out.empty();
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

bool path_like(const std::string& value) {
    return value.find("content") != std::string::npos ||
           value.find("boot.sc.xml") != std::string::npos ||
           value.find("boot") != std::string::npos ||
           value.find(".rpf") != std::string::npos ||
           value.find("\\") != std::string::npos ||
           value.find("/") != std::string::npos;
}

std::string byte_string_probe(std::uintptr_t address) {
    if (address < 0x10000u) return {};

#ifdef _WIN32
    std::array<unsigned char, 512> bytes{};
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(),
                           reinterpret_cast<const void*>(address),
                           bytes.data(),
                           bytes.size(),
                           &bytesRead) ||
        bytesRead == 0) {
        return {};
    }

    for (std::size_t i = 0; i < bytesRead;) {
        const std::size_t start = i;
        while (i < bytesRead && bytes[i] >= 32 && bytes[i] <= 126) ++i;
        if (i - start >= 6) {
            const std::string candidate(
                reinterpret_cast<const char*>(bytes.data() + start), i - start);
            if (path_like(candidate)) return "ascii@" + std::to_string(start) + ":" + candidate;
        }
        ++i;
    }

    if (bytesRead >= 2) {
        for (std::size_t start = 0; start + 1 < bytesRead; start += 2) {
            std::string candidate;
            std::size_t i = start;
            while (i + 1 < bytesRead && bytes[i] >= 32 && bytes[i] <= 126 && bytes[i + 1] == 0) {
                candidate.push_back(static_cast<char>(bytes[i]));
                i += 2;
            }
            if (candidate.size() >= 6 && path_like(candidate)) {
                return "utf16@" + std::to_string(start) + ":" + candidate;
            }
        }
    }
#else
    (void)address;
#endif

    return {};
}

std::string pointer_candidates(std::initializer_list<std::uintptr_t> values) {
    std::ostringstream out;
    std::size_t index = 0;
    for (const auto value : values) {
        std::string ascii;
        std::string utf16;
        bool emitted = false;

        if (guarded_ascii(value, ascii) && path_like(ascii)) {
            out << " arg" << index << "=ascii:" << ascii;
            emitted = true;
        }

        if (!emitted && guarded_utf16(value, utf16) && path_like(utf16)) {
            out << " arg" << index << "=utf16:" << utf16;
        }

        ++index;
    }
    return out.str();
}

std::string deep_string_probe(std::uintptr_t root) {
    std::ostringstream out;
    if (root < 0x10000u) return out.str();

#ifdef _WIN32
    std::array<std::uintptr_t, 64> pending{};
    std::array<std::uintptr_t, 64> seen{};
    std::size_t pendingCount = 0;
    std::size_t head = 0;
    pending[pendingCount++] = root;

    while (head < pendingCount && head < 16) {
        const auto address = pending[head++];
        bool alreadySeen = false;
        for (std::size_t i = 0; i < head - 1 && i < seen.size(); ++i) {
            if (seen[i] == address) {
                alreadySeen = true;
                break;
            }
        }
        if (alreadySeen) continue;
        if (head - 1 < seen.size()) seen[head - 1] = address;

        const auto direct = byte_string_probe(address);
        if (!direct.empty()) {
            out << " ptr=0x" << std::hex << address << std::dec << " bytes=" << direct;
        }

        std::array<std::uintptr_t, 16> words{};
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(GetCurrentProcess(),
                               reinterpret_cast<const void*>(address),
                               words.data(),
                               sizeof(words),
                               &bytesRead) ||
            bytesRead < sizeof(std::uintptr_t)) {
            continue;
        }

        const std::size_t count = bytesRead / sizeof(std::uintptr_t);
        for (std::size_t i = 0; i < count && i < 8; ++i) {
            const auto value = words[i];
            if (value < 0x10000u) continue;

            std::string ascii;
            std::string utf16;
            if (guarded_ascii(value, ascii) && path_like(ascii)) {
                out << " q" << i << "=0x" << std::hex << value << std::dec
                    << ":ascii=" << ascii;
            } else if (guarded_utf16(value, utf16) && path_like(utf16)) {
                out << " q" << i << "=0x" << std::hex << value << std::dec
                    << ":utf16=" << utf16;
            } else if (pendingCount < pending.size() &&
                       std::find(seen.begin(), seen.begin() + std::min(head, seen.size()), value) ==
                           seen.begin() + std::min(head, seen.size())) {
                pending[pendingCount++] = value;
            }
        }
    }
#else
    (void)root;
#endif

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

void append_address_probe(std::ostringstream& out,
                          const char* name,
                          std::uintptr_t address) {
    const auto bytes = byte_string_probe(address);
    if (!bytes.empty()) out << " " << name << "Bytes=" << bytes;

    const auto qwords = memory_qword_probe(address);
    if (!qwords.empty()) out << " " << name << "Probe=" << qwords;
}

std::uintptr_t hooked_full_read_path(std::uintptr_t a,
                                     std::uintptr_t b,
                                     std::uintptr_t c,
                                     std::uintptr_t d,
                                     std::uintptr_t e) {
    if (!g_originalFullReadPath) return 0;

    thread_local bool insideHook = false;
    if (insideHook) {
        return g_originalFullReadPath(a, b, c, d, e);
    }

    insideHook = true;
    const auto callIndex = g_observedCalls.fetch_add(1, std::memory_order_relaxed);

    if (callIndex < 128) {
        std::uintptr_t caller = 0;
#ifdef _MSC_VER
        caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
#endif
        std::ostringstream before;
        before << "[FrontierAsset] fullReadPath call=" << callIndex
               << " caller=0x" << std::hex << caller
               << " a=0x" << a
               << " b=0x" << b
               << " c=0x" << c
               << " d=0x" << d
               << " e=0x" << e
               << std::dec
               << pointer_candidates({a, b, c, d, e});

        append_address_probe(before, "a", a);
        append_address_probe(before, "b", b);
        append_address_probe(before, "e", e);
        before << " aDeep=" << deep_string_probe(a);
        before << " bDeep=" << deep_string_probe(b);
        before << " eDeep=" << deep_string_probe(e);

        log_line(before.str());
    }

    const auto result = g_originalFullReadPath(a, b, c, d, e);

    if (callIndex < 128) {
        std::ostringstream after;
        after << "[FrontierAsset] fullReadPath return=0x" << std::hex << result << std::dec
              << pointer_candidates({result, a, b, c, d, e});

        append_address_probe(after, "return", result);
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
