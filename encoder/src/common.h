// Shared primitives for the ISO 21496-1 encoder.
#pragma once

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace iso21496 {

// Thrown for every recoverable failure; main() turns it into exit code 1 plus a
// single-line message on stderr so the Lightroom plugin can surface it.
class Error : public std::runtime_error {
 public:
  explicit Error(const std::string& what) : std::runtime_error(what) {}
};

[[noreturn]] inline void fail(const std::string& what) { throw Error(what); }

using Bytes = std::vector<uint8_t>;

// Big-endian append helpers. Every container we write (JPEG, Exif/TIFF in MM
// order, ICC, MPF) is big-endian, so these are the common case.
inline void putU8(Bytes& b, uint8_t v) { b.push_back(v); }

inline void putU16BE(Bytes& b, uint16_t v) {
  b.push_back(static_cast<uint8_t>(v >> 8));
  b.push_back(static_cast<uint8_t>(v));
}

inline void putU32BE(Bytes& b, uint32_t v) {
  b.push_back(static_cast<uint8_t>(v >> 24));
  b.push_back(static_cast<uint8_t>(v >> 16));
  b.push_back(static_cast<uint8_t>(v >> 8));
  b.push_back(static_cast<uint8_t>(v));
}

inline void putS32BE(Bytes& b, int32_t v) { putU32BE(b, static_cast<uint32_t>(v)); }

inline void putBytes(Bytes& b, const void* data, size_t n) {
  const auto* p = static_cast<const uint8_t*>(data);
  b.insert(b.end(), p, p + n);
}

inline void putString(Bytes& b, const char* s) {
  while (*s) b.push_back(static_cast<uint8_t>(*s++));
}

inline void writeU16BE(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}

inline void writeU32BE(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

inline uint16_t readU16BE(const uint8_t* p) {
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

inline uint32_t readU32BE(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline uint16_t readU16LE(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t readU32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

Bytes readFile(const std::string& path);
void writeFile(const std::string& path, const Bytes& data);

// Verbose logging is opt-in (--verbose); the plugin parses stdout for the JSON
// report, so all chatter goes to stderr.
extern bool g_verbose;
void logf(const char* fmt, ...);

}  // namespace iso21496
