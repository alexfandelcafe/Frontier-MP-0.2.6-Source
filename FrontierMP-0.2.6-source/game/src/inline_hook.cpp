#include "frontier/game/inline_hook.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <cstring>
#include <initializer_list>
#include <limits>

namespace frontier::game {

#ifdef _WIN32
namespace {

// FF 25 00 00 00 00
// <8-byte absolute target>
//
// Unlike "mov rax, imm64; jmp rax", this form does not clobber a GPR.
// The hooked function may legitimately rely on RAX across the trampoline
// return (for example, when a call in the stolen prologue produced a value
// consumed by the following instruction).
constexpr std::size_t kAbsoluteJumpSize = 14;
constexpr std::size_t kRelaySize = kAbsoluteJumpSize;
constexpr std::uintptr_t kAllocationGranularity = 0x10000ull;
constexpr std::uintptr_t kRelaySearchStep = 0x1000000ull;

void write_absolute_jump(std::uint8_t* destination, std::uintptr_t target) {
    destination[0] = 0xFF; // jmp qword ptr [rip+0]
    destination[1] = 0x25;
    destination[2] = 0x00;
    destination[3] = 0x00;
    destination[4] = 0x00;
    destination[5] = 0x00;
    std::memcpy(destination + 6, &target, sizeof(target));
}

bool rel32_reachable(std::uintptr_t nextInstruction, std::uintptr_t target) {
    const auto delta =
        static_cast<std::intptr_t>(target) - static_cast<std::intptr_t>(nextInstruction);
    return delta >= static_cast<std::intptr_t>(std::numeric_limits<std::int32_t>::min()) &&
           delta <= static_cast<std::intptr_t>(std::numeric_limits<std::int32_t>::max());
}

void* allocate_near(std::uintptr_t nearAddress) {
    const auto aligned = nearAddress & ~(kAllocationGranularity - 1ull);
    constexpr std::uintptr_t kMaxDistance =
        static_cast<std::uintptr_t>(std::numeric_limits<std::int32_t>::max());

    for (std::uintptr_t distance = 0; distance <= kMaxDistance; distance += kRelaySearchStep) {
        const std::uintptr_t candidates[] = {
            aligned + distance,
            distance <= aligned ? aligned - distance : 0
        };

        for (const auto candidate : candidates) {
            if (candidate == 0 || !rel32_reachable(nearAddress + 5u, candidate)) continue;

            void* allocation = VirtualAlloc(
                reinterpret_cast<void*>(candidate),
                kRelaySize,
                MEM_COMMIT | MEM_RESERVE,
                PAGE_EXECUTE_READWRITE);
            if (!allocation) continue;

            const auto address = reinterpret_cast<std::uintptr_t>(allocation);
            if (rel32_reachable(nearAddress + 5u, address)) {
                return allocation;
            }

            VirtualFree(allocation, 0, MEM_RELEASE);
        }

        if (distance > kMaxDistance - kRelaySearchStep) break;
    }

    return nullptr;
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

    // Build a trampoline. The known RDR fullReadPath entry contains an E8 rel32
    // call in the first 16 bytes. Re-encode that relative call with volatile R11
    // so the trampoline does not depend on +/-2 GiB and, importantly, does not
    // destroy RAX.
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

        // Relocate the RIP-relative LEA used by the historical Wait
        // target prologue (for example: 4C 8D 05 disp32). A copied RIP-relative
        // displacement is relative to the trampoline, not the original target,
        // so leaving it unchanged makes the original function dereference/read
        // the wrong address after the detour.
        if (sourceOffset + 7 <= patchSize &&
            originalBytes_[sourceOffset + 1] == 0x8D &&
            originalBytes_[sourceOffset + 2] == 0x05 &&
            (originalBytes_[sourceOffset] & 0xF8u) == 0x48u &&
            originalBytes_[sourceOffset] != 0x4Bu) {
            const auto rex = originalBytes_[sourceOffset];
            std::int32_t displacement = 0;
            std::memcpy(
                &displacement,
                originalBytes_.data() + sourceOffset + 3,
                sizeof(displacement));

            const auto sourceInstruction = target + sourceOffset;
            const auto absoluteAddress =
                sourceInstruction + 7 + static_cast<std::intptr_t>(displacement);

            // mov r11, imm64
            trampoline[trampolineOffset++] = 0x49;
            trampoline[trampolineOffset++] = 0xBB;
            std::memcpy(
                trampoline + trampolineOffset,
                &absoluteAddress,
                sizeof(absoluteAddress));
            trampolineOffset += sizeof(absoluteAddress);

            // mov <LEA-destination>, r11
            // The LEA destination is encoded in ModRM.reg plus REX.R.
            const auto destinationRegister =
                static_cast<std::uint8_t>(
                    ((rex >> 2u) & 1u) * 8u);
            trampoline[trampolineOffset++] =
                static_cast<std::uint8_t>(
                    0x49u | ((destinationRegister >= 8u) ? 0x04u : 0x00u));
            trampoline[trampolineOffset++] = 0x8B;
            trampoline[trampolineOffset++] =
                static_cast<std::uint8_t>(
                    0xC0u |
                    static_cast<std::uint8_t>((destinationRegister & 7u) << 3u) |
                    0x03u);

            sourceOffset += 7;
            continue;
        }

        if (opcode == 0xE8 && sourceOffset == 4 && sourceOffset + 5 <= patchSize) {
            std::int32_t displacement = 0;
            std::memcpy(&displacement,
                        originalBytes_.data() + sourceOffset + 1,
                        sizeof(displacement));

            const auto sourceInstruction = target + sourceOffset;
            const auto branchTarget =
                sourceInstruction + 5 + static_cast<std::intptr_t>(displacement);

            // mov r11, imm64; call r11
            trampoline[trampolineOffset++] = 0x49;
            trampoline[trampolineOffset++] = 0xBB;
            std::memcpy(trampoline + trampolineOffset, &branchTarget, sizeof(branchTarget));
            trampolineOffset += sizeof(branchTarget);
            trampoline[trampolineOffset++] = 0x41;
            trampoline[trampolineOffset++] = 0xFF;
            trampoline[trampolineOffset++] = 0xD3;

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
    ownsTrampoline_ = true;
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


bool InlineHook::install_preserving_entry_registers(
    std::uintptr_t target,
    std::uintptr_t callback,
    std::size_t patchSize,
    const std::string& name,
    std::string& error) {
    error.clear();

#ifdef _WIN32
    // Build the normal trampoline first. The temporary replacement is the
    // callback itself; the target is repatched to the register-preserving
    // gateway before the hook becomes observable during normal game execution.
    if (!install(target, callback, patchSize, name, error)) {
        return false;
    }

    constexpr std::size_t kGatewaySize = 256u;
    // Leave the mandatory Windows x64 shadow space untouched. The saved GPRs
    // begin at +0x40 so the callback cannot overwrite them while using its
    // home space at +0x00..+0x1F.
    constexpr std::uint32_t kShadowSpaceSize = 0x40u;
    constexpr std::uint32_t kStackFrameSize = 0xC8u;
    const auto* trampolineAddress =
        reinterpret_cast<const std::uint8_t*>(trampoline_);

    auto* gateway = static_cast<std::uint8_t*>(
        VirtualAlloc(
            nullptr,
            kGatewaySize,
            MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE));
    if (gateway == nullptr) {
        reset();
        error = name + ": register-preserving gateway allocation failed";
        return false;
    }

    std::size_t offset = 0;
    auto emit = [&](std::initializer_list<std::uint8_t> bytes) {
        for (const auto byte : bytes) {
            gateway[offset++] = byte;
        }
    };
    auto emit_u64 = [&](std::uintptr_t value) {
        std::memcpy(gateway + offset, &value, sizeof(value));
        offset += sizeof(value);
    };
    auto emit_stack_access = [&](std::uint8_t opcode,
                                  std::uint8_t reg,
                                  std::uint32_t stackOffset) {
        const std::uint8_t rex = static_cast<std::uint8_t>(
            0x48u | (reg >= 8u ? 0x04u : 0u));
        if (stackOffset <= 0x7Fu) {
            const std::uint8_t modrm = static_cast<std::uint8_t>(
                0x44u | ((reg & 7u) << 3u));
            emit({rex, opcode, modrm, 0x24u,
                  static_cast<std::uint8_t>(stackOffset)});
            return;
        }

        // RSP-relative disp8 is signed. Offsets >= 0x80 must use a disp32;
        // otherwise 0x80 becomes -128 and the gateway writes below RSP.
        const std::uint8_t modrm = static_cast<std::uint8_t>(
            0x84u | ((reg & 7u) << 3u));
        emit({rex, opcode, modrm, 0x24u});
        emit({
            static_cast<std::uint8_t>(stackOffset & 0xFFu),
            static_cast<std::uint8_t>((stackOffset >> 8u) & 0xFFu),
            static_cast<std::uint8_t>((stackOffset >> 16u) & 0xFFu),
            static_cast<std::uint8_t>((stackOffset >> 24u) & 0xFFu)
        });
    };
    auto emit_save = [&](std::uint8_t reg) {
        emit_stack_access(
            0x89u,
            reg,
            kShadowSpaceSize + static_cast<std::uint32_t>(reg) * 8u);
    };
    auto emit_restore = [&](std::uint8_t reg) {
        emit_stack_access(
            0x8Bu,
            reg,
            kShadowSpaceSize + static_cast<std::uint32_t>(reg) * 8u);
    };

    // Preserve every GPR except RSP. Present has no stack arguments, so the
    // saved original argument registers remain valid for the C++ callback.
    emit({0x48u, 0x81u, 0xECu,
          static_cast<std::uint8_t>(kStackFrameSize & 0xFFu),
          static_cast<std::uint8_t>((kStackFrameSize >> 8u) & 0xFFu),
          static_cast<std::uint8_t>((kStackFrameSize >> 16u) & 0xFFu),
          static_cast<std::uint8_t>((kStackFrameSize >> 24u) & 0xFFu)});

    constexpr std::uint8_t kSavedRegisters[] = {
        0u, 1u, 2u, 3u, 5u, 6u, 7u,
        8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u};
    for (const auto reg : kSavedRegisters) {
        emit_save(reg);
    }

    // RAX is saved above, so it is safe to use as a volatile absolute-call
    // register for the C++ callback.
    emit({0x48u, 0xB8u});
    emit_u64(callback);
    emit({0xFFu, 0xD0u});

    for (std::size_t index = sizeof(kSavedRegisters) / sizeof(kSavedRegisters[0]); index != 0; --index) {
        emit_restore(kSavedRegisters[index - 1]);
    }

    emit({0x48u, 0x81u, 0xC4u,
          static_cast<std::uint8_t>(kStackFrameSize & 0xFFu),
          static_cast<std::uint8_t>((kStackFrameSize >> 8u) & 0xFFu),
          static_cast<std::uint8_t>((kStackFrameSize >> 16u) & 0xFFu),
          static_cast<std::uint8_t>((kStackFrameSize >> 24u) & 0xFFu)});

    // Register-free absolute jump back to the trampoline. The pointer lives
    // after the six-byte FF 25 instruction, so no restored register is clobbered.
    emit({0xFFu, 0x25u, 0x00u, 0x00u, 0x00u, 0x00u});
    emit_u64(reinterpret_cast<std::uintptr_t>(trampolineAddress));

    if (offset > kGatewaySize) {
        VirtualFree(gateway, 0, MEM_RELEASE);
        reset();
        error = name + ": register-preserving gateway overflow";
        return false;
    }

    FlushInstructionCache(
        GetCurrentProcess(),
        gateway,
        offset);

    gateway_ = reinterpret_cast<std::uintptr_t>(gateway);
    ownsGateway_ = true;

    std::vector<std::uint8_t> patch(patchSize, 0x90u);
    write_absolute_jump(patch.data(), gateway_);

    if (!patch_function(target_, patch.data(), patch.size(), error)) {
        reset();
        return false;
    }

    return true;
#else
    (void)target;
    (void)callback;
    (void)patchSize;
    (void)name;
    error = "register-preserving inline hooks are Windows-only";
    return false;
#endif
}

bool InlineHook::install_call_site(std::uintptr_t callSite,
                                  std::uintptr_t replacement,
                                  const std::string& name,
                                  std::string& error) {
    error.clear();
    reset();

#ifdef _WIN32
    constexpr std::size_t kCallInstructionSize = 5;
    if (callSite == 0 || replacement == 0) {
        error = name + ": null call-site/replacement";
        return false;
    }
    if (!executable_region(callSite, kCallInstructionSize)) {
        error = name + ": call-site is not an executable memory region";
        return false;
    }

    std::uint8_t original[kCallInstructionSize]{};
    std::memcpy(original, reinterpret_cast<const void*>(callSite), kCallInstructionSize);
    if (original[0] != 0xE8) {
        error = name + ": expected E8 rel32 call-site";
        return false;
    }

    std::int32_t originalDisplacement = 0;
    std::memcpy(&originalDisplacement, original + 1, sizeof(originalDisplacement));
    const auto originalTarget =
        callSite + kCallInstructionSize + static_cast<std::intptr_t>(originalDisplacement);
    if (!executable_region(originalTarget, 1)) {
        error = name + ": decoded original target is not executable";
        return false;
    }

    const auto nextInstruction = callSite + kCallInstructionSize;
    std::uintptr_t callTarget = replacement;
    void* relay = nullptr;

    if (!rel32_reachable(nextInstruction, replacement)) {
        relay = allocate_near(callSite);
        if (!relay) {
            error = name + ": replacement is outside E8 rel32 range and nearby relay allocation failed";
            return false;
        }

        callTarget = reinterpret_cast<std::uintptr_t>(relay);
        write_absolute_jump(
            static_cast<std::uint8_t*>(relay),
            replacement);
        FlushInstructionCache(
            GetCurrentProcess(),
            relay,
            kRelaySize);
    }

    const auto replacementDelta =
        static_cast<std::intptr_t>(callTarget) - static_cast<std::intptr_t>(nextInstruction);

    originalBytes_.assign(original, original + kCallInstructionSize);

    std::uint8_t patch[kCallInstructionSize]{};
    patch[0] = 0xE8;
    const auto replacementDisplacement = static_cast<std::int32_t>(replacementDelta);
    std::memcpy(patch + 1, &replacementDisplacement, sizeof(replacementDisplacement));

    if (!patch_function(callSite, patch, kCallInstructionSize, error)) {
        originalBytes_.clear();
        return false;
    }

    target_ = callSite;
    trampoline_ = originalTarget;
    patchSize_ = kCallInstructionSize;
    ownsTrampoline_ = false;
    relay_ = reinterpret_cast<std::uintptr_t>(relay);
    ownsRelay_ = relay != nullptr;
    return true;
#else
    (void)callSite;
    (void)replacement;
    (void)name;
    error = "inline hooks are Windows-only";
    return false;
#endif
}

void InlineHook::uninstall() {
    reset();
}

void InlineHook::reset() {
#ifdef _WIN32
    if (target_ != 0 && !originalBytes_.empty()) {
        restore_function(target_, originalBytes_);
    }
    if (trampoline_ != 0 && ownsTrampoline_) {
        VirtualFree(reinterpret_cast<void*>(trampoline_), 0, MEM_RELEASE);
    }
    if (relay_ != 0 && ownsRelay_) {
        VirtualFree(reinterpret_cast<void*>(relay_), 0, MEM_RELEASE);
    }
    if (gateway_ != 0 && ownsGateway_) {
        VirtualFree(reinterpret_cast<void*>(gateway_), 0, MEM_RELEASE);
    }
#endif

    target_ = 0;
    trampoline_ = 0;
    patchSize_ = 0;
    ownsTrampoline_ = false;
    relay_ = 0;
    ownsRelay_ = false;
    gateway_ = 0;
    ownsGateway_ = false;
    originalBytes_.clear();
}

} // namespace frontier::game
