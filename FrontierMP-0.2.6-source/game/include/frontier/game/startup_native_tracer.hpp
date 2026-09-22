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
    static void launch_new_script_with_args_hook(void* context);
    static void set_mission_info_hook(void* context);
    static void was_last_reset_for_multiplayer_hook(void* context);
    static void is_launch_retail_hook(void* context);
    static void is_simulate_start_press_hook(void* context);
    static void is_script_valid_hook(void* context);

    static void log(const char* message);

    NativeInvoker* invoker_{};
    NativeInvoker::NativeHandler originalSetStartPos_{};
    NativeInvoker::NativeHandler originalScriptDoneLoading_{};
    NativeInvoker::NativeHandler originalClearMissionInfo_{};
    NativeInvoker::NativeHandler originalLaunchNewScript_{};
    NativeInvoker::NativeHandler originalLaunchNewScriptWithArgs_{};
    NativeInvoker::NativeHandler originalSetMissionInfo_{};
    NativeInvoker::NativeHandler originalWasLastResetForMultiplayer_{};
    NativeInvoker::NativeHandler originalIsLaunchRetail_{};
    NativeInvoker::NativeHandler originalIsSimulateStartPress_{};
    NativeInvoker::NativeHandler originalIsScriptValid_{};
    bool attached_{};
};

} // namespace frontier::game
