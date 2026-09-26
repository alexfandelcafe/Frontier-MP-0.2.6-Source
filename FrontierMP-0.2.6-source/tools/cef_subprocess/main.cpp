#include <windows.h>

#include "include/cef_app.h"
#include "include/cef_process_message.h"
#include "include/cef_v8.h"

#include <string>
#include <utility>

namespace {

class FrontierAppV8Handler final : public CefV8Handler {
public:
    explicit FrontierAppV8Handler(std::string command)
        : command_(std::move(command)) {}

    bool Execute(
        const CefString&,
        CefRefPtr<CefV8Value>,
        const CefV8ValueList& arguments,
        CefRefPtr<CefV8Value>& retval,
        CefString& exception) override {
        retval = CefV8Value::CreateBool(false);

        if (arguments.size() > 8) {
            exception = "too many app command arguments";
            return true;
        }

        auto message = CefProcessMessage::Create("frontier.app_command");
        auto values = message->GetArgumentList();
        values->SetString(0, command_);

        for (std::size_t i = 0; i < arguments.size(); ++i) {
            const auto& value = arguments[i];
            if (value == nullptr) continue;

            const std::size_t index = i + 1;
            if (value->IsString()) {
                values->SetString(index, value->GetStringValue());
            } else if (value->IsInt()) {
                values->SetInt(index, value->GetIntValue());
            } else if (value->IsBool()) {
                values->SetBool(index, value->GetBoolValue());
            } else {
                exception = "unsupported app command argument type";
                return true;
            }
        }

        auto context = CefV8Context::GetCurrentContext();
        if (context != nullptr) {
            auto frame = context->GetFrame();
            if (frame != nullptr &&
                frame->SendProcessMessage(PID_BROWSER, message)) {
                retval = CefV8Value::CreateBool(true);
            }
        }

        return true;
    }

private:
    std::string command_;

    IMPLEMENT_REFCOUNTING(FrontierAppV8Handler);
};

class FrontierCefApp final
    : public CefApp
    , public CefRenderProcessHandler {
public:
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
        return this;
    }

    void OnContextCreated(
        CefRefPtr<CefBrowser> browser,
        CefRefPtr<CefFrame> frame,
        CefRefPtr<CefV8Context> context) override {
        if (browser == nullptr || frame == nullptr || context == nullptr) return;

        auto app = CefV8Value::CreateObject(nullptr, nullptr);
        app->SetValue(
            "connect",
            CefV8Value::CreateFunction(
                "connect",
                new FrontierAppV8Handler("connect")),
            V8_PROPERTY_ATTRIBUTE_NONE);
        app->SetValue(
            "quit",
            CefV8Value::CreateFunction(
                "quit",
                new FrontierAppV8Handler("quit")),
            V8_PROPERTY_ATTRIBUTE_NONE);

        context->GetGlobal()->SetValue(
            "app",
            app,
            V8_PROPERTY_ATTRIBUTE_NONE);
    }

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
