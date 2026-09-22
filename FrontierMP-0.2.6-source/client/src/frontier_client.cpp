#include "frontier/client/client_runtime.hpp"

#include <windows.h>

namespace { frontier::client::ClientRuntime g_runtime; }

DWORD WINAPI FrontierClientWorker(LPVOID) {
    Sleep(1500);
    if (!g_runtime.initialize_from_process_command_line()) {
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
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE worker = CreateThread(nullptr, 0, FrontierClientWorker, nullptr, 0, nullptr);
        if (worker) CloseHandle(worker);
    }
    return TRUE;
}
