#include "frontier/client/client_runtime.hpp"

#include <windows.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <cstdio>

namespace {

frontier::client::ClientRuntime g_runtime;

void write_crash_log(EXCEPTION_POINTERS* exceptionPointers) {
    char localAppData[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;

    const std::filesystem::path dir =
        std::filesystem::path(localAppData) / "FrontierMP" / "logs";

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    const std::filesystem::path path = dir / "client_crash.log";
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
}

LONG WINAPI frontier_unhandled_exception_filter(EXCEPTION_POINTERS* exceptionPointers) {
    write_crash_log(exceptionPointers);
    return EXCEPTION_EXECUTE_HANDLER;
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
    signal_bootstrap_ready();

    if (!initialized) {
        OutputDebugStringA("[FrontierClient] initialization failed\n");
        return 0;
    }

    g_runtime.run_loop();
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
        SetUnhandledExceptionFilter(frontier_unhandled_exception_filter);
        HANDLE worker = CreateThread(nullptr, 0, FrontierClientWorker, nullptr, 0, nullptr);
        if (worker) CloseHandle(worker);
    }
    return TRUE;
}
