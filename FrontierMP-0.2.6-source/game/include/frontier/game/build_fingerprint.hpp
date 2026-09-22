#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace frontier::game {

struct ExecutableFingerprint final {
    std::uint32_t peTimestamp{};
    std::uint32_t imageSize{};
    std::uint32_t textRva{};
    std::uint32_t textSize{};
    std::uint64_t textHash{};
    std::string fileVersion;
    std::string machine;
};

enum class KnownBuild {
    Unknown,
    RdrPcPublic_1_0_42_46611_FingerprintA,
};

struct FeatureCapabilities final {
    bool patternScan{};
    bool nativeInvocation{};
    bool actorAccess{};
    bool input{};
    bool renderHook{};
    bool scriptTick{};
};

struct BuildDescriptor final {
    KnownBuild id{KnownBuild::Unknown};
    const char* label{"unknown"};
    ExecutableFingerprint expected{};
    FeatureCapabilities capabilities{};
    bool fingerprintComplete{};
    bool runtimeCompatibilityVerified{};
};

class BuildDetector final {
public:
    static bool inspect_loaded_module(ExecutableFingerprint& output);
    static bool inspect_file(const std::wstring& path, ExecutableFingerprint& output);
    static KnownBuild match(const ExecutableFingerprint& fingerprint);
    static BuildDescriptor descriptor(KnownBuild build);
};

std::uint64_t fnv1a64(const std::uint8_t* data, std::size_t size);
std::string build_label(KnownBuild build);

} // namespace frontier::game
