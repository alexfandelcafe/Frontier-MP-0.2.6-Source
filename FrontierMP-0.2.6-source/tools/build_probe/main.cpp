#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winver.h>

#include "frontier/game/build_fingerprint.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <cstring>

namespace {
std::string file_version(const std::wstring& path) {
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (size == 0) return {};
    std::vector<std::uint8_t> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || info == nullptr) return {};
    return std::to_string(HIWORD(info->dwFileVersionMS)) + "." +
           std::to_string(LOWORD(info->dwFileVersionMS)) + "." +
           std::to_string(HIWORD(info->dwFileVersionLS)) + "." +
           std::to_string(LOWORD(info->dwFileVersionLS));
}

bool read_all(const std::wstring& path, std::vector<std::uint8_t>& bytes) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    file.seekg(0, std::ios::end);
    const auto size = file.tellg();
    if (size <= 0) return false;
    file.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    return file.good();
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::wcout << L"FrontierMP Build Probe\n\n"
                   << L"Usage:\n"
                   << L"  frontier_build_probe.exe \"C:\\Path\\To\\RDR.exe\"\n\n"
                   << L"This tool inspects a local RDR1 PC executable.\n"
                   << L"It does not modify the game and does not bypass any authentication or DRM.\n\n";
        std::wcout << L"No executable path was supplied.\n";
        std::wcout << L"Press Enter to close...";
        std::wstring ignored;
        std::getline(std::wcin, ignored);
        return 0;
    }

    const std::wstring path = argv[1];
    std::vector<std::uint8_t> file;
    if (!read_all(path, file)) {
        std::wcerr << L"Unable to read: " << path << L"\n";
        std::wcerr << L"Press Enter to close..." << std::endl;
        std::wstring ignored;
        std::getline(std::wcin, ignored);
        return 1;
    }
    if (file.size() < sizeof(IMAGE_DOS_HEADER)) return 2;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 3;
    if (dos->e_lfanew <= 0 || static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > file.size()) return 4;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return 5;

    frontier::game::ExecutableFingerprint fp{};
    fp.peTimestamp = nt->FileHeader.TimeDateStamp;
    fp.imageSize = nt->OptionalHeader.SizeOfImage;
    fp.machine = "AMD64";
    fp.fileVersion = file_version(path);

    const auto* sections = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char name[9]{};
        std::memcpy(name, sections[i].Name, 8);
        if (std::string(name) != ".text") continue;
        fp.textRva = sections[i].VirtualAddress;
        fp.textSize = sections[i].Misc.VirtualSize;
        const std::size_t raw = sections[i].PointerToRawData;
        const std::size_t rawSize = sections[i].SizeOfRawData;
        if (raw + rawSize > file.size()) return 6;
        const std::size_t hashSize = std::min<std::size_t>(sections[i].Misc.VirtualSize, rawSize);
        fp.textHash = frontier::game::fnv1a64(file.data() + raw, hashSize);
        break;
    }

    std::cout << "path=" << std::string(path.begin(), path.end()) << "\n"
              << "machine=" << fp.machine << "\n"
              << "file_version=" << fp.fileVersion << "\n"
              << "pe_timestamp=0x" << std::hex << fp.peTimestamp << std::dec << "\n"
              << "image_size=0x" << std::hex << fp.imageSize << std::dec << "\n"
              << "text_rva=0x" << std::hex << fp.textRva << std::dec << "\n"
              << "text_size=0x" << std::hex << fp.textSize << std::dec << "\n"
              << "text_fnv1a64=0x" << std::hex << fp.textHash << std::dec << "\n";
    const auto matched = frontier::game::BuildDetector::match(fp);
    const auto desc = frontier::game::BuildDetector::descriptor(matched);
    std::cout << "known_build_match=" << (matched != frontier::game::KnownBuild::Unknown ? "yes" : "no") << "\n"
              << "build_label=" << desc.label << "\n"
              << "runtime_compatibility_verified=" << (desc.runtimeCompatibilityVerified ? "yes" : "no") << "\n";
    return 0;
}
