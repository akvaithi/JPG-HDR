// Minimal assertion helpers and synthetic input generators for the tests.
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "common.h"

namespace iso21496 {
namespace test {

extern int g_failures;

inline void reportFailure(const char* file, int line, const std::string& what) {
  std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, what.c_str());
  ++g_failures;
}

// std::to_string does not accept strings; CHECK_EQ needs both.
inline std::string show(const std::string& s) { return "\"" + s + "\""; }
inline std::string show(const char* s) { return std::string("\"") + s + "\""; }
template <typename T>
inline std::string show(const T& v) {
  return std::to_string(v);
}

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond))                                                           \
      ::iso21496::test::reportFailure(__FILE__, __LINE__, "CHECK(" #cond ")"); \
  } while (0)

#define CHECK_EQ(a, b)                                                     \
  do {                                                                     \
    auto va_ = (a);                                                        \
    auto vb_ = (b);                                                        \
    if (!(va_ == vb_))                                                     \
      ::iso21496::test::reportFailure(                                     \
          __FILE__, __LINE__,                                              \
          std::string(#a " == " #b " (") + ::iso21496::test::show(va_) +   \
              " vs " + ::iso21496::test::show(vb_) + ")");                 \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                              \
  do {                                                                     \
    double va_ = static_cast<double>(a);                                   \
    double vb_ = static_cast<double>(b);                                   \
    if (!(std::fabs(va_ - vb_) <= (tol)))                                  \
      ::iso21496::test::reportFailure(                                     \
          __FILE__, __LINE__,                                              \
          std::string(#a " ~= " #b " (") + std::to_string(va_) + " vs " +  \
              std::to_string(vb_) + ")");                                  \
  } while (0)

#define CHECK_THROWS(expr)                                                 \
  do {                                                                     \
    bool threw_ = false;                                                   \
    try {                                                                  \
      (void)(expr);                                                        \
    } catch (const ::iso21496::Error&) {                                   \
      threw_ = true;                                                       \
    }                                                                      \
    if (!threw_)                                                           \
      ::iso21496::test::reportFailure(__FILE__, __LINE__,                  \
                                      "expected " #expr " to throw");      \
  } while (0)

#define TEST_MAIN(body)                                                    \
  int ::iso21496::test::g_failures = 0;                                    \
  int main() {                                                             \
    body();                                                                \
    if (::iso21496::test::g_failures)                                      \
      std::fprintf(stderr, "%d check(s) failed\n",                         \
                   ::iso21496::test::g_failures);                          \
    return ::iso21496::test::g_failures ? 1 : 0;                           \
  }

// ------------------------------------------------------------ TIFF writer ---

struct TiffSpec {
  uint32_t width = 16;
  uint32_t height = 16;
  int channels = 3;
  int bitsPerSample = 16;
  bool floatSamples = false;
  bool littleEndian = true;
  uint32_t rowsPerStrip = 0;  // 0 = one strip
  Bytes iccProfile;
  bool withExif = true;
};

// Writes an uncompressed baseline TIFF. `pixels` holds width*height*channels
// samples in [0, +inf) — values above 1.0 are the HDR highlights.
Bytes makeTiff(const TiffSpec& spec, const std::vector<float>& pixels);

// A synthetic HDR test image: a mid-grey field, a smooth gradient, and a
// specular patch reaching `peak` times SDR white.
std::vector<float> makeHdrPattern(uint32_t w, uint32_t h, float peak);

// --------------------------------------------------------- JPEG structure ---

struct JpegSegment {
  uint8_t marker = 0;
  size_t offset = 0;      // offset of the 0xFF byte
  size_t payloadOffset = 0;
  size_t payloadSize = 0;
};

// Walks the marker structure of one JPEG image starting at `start`. Stops
// after EOI and reports where that was, so a multi-picture file can be split.
std::vector<JpegSegment> walkJpeg(const Bytes& data, size_t start,
                                  size_t* endOffset);

}  // namespace test
}  // namespace iso21496
