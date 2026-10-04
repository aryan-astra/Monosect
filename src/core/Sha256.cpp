#include "core/Sha256.hpp"

#include <cstring>

namespace monosect::core {

namespace {

constexpr std::array<uint32_t, 64> kRoundConstants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

constexpr uint32_t rotr(uint32_t value, uint32_t count) noexcept {
    return (value >> count) | (value << (32U - count));
}

} // namespace

Sha256::Sha256() {
    state_ = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
              0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
}

void Sha256::transform(const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[i * 4]) << 24U) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 16U) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 8U) |
               static_cast<uint32_t>(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];
    uint32_t e = state_[4];
    uint32_t f = state_[5];
    uint32_t g = state_[6];
    uint32_t h = state_[7];

    for (int i = 0; i < 64; ++i) {
        const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t temp1 = h + s1 + ch + kRoundConstants[static_cast<std::size_t>(i)] + w[i];
        const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const void* data, std::size_t size) {
    if (finalized_ || data == nullptr || size == 0) {
        return;
    }
    const auto* bytes = static_cast<const uint8_t*>(data);
    totalBytes_ += size;

    if (bufferLength_ > 0) {
        const std::size_t needed = 64 - bufferLength_;
        const std::size_t take = size < needed ? size : needed;
        std::memcpy(buffer_.data() + bufferLength_, bytes, take);
        bufferLength_ += take;
        bytes += take;
        size -= take;
        if (bufferLength_ == 64) {
            transform(buffer_.data());
            bufferLength_ = 0;
        }
    }

    while (size >= 64) {
        transform(bytes);
        bytes += 64;
        size -= 64;
    }

    if (size > 0) {
        std::memcpy(buffer_.data(), bytes, size);
        bufferLength_ = size;
    }
}

void Sha256::update(std::string_view text) {
    update(text.data(), text.size());
}

std::array<uint8_t, 32> Sha256::digest() {
    if (!finalized_) {
        const uint64_t bitLength = totalBytes_ * 8ULL;
        const uint8_t padding = 0x80;
        update(&padding, 1);
        const uint8_t zero = 0x00;
        while (bufferLength_ != 56) {
            update(&zero, 1);
        }
        uint8_t lengthBytes[8];
        for (int i = 0; i < 8; ++i) {
            lengthBytes[i] = static_cast<uint8_t>((bitLength >> (56 - 8 * i)) & 0xFFU);
        }
        // Bypass the length counter for the trailer.
        const uint64_t savedTotal = totalBytes_;
        update(lengthBytes, 8);
        totalBytes_ = savedTotal;
        finalized_ = true;
    }

    std::array<uint8_t, 32> out{};
    for (std::size_t i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<uint8_t>((state_[i] >> 24U) & 0xFFU);
        out[i * 4 + 1] = static_cast<uint8_t>((state_[i] >> 16U) & 0xFFU);
        out[i * 4 + 2] = static_cast<uint8_t>((state_[i] >> 8U) & 0xFFU);
        out[i * 4 + 3] = static_cast<uint8_t>(state_[i] & 0xFFU);
    }
    return out;
}

std::string toHex(const uint8_t* data, std::size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(kDigits[(data[i] >> 4U) & 0x0FU]);
        out.push_back(kDigits[data[i] & 0x0FU]);
    }
    return out;
}

std::string sha256HexOfBytes(const void* data, std::size_t size) {
    Sha256 hasher;
    hasher.update(data, size);
    const auto digestValue = hasher.digest();
    return toHex(digestValue.data(), digestValue.size());
}

std::string sha256Hex(std::string_view text) {
    return sha256HexOfBytes(text.data(), text.size());
}

} // namespace monosect::core
