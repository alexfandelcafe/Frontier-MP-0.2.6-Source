#pragma once

#include "frontier/game/native_invoker.hpp"

#include <cstdint>
#include <string>

namespace frontier::game {

class StartupNativeTracer final {
public:
    bool attach(NativeInvoker& invoker, std::string& error);
    bool attached() const { return attached_; }

private:
    static void set_start_pos_hook(void* context);
    static void script_done_loading_hook(void* context);
    static void clear_mission_info_hook(void* context);
    static void launch_new_script_hook(void* context);

    static void log(const char* message);

    NativeInvoker* invoker_{};
    NativeInvoker::NativeHandler originalSetStartPos_{};
    NativeInvoker::NativeHandler originalScriptDoneLoading_{};
    NativeInvoker::NativeHandler originalClearMissionInfo_{};
    NativeInvoker::NativeHandler originalLaunchNewScript_{};
    bool attached_{};
};

} // namespace frontier::game
