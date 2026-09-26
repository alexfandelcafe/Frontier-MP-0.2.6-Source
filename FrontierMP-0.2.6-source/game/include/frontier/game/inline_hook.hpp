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

    // Historical game-core locates scrThread::Wait through an E8 call-site
    // pattern, then hooks the decoded call target. This helper redirects only
    // that 5-byte CALL and exposes the original decoded target as trampoline().
    bool install_call_site(std::uintptr_t callSite,
                           std::uintptr_t replacement,
                           const std::string& name,
                           std::string& error);

    bool attached() const { return target_ != 0 && trampoline_ != 0; }
    std::uintptr_t trampoline() const { return trampoline_; }

private:
    void reset();

    std::uintptr_t target_{};
    std::uintptr_t trampoline_{};
    std::size_t patchSize_{};
    bool ownsTrampoline_{};
    std::vector<std::uint8_t> originalBytes_{};
    std::uintptr_t relay_{};
    bool ownsRelay_{};
};

} // namespace frontier::game
