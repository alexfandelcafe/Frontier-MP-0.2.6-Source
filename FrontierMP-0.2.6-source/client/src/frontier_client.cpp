#include "frontier/client/client_runtime.hpp"

#ifdef FRONTIER_ENABLE_CEF
#include "frontier/client/cef_overlay.hpp"
#endif

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <cstdio>

namespace {

frontier::client::ClientRuntime g_runtime;
#ifdef FRONTIER_ENABLE_CEF
frontier::client::CefOverlay g_cefOverlay;
#endif

void write_crash_log(EXCEPTION_POINTERS* exceptionPointers) {
    char localAppData[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;

    const std::filesystem::path dir =
        std::filesystem::path(localAppData) / "FrontierMP" / "logs";

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    const std::filesystem::path path = dir / "client_crash.log";
    const std::filesystem::path clientLogPath = dir / "client.log";
    const HANDLE file = CreateFileA(
        path.string().c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    char modulePath[MAX_PATH]{};
    char buffer[2048]{};

    DWORD modulePathLength = 0;
    std::uintptr_t rip = 0;
    std::uintptr_t faultAddress = 0;
    DWORD code = 0;

    if (exceptionPointers != nullptr && exceptionPointers->ExceptionRecord != nullptr) {
        const EXCEPTION_RECORD* record = exceptionPointers->ExceptionRecord;
        code = record->ExceptionCode;
        faultAddress = reinterpret_cast<std::uintptr_t>(record->ExceptionAddress);

        if (code == EXCEPTION_ACCESS_VIOLATION &&
            record->NumberParameters >= 2) {
            faultAddress = static_cast<std::uintptr_t>(record->ExceptionInformation[1]);
        }
    }

#if defined(_M_X64)
    if (exceptionPointers != nullptr && exceptionPointers->ContextRecord != nullptr) {
        rip = static_cast<std::uintptr_t>(exceptionPointers->ContextRecord->Rip);
    }
#endif

    HMODULE module = nullptr;
    if (rip != 0) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(
                reinterpret_cast<const void*>(rip),
                &mbi,
                sizeof(mbi)) == sizeof(mbi)) {
            module = static_cast<HMODULE>(mbi.AllocationBase);
        }
    }

    if (module != nullptr) {
        modulePathLength = GetModuleFileNameA(module, modulePath, MAX_PATH);
    }

    const DWORD operation =
        exceptionPointers != nullptr &&
        exceptionPointers->ExceptionRecord != nullptr &&
        exceptionPointers->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        exceptionPointers->ExceptionRecord->NumberParameters >= 1
            ? static_cast<DWORD>(exceptionPointers->ExceptionRecord->ExceptionInformation[0])
            : 0xFFFFFFFFu;

    const std::uintptr_t moduleBase =
        reinterpret_cast<std::uintptr_t>(module);

    const std::uintptr_t ripRva =
        moduleBase != 0 && rip >= moduleBase
            ? rip - moduleBase
            : 0;

    const int length = std::snprintf(
        buffer,
        sizeof(buffer),
        "[FrontierCrash] exception=0x%08lX accessOp=%lu fault=0x%llX rip=0x%llX ripRva=0x%llX module=%s\n",
        static_cast<unsigned long>(code),
        static_cast<unsigned long>(operation),
        static_cast<unsigned long long>(faultAddress),
        static_cast<unsigned long long>(rip),
        static_cast<unsigned long long>(ripRva),
        modulePathLength != 0 ? modulePath : "<unknown>");

    DWORD written = 0;
    if (length > 0) {
        WriteFile(file, buffer, static_cast<DWORD>(length), &written, nullptr);
    }
    CloseHandle(file);

    // Also mirror the crash line into the regular client.log so the crash
    // address travels with the log already collected by the launcher workflow.
    const HANDLE clientLog = CreateFileA(
        clientLogPath.string().c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (clientLog != INVALID_HANDLE_VALUE) {
        DWORD clientWritten = 0;
        if (length > 0) {
            WriteFile(clientLog, buffer, static_cast<DWORD>(length), &clientWritten, nullptr);
        }
        CloseHandle(clientLog);
    }
}

LONG WINAPI frontier_unhandled_exception_filter(EXCEPTION_POINTERS* exceptionPointers) {
    write_crash_log(exceptionPointers);
    return EXCEPTION_EXECUTE_HANDLER;
}


bool environment_flag_enabled(const char* name) {
    if (name == nullptr || *name == '\0') return false;

    char value[16]{};
    const DWORD length =
        GetEnvironmentVariableA(
            name,
            value,
            static_cast<DWORD>(std::size(value)));

    if (length == 0 || length >= std::size(value)) {
        return false;
    }

    return std::strcmp(value, "1") == 0 ||
           std::strcmp(value, "true") == 0 ||
           std::strcmp(value, "TRUE") == 0 ||
           std::strcmp(value, "yes") == 0 ||
           std::strcmp(value, "YES") == 0;
}

void log_line(const std::string& line);

PVOID g_frontierVectoredHandler = nullptr;
volatile LONG g_frontierFirstChanceLogged = 0;

std::uintptr_t g_frontierDiagnosticCallSite = 0;
std::uintptr_t g_frontierDiagnosticReturnSite = 0;
unsigned char g_frontierDiagnosticCallOriginalByte = 0;
unsigned char g_frontierDiagnosticReturnOriginalByte = 0;
volatile LONG g_frontierDiagnosticCallArmed = 0;
volatile LONG g_frontierDiagnosticReturnArmed = 0;

bool frontier_patch_byte(
    std::uintptr_t address,
    unsigned char value,
    unsigned char* originalByte) {
    if (address == 0) return false;

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(
            reinterpret_cast<const void*>(address),
            &mbi,
            sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT) {
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(
            reinterpret_cast<void*>(address),
            1,
            PAGE_EXECUTE_READWRITE,
            &oldProtect)) {
        return false;
    }

    if (originalByte != nullptr) {
        *originalByte =
            *reinterpret_cast<const unsigned char*>(address);
    }

    *reinterpret_cast<unsigned char*>(address) = value;
    FlushInstructionCache(
        GetCurrentProcess(),
        reinterpret_cast<const void*>(address),
        1);

    DWORD unusedProtect = 0;
    VirtualProtect(
        reinterpret_cast<void*>(address),
        1,
        oldProtect,
        &unusedProtect);

    return true;
}

bool frontier_write_byte(
    std::uintptr_t address,
    unsigned char value) {
    if (address == 0) return false;

    DWORD oldProtect = 0;
    if (!VirtualProtect(
            reinterpret_cast<void*>(address),
            1,
            PAGE_EXECUTE_READWRITE,
            &oldProtect)) {
        return false;
    }

    *reinterpret_cast<unsigned char*>(address) = value;
    FlushInstructionCache(
        GetCurrentProcess(),
        reinterpret_cast<const void*>(address),
        1);

    DWORD unusedProtect = 0;
    VirtualProtect(
        reinterpret_cast<void*>(address),
        1,
        oldProtect,
        &unusedProtect);

    return true;
}

bool install_frontier_callsite_diagnostic() {
    HMODULE module = GetModuleHandleA("RDR.exe");
    if (module == nullptr) return false;

    const std::uintptr_t moduleBase =
        reinterpret_cast<std::uintptr_t>(module);

    constexpr std::uintptr_t kCallSiteRva = 0x1FDD7C;
    constexpr std::uintptr_t kReturnSiteRva = kCallSiteRva + 5;

    const auto callSite = moduleBase + kCallSiteRva;
    const auto returnSite = moduleBase + kReturnSiteRva;

    if (*reinterpret_cast<const unsigned char*>(callSite) != 0xE8 ||
        *reinterpret_cast<const unsigned char*>(returnSite) != 0x84 ||
        *reinterpret_cast<const unsigned char*>(returnSite + 1) != 0xC0) {
        return false;
    }

    unsigned char originalCallByte = 0;
    if (!frontier_patch_byte(
            callSite,
            0xCC,
            &originalCallByte)) {
        return false;
    }

    g_frontierDiagnosticCallSite = callSite;
    g_frontierDiagnosticReturnSite = returnSite;
    g_frontierDiagnosticCallOriginalByte = originalCallByte;
    g_frontierDiagnosticReturnOriginalByte =
        *reinterpret_cast<const unsigned char*>(returnSite);
    InterlockedExchange(&g_frontierDiagnosticReturnArmed, 0);
    InterlockedExchange(&g_frontierDiagnosticCallArmed, 1);

    char message[256]{};
    std::snprintf(
        message,
        sizeof(message),
        "[FrontierDiag] pre-call breakpoint installed rva=0x%llX returnRva=0x%llX",
        static_cast<unsigned long long>(kCallSiteRva),
        static_cast<unsigned long long>(kReturnSiteRva));
    log_line(message);
    return true;
}

void write_first_chance_exception_log(EXCEPTION_POINTERS* exceptionPointers) {
    if (exceptionPointers == nullptr || exceptionPointers->ExceptionRecord == nullptr) return;

    const EXCEPTION_RECORD* record = exceptionPointers->ExceptionRecord;
    const DWORD code = record->ExceptionCode;

    // We specifically need STATUS_BREAKPOINT, but keep AV coverage here as well
    // in case RDR converts the failure into a first-chance access violation.
    if (code != EXCEPTION_BREAKPOINT &&
        code != EXCEPTION_ACCESS_VIOLATION) {
        return;
    }

    // Avoid flooding client.log if the process emits repeated first-chance
    // exceptions while unwinding or handling the same failure.
    if (InterlockedCompareExchange(&g_frontierFirstChanceLogged, 1, 0) != 0) {
        return;
    }

    std::uintptr_t rip = 0;
    std::uintptr_t faultAddress =
        reinterpret_cast<std::uintptr_t>(record->ExceptionAddress);

#if defined(_M_X64)
    if (exceptionPointers->ContextRecord != nullptr) {
        rip = static_cast<std::uintptr_t>(exceptionPointers->ContextRecord->Rip);
    }
#endif

    if (code == EXCEPTION_ACCESS_VIOLATION &&
        record->NumberParameters >= 2) {
        faultAddress =
            static_cast<std::uintptr_t>(record->ExceptionInformation[1]);
    }

#if defined(_M_X64)
    std::uintptr_t regRax = 0;
    std::uintptr_t regRbx = 0;
    std::uintptr_t regRcx = 0;
    std::uintptr_t regRdx = 0;
    std::uintptr_t regRsi = 0;
    std::uintptr_t regRdi = 0;
    std::uintptr_t regR8 = 0;
    std::uintptr_t regR9 = 0;
    std::uintptr_t regR10 = 0;
    std::uintptr_t regR11 = 0;
    std::uintptr_t regR12 = 0;
    std::uintptr_t regR13 = 0;
    std::uintptr_t regR14 = 0;
    std::uintptr_t regR15 = 0;
    std::uintptr_t regRsp = 0;
    std::uintptr_t regRbp = 0;
    std::uintptr_t regEFlags = 0;
    if (exceptionPointers->ContextRecord != nullptr) {
        const CONTEXT* context = exceptionPointers->ContextRecord;
        regRax = static_cast<std::uintptr_t>(context->Rax);
        regRbx = static_cast<std::uintptr_t>(context->Rbx);
        regRcx = static_cast<std::uintptr_t>(context->Rcx);
        regRdx = static_cast<std::uintptr_t>(context->Rdx);
        regRsi = static_cast<std::uintptr_t>(context->Rsi);
        regRdi = static_cast<std::uintptr_t>(context->Rdi);
        regR8 = static_cast<std::uintptr_t>(context->R8);
        regR9 = static_cast<std::uintptr_t>(context->R9);
        regR10 = static_cast<std::uintptr_t>(context->R10);
        regR11 = static_cast<std::uintptr_t>(context->R11);
        regR12 = static_cast<std::uintptr_t>(context->R12);
        regR13 = static_cast<std::uintptr_t>(context->R13);
        regR14 = static_cast<std::uintptr_t>(context->R14);
        regR15 = static_cast<std::uintptr_t>(context->R15);
        regRsp = static_cast<std::uintptr_t>(context->Rsp);
        regRbp = static_cast<std::uintptr_t>(context->Rbp);
        regEFlags = static_cast<std::uintptr_t>(context->EFlags);
    }
#endif

    HMODULE module = nullptr;
    MEMORY_BASIC_INFORMATION mbi{};
    if (rip != 0 &&
        VirtualQuery(
            reinterpret_cast<const void*>(rip),
            &mbi,
            sizeof(mbi)) == sizeof(mbi)) {
        module = static_cast<HMODULE>(mbi.AllocationBase);
    }

    char modulePath[MAX_PATH]{};
    DWORD modulePathLength = 0;
    if (module != nullptr) {
        modulePathLength = GetModuleFileNameA(
            module,
            modulePath,
            MAX_PATH);
    }

    const std::uintptr_t moduleBase =
        reinterpret_cast<std::uintptr_t>(module);
    const std::uintptr_t ripRva =
        moduleBase != 0 && rip >= moduleBase
            ? rip - moduleBase
            : 0;

    const DWORD accessOp =
        code == EXCEPTION_ACCESS_VIOLATION &&
        record->NumberParameters >= 1
            ? static_cast<DWORD>(record->ExceptionInformation[0])
            : 0xFFFFFFFFu;

    unsigned char instructionBefore[16]{};
    unsigned char instructionBytes[32]{};
    SIZE_T beforeRead = 0;
    SIZE_T bytesRead = 0;
    bool instructionReadable = false;

    if (rip != 0) {
        MEMORY_BASIC_INFORMATION instructionMbi{};
        if (VirtualQuery(
                reinterpret_cast<const void*>(rip),
                &instructionMbi,
                sizeof(instructionMbi)) == sizeof(instructionMbi) &&
            instructionMbi.State == MEM_COMMIT &&
            (instructionMbi.Protect & 0xFF) != PAGE_NOACCESS &&
            (instructionMbi.Protect & 0xFF) != PAGE_GUARD) {
            if (rip >= reinterpret_cast<std::uintptr_t>(instructionMbi.BaseAddress) + 16) {
                ReadProcessMemory(
                    GetCurrentProcess(),
                    reinterpret_cast<const void*>(rip - 16),
                    instructionBefore,
                    sizeof(instructionBefore),
                    &beforeRead);
            }

            instructionReadable =
                ReadProcessMemory(
                    GetCurrentProcess(),
                    reinterpret_cast<const void*>(rip),
                    instructionBytes,
                    sizeof(instructionBytes),
                    &bytesRead) &&
                bytesRead > 0;
        }
    }

    char instructionBeforeHex[16 * 3 + 1]{};
    std::size_t instructionBeforeHexLength = 0;
    for (SIZE_T i = 0;
         i < beforeRead && instructionBeforeHexLength + 3 < sizeof(instructionBeforeHex);
         ++i) {
        const int n = std::snprintf(
            instructionBeforeHex + instructionBeforeHexLength,
            sizeof(instructionBeforeHex) - instructionBeforeHexLength,
            "%02X%s",
            static_cast<unsigned int>(instructionBefore[i]),
            i + 1 < beforeRead ? " " : "");
        if (n <= 0) break;
        instructionBeforeHexLength += static_cast<std::size_t>(n);
    }

    char instructionHex[32 * 3 + 1]{};
    std::size_t instructionHexLength = 0;
    for (SIZE_T i = 0;
         i < bytesRead && instructionHexLength + 3 < sizeof(instructionHex);
         ++i) {
        const int n = std::snprintf(
            instructionHex + instructionHexLength,
            sizeof(instructionHex) - instructionHexLength,
            "%02X%s",
            static_cast<unsigned int>(instructionBytes[i]),
            i + 1 < bytesRead ? " " : "");
        if (n <= 0) break;
        instructionHexLength += static_cast<std::size_t>(n);
    }

    const DWORD threadId = GetCurrentThreadId();

    std::uintptr_t callTarget = 0;
    if (rip == reinterpret_cast<std::uintptr_t>(module) + 0x1FDD85 &&
        rip >= 5) {
        const auto callInstruction =
            reinterpret_cast<const unsigned char*>(rip - 9);
        if (callInstruction[0] == 0xE8) {
            std::int32_t relative = 0;
            std::memcpy(&relative, callInstruction + 1, sizeof(relative));
            callTarget = rip - 9 + 5 +
                static_cast<std::int64_t>(relative);
        }
    }

    std::uintptr_t stackArg5 = 0;
    std::uintptr_t stackArg6 = 0;
    SIZE_T stackArgBytesRead = 0;
#if defined(_M_X64)
    if (exceptionPointers->ContextRecord != nullptr &&
        exceptionPointers->ContextRecord->Rsp != 0) {
        const auto stackArguments =
            reinterpret_cast<const std::uintptr_t*>(
                exceptionPointers->ContextRecord->Rsp + 0x28);
        ReadProcessMemory(
            GetCurrentProcess(),
            stackArguments,
            &stackArg5,
            sizeof(stackArg5),
            &stackArgBytesRead);
        if (stackArgBytesRead == sizeof(stackArg5)) {
            ReadProcessMemory(
                GetCurrentProcess(),
                stackArguments + 1,
                &stackArg6,
                sizeof(stackArg6),
                &stackArgBytesRead);
        }
    }
#endif

    std::uintptr_t unwindIps[12]{};
    std::uintptr_t unwindRvas[12]{};
    HMODULE unwindModules[12]{};
    std::size_t unwindCount = 0;

#if defined(_M_X64)
    if (exceptionPointers->ContextRecord != nullptr) {
        CONTEXT unwindContext = *exceptionPointers->ContextRecord;

        for (std::size_t i = 0; i < std::size(unwindIps); ++i) {
            const std::uintptr_t frameRip =
                static_cast<std::uintptr_t>(unwindContext.Rip);
            if (frameRip == 0) break;

            unwindIps[unwindCount] = frameRip;

            MEMORY_BASIC_INFORMATION frameMbi{};
            if (VirtualQuery(
                    reinterpret_cast<const void*>(frameRip),
                    &frameMbi,
                    sizeof(frameMbi)) == sizeof(frameMbi)) {
                HMODULE frameModule =
                    static_cast<HMODULE>(frameMbi.AllocationBase);
                unwindModules[unwindCount] = frameModule;

                const std::uintptr_t frameModuleBase =
                    reinterpret_cast<std::uintptr_t>(frameModule);
                if (frameModuleBase != 0 && frameRip >= frameModuleBase) {
                    unwindRvas[unwindCount] = frameRip - frameModuleBase;
                }
            }

            ++unwindCount;

            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION runtimeFunction =
                RtlLookupFunctionEntry(
                    unwindContext.Rip,
                    &imageBase,
                    nullptr);

            if (runtimeFunction == nullptr) {
                // Leaf function or missing unwind metadata: recover the caller
                // from the saved return address at RSP.
                const auto returnAddress =
                    reinterpret_cast<const std::uintptr_t*>(
                        unwindContext.Rsp);
                std::uintptr_t nextRip = 0;
                SIZE_T copied = 0;

                if (returnAddress != nullptr &&
                    ReadProcessMemory(
                        GetCurrentProcess(),
                        returnAddress,
                        &nextRip,
                        sizeof(nextRip),
                        &copied) &&
                    copied == sizeof(nextRip)) {
                    unwindContext.Rip = nextRip;
                    unwindContext.Rsp += sizeof(std::uintptr_t);
                    if (unwindContext.Rip == 0) break;
                    continue;
                }

                break;
            }

            PVOID handlerData = nullptr;
            DWORD64 establisherFrame = 0;
            CONTEXT previousContext = unwindContext;

            RtlVirtualUnwind(
                UNW_FLAG_NHANDLER,
                imageBase,
                unwindContext.Rip,
                runtimeFunction,
                &unwindContext,
                &handlerData,
                &establisherFrame,
                nullptr);

            if (unwindContext.Rip == 0 ||
                unwindContext.Rip == previousContext.Rip ||
                unwindContext.Rsp <= previousContext.Rsp) {
                break;
            }
        }
    }
#endif

    char unwindText[12 * 96 + 1]{};
    std::size_t unwindTextLength = 0;
    for (std::size_t i = 0; i < unwindCount; ++i) {
        char modulePath[MAX_PATH]{};
        DWORD modulePathLength = 0;
        if (unwindModules[i] != nullptr) {
            modulePathLength = GetModuleFileNameA(
                unwindModules[i],
                modulePath,
                MAX_PATH);
        }

        const int n = std::snprintf(
            unwindText + unwindTextLength,
            sizeof(unwindText) - unwindTextLength,
            "%s%zu:rip=0x%llX:rva=0x%llX:module=%s",
            i == 0 ? "" : " | ",
            i,
            static_cast<unsigned long long>(unwindIps[i]),
            static_cast<unsigned long long>(unwindRvas[i]),
            modulePathLength != 0 ? modulePath : "<unknown>");

        if (n <= 0) break;
        unwindTextLength += static_cast<std::size_t>(n);
        if (unwindTextLength >= sizeof(unwindText)) {
            unwindText[sizeof(unwindText) - 1] = '\0';
            break;
        }
    }

    char localAppData[MAX_PATH]{};
    const DWORD envLength =
        GetEnvironmentVariableA(
            "LOCALAPPDATA",
            localAppData,
            MAX_PATH);
    if (envLength == 0 || envLength >= MAX_PATH) return;

    char clientLogPath[MAX_PATH * 2]{};
    const int pathLength = std::snprintf(
        clientLogPath,
        sizeof(clientLogPath),
        "%s\\FrontierMP\\logs\\client.log",
        localAppData);
    if (pathLength <= 0 ||
        static_cast<std::size_t>(pathLength) >= sizeof(clientLogPath)) {
        return;
    }

    const HANDLE file = CreateFileA(
        clientLogPath,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return;

    char buffer[2048]{};
    const int length = std::snprintf(
        buffer,
        sizeof(buffer),
        "[FrontierFirstChance] exception=0x%08lX accessOp=%lu fault=0x%llX rip=0x%llX ripRva=0x%llX thread=%lu module=%s before=%s bytes=%s callTarget=0x%llX callTargetRva=0x%llX stackArg5=0x%llX stackArg6=0x%llX regs=RAX:%llX RBX:%llX RCX:%llX RDX:%llX RSI:%llX RDI:%llX R8:%llX R9:%llX R10:%llX R11:%llX R12:%llX R13:%llX R14:%llX R15:%llX RSP:%llX RBP:%llX EFLAGS:%llX unwind=%s\\n",
        static_cast<unsigned long>(code),
        static_cast<unsigned long>(accessOp),
        static_cast<unsigned long long>(faultAddress),
        static_cast<unsigned long long>(rip),
        static_cast<unsigned long long>(ripRva),
        static_cast<unsigned long>(threadId),
        modulePathLength != 0 ? modulePath : "<unknown>",
        beforeRead != 0 ? instructionBeforeHex : "<unreadable>",
        instructionReadable ? instructionHex : "<unreadable>",
        static_cast<unsigned long long>(callTarget),
        static_cast<unsigned long long>(
            callTarget != 0 &&
            moduleBase != 0 &&
            callTarget >= moduleBase
                ? callTarget - moduleBase
                : 0),
        static_cast<unsigned long long>(stackArg5),
        static_cast<unsigned long long>(stackArg6),
        static_cast<unsigned long long>(regRax),
        static_cast<unsigned long long>(regRbx),
        static_cast<unsigned long long>(regRcx),
        static_cast<unsigned long long>(regRdx),
        static_cast<unsigned long long>(regRsi),
        static_cast<unsigned long long>(regRdi),
        static_cast<unsigned long long>(regR8),
        static_cast<unsigned long long>(regR9),
        static_cast<unsigned long long>(regR10),
        static_cast<unsigned long long>(regR11),
        static_cast<unsigned long long>(regR12),
        static_cast<unsigned long long>(regR13),
        static_cast<unsigned long long>(regR14),
        static_cast<unsigned long long>(regR15),
        static_cast<unsigned long long>(regRsp),
        static_cast<unsigned long long>(regRbp),
        static_cast<unsigned long long>(regEFlags),
        unwindCount != 0 ? unwindText : "<unavailable>");

    DWORD written = 0;
    if (length > 0) {
        WriteFile(
            file,
            buffer,
            static_cast<DWORD>(length),
            &written,
            nullptr);
    }

    CloseHandle(file);
}

LONG WINAPI frontier_vectored_exception_handler(EXCEPTION_POINTERS* exceptionPointers) {
    if (exceptionPointers != nullptr &&
        exceptionPointers->ExceptionRecord != nullptr &&
        exceptionPointers->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) {
        const std::uintptr_t exceptionAddress =
            reinterpret_cast<std::uintptr_t>(
                exceptionPointers->ExceptionRecord->ExceptionAddress);

#if defined(_M_X64)
        if (exceptionAddress == g_frontierDiagnosticCallSite &&
            InterlockedCompareExchange(
                &g_frontierDiagnosticCallArmed,
                0,
                1) == 1) {
            const CONTEXT* context = exceptionPointers->ContextRecord;
            if (context != nullptr) {
                std::uintptr_t stackArg5 = 0;
                std::uintptr_t stackArg6 = 0;
                SIZE_T copied = 0;

                ReadProcessMemory(
                    GetCurrentProcess(),
                    reinterpret_cast<const void*>(context->Rsp + 0x20),
                    &stackArg5,
                    sizeof(stackArg5),
                    &copied);

                copied = 0;
                ReadProcessMemory(
                    GetCurrentProcess(),
                    reinterpret_cast<const void*>(context->Rsp + 0x28),
                    &stackArg6,
                    sizeof(stackArg6),
                    &copied);

                char message[1536]{};
                std::snprintf(
                    message,
                    sizeof(message),
                    "[FrontierDiag] pre-call target=0x%llX targetRva=0x%llX "
                    "RCX=%llX RDX=%llX R8=%llX R9=%llX "
                    "RAX=%llX RSI=%llX RDI=%llX RSP=%llX "
                    "stack+20=%llX stack+28=%llX",
                    static_cast<unsigned long long>(
                        g_frontierDiagnosticCallSite + 5 +
                        *reinterpret_cast<const std::int32_t*>(
                            g_frontierDiagnosticCallSite + 1)),
                    static_cast<unsigned long long>(
                        (g_frontierDiagnosticCallSite + 5 +
                         *reinterpret_cast<const std::int32_t*>(
                             g_frontierDiagnosticCallSite + 1)) -
                        reinterpret_cast<std::uintptr_t>(
                            GetModuleHandleA("RDR.exe"))),
                    static_cast<unsigned long long>(context->Rcx),
                    static_cast<unsigned long long>(context->Rdx),
                    static_cast<unsigned long long>(context->R8),
                    static_cast<unsigned long long>(context->R9),
                    static_cast<unsigned long long>(context->Rax),
                    static_cast<unsigned long long>(context->Rsi),
                    static_cast<unsigned long long>(context->Rdi),
                    static_cast<unsigned long long>(context->Rsp),
                    static_cast<unsigned long long>(stackArg5),
                    static_cast<unsigned long long>(stackArg6));
                log_line(message);
            }

            // Restore the original CALL and put an INT3 on the instruction
            // immediately after it. The CALL then runs normally and breaks
            // once on return, before TEST AL,AL consumes the return value.
            frontier_write_byte(
                g_frontierDiagnosticCallSite,
                g_frontierDiagnosticCallOriginalByte);
            frontier_write_byte(
                g_frontierDiagnosticReturnSite,
                0xCC);
            InterlockedExchange(&g_frontierDiagnosticReturnArmed, 1);

            exceptionPointers->ContextRecord->Rip =
                g_frontierDiagnosticCallSite;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        if (exceptionAddress == g_frontierDiagnosticReturnSite &&
            InterlockedCompareExchange(
                &g_frontierDiagnosticReturnArmed,
                0,
                1) == 1) {
            const CONTEXT* context = exceptionPointers->ContextRecord;
            if (context != nullptr) {
                char message[768]{};
                std::snprintf(
                    message,
                    sizeof(message),
                    "[FrontierDiag] post-call return RAX=%llX AL=%02X "
                    "RIP=0x%llX",
                    static_cast<unsigned long long>(context->Rax),
                    static_cast<unsigned int>(
                        context->Rax & 0xFFu),
                    static_cast<unsigned long long>(context->Rip));
                log_line(message);
            }

            frontier_write_byte(
                g_frontierDiagnosticReturnSite,
                g_frontierDiagnosticReturnOriginalByte);

            exceptionPointers->ContextRecord->Rip =
                g_frontierDiagnosticReturnSite;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
#endif
    }

    write_first_chance_exception_log(exceptionPointers);
    return EXCEPTION_CONTINUE_SEARCH;
}

void log_line(const std::string& line) {
#ifdef _WIN32
    char localAppData[MAX_PATH]{};
    const DWORD n =
        GetEnvironmentVariableA(
            "LOCALAPPDATA",
            localAppData,
            MAX_PATH);

    if (n != 0 && n < MAX_PATH) {
        std::filesystem::path dir =
            std::filesystem::path(localAppData) /
            "FrontierMP" /
            "logs";

        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ofstream file(dir / "client.log", std::ios::app);
        if (file) file << line << "\n";
    }
#else
    (void)line;
#endif
}

void signal_bootstrap_ready() {
    wchar_t name[512]{};
    const DWORD length =
        GetEnvironmentVariableW(L"FRONTIER_BOOTSTRAP_EVENT", name, static_cast<DWORD>(std::size(name)));

    if (length == 0 || length >= std::size(name)) {
        log_line("[FrontierClient] bootstrap event name unavailable");
        return;
    }

    std::string eventName;
    eventName.reserve(length);
    for (DWORD i = 0; i < length; ++i) {
        const wchar_t ch = name[i];
        eventName.push_back(
            ch >= 32 && ch <= 126 ? static_cast<char>(ch) : '?');
    }
    log_line("[FrontierClient] bootstrap event name=" + eventName);

    HANDLE event =
        OpenEventW(
            EVENT_MODIFY_STATE,
            FALSE,
            name);

    if (!event) {
        log_line(
            "[FrontierClient] bootstrap event open failed error=" +
            std::to_string(GetLastError()));
        return;
    }

    if (!SetEvent(event)) {
        log_line(
            "[FrontierClient] bootstrap event signal failed error=" +
            std::to_string(GetLastError()));
        CloseHandle(event);
        return;
    }

    log_line("[FrontierClient] bootstrap event signaled");
    CloseHandle(event);
}

} // namespace

DWORD WINAPI FrontierClientWorker(LPVOID) {
    log_line("[FrontierClient] worker starting");
    const bool initialized =
        g_runtime.initialize_from_process_command_line();
    log_line(
        std::string("[FrontierClient] worker initialization result=") +
        (initialized ? "success" : "failure"));

    if (initialized &&
        environment_flag_enabled("FRONTIER_ENABLE_RDR_CALLSITE_DIAGNOSTIC")) {
        if (install_frontier_callsite_diagnostic()) {
            log_line("[FrontierDiag] RDR callsite diagnostic ready");
        } else {
            log_line("[FrontierDiag] RDR callsite diagnostic requested but unavailable");
        }
    } else {
        log_line("[FrontierDiag] RDR callsite diagnostic disabled");
    }

    if (!initialized) {
        OutputDebugStringA("[FrontierClient] initialization failed\n");
        return 0;
    }

#ifdef FRONTIER_ENABLE_CEF
    log_line("[FrontierCEF] compile support enabled");
    if (g_cefOverlay.start()) {
        log_line("[FrontierCEF] overlay startup requested");
    } else {
        log_line("[FrontierCEF] overlay startup failed");
    }
#endif

    signal_bootstrap_ready();
    g_runtime.run_loop();

#ifdef FRONTIER_ENABLE_CEF
    g_cefOverlay.stop();
#else
    log_line("[FrontierCEF] compile support disabled");
#endif

    return 0;
}

extern "C" __declspec(dllexport)
bool FrontierClient_Initialize(const char* host, unsigned short port, const char* playerName) {
    if (host == nullptr || playerName == nullptr) return false;
    return g_runtime.initialize(host, port, playerName);
}

extern "C" __declspec(dllexport)
void FrontierClient_Update() { g_runtime.update(); }

extern "C" __declspec(dllexport)
void FrontierClient_Shutdown() { g_runtime.shutdown(); }

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    (void)module;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        g_frontierVectoredHandler = AddVectoredExceptionHandler(
            1,
            frontier_vectored_exception_handler);
        SetUnhandledExceptionFilter(frontier_unhandled_exception_filter);
        HANDLE worker = CreateThread(nullptr, 0, FrontierClientWorker, nullptr, 0, nullptr);
        if (worker) CloseHandle(worker);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_frontierVectoredHandler != nullptr) {
            RemoveVectoredExceptionHandler(g_frontierVectoredHandler);
            g_frontierVectoredHandler = nullptr;
        }
    }
    return TRUE;
}
