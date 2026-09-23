#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace frontier::game {

class NativeInvoker final {
public:
    using NativeHandler = void(*)(void* context);

    bool initialize(std::uintptr_t moduleBase, std::size_t imageSize, std::uintptr_t textRva, std::size_t textSize);
    bool ready() const { return ready_; }
    const std::string& last_error() const { return lastError_; }

    bool has_handler(std::uint32_t hash) const;
    bool current_handler(std::uint32_t hash, NativeHandler& out) const;
    bool invoke_u32(std::uint32_t hash, std::uint32_t& out) const;
    bool invoke_raw(std::uint32_t hash, const std::uintptr_t* arguments,
                    std::size_t argumentCount, std::uintptr_t& out) const;
    bool invoke_raw_mutable(std::uint32_t hash, std::uintptr_t* arguments,
                             std::size_t argumentCount, std::uintptr_t& out) const;
    bool invoke_bool(std::uint32_t hash, bool& out) const;

    bool hook_native(std::uint32_t hash, NativeHandler replacement, NativeHandler& original, std::string* error = nullptr);
    bool unhook_native(std::uint32_t hash, NativeHandler replacement, NativeHandler original);

private:

    NativeHandler get_handler(std::uint32_t hash) const;
    std::uintptr_t find_handler_in_table(std::uintptr_t table, std::uint32_t modulator, std::uint32_t hash) const;
    std::uintptr_t find_entry_in_table(std::uintptr_t table, std::uint32_t modulator, std::uint32_t hash) const;
    bool read_table(std::uintptr_t storage, std::uintptr_t& table, std::uint32_t& modulator, std::string* diagnostics = nullptr) const;
    std::uintptr_t resolve_rip_target(std::uintptr_t instruction) const;
    bool readable(std::uintptr_t address, std::size_t size) const;

    std::uintptr_t moduleBase_{};
    std::size_t imageSize_{};
    std::uintptr_t nativeRegistrationStorage_{};
    std::uintptr_t registerNative_{};
    bool registrationStorageResolved_{};
    bool ready_{};
    std::string lastError_;
};

} // namespace frontier::game
