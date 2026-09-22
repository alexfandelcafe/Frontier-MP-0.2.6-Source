#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "frontier/launcher/launcher.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

bool has_prefix(const std::wstring& arg, const wchar_t* prefix) {
    return arg.rfind(prefix, 0) == 0;
}

std::wstring value_after_equals(const std::wstring& arg) {
    const auto pos = arg.find(L'=');
    return pos == std::wstring::npos ? std::wstring{} : arg.substr(pos + 1);
}

std::string wide_to_utf8(const std::wstring& input) {
    if (input.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
                                          static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return std::string(input.begin(), input.end());
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
                        result.data(), count, nullptr, nullptr);
    return result;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    frontier::launcher::LaunchOptions options{};

    wchar_t exePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return 1;

    const std::filesystem::path launcherPath(std::wstring(exePath, length));
    const auto launcherDir = launcherPath.parent_path();
    options.clientDll = launcherDir / L"FrontierClient.dll";

    // Backward-compatible fallback for older build trees where the Windows
    // shared-library target was emitted beside the executable at the
    // configuration directory root instead of under bin\<Config>.
    const auto legacyDll = launcherDir.parent_path().parent_path() / L"FrontierClient.dll";
    if (!std::filesystem::exists(options.clientDll) && std::filesystem::exists(legacyDll)) {
        options.clientDll = legacyDll;
    }

    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (has_prefix(arg, L"--game=")) options.gameExe = value_after_equals(arg);
        else if (has_prefix(arg, L"--server=")) options.serverHost = wide_to_utf8(value_after_equals(arg));
        else if (has_prefix(arg, L"--port=")) {
            try { options.serverPort = static_cast<unsigned short>(std::stoul(value_after_equals(arg))); }
            catch (...) { options.serverPort = 30120; }
        }
        else if (has_prefix(arg, L"--name=")) options.playerName = wide_to_utf8(value_after_equals(arg));
        else if (arg == L"--help") {
            std::wcout << L"FrontierMP launcher\n\n"
                       << L"Usage:\n"
                       << L"  FrontierMP.exe --game=\"C:\\Path\\RDR.exe\" [--server=127.0.0.1] [--port=30120] [--name=Player]\n";
            return 0;
        }
        else options.gameArguments.push_back(arg);
    }

    if (options.gameExe.empty()) {
        std::wcout << L"FrontierMP launcher\n\nEnter the full path to RDR.exe:\n> ";
        std::getline(std::wcin, options.gameExe);
    }
    if (options.gameExe.empty()) return 2;

    if (options.playerName == "Player") {
        std::wcout << L"Player name [Player]: ";
        std::wstring entered;
        if (std::getline(std::wcin, entered) && !entered.empty()) options.playerName = wide_to_utf8(entered);
    }

    return frontier::launcher::Launcher{}.run(options);
}
