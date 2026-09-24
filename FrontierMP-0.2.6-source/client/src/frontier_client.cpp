#include "frontier/client/client_runtime.hpp"

#include <windows.h>

#include <array>
#include <string>

namespace {

frontier::client::ClientRuntime g_runtime;

void signal_bootstrap_ready() {
    wchar_t name[512]{};
    const DWORD length =
        GetEnvironmentVariableW(L"FRONTIER_BOOTSTRAP_EVENT", name, static_cast<DWORD>(std::size(name)));

    if (length == 0 || length >= std::size(name)) {
        OutputDebugStringA("[FrontierClient] bootstrap readiness event name unavailable\n");
        return;
    }

    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, name);
    if (!event) {
        OutputDebugStringA("[FrontierClient] bootstrap readiness event creation failed\n");
        return;
    }

    SetEvent(event);
    CloseHandle(event);
}

} // namespace

DWORD WINAPI FrontierClientWorker(LPVOID) {
    const bool initialized = g_runtime.initialize_from_process_command_line();
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
