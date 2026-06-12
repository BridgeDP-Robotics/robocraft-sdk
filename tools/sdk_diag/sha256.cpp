#include "sha256.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace sdk_diag {
namespace {

struct Sha256Ctx {
  uint32_t state[8];
  uint64_t bitlen;
  uint8_t buffer[64];
  std::size_t buflen;
};

inline uint32_t Rotr(uint32_t x, uint32_t n) {
  return (x >> n) | (x << (32 - n));
}

const uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

void Init(Sha256Ctx& c) {
  c.state[0] = 0x6a09e667;
  c.state[1] = 0xbb67ae85;
  c.state[2] = 0x3c6ef372;
  c.state[3] = 0xa54ff53a;
  c.state[4] = 0x510e527f;
  c.state[5] = 0x9b05688c;
  c.state[6] = 0x1f83d9ab;
  c.state[7] = 0x5be0cd19;
  c.bitlen = 0;
  c.buflen = 0;
}

void Transform(Sha256Ctx& c, const uint8_t* data) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (uint32_t(data[i * 4]) << 24) | (uint32_t(data[i * 4 + 1]) << 16) |
           (uint32_t(data[i * 4 + 2]) << 8) | (uint32_t(data[i * 4 + 3]));
  }
  for (int i = 16; i < 64; ++i) {
    uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = c.state[0], b = c.state[1], cc = c.state[2], d = c.state[3];
  uint32_t e = c.state[4], f = c.state[5], g = c.state[6], h = c.state[7];

  for (int i = 0; i < 64; ++i) {
    uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + s1 + ch + kK[i] + w[i];
    uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
    uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
    uint32_t t2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = cc;
    cc = b;
    b = a;
    a = t1 + t2;
  }

  c.state[0] += a;
  c.state[1] += b;
  c.state[2] += cc;
  c.state[3] += d;
  c.state[4] += e;
  c.state[5] += f;
  c.state[6] += g;
  c.state[7] += h;
}

void Update(Sha256Ctx& c, const uint8_t* data, std::size_t len) {
  for (std::size_t i = 0; i < len; ++i) {
    c.buffer[c.buflen++] = data[i];
    if (c.buflen == 64) {
      Transform(c, c.buffer);
      c.bitlen += 512;
      c.buflen = 0;
    }
  }
}

void Final(Sha256Ctx& c, uint8_t out[32]) {
  std::size_t i = c.buflen;
  c.buffer[i++] = 0x80;
  if (i > 56) {
    while (i < 64) c.buffer[i++] = 0x00;
    Transform(c, c.buffer);
    i = 0;
  }
  while (i < 56) c.buffer[i++] = 0x00;

  c.bitlen += static_cast<uint64_t>(c.buflen) * 8;
  for (int j = 7; j >= 0; --j) {
    c.buffer[56 + (7 - j)] = static_cast<uint8_t>(c.bitlen >> (j * 8));
  }
  Transform(c, c.buffer);

  for (int j = 0; j < 8; ++j) {
    out[j * 4] = static_cast<uint8_t>(c.state[j] >> 24);
    out[j * 4 + 1] = static_cast<uint8_t>(c.state[j] >> 16);
    out[j * 4 + 2] = static_cast<uint8_t>(c.state[j] >> 8);
    out[j * 4 + 3] = static_cast<uint8_t>(c.state[j]);
  }
}

std::string ToHex(const uint8_t digest[32]) {
  static const char* kHex = "0123456789abcdef";
  std::string s;
  s.resize(64);
  for (int i = 0; i < 32; ++i) {
    s[i * 2] = kHex[digest[i] >> 4];
    s[i * 2 + 1] = kHex[digest[i] & 0x0f];
  }
  return s;
}

}  // namespace

std::string Sha256Hex(const void* data, std::size_t len) {
  Sha256Ctx c;
  Init(c);
  Update(c, static_cast<const uint8_t*>(data), len);
  uint8_t digest[32];
  Final(c, digest);
  return ToHex(digest);
}

std::string Sha256File(const std::string& path, bool* ok) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    if (ok) *ok = false;
    return {};
  }
  Sha256Ctx c;
  Init(c);
  std::vector<uint8_t> buf(64 * 1024);
  std::size_t n;
  while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
    Update(c, buf.data(), n);
  }
  bool read_err = std::ferror(f) != 0;
  std::fclose(f);
  if (read_err) {
    if (ok) *ok = false;
    return {};
  }
  uint8_t digest[32];
  Final(c, digest);
  if (ok) *ok = true;
  return ToHex(digest);
}

}  // namespace sdk_diag
