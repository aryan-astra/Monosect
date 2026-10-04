// MonoSect — SHA-256 (FIPS 180-4) used for dependency and output verification.
//
// The FFmpeg bootstrap identifies the tools it runs with this hash
// (core/DependencyPin.cpp: a pinned release hash, or the hash of the first
// download recorded and enforced afterwards, TODO 221/315).
// Segment content hashes for the job database use the same code (TODO 138).
//
// Implemented locally so that the exact same, test-covered code is used on
// Windows and in the Linux verification harness.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace monosect::core {

class Sha256 {
public:
    Sha256();

    void update(const void* data, std::size_t size);
    void update(std::string_view text);
    [[nodiscard]] std::array<uint8_t, 32> digest();

private:
    void transform(const uint8_t block[64]);

    std::array<uint32_t, 8> state_{};
    std::array<uint8_t, 64> buffer_{};
    uint64_t totalBytes_ = 0;
    std::size_t bufferLength_ = 0;
    bool finalized_ = false;
};

[[nodiscard]] std::string toHex(const uint8_t* data, std::size_t size);
[[nodiscard]] std::string sha256Hex(std::string_view text);
[[nodiscard]] std::string sha256HexOfBytes(const void* data, std::size_t size);

} // namespace monosect::core
