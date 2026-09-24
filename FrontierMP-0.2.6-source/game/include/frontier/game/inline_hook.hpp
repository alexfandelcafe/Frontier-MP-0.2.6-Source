#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace frontier::game {

class InlineHook final {
public:
    InlineHook() = default;
    ~InlineHook();

    InlineHook(const InlineHook&) = delete;
    InlineHook& operator=(const InlineHook&) = delete;

    bool install(std::uintptr_t target,
                 std::uintptr_t replacement,
                 std::size_t patchSize,
                 const std::string& name,
                 std::string& error);

    bool attached() const { return target_ != 0 && trampoline_ != 0; }
    std::uintptr_t trampoline() const { return trampoline_; }

private:
    void reset();

    std::uintptr_t target_{};
    std::uintptr_t trampoline_{};
    std::size_t patchSize_{};
    std::vector<std::uint8_t> originalBytes_{};
};

} // namespace frontier::game
