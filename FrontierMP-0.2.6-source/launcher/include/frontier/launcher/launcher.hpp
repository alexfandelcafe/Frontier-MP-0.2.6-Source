#pragma once

#include <string>
#include <vector>

namespace frontier::launcher {

struct LaunchOptions final {
    std::wstring gameExe;
    std::wstring clientDll;
    std::string serverHost{"127.0.0.1"};
    unsigned short serverPort{30120};
    std::string playerName{"Player"};
    std::vector<std::wstring> gameArguments;
};

class Launcher final {
public:
    int run(const LaunchOptions& options) const;
    static std::wstring quote(const std::wstring& value);
    static std::vector<wchar_t> build_environment(const LaunchOptions& options);

private:
    bool verify_game(const std::wstring& gameExe) const;
    bool inject_client(void* processHandle, const std::wstring& clientDll, unsigned long& loadResult) const;
};

} // namespace frontier::launcher
