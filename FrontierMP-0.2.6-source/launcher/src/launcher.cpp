#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "frontier/launcher/launcher.hpp"
#include "frontier/game/build_fingerprint.hpp"

#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace frontier::launcher {

namespace {

std::wstring narrow_to_wide(const std::string& input) {
    if (input.empty()) return {};

    const int count = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        input.data(),
        static_cast<int>(input.size()),
        nullptr,
        0);

    if (count <= 0) {
        return std::wstring(input.begin(), input.end());
    }

    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        input.data(),
        static_cast<int>(input.size()),
        result.data(),
        count);

    return result;
}

std::wstring make_command_line(const LaunchOptions& options) {
    std::wostringstream cmd;
    cmd << Launcher::quote(options.gameExe);

    for (const auto& arg : options.gameArguments) {
        cmd << L' ' << Launcher::quote(arg);
    }

    return cmd.str();
}

std::wstring make_bootstrap_event_name(
    unsigned long launcherPid,
    unsigned long long nonce) {
    std::wostringstream name;
    name << L"Local\\FrontierMP_Bootstrap_"
         << launcherPid
         << L"_"
         << nonce;
    return name.str();
}

} // namespace

std::wstring Launcher::quote(const std::wstring& value) {
    std::wstring out = L"\"";

    for (wchar_t ch : value) {
        if (ch == L'"') {
            out += L'\\';
        }
        out += ch;
    }

    out += L"\"";
    return out;
}

bool Launcher::verify_game(const std::wstring& gameExe) const {
    frontier::game::ExecutableFingerprint fp{};

    if (!frontier::game::BuildDetector::inspect_file(gameExe, fp)) {
        std::wcerr << L"Unable to inspect RDR.exe: " << gameExe << L"\n";
        return false;
    }

    const auto build = frontier::game::BuildDetector::match(fp);
    const auto desc = frontier::game::BuildDetector::descriptor(build);

    std::cout << "RDR fingerprint: " << desc.label << "\n"
              << "  file_version=" << fp.fileVersion << "\n"
              << "  pe_timestamp=0x" << std::hex << fp.peTimestamp << std::dec << "\n"
              << "  image_size=0x" << std::hex << fp.imageSize << std::dec << "\n"
              << "  text_hash=0x" << std::hex << fp.textHash << std::dec << "\n";

    if (build == frontier::game::KnownBuild::Unknown) {
        std::wcerr << L"Unsupported/unknown RDR.exe fingerprint.\n";
        return false;
    }

    return true;
}

bool Launcher::inject_client(
    void* processHandleRaw,
    const std::wstring& clientDll,
    unsigned long& loadResult) const {
    HANDLE process = static_cast<HANDLE>(processHandleRaw);
    const std::size_t bytes =
        (clientDll.size() + 1) * sizeof(wchar_t);

    void* remoteString = VirtualAllocEx(
        process,
        nullptr,
        bytes,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);

    if (!remoteString) {
        std::cerr << "VirtualAllocEx failed: " << GetLastError() << "\n";
        return false;
    }

    SIZE_T written = 0;
    if (!WriteProcessMemory(
            process,
            remoteString,
            clientDll.c_str(),
            bytes,
            &written) ||
        written != bytes) {
        std::cerr << "WriteProcessMemory failed: " << GetLastError() << "\n";
        VirtualFreeEx(process, remoteString, 0, MEM_RELEASE);
        return false;
    }

    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32) {
        VirtualFreeEx(process, remoteString, 0, MEM_RELEASE);
        return false;
    }

    const auto loadLibraryW =
        reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(kernel32, "LoadLibraryW"));

    if (!loadLibraryW) {
        VirtualFreeEx(process, remoteString, 0, MEM_RELEASE);
        return false;
    }

    HANDLE thread = CreateRemoteThread(
        process,
        nullptr,
        0,
        loadLibraryW,
        remoteString,
        0,
        nullptr);

    if (!thread) {
        std::cerr << "CreateRemoteThread failed: " << GetLastError() << "\n";
        VirtualFreeEx(process, remoteString, 0, MEM_RELEASE);
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(thread, 15000);
    if (waitResult != WAIT_OBJECT_0) {
        std::cerr << "Waiting for FrontierClient.dll load failed.\n";
        CloseHandle(thread);
        VirtualFreeEx(process, remoteString, 0, MEM_RELEASE);
        return false;
    }

    DWORD exitCode = 0;
    const bool ok =
        GetExitCodeThread(thread, &exitCode) &&
        exitCode != 0;

    if (!ok) {
        std::cerr << "LoadLibraryW failed inside RDR.exe.\n";
    }

    loadResult = exitCode;

    CloseHandle(thread);
    VirtualFreeEx(process, remoteString, 0, MEM_RELEASE);
    return ok;
}

bool Launcher::wait_for_bootstrap_ready(
    const std::wstring& bootstrapEventName,
    unsigned long timeoutMs) const {
    HANDLE event =
        OpenEventW(
            SYNCHRONIZE,
            FALSE,
            bootstrapEventName.c_str());

    if (!event) {
        std::wcerr
            << L"OpenEventW failed for bootstrap readiness event: "
            << GetLastError()
            << L"\n";
        return false;
    }

    const DWORD result =
        WaitForSingleObject(event, timeoutMs);
    CloseHandle(event);

    if (result == WAIT_OBJECT_0) {
        return true;
    }

    if (result == WAIT_TIMEOUT) {
        std::wcerr
            << L"FrontierClient bootstrap readiness event timed out.\n";
    } else {
        std::wcerr
            << L"FrontierClient bootstrap readiness wait failed: "
            << GetLastError()
            << L"\n";
    }

    return false;
}

