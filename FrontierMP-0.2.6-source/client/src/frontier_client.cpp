#include "frontier/client/client_runtime.hpp"

#include <windows.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

frontier::client::ClientRuntime g_runtime;

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
        HANDLE worker = CreateThread(nullptr, 0, FrontierClientWorker, nullptr, 0, nullptr);
        if (worker) CloseHandle(worker);
    }
    return TRUE;
}
