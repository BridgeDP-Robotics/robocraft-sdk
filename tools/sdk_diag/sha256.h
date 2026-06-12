// SHA-256 — self-contained public-domain implementation (no OpenSSL).
// sdk_diag is an open-source standalone tool; keep dependencies minimal.
#ifndef SDK_DIAG_SHA256_H
#define SDK_DIAG_SHA256_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace sdk_diag {

// Returns the lowercase hex SHA-256 of a memory buffer.
std::string Sha256Hex(const void* data, std::size_t len);

// Returns the lowercase hex SHA-256 of a file's contents.
// On error, ok is set to false and the return value is empty.
std::string Sha256File(const std::string& path, bool* ok);

}  // namespace sdk_diag

#endif  // SDK_DIAG_SHA256_H
