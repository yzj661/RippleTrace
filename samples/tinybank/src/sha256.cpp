// tinybank - SHA-256 digest (FIPS 180-4)
#include "tinybank/sha256.h"

#include <sstream>

namespace tinybank {

namespace {

const std::uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

inline std::uint32_t rotr(std::uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}

inline std::uint32_t choose(std::uint32_t e, std::uint32_t f, std::uint32_t g) {
    return (e & f) ^ (~e & g);
}

inline std::uint32_t majority(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    return (a & b) ^ (a & c) ^ (b & c);
}

inline std::uint32_t big_sigma0(std::uint32_t x) {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

inline std::uint32_t big_sigma1(std::uint32_t x) {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

inline std::uint32_t small_sigma0(std::uint32_t x) {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

inline std::uint32_t small_sigma1(std::uint32_t x) {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

}  // namespace

std::vector<std::uint8_t> sha256_bytes(const std::string& data) {
    std::uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                          0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

    std::vector<std::uint8_t> message(data.begin(), data.end());
    const std::uint64_t bitLen = static_cast<std::uint64_t>(message.size()) * 8u;

    message.push_back(0x80);
    while (message.size() % 64 != 56) message.push_back(0x00);
    for (int i = 7; i >= 0; --i) {
        message.push_back(static_cast<std::uint8_t>((bitLen >> (i * 8)) & 0xffu));
    }

    for (std::size_t block = 0; block < message.size(); block += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const std::size_t o = block + static_cast<std::size_t>(i) * 4;
            w[i] = (static_cast<std::uint32_t>(message[o]) << 24) |
                   (static_cast<std::uint32_t>(message[o + 1]) << 16) |
                   (static_cast<std::uint32_t>(message[o + 2]) << 8) |
                   static_cast<std::uint32_t>(message[o + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            w[i] = small_sigma1(w[i - 2]) + w[i - 7] + small_sigma0(w[i - 15]) + w[i - 16];
        }

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        std::uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

        for (int i = 0; i < 64; ++i) {
            const std::uint32_t t1 =
                hh + big_sigma1(e) + choose(e, f, g) + K[i] + w[i];
            const std::uint32_t t2 = big_sigma0(a) + majority(a, b, c);
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    std::vector<std::uint8_t> digest(32);
    for (int i = 0; i < 8; ++i) {
        digest[i * 4 + 0] = static_cast<std::uint8_t>((h[i] >> 24) & 0xffu);
        digest[i * 4 + 1] = static_cast<std::uint8_t>((h[i] >> 16) & 0xffu);
        digest[i * 4 + 2] = static_cast<std::uint8_t>((h[i] >> 8) & 0xffu);
        digest[i * 4 + 3] = static_cast<std::uint8_t>(h[i] & 0xffu);
    }
    return digest;
}

std::string sha256_hex(const std::string& data) {
    static const char* digits = "0123456789abcdef";
    const std::vector<std::uint8_t> digest = sha256_bytes(data);
    std::string out;
    out.reserve(digest.size() * 2);
    for (std::uint8_t byte : digest) {
        out.push_back(digits[(byte >> 4) & 0x0f]);
        out.push_back(digits[byte & 0x0f]);
    }
    return out;
}

}  // namespace tinybank
