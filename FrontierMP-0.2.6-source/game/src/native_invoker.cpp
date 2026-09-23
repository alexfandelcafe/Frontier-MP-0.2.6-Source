#include "frontier/game/native_invoker.hpp"

#include "frontier/game/pattern_scanner.hpp"

#include <cstring>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#endif

namespace frontier::game {

namespace {

constexpr const char* kNativeRegistrationPattern =
    "4C 8B 1D ? ? ? ? 41 8B C1";

constexpr const char* kRegisterNativePattern =
    "48 89 5C 24 ? 57 48 83 EC 20 44 8B 0D ? ? ? ?";

struct NativeTempContext final {
    void* returnBuffer{};
    std::uint32_t argumentCount{};
    void* argumentBuffer{};
    std::uint32_t dataCount{};
    void* outputVectors[4]{};
    std::uint8_t inputVectors[0x30]{};
    std::uintptr_t stack[32]{};
};

} // namespace


#ifdef _WIN32
bool guarded_copy(const void* source, void* destination, std::size_t size) noexcept {
    if (!source || !destination || size == 0) return false;
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool invoke_handler_guarded(void (*handler)(void*), void* context) noexcept {
    if (!handler || !context) return false;
    __try {
        handler(context);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#endif

std::uintptr_t NativeInvoker::resolve_rip_target(std::uintptr_t instruction) const {
#ifdef _WIN32
    if (!readable(instruction + 3, sizeof(std::int32_t))) return 0;
    std::int32_t displacement = 0;
    std::memcpy(&displacement, reinterpret_cast<const void*>(instruction + 3), sizeof(displacement));
    return instruction + 7 + static_cast<std::intptr_t>(displacement);
#else
    (void)instruction;
    return 0;
#endif
}

bool NativeInvoker::readable(std::uintptr_t address, std::size_t size) const {
#ifdef _WIN32
    if (address == 0 || size == 0) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if ((mbi.Protect & PAGE_GUARD) != 0 || (mbi.Protect & PAGE_NOACCESS) != 0) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    const auto end = begin + mbi.RegionSize;
    if (address < begin || address >= end || size > end - address) return false;
    const DWORD readableFlags = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & readableFlags) != 0;
#else
    (void)address;
    (void)size;
    return false;
#endif
}

std::uintptr_t NativeInvoker::find_entry_in_table(std::uintptr_t table,
                                                       std::uint32_t modulator,
                                                       std::uint32_t hash) const {
#ifdef _WIN32
    if (!table || modulator == 0 || modulator > 0x100000u) return 0;
    std::uint32_t tempHash = hash;
    std::uint32_t index = hash % modulator;
    for (std::uint32_t probes = 0; probes < modulator; ++probes) {
        const auto entry = table + static_cast<std::uintptr_t>(index) * 16u;
        if (!readable(entry, 16)) return 0;

        std::uint32_t entryHash = 0;
        if (!guarded_copy(reinterpret_cast<const void*>(entry), &entryHash, sizeof(entryHash))) {
            return 0;
        }
        if (entryHash == hash) return entry;
        if (entryHash == 0) return 0;

        tempHash = (tempHash >> 1u) + 1u;
        index = (tempHash + index) % modulator;
    }
#endif
    (void)table;
    (void)modulator;
    (void)hash;
    return 0;
}

std::uintptr_t NativeInvoker::find_handler_in_table(std::uintptr_t table,
                                                        std::uint32_t modulator,
                                                        std::uint32_t hash) const {
#ifdef _WIN32
    if (!table || modulator == 0 || modulator > 0x100000u) return 0;
    std::uint32_t tempHash = hash;
    std::uint32_t index = hash % modulator;
    for (std::uint32_t probes = 0; probes < modulator; ++probes) {
        const auto entry = table + static_cast<std::uintptr_t>(index) * 16u;
        if (!readable(entry, 16)) return 0;

        std::uint32_t entryHash = 0;
        std::uintptr_t handler = 0;
        if (!guarded_copy(reinterpret_cast<const void*>(entry), &entryHash, sizeof(entryHash)) ||
            !guarded_copy(reinterpret_cast<const void*>(entry + 8), &handler, sizeof(handler))) {
            return 0;
        }

        if (entryHash == hash) return handler;
        if (entryHash == 0) return 0;

        tempHash = (tempHash >> 1u) + 1u;
        index = (tempHash + index) % modulator;
    }
#endif
    (void)table;
    (void)modulator;
    (void)hash;
    return 0;
}

bool NativeInvoker::read_table(std::uintptr_t storage, std::uintptr_t& table,
                               std::uint32_t& modulator, std::string* diagnostics) const {
#ifdef _WIN32
    table = 0;
    modulator = 0;
    if (diagnostics) diagnostics->clear();

    if (!storage) {
        if (diagnostics) *diagnostics = "storage=0";
        return false;
    }
    // G.R.E-Lab models sm_CommandsRegistrationTable as:
    //   +0x00: pointer to the 16-byte native registration entries
    //   +0x08: uint32 modulator
    // The modulator therefore belongs to the storage object, not the table.
    if (!readable(storage, sizeof(std::uintptr_t) + sizeof(std::uint32_t))) {
        if (diagnostics) {
            char buffer[128]{};
            std::snprintf(buffer, sizeof(buffer), "storage=0x%llX unreadable",
                          static_cast<unsigned long long>(storage));
            *diagnostics = buffer;
        }
        return false;
    }

    std::uintptr_t rawTable = 0;
    if (!guarded_copy(reinterpret_cast<const void*>(storage), &rawTable, sizeof(rawTable))) {
        if (diagnostics) {
            char buffer[128]{};
            std::snprintf(buffer, sizeof(buffer), "storage=0x%llX table-read-failed",
                          static_cast<unsigned long long>(storage));
            *diagnostics = buffer;
        }
        return false;
    }

    std::uint32_t rawModulator = 0;
    if (!guarded_copy(reinterpret_cast<const void*>(storage + sizeof(std::uintptr_t)),
                      &rawModulator, sizeof(rawModulator))) {
        if (diagnostics) {
            char buffer[128]{};
            std::snprintf(buffer, sizeof(buffer), "storage=0x%llX mod-read-failed",
                          static_cast<unsigned long long>(storage));
            *diagnostics = buffer;
        }
        return false;
    }

    table = rawTable;
    modulator = rawModulator;

    if (!table) {
        if (diagnostics) {
            char buffer[160]{};
            std::snprintf(buffer, sizeof(buffer), "storage=0x%llX table=0 mod=%u",
                          static_cast<unsigned long long>(storage), rawModulator);
            *diagnostics = buffer;
        }
        return false;
    }

    if (!readable(table, 16)) {
        if (diagnostics) {
            char buffer[160]{};
            std::snprintf(buffer, sizeof(buffer), "storage=0x%llX table=0x%llX unreadable",
                          static_cast<unsigned long long>(storage),
                          static_cast<unsigned long long>(table));
            *diagnostics = buffer;
        }
        table = 0;
        return false;
    }

    if (modulator == 0 || modulator > 0x100000u) {
        if (diagnostics) {
            char buffer[192]{};
            std::snprintf(buffer, sizeof(buffer), "storage=0x%llX table=0x%llX mod=%u invalid",
                          static_cast<unsigned long long>(storage),
                          static_cast<unsigned long long>(table),
                          rawModulator);
            *diagnostics = buffer;
        }
        table = 0;
        modulator = 0;
        return false;
    }

    if (diagnostics) {
        char buffer[160]{};
        std::snprintf(buffer, sizeof(buffer), "storage=0x%llX table=0x%llX mod=%u",
                      static_cast<unsigned long long>(storage),
                      static_cast<unsigned long long>(table),
                      rawModulator);
        *diagnostics = buffer;
    }
    return true;
#else
    (void)storage;
    (void)diagnostics;
    table = 0;
    modulator = 0;
    return false;
#endif
}

bool NativeInvoker::initialize(std::uintptr_t moduleBase, std::size_t imageSize,
                               std::uintptr_t textRva, std::size_t textSize) {
#ifdef _WIN32
    if (ready_) return true;
    if (!moduleBase || !imageSize || !textRva || !textSize) {
        lastError_ = "invalid module/text metadata";
        return false;
    }

    if (moduleBase_ != moduleBase || imageSize_ != imageSize) {
        moduleBase_ = moduleBase;
        imageSize_ = imageSize;
        nativeRegistrationStorage_ = 0;
        registerNative_ = 0;
        registrationStorageResolved_ = false;
        ready_ = false;
        lastError_.clear();
    }

    // Resolve the RIP-relative global only once. The pointed-to table is initialized
    // by the game at a later stage, so subsequent calls poll the same storage rather
    // than rescanning the full .text section every frame.
    if (!registrationStorageResolved_) {
        const auto* text = reinterpret_cast<const std::uint8_t*>(moduleBase_ + textRva);
        const auto registrationPattern = BytePattern::parse(kNativeRegistrationPattern);
        if (!registrationPattern) {
            lastError_ = "failed to parse native registration pattern";
            return false;
        }

        const auto& bytes = registrationPattern->bytes;
        if (bytes.empty() || textSize < bytes.size()) {
            lastError_ = "native registration pattern invalid for text section";
            return false;
        }

        std::size_t candidates = 0;
        std::uintptr_t selectedStorage = 0;
        for (std::size_t offset = 0; offset + bytes.size() <= textSize; ++offset) {
            bool match = true;
            for (std::size_t i = 0; i < bytes.size(); ++i) {
                if (bytes[i].has_value() && text[offset + i] != bytes[i].value()) {
                    match = false;
                    break;
                }
            }
            if (!match) continue;

            ++candidates;
            const auto hit = reinterpret_cast<std::uintptr_t>(text + offset);
            if (!PatternScanner::validate_in_module(hit, moduleBase_, imageSize_)) continue;

            const auto storage = resolve_rip_target(hit);
            if (!storage) continue;

            if (!readable(storage, sizeof(std::uintptr_t))) continue;
            if (!PatternScanner::validate_in_module(storage, moduleBase_, imageSize_)) {
                // A RIP-referenced registration object should live inside the main image.
                continue;
            }

            selectedStorage = storage;
            break;
        }

        if (!selectedStorage) {
            char buffer[192]{};
            std::snprintf(buffer, sizeof(buffer),
                          "no registration storage: candidates=%zu", candidates);
            lastError_ = buffer;
            return false;
        }

        nativeRegistrationStorage_ = selectedStorage;
        registrationStorageResolved_ = true;
        lastError_.clear();
        std::fprintf(stderr,
            "[FrontierNative] resolved registration storage=0x%llX candidates=%zu\n",
            static_cast<unsigned long long>(nativeRegistrationStorage_), candidates);
    }

    std::uintptr_t table = 0;
    std::uint32_t modulator = 0;
    std::string diagnostics;
    if (!read_table(nativeRegistrationStorage_, table, modulator, &diagnostics)) {
        lastError_ = "registration table not initialized: " + diagnostics;
        return false;
    }

    const auto sentinelHandler = find_handler_in_table(table, modulator, 0xA0AE0C98u);
    const auto getPositionHandler = find_handler_in_table(table, modulator, 0x99BD9D6Fu);
    const auto getGameStateHandler = find_handler_in_table(table, modulator, 0xDD9BD22Bu);

    const auto streamingWorldLoadedHandler = find_handler_in_table(table, modulator, 0x87B74064u);
    const auto simulateStartMultiplayerHandler = find_handler_in_table(table, modulator, 0x9A73C2CDu);
    const auto startPosCommandLineHandler = find_handler_in_table(table, modulator, 0x814D97E8u);

    std::fprintf(stderr,
        "[FrontierNative] table=0x%llX mod=%u sentinel=%s getPosition=%s getGameState=%s worldLoaded=%s simulateMp=%s startPos=%s\n",
        static_cast<unsigned long long>(table), modulator,
        sentinelHandler ? "yes" : "no",
        getPositionHandler ? "yes" : "no",
        getGameStateHandler ? "yes" : "no",
        streamingWorldLoadedHandler ? "yes" : "no",
        simulateStartMultiplayerHandler ? "yes" : "no",
        startPosCommandLineHandler ? "yes" : "no");

    if (!getPositionHandler && !getGameStateHandler) {
        lastError_ = "native table found but known RDR native handlers were not found";
        return false;
    }

    ready_ = true;
    lastError_.clear();
    return true;
#else
    (void)moduleBase;
    (void)imageSize;
    (void)textRva;
    (void)textSize;
    return false;
#endif
}

bool NativeInvoker::has_handler(std::uint32_t hash) const {
#ifdef _WIN32
    return get_handler(hash) != nullptr;
#else
    (void)hash;
    return false;
#endif
}

bool NativeInvoker::current_handler(std::uint32_t hash, NativeHandler& out) const {
    out = nullptr;
#ifdef _WIN32
    out = get_handler(hash);
    return out != nullptr;
#else
    (void)hash;
    return false;
#endif
}

NativeInvoker::NativeHandler NativeInvoker::get_handler(std::uint32_t hash) const {
#ifdef _WIN32
    if (!ready_ || !nativeRegistrationStorage_) return nullptr;

    std::uintptr_t table = 0;
    std::uint32_t modulator = 0;
    if (!read_table(nativeRegistrationStorage_, table, modulator)) return nullptr;
    const auto handler = find_handler_in_table(table, modulator, hash);
    if (!handler) return nullptr;
    return reinterpret_cast<NativeHandler>(handler);
#else
    (void)hash;
    return nullptr;
#endif
}

bool NativeInvoker::invoke_u32(std::uint32_t hash, std::uint32_t& out) const {
#ifdef _WIN32
    out = 0;
    const auto handler = get_handler(hash);
    if (!handler) return false;

    NativeTempContext context{};
    context.returnBuffer = context.stack;
    context.argumentBuffer = context.stack;
    context.argumentCount = 0;
    context.dataCount = 0;

    if (!invoke_handler_guarded(handler, &context)) return false;
    return guarded_copy(&returnValue, &out, sizeof(out));
#else
    (void)hash;
    out = 0;
    return false;
#endif
}

bool NativeInvoker::invoke_raw(std::uint32_t hash, const std::uintptr_t* arguments,
                                  std::size_t argumentCount, std::uintptr_t& out) const {
#ifdef _WIN32
    out = 0;
    if (argumentCount > 32u) return false;

    const auto handler = get_handler(hash);
    if (!handler) return false;

    NativeTempContext context{};
    context.returnBuffer = context.stack;
    context.argumentBuffer = context.stack;
    context.argumentCount = static_cast<std::uint32_t>(argumentCount);
    context.dataCount = 0;

    if (argumentCount != 0u) {
        if (!arguments) return false;
        std::memcpy(context.stack, arguments, argumentCount * sizeof(context.stack[0]));
    }

    if (!invoke_handler_guarded(handler, &context)) return false;
    return guarded_copy(context.stack, &out, sizeof(out));
#else
    (void)hash;
    (void)arguments;
    (void)argumentCount;
    out = 0;
    return false;
#endif
}

bool NativeInvoker::invoke_raw_mutable(std::uint32_t hash, std::uintptr_t* arguments,
                                            std::size_t argumentCount, std::uintptr_t& out) const {
#ifdef _WIN32
    out = 0;
    if (argumentCount > 32u) return false;

    const auto handler = get_handler(hash);
    if (!handler) return false;
    if (argumentCount != 0u && !arguments) return false;

    NativeTempContext context{};
    // Match the live native context: returnBuffer and argumentBuffer are distinct.
    // CREATE_PLAYER_ACTOR_IN_LAYOUT returns PlayerId while rewriting argument 0 with ActorRef.
    std::uintptr_t returnValue = 0;
    context.returnBuffer = &returnValue;
    context.argumentBuffer = context.stack;
    context.argumentCount = static_cast<std::uint32_t>(argumentCount);
    context.dataCount = 0;

    if (argumentCount != 0u) {
        std::memcpy(context.stack, arguments, argumentCount * sizeof(context.stack[0]));
    }

    if (!invoke_handler_guarded(handler, &context)) return false;

    if (argumentCount != 0u) {
        std::memcpy(arguments, context.stack, argumentCount * sizeof(context.stack[0]));
    }
    return guarded_copy(&returnValue, &out, sizeof(out));
#else
    (void)hash;
    (void)arguments;
    (void)argumentCount;
    out = 0;
    return false;
#endif
}

bool NativeInvoker::hook_native(std::uint32_t hash, NativeHandler replacement,
                                      NativeHandler& original, std::string* error) {
#ifdef _WIN32
    original = nullptr;
    if (error) error->clear();
    if (!ready_ || !nativeRegistrationStorage_) {
        if (error) *error = "native invoker not ready";
        return false;
    }
    if (!replacement) {
        if (error) *error = "replacement handler is null";
        return false;
    }

    std::uintptr_t table = 0;
    std::uint32_t modulator = 0;
    if (!read_table(nativeRegistrationStorage_, table, modulator, error)) return false;

    const auto entry = find_entry_in_table(table, modulator, hash);
    if (!entry) {
        if (error) {
            char buffer[128]{};
            std::snprintf(buffer, sizeof(buffer), "native handler not found: 0x%08X", hash);
            *error = buffer;
        }
        return false;
    }

    std::uintptr_t current = 0;
    if (!guarded_copy(reinterpret_cast<const void*>(entry + 8), &current, sizeof(current))) {
        if (error) *error = "native handler pointer read failed";
        return false;
    }
    if (!current) {
        if (error) *error = "native handler pointer is null";
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(entry + 8), sizeof(std::uintptr_t),
                        PAGE_READWRITE, &oldProtect)) {
        if (error) *error = "native handler table protection change failed";
        return false;
    }

    const auto replacementPtr = reinterpret_cast<std::uintptr_t>(replacement);
    const auto previousPtr = reinterpret_cast<std::uintptr_t>(
        InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(entry + 8),
                                    reinterpret_cast<PVOID>(replacementPtr)));

    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(entry + 8), sizeof(std::uintptr_t),
                   oldProtect, &ignored);

