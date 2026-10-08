#include "pkce.hpp"

#include <cstdint>

#include <array>
#include <random>
#include <vector>

namespace supabase::auth::pkce
{

namespace
{
    constexpr std::array<std::uint32_t, 64> kRound
        = { 0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

    constexpr std::uint32_t rotr(std::uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

    std::array<std::uint8_t, 32> sha256(std::string_view message)
    {
        std::array<std::uint32_t, 8> h = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };

        std::vector<std::uint8_t> data(message.begin(), message.end());
        const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
        data.push_back(0x80);
        while (data.size() % 64 != 56)
            data.push_back(0);
        for (int shift = 56; shift >= 0; shift -= 8)
            data.push_back(static_cast<std::uint8_t>(bits >> shift));

        for (std::size_t offset = 0; offset < data.size(); offset += 64)
        {
            std::array<std::uint32_t, 64> w {};
            for (std::size_t i = 0; i < 16; ++i)
            {
                const std::uint8_t* p = &data[offset + i * 4];
                w[i]                  = (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
            }
            for (std::size_t i = 16; i < 64; ++i)
            {
                const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
                const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
                w[i]          = w[i - 16] + s0 + w[i - 7] + s1;
            }

            auto v = h;
            for (std::size_t i = 0; i < 64; ++i)
            {
                const auto s1    = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
                const auto ch    = (v[4] & v[5]) ^ (~v[4] & v[6]);
                const auto temp1 = v[7] + s1 + ch + kRound[i] + w[i];
                const auto s0    = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
                const auto maj   = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
                const auto temp2 = s0 + maj;
                v[7]             = v[6];
                v[6]             = v[5];
                v[5]             = v[4];
                v[4]             = v[3] + temp1;
                v[3]             = v[2];
                v[2]             = v[1];
                v[1]             = v[0];
                v[0]             = temp1 + temp2;
            }
            for (std::size_t i = 0; i < 8; ++i)
                h[i] += v[i];
        }

        std::array<std::uint8_t, 32> digest {};
        for (std::size_t i = 0; i < 8; ++i)
            for (std::size_t b = 0; b < 4; ++b)
                digest[i * 4 + b] = static_cast<std::uint8_t>(h[i] >> (24 - 8 * b));
        return digest;
    }

    std::string base64Url(const std::uint8_t* data, std::size_t size)
    {
        constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        std::string out;
        for (std::size_t i = 0; i < size; i += 3)
        {
            const std::size_t left = size - i;
            const std::uint32_t chunk
                = (std::uint32_t(data[i]) << 16) | (left > 1 ? std::uint32_t(data[i + 1]) << 8 : 0) | (left > 2 ? data[i + 2] : 0);
            out += alphabet[(chunk >> 18) & 63];
            out += alphabet[(chunk >> 12) & 63];
            if (left > 1)
                out += alphabet[(chunk >> 6) & 63];
            if (left > 2)
                out += alphabet[chunk & 63];
        }
        return out;
    }
}

std::string generateVerifier()
{
    constexpr std::string_view charset = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    std::random_device device;
    std::string out;
    out.reserve(64);
    // charset size is 66, so a modulo of a 32-bit value has negligible bias for a nonce.
    for (int i = 0; i < 64; ++i)
        out += charset[device() % charset.size()];
    return out;
}

std::string challengeS256(std::string_view verifier)
{
    const auto digest = sha256(verifier);
    return base64Url(digest.data(), digest.size());
}

}
