#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace websocket {
// Dependency-free SHA-1 for the public WebSocket handshake only (RFC 6455).
// Not intended for passwords, authentication, signatures or encryption.
inline std::string sha1Base64(const std::string& input) {
    std::vector<std::uint8_t> bytes(input.begin(), input.end());
    const auto bits = static_cast<std::uint64_t>(bytes.size()) * 8;
    bytes.push_back(0x80);
    while (bytes.size() % 64 != 56) bytes.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8) bytes.push_back(static_cast<std::uint8_t>(bits >> shift));
    const auto rotate = [](std::uint32_t value, unsigned count) { return (value << count) | (value >> (32 - count)); };
    std::array<std::uint32_t, 5> hash{{0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0}};
    for (std::size_t block = 0; block < bytes.size(); block += 64) {
        std::array<std::uint32_t, 80> words{};
        for (std::size_t index = 0; index < 16; ++index)
            for (std::size_t octet = 0; octet < 4; ++octet)
                words[index] = (words[index] << 8) | bytes[block + index * 4 + octet];
        for (std::size_t index = 16; index < 80; ++index)
            words[index] = rotate(words[index - 3] ^ words[index - 8] ^ words[index - 14] ^ words[index - 16], 1);
        auto a = hash[0], b = hash[1], c = hash[2], d = hash[3], e = hash[4];
        for (std::size_t index = 0; index < 80; ++index) {
            std::uint32_t f, k;
            if (index < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
            else if (index < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
            else if (index < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
            else { f = b ^ c ^ d; k = 0xca62c1d6; }
            const std::uint32_t next = rotate(a, 5) + f + e + k + words[index];
            e = d; d = c; c = rotate(b, 30); b = a; a = next;
        }
        hash[0] += a; hash[1] += b; hash[2] += c; hash[3] += d; hash[4] += e;
    }
    std::array<std::uint8_t, 20> digest{};
    for (std::size_t index = 0; index < 20; ++index)
        digest[index] = static_cast<std::uint8_t>(hash[index / 4] >> (24 - 8 * (index % 4)));
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    for (std::size_t index = 0; index < digest.size(); index += 3) {
        std::uint32_t value = static_cast<std::uint32_t>(digest[index]) << 16;
        if (index + 1 < digest.size()) value |= static_cast<std::uint32_t>(digest[index + 1]) << 8;
        if (index + 2 < digest.size()) value |= digest[index + 2];
        output += alphabet[(value >> 18) & 63]; output += alphabet[(value >> 12) & 63];
        output += index + 1 < digest.size() ? alphabet[(value >> 6) & 63] : '=';
        output += index + 2 < digest.size() ? alphabet[value & 63] : '=';
    }
    return output;
}
inline bool acceptKey(const std::string& key, std::string& accept) {
    accept.clear();
    // A browser sends a canonical base64-encoded 16-byte nonce (24 characters).
    const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (key.size() != 24 || key.substr(22) != "==") return false;
    for (std::size_t index = 0; index < 22; ++index)
        if (alphabet.find(key[index]) == std::string::npos) return false;
    if ((alphabet.find(key[21]) & 15) != 0) return false;
    accept = sha1Base64(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    return true;
}
} // namespace websocket