std::vector<wchar_t> Launcher::build_environment(
    const LaunchOptions& options,
    const std::wstring& bootstrapEventName) {
    std::vector<std::wstring> entries;

    LPWCH raw = GetEnvironmentStringsW();
    if (raw) {
        const wchar_t* cursor = raw;

        while (*cursor != L'\0') {
            const std::wstring entry(cursor);
            const std::size_t eq = entry.find(L'=');
            const std::wstring key =
                eq == std::wstring::npos
                    ? entry
                    : entry.substr(0, eq);

            if (_wcsicmp(key.c_str(), L"FRONTIER_SERVER") != 0 &&
                _wcsicmp(key.c_str(), L"FRONTIER_PORT") != 0 &&
                _wcsicmp(key.c_str(), L"FRONTIER_NAME") != 0 &&
                _wcsicmp(key.c_str(), L"FRONTIER_SESSION_MODE") != 0 &&
                _wcsicmp(key.c_str(), L"FRONTIER_BOOTSTRAP_EVENT") != 0) {
                entries.push_back(entry);
            }

            cursor += entry.size() + 1;
        }

        FreeEnvironmentStringsW(raw);
    }

    entries.push_back(
        L"FRONTIER_SERVER=" +
        narrow_to_wide(options.serverHost));

    entries.push_back(
        L"FRONTIER_PORT=" +
        std::to_wstring(options.serverPort));

    entries.push_back(
        L"FRONTIER_NAME=" +
        narrow_to_wide(options.playerName));

    entries.push_back(L"FRONTIER_SESSION_MODE=freeroam");

    entries.push_back(
        L"FRONTIER_BOOTSTRAP_EVENT=" +
        bootstrapEventName);

    std::vector<wchar_t> block;

    for (const auto& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }

    block.push_back(L'\0');
    return block;
}

int Launcher::run(const LaunchOptions& options) const {
    std::error_code ec;

    const auto gamePath =
        std::filesystem::weakly_canonical(options.gameExe, ec);

    if (ec || !std::filesystem::exists(gamePath)) {
        std::wcerr << L"RDR.exe not found.\n";
        return 2;
    }

    ec.clear();

    const auto dllPath =
        std::filesystem::weakly_canonical(options.clientDll, ec);

    if (ec || !std::filesystem::exists(dllPath)) {
        std::wcerr
            << L"FrontierClient.dll not found: "
            << options.clientDll
            << L"\n";
        return 2;
    }

    if (!verify_game(gamePath.wstring())) {
        return 3;
    }

    std::wstring commandLine = make_command_line(
        LaunchOptions{
            gamePath.wstring(),
            dllPath.wstring(),
            options.serverHost,
            options.serverPort,
            options.playerName,
            options.gameArguments
        });

    std::vector<wchar_t> mutableCommand(
        commandLine.begin(),
        commandLine.end());

    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);

    PROCESS_INFORMATION processInfo{};

    const std::wstring bootstrapEventName =
        make_bootstrap_event_name(
            GetCurrentProcessId(),
            static_cast<unsigned long long>(
                GetTickCount64()));

    HANDLE bootstrapEvent =
        CreateEventW(
            nullptr,
            TRUE,
            FALSE,
            bootstrapEventName.c_str());

    if (!bootstrapEvent) {
        std::wcerr
            << L"CreateEventW failed for bootstrap readiness event: "
            << GetLastError()
            << L"\n";
        return 4;
    }

    auto environment =
        build_environment(options, bootstrapEventName);

    if (!CreateProcessW(
            gamePath.c_str(),
            mutableCommand.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
            environment.data(),
            gamePath.parent_path().c_str(),
            &startup,
            &processInfo)) {
        std::wcerr
            << L"CreateProcessW failed: "
            << GetLastError()
            << L"\n";
        CloseHandle(bootstrapEvent);
        return 4;
    }

    unsigned long loadResult = 0;

    if (!inject_client(
            processInfo.hProcess,
            dllPath.wstring(),
            loadResult)) {
        TerminateProcess(processInfo.hProcess, 5);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        CloseHandle(bootstrapEvent);
        return 5;
    }

    std::cout
        << "FrontierClient.dll loaded successfully.\n";

    if (!wait_for_bootstrap_ready(
            bootstrapEventName,
            15000)) {
        std::wcerr
            << L"FrontierClient bootstrap readiness was not "
               L"confirmed; continuing with RDR.exe resume.\n";
    } else {
        std::cout
            << "FrontierClient bootstrap ready; resuming RDR.exe.\n";
    }

    CloseHandle(bootstrapEvent);

    if (ResumeThread(processInfo.hThread) ==
        static_cast<DWORD>(-1)) {
        std::wcerr
            << L"ResumeThread failed: "
            << GetLastError()
            << L"\n";

        TerminateProcess(processInfo.hProcess, 6);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
        return 6;
    }

    CloseHandle(processInfo.hThread);

    WaitForSingleObject(
        processInfo.hProcess,
        INFINITE);

    DWORD exitCode = 0;
    GetExitCodeProcess(
        processInfo.hProcess,
        &exitCode);

    CloseHandle(processInfo.hProcess);
    return static_cast<int>(exitCode);
}

} // namespace frontier::launcher
