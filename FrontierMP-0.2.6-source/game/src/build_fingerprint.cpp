#include "frontier/game/build_fingerprint.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <winver.h>
#endif

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

namespace frontier::game {

std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t size) {
    constexpr std::uint64_t kOffset = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    std::uint64_t hash = kOffset;
    for (std::size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * kPrime;
    return hash;
}

namespace {

std::string get_file_version_from_path(const std::wstring& path) {
#ifdef _WIN32
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (size == 0) return {};
    std::vector<std::uint8_t> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info) return {};
    std::ostringstream out;
    out << HIWORD(info->dwFileVersionMS) << '.' << LOWORD(info->dwFileVersionMS) << '.'
        << HIWORD(info->dwFileVersionLS) << '.' << LOWORD(info->dwFileVersionLS);
    return out.str();
#else
    (void)path;
    return {};
#endif
}

template <typename RangePtr>
bool inspect_pe_buffer(RangePtr bytes, std::size_t size, ExecutableFingerprint& output) {
#ifdef _WIN32
    if (!bytes || size < sizeof(IMAGE_DOS_HEADER)) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    if (dos->e_lfanew <= 0 || static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > size) return false;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const std::uint8_t*>(bytes) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;

    output = {};
    output.peTimestamp = nt->FileHeader.TimeDateStamp;
    output.imageSize = nt->OptionalHeader.SizeOfImage;
    output.machine = "AMD64";

    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char name[9]{};
        std::memcpy(name, sections[i].Name, 8);
        if (std::string(name) != ".text") continue;

        output.textRva = sections[i].VirtualAddress;
        output.textSize = sections[i].Misc.VirtualSize;
        const auto raw = static_cast<std::size_t>(sections[i].PointerToRawData);
        const auto rawSize = static_cast<std::size_t>(sections[i].SizeOfRawData);
        const auto hashSize = std::min<std::size_t>(sections[i].Misc.VirtualSize, rawSize);
        if (raw + hashSize > size) return false;

        output.textHash = fnv1a64(reinterpret_cast<const std::uint8_t*>(bytes) + raw, hashSize);
        return true;
    }
    return false;
#else
    (void)bytes; (void)size; (void)output;
    return false;
#endif
}

std::string get_file_version_from_module(HMODULE module) {
#ifdef _WIN32
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return get_file_version_from_path(std::wstring(path, length));
#else
    (void)module;
    return {};
#endif
}

} // namespace

bool BuildDetector::inspect_loaded_module(ExecutableFingerprint& output) {
#ifdef _WIN32
    HMODULE module = GetModuleHandleW(nullptr);
    if (!module) return false;

    const auto* base = reinterpret_cast<const std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return false;

    output = {};
    output.peTimestamp = nt->FileHeader.TimeDateStamp;
    output.imageSize = nt->OptionalHeader.SizeOfImage;
    output.machine = "AMD64";
    output.fileVersion = get_file_version_from_module(module);

    const IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char name[9]{};
        std::memcpy(name, sections[i].Name, 8);
        if (std::string(name) != ".text") continue;

        output.textRva = sections[i].VirtualAddress;
        output.textSize = sections[i].Misc.VirtualSize;
        const auto hashSize = std::min<std::uint32_t>(sections[i].Misc.VirtualSize, sections[i].SizeOfRawData);
        output.textHash = fnv1a64(base + output.textRva, hashSize);
        return true;
    }
    return false;
#else
    (void)output;
    return false;
#endif
}

bool BuildDetector::inspect_file(const std::wstring& path, ExecutableFingerprint& output) {
#ifdef _WIN32
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    file.seekg(0, std::ios::end);
    const auto end = file.tellg();
    if (end <= 0) return false;
    file.seekg(0, std::ios::beg);

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) return false;

    if (!inspect_pe_buffer(bytes.data(), bytes.size(), output)) return false;
    output.fileVersion = get_file_version_from_path(path);
    return true;
#else
    (void)path; (void)output;
    return false;
#endif
}

KnownBuild BuildDetector::match(const ExecutableFingerprint& fingerprint) {
    const BuildDescriptor candidate = descriptor(KnownBuild::RdrPcPublic_1_0_42_46611_FingerprintA);
    if (!candidate.fingerprintComplete) return KnownBuild::Unknown;
    return fingerprint.peTimestamp == candidate.expected.peTimestamp &&
           fingerprint.imageSize == candidate.expected.imageSize &&
           fingerprint.textRva == candidate.expected.textRva &&
           fingerprint.textSize == candidate.expected.textSize &&
           fingerprint.textHash == candidate.expected.textHash
        ? candidate.id
        : KnownBuild::Unknown;
}

BuildDescriptor BuildDetector::descriptor(KnownBuild build) {
    switch (build) {
    case KnownBuild::RdrPcPublic_1_0_42_46611_FingerprintA:
        return BuildDescriptor{
            build,
            "rdr-pc-1.0.42.46611-fingerprint-a",
            ExecutableFingerprint{
                0x673783F3u,
                0x5A5EC600u,
                0x1000u,
                0x104A140u,
                0xB213CBF3DEE9B6BFULL,
                "1.0.42.46611",
                "AMD64",
            },
            {true, true, false, false, false, false},
            true,
            false,
        };
    default:
        return BuildDescriptor{};
    }
}

std::string build_label(KnownBuild build) {
    return BuildDetector::descriptor(build).label;
}

} // namespace frontier::game
