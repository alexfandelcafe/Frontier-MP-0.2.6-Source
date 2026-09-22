#include "frontier/game/pattern_scanner.hpp"

#include <cassert>
#include <cstdint>
#include <vector>
#include <cstdio>

int main() {
    using namespace frontier::game;
    const std::vector<std::uint8_t> bytes = {0x90, 0x48, 0x8B, 0x11, 0x22, 0x89, 0xCC};
    const auto pattern = BytePattern::parse("48 8B ? ? 89");
    assert(pattern.has_value());
    const auto found = PatternScanner::scan_buffer(bytes.data(), bytes.size(), *pattern);
    assert(found.has_value());
    assert(*found == reinterpret_cast<std::uintptr_t>(bytes.data() + 1));
    assert(!BytePattern::parse("GG").has_value());
    std::printf("FrontierMP pattern tests: OK\n");
    return 0;
}