    original = reinterpret_cast<NativeHandler>(previousPtr);
    if (!original) {
        if (error) *error = "native handler replacement returned null original";
        return false;
    }

    std::fprintf(stderr,
                 "[FrontierNative] hooked hash=0x%08X entry=0x%llX original=0x%llX replacement=0x%llX\n",
                 hash,
                 static_cast<unsigned long long>(entry),
                 static_cast<unsigned long long>(previousPtr),
                 static_cast<unsigned long long>(replacementPtr));
    return true;
#else
    (void)hash;
    (void)replacement;
    original = nullptr;
    if (error) *error = "native hooking is Windows-only";
    return false;
#endif
}

bool NativeInvoker::unhook_native(std::uint32_t hash, NativeHandler replacement, NativeHandler original) {
#ifdef _WIN32
    if (!ready_ || !nativeRegistrationStorage_ || !replacement || !original) return false;

    std::uintptr_t table = 0;
    std::uint32_t modulator = 0;
    if (!read_table(nativeRegistrationStorage_, table, modulator)) return false;

    const auto entry = find_entry_in_table(table, modulator, hash);
    if (!entry) return false;

    std::uintptr_t current = 0;
    if (!guarded_copy(reinterpret_cast<const void*>(entry + 8), &current, sizeof(current))) return false;
    if (current != reinterpret_cast<std::uintptr_t>(replacement)) return false;

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(entry + 8), sizeof(std::uintptr_t),
                        PAGE_READWRITE, &oldProtect)) {
        return false;
    }

    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(entry + 8),
                                reinterpret_cast<PVOID>(reinterpret_cast<std::uintptr_t>(original)));

    DWORD ignored = 0;
    VirtualProtect(reinterpret_cast<void*>(entry + 8), sizeof(std::uintptr_t),
                   oldProtect, &ignored);
    return true;
#else
    (void)hash;
    (void)replacement;
    (void)original;
    return false;
#endif
}

bool NativeInvoker::invoke_bool(std::uint32_t hash, bool& out) const {
    std::uint32_t result = 0;
    if (!invoke_u32(hash, result)) {
        out = false;
        return false;
    }
    out = result != 0;
    return true;
}

} // namespace frontier::game
