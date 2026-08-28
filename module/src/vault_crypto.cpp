// SPDX-License-Identifier: MIT OR Apache-2.0
#include "vault_crypto.h"

#include <array>
#include <cstring>
#include <random>

namespace logos::agent::vault {
namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

inline u32 rotr(u32 x, int n) { return (x >> n) | (x << (32 - n)); }

// One SHA-256 compression over a 64-byte block, updating h[0..7].
void block(u32 h[8], const u8 *p)
{
    static const u32 K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    u32 w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (u32(p[i*4])<<24)|(u32(p[i*4+1])<<16)|(u32(p[i*4+2])<<8)|u32(p[i*4+3]);
    for (int i = 16; i < 64; ++i) {
        u32 s0 = rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3);
        u32 s1 = rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10);
        w[i] = w[i-16]+s0+w[i-7]+s1;
    }
    u32 a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 64; ++i) {
        u32 S1 = rotr(e,6)^rotr(e,11)^rotr(e,25);
        u32 ch = (e&f)^(~e&g);
        u32 t1 = hh+S1+ch+K[i]+w[i];
        u32 S0 = rotr(a,2)^rotr(a,13)^rotr(a,22);
        u32 maj = (a&b)^(a&c)^(b&c);
        u32 t2 = S0+maj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}

} // namespace

std::vector<u8> sha256(const std::vector<u8> &data)
{
    u32 h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::vector<u8> msg = data;
    const u64 bits = u64(data.size()) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back(u8((bits >> (i*8)) & 0xff));
    for (std::size_t i = 0; i < msg.size(); i += 64) block(h, msg.data() + i);
    std::vector<u8> out(32);
    for (int i = 0; i < 8; ++i) {
        out[i*4]   = u8(h[i]>>24); out[i*4+1] = u8(h[i]>>16);
        out[i*4+2] = u8(h[i]>>8);  out[i*4+3] = u8(h[i]);
    }
    return out;
}

std::vector<u8> hmacSha256(const std::vector<u8> &key, const std::vector<u8> &message)
{
    std::vector<u8> k = key;
    if (k.size() > 64) k = sha256(k);
    k.resize(64, 0);
    std::vector<u8> inner(64), outer(64);
    for (int i = 0; i < 64; ++i) { inner[i] = k[i]^0x36; outer[i] = k[i]^0x5c; }
    inner.insert(inner.end(), message.begin(), message.end());
    std::vector<u8> ih = sha256(inner);
    outer.insert(outer.end(), ih.begin(), ih.end());
    return sha256(outer);
}

namespace {
// Keystream block i = SHA-256(encKey || nonce || u64_be(i)); encKey and macKey
// are split from the vault key so the two never share a value.
std::vector<u8> derive(const std::vector<u8> &key, const char *tag)
{
    std::vector<u8> in = key;
    for (const char *c = tag; *c; ++c) in.push_back(u8(*c));
    return sha256(in);
}
constexpr std::size_t NONCE = 24, TAG = 32;
} // namespace

std::vector<u8> seal(const std::vector<u8> &key, const std::vector<u8> &plaintext)
{
    std::vector<u8> nonce = newKey();       // 32 bytes; use the first 24
    nonce.resize(NONCE);
    std::vector<u8> encKey = derive(key, "vault-enc"), macKey = derive(key, "vault-mac");

    std::vector<u8> ct(plaintext.size());
    for (std::size_t off = 0; off < plaintext.size(); off += 32) {
        std::vector<u8> seed = encKey;
        seed.insert(seed.end(), nonce.begin(), nonce.end());
        u64 ctr = off / 32;
        for (int i = 7; i >= 0; --i) seed.push_back(u8((ctr >> (i*8)) & 0xff));
        std::vector<u8> ks = sha256(seed);
        for (std::size_t j = 0; j < 32 && off + j < plaintext.size(); ++j)
            ct[off+j] = plaintext[off+j] ^ ks[j];
    }
    std::vector<u8> macInput = nonce;
    macInput.insert(macInput.end(), ct.begin(), ct.end());
    std::vector<u8> tag = hmacSha256(macKey, macInput);

    std::vector<u8> out = nonce;
    out.insert(out.end(), ct.begin(), ct.end());
    out.insert(out.end(), tag.begin(), tag.end());
    return out;
}

bool open(const std::vector<u8> &key, const std::vector<u8> &blob, std::vector<u8> &out)
{
    out.clear();
    if (blob.size() < NONCE + TAG) return false;
    std::vector<u8> nonce(blob.begin(), blob.begin() + NONCE);
    std::vector<u8> ct(blob.begin() + NONCE, blob.end() - TAG);
    std::vector<u8> tag(blob.end() - TAG, blob.end());
    std::vector<u8> encKey = derive(key, "vault-enc"), macKey = derive(key, "vault-mac");

    std::vector<u8> macInput = nonce;
    macInput.insert(macInput.end(), ct.begin(), ct.end());
    std::vector<u8> expect = hmacSha256(macKey, macInput);
    u8 diff = 0;                            // constant-time compare
    for (std::size_t i = 0; i < TAG; ++i) diff |= u8(expect[i] ^ tag[i]);
    if (diff != 0) return false;

    out.resize(ct.size());
    for (std::size_t off = 0; off < ct.size(); off += 32) {
        std::vector<u8> seed = encKey;
        seed.insert(seed.end(), nonce.begin(), nonce.end());
        u64 ctr = off / 32;
        for (int i = 7; i >= 0; --i) seed.push_back(u8((ctr >> (i*8)) & 0xff));
        std::vector<u8> ks = sha256(seed);
        for (std::size_t j = 0; j < 32 && off + j < ct.size(); ++j)
            out[off+j] = ct[off+j] ^ ks[j];
    }
    return true;
}

std::vector<u8> newKey()
{
    std::random_device rd;
    std::vector<u8> k(32);
    for (auto &b : k) b = u8(rd() & 0xff);
    return k;
}

} // namespace logos::agent::vault
