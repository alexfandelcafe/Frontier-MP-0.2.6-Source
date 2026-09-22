#include "frontier/game/pattern_scanner.hpp"

#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace frontier::game {

std::optional<BytePattern> BytePattern::parse(const std::string& text) {
    BytePattern result;
    std::istringstream stream(text);
    std::string token;
    while (stream >> token) {
        if (token == "?" || token == "??") {
            result.bytes.emplace_back(std::nullopt);
            continue;
        }
        if (token.size() != 2) return std::nullopt;
        const auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        const int hi = hex(token[0]);
        const int lo = hex(token[1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        result.bytes.emplace_back(static_cast<std::uint8_t>((hi << 4) | lo));
    }
    if (result.bytes.empty()) return std::nullopt;
    return result;
}

std::optional<std::uintptr_t> PatternScanner::scan_buffer(const std::uint8_t* data, std::size_t size, const BytePattern& pattern) {
    if (data == nullptr || pattern.bytes.empty() || size < pattern.bytes.size()) return std::nullopt;
    for (std::size_t i = 0; i + pattern.bytes.size() <= size; ++i) {
        bool match = true;
        for (std::size_t j = 0; j < pattern.bytes.size(); ++j) {
            if (pattern.bytes[j].has_value() && data[i + j] != pattern.bytes[j].value()) { match = false; break; }
        }
        if (match) return reinterpret_cast<std::uintptr_t>(data + i);
    }
    return std::nullopt;
}

bool PatternScanner::validate_in_module(std::uintptr_t address, std::uintptr_t moduleBase, std::size_t imageSize) {
    if (address < moduleBase) return false;
    const std::uintptr_t end = moduleBase + imageSize;
    return address < end;
}

} // namespace frontier::game
