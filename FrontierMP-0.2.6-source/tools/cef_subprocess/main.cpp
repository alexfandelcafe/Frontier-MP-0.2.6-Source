#include <windows.h>

#include "include/cef_app.h"

namespace {

class FrontierCefApp final : public CefApp {
public:
    IMPLEMENT_REFCOUNTING(FrontierCefApp);
};

} // namespace

int WINAPI WinMain(
    HINSTANCE instance,
    HINSTANCE,
    LPSTR,
    int) {
    CefMainArgs mainArgs(instance);
    CefRefPtr<FrontierCefApp> app = new FrontierCefApp();
    return CefExecuteProcess(mainArgs, app, nullptr);
}
