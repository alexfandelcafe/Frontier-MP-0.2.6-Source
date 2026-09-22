#pragma once

#include <cstdint>
#include <string>

namespace frontier::game {

class NativeInvoker;

class StartupNativeTracer final {
public:
    bool attach(NativeInvoker& invoker, std::string& error);
    bool attached() const { return attached_; }

private:
    static void set_start_pos_hook(void* context);
    static void script_done_loading_hook(void* context);
    static void clear_mission_info_hook(void* context);

    static void log(const char* message);

    NativeInvoker* invoker_{};
    NativeInvoker::NativeHandler originalSetStartPos_{};
    NativeInvoker::NativeHandler originalScriptDoneLoading_{};
    NativeInvoker::NativeHandler originalClearMissionInfo_{};
    bool attached_{};
};

} // namespace frontier::game
