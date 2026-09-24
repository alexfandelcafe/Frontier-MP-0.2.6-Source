#include "frontier/game/inline_hook.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <cstring>

namespace frontier::game {

#ifdef _WIN32
namespace {

constexpr std::size_t kAbsoluteJumpSize = 12;

void write_absolute_jump(std::uint8_t* destination, std::uintptr_t target) {
    destination[0] = 0x48; // mov rax, imm64
    destination[1] = 0xB8;
    std::memcpy(destination + 2, &target, sizeof(target));
    destination[10] = 0xFF; // jmp rax
    destination[11] = 0xE0;
}

bool executable_region(std::uintptr_t address, std::size_t size) {
    if (!address || !size) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0 || (mbi.Protect & PAGE_NOACCESS) != 0) {
        return false;
    }
    const DWORD executableFlags =
        PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & executableFlags) == 0) return false;

    const auto begin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    const auto end = begin + mbi.RegionSize;
    return address >= begin && address <= end && size <= end - address;
}

bool patch_function(std::uintptr_t target,
                    const std::uint8_t* patch,
                    std::size_t patchSize,
                    std::string& error) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(target), patchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        error = "VirtualProtect(target) failed";
        return false;
    }

    std::memcpy(reinterpret_cast<void*>(target), patch, patchSize);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(target), patchSize);

    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(target), patchSize, oldProtect, &ignored);
    return true;
}

bool restore_function(std::uintptr_t target,
                      const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) return true;

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(target), bytes.size(), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }

    std::memcpy(reinterpret_cast<void*>(target), bytes.data(), bytes.size());
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void*>(target), bytes.size());

    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(target), bytes.size(), oldProtect, &ignored);
    return true;
}

} // namespace
#endif

InlineHook::~InlineHook() {
    reset();
}

bool InlineHook::install(std::uintptr_t target,
                         std::uintptr_t replacement,
                         std::size_t patchSize,
                         const std::string& name,
                         std::string& error) {
    error.clear();
    reset();

#ifdef _WIN32
    if (target == 0 || replacement == 0) {
        error = name + ": null target/replacement";
        return false;
    }
    if (patchSize < kAbsoluteJumpSize) {
        error = name + ": patch size too small";
        return false;
    }
    if (!executable_region(target, patchSize)) {
        error = name + ": target is not an executable memory region";
        return false;
    }
    originalBytes_.resize(patchSize);
    std::memcpy(originalBytes_.data(), reinterpret_cast<const void*>(target), patchSize);

    // Build a trampoline. The known RDR fullReadPath entry contains an E8 rel32 call
    // in the first 16 bytes. Re-encode relative calls as absolute calls so the
    // trampoline does not depend on being within +/-2 GiB of the original function.
    const std::size_t trampolineCapacity = patchSize + 32;
    auto* trampoline = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, trampolineCapacity, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        originalBytes_.clear();
        error = name + ": VirtualAlloc(trampoline) failed";
        return false;
    }

    std::size_t sourceOffset = 0;
    std::size_t trampolineOffset = 0;
    while (sourceOffset < patchSize) {
        const auto opcode = originalBytes_[sourceOffset];
        if ((opcode == 0xE8 || opcode == 0xE9) && sourceOffset + 5 <= patchSize) {
            std::int32_t displacement = 0;
            std::memcpy(&displacement, originalBytes_.data() + sourceOffset + 1, sizeof(displacement));
            const auto sourceInstruction =
                target + sourceOffset;
            const auto branchTarget =
                sourceInstruction + 5 + static_cast<std::intptr_t>(displacement);

            if (opcode == 0xE8) {
                // mov rax, imm64; call rax
                trampoline[trampolineOffset++] = 0x48;
                trampoline[trampolineOffset++] = 0xB8;
                std::memcpy(trampoline + trampolineOffset, &branchTarget, sizeof(branchTarget));
                trampolineOffset += sizeof(branchTarget);
                trampoline[trampolineOffset++] = 0xFF;
                trampoline[trampolineOffset++] = 0xD0;
            } else {
                // A relative JMP is not used by the known fullReadPath prologue.
                // Refuse rather than silently emit an incorrect trampoline.
                VirtualFree(trampoline, 0, MEM_RELEASE);
                originalBytes_.clear();
                error = name + ": relative JMP inside patch window is unsupported";
                return false;
            }

            sourceOffset += 5;
            continue;
        }

        trampoline[trampolineOffset++] = originalBytes_[sourceOffset++];
    }

    write_absolute_jump(trampoline + trampolineOffset, target + patchSize);
    trampolineOffset += kAbsoluteJumpSize;

    std::vector<std::uint8_t> patch(patchSize, 0x90);
    write_absolute_jump(patch.data(), replacement);

    if (!patch_function(target, patch.data(), patch.size(), error)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        originalBytes_.clear();
        return false;
    }

    target_ = target;
    trampoline_ = reinterpret_cast<std::uintptr_t>(trampoline);
    patchSize_ = patchSize;
    (void)trampolineOffset;
    return true;
#else
    (void)target;
    (void)replacement;
    (void)patchSize;
    (void)name;
    error = "inline hooks are Windows-only";
    return false;
#endif
}

void InlineHook::reset() {
#ifdef _WIN32
    if (target_ != 0 && !originalBytes_.empty()) {
        restore_function(target_, originalBytes_);
    }
    if (trampoline_ != 0) {
        VirtualFree(reinterpret_cast<void*>(trampoline_), 0, MEM_RELEASE);
    }
#endif

    target_ = 0;
    trampoline_ = 0;
    patchSize_ = 0;
    originalBytes_.clear();
}

} // namespace frontier::game
