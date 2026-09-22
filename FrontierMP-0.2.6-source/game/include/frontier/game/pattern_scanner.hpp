#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace frontier::game {

struct BytePattern final {
    std::vector<std::optional<std::uint8_t>> bytes;

    static std::optional<BytePattern> parse(const std::string& text);
};

class PatternScanner final {
public:
    static std::optional<std::uintptr_t> scan_buffer(const std::uint8_t* data, std::size_t size, const BytePattern& pattern);
    static bool validate_in_module(std::uintptr_t address, std::uintptr_t moduleBase, std::size_t imageSize);
};

} // namespace frontier::game
