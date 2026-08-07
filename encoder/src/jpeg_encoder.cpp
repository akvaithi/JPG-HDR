#include "jpeg_encoder.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "threads.h"

namespace iso21496 {
namespace {

// ---------------------------------------------------------------- tables ---

const int kZigZag[64] = {
    0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
   12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
   35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
   58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

// ITU-T T.81 Annex K.1 example quantisation tables.
const int kQuantLuma[64] = {
    16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};

const int kQuantChroma[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

// Annex K.3 default Huffman tables, used when --no-optimize is in effect.
const uint8_t kStdDcLumaBits[17] = {0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
const uint8_t kStdDcLumaVal[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
const uint8_t kStdDcChromaBits[17] = {0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
const uint8_t kStdDcChromaVal[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

const uint8_t kStdAcLumaBits[17] = {0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
const uint8_t kStdAcLumaVal[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,
    0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
    0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4,
    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

const uint8_t kStdAcChromaBits[17] = {0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
const uint8_t kStdAcChromaVal[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41,
    0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
    0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
    0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74,
    0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
    0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4,
    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

// AAN scale factors: cos(k*pi/16)*sqrt(2) style pre-scaling folded into the
// quantisation divisors so the DCT itself needs no final normalisation.
const double kAanScale[8] = {1.0,        1.387039845, 1.306562965, 1.175875602,
                             1.0,        0.785694958, 0.541196100, 0.275899379};

struct HuffTable {
  uint8_t bits[17] = {0};
  std::vector<uint8_t> values;
  // Derived encoding tables.
  uint16_t code[256] = {0};
  uint8_t size[256] = {0};

  void deriveCodes() {
    std::memset(code, 0, sizeof(code));
    std::memset(size, 0, sizeof(size));
    uint16_t c = 0;
    size_t k = 0;
    for (int len = 1; len <= 16; ++len) {
      for (int i = 0; i < bits[len]; ++i) {
        if (k >= values.size()) return;
        code[values[k]] = c;
        size[values[k]] = static_cast<uint8_t>(len);
        ++c;
        ++k;
      }
      c <<= 1;
    }
  }

  void setStandard(const uint8_t* b, const uint8_t* v, size_t n) {
    std::memcpy(bits, b, 17);
    values.assign(v, v + n);
    deriveCodes();
  }
};

// libjpeg's jpeg_gen_optimal_table: a length-limited Huffman construction that
// guarantees no code longer than 16 bits.
void generateOptimalTable(const std::vector<uint32_t>& freqIn, HuffTable* out) {
  std::vector<long> freq(257, 0);
  for (size_t i = 0; i < 256 && i < freqIn.size(); ++i)
    freq[i] = static_cast<long>(freqIn[i]);
  freq[256] = 1;  // reserve one code point so the all-ones code never occurs

  std::vector<int> codesize(257, 0);
  std::vector<int> others(257, -1);

  for (;;) {
    int c1 = -1, c2 = -1;
    long v1 = -1, v2 = -1;
    for (int i = 0; i <= 256; ++i) {
      if (freq[i] && (v1 < 0 || freq[i] <= v1)) {
        v2 = v1; c2 = c1;
        v1 = freq[i]; c1 = i;
      } else if (freq[i] && (v2 < 0 || freq[i] <= v2)) {
        v2 = freq[i]; c2 = i;
      }
    }
    if (c2 < 0) break;
    freq[c1] += freq[c2];
    freq[c2] = 0;
    ++codesize[c1];
    while (others[c1] >= 0) {
      c1 = others[c1];
      ++codesize[c1];
    }
    others[c1] = c2;
    ++codesize[c2];
    while (others[c2] >= 0) {
      c2 = others[c2];
      ++codesize[c2];
    }
  }

  constexpr int kMaxCodeLen = 32;
  int bits[kMaxCodeLen + 1] = {0};
  for (int i = 0; i <= 256; ++i) {
    if (codesize[i]) {
      if (codesize[i] > kMaxCodeLen)
        fail("internal error: Huffman code length overflow");
      ++bits[codesize[i]];
    }
  }

  // Push codes longer than 16 bits back into the 16-bit budget.
  for (int i = kMaxCodeLen; i > 16; --i) {
    while (bits[i] > 0) {
      int j = i - 2;
      while (bits[j] == 0) --j;
      bits[i] -= 2;
      ++bits[i - 1];
      bits[j + 1] += 2;
      --bits[j];
    }
  }
  {  // Drop the reserved pseudo-symbol from the longest code length in use.
    int i = 16;
    while (i > 0 && bits[i] == 0) --i;
    if (i > 0) --bits[i];
  }

  std::memset(out->bits, 0, sizeof(out->bits));
  for (int i = 1; i <= 16; ++i) out->bits[i] = static_cast<uint8_t>(bits[i]);

  out->values.clear();
  for (int len = 1; len <= 16; ++len)
    for (int sym = 0; sym < 256; ++sym)
      if (codesize[sym] == len) out->values.push_back(static_cast<uint8_t>(sym));
  out->deriveCodes();
}

// ------------------------------------------------------------------ DCT ---

// AAN float forward DCT (after libjpeg's jfdctflt.c).
void forwardDct(float* d) {
  for (int c = 0; c < 8; ++c) {
    float* p = d + c * 8;
    float t0 = p[0] + p[7], t7 = p[0] - p[7];
    float t1 = p[1] + p[6], t6 = p[1] - p[6];
    float t2 = p[2] + p[5], t5 = p[2] - p[5];
    float t3 = p[3] + p[4], t4 = p[3] - p[4];
    float t10 = t0 + t3, t13 = t0 - t3;
    float t11 = t1 + t2, t12 = t1 - t2;
    p[0] = t10 + t11;
    p[4] = t10 - t11;
    float z1 = (t12 + t13) * 0.707106781f;
    p[2] = t13 + z1;
    p[6] = t13 - z1;
    t10 = t4 + t5;
    t11 = t5 + t6;
    t12 = t6 + t7;
    float z5 = (t10 - t12) * 0.382683433f;
    float z2 = 0.541196100f * t10 + z5;
    float z4 = 1.306562965f * t12 + z5;
    float z3 = t11 * 0.707106781f;
    float z11 = t7 + z3, z13 = t7 - z3;
    p[5] = z13 + z2;
    p[3] = z13 - z2;
    p[1] = z11 + z4;
    p[7] = z11 - z4;
  }
  for (int c = 0; c < 8; ++c) {
    float* p = d + c;
    float t0 = p[0] + p[56], t7 = p[0] - p[56];
    float t1 = p[8] + p[48], t6 = p[8] - p[48];
    float t2 = p[16] + p[40], t5 = p[16] - p[40];
    float t3 = p[24] + p[32], t4 = p[24] - p[32];
    float t10 = t0 + t3, t13 = t0 - t3;
    float t11 = t1 + t2, t12 = t1 - t2;
    p[0] = t10 + t11;
    p[32] = t10 - t11;
    float z1 = (t12 + t13) * 0.707106781f;
    p[16] = t13 + z1;
    p[48] = t13 - z1;
    t10 = t4 + t5;
    t11 = t5 + t6;
    t12 = t6 + t7;
    float z5 = (t10 - t12) * 0.382683433f;
    float z2 = 0.541196100f * t10 + z5;
    float z4 = 1.306562965f * t12 + z5;
    float z3 = t11 * 0.707106781f;
    float z11 = t7 + z3, z13 = t7 - z3;
    p[40] = z13 + z2;
    p[24] = z13 - z2;
    p[8] = z11 + z4;
    p[56] = z11 - z4;
  }
}

// ------------------------------------------------------------ bit writer ---

class BitWriter {
 public:
  explicit BitWriter(Bytes& out) : out_(out) {}

  void put(uint16_t code, int length) {
    for (int i = length - 1; i >= 0; --i) {
      buffer_ = static_cast<uint32_t>((buffer_ << 1) | ((code >> i) & 1));
      if (++count_ == 8) flushByte();
    }
  }

  void alignToByte() {
    while (count_ != 0) {
      buffer_ = (buffer_ << 1) | 1;  // pad with 1 bits per T.81
      if (++count_ == 8) flushByte();
    }
  }

 private:
  void flushByte() {
    uint8_t b = static_cast<uint8_t>(buffer_ & 0xff);
    out_.push_back(b);
    if (b == 0xff) out_.push_back(0x00);  // byte stuffing
    buffer_ = 0;
    count_ = 0;
  }

  Bytes& out_;
  uint32_t buffer_ = 0;
  int count_ = 0;
};

int magnitudeCategory(int v) {
  int a = v < 0 ? -v : v;
  int n = 0;
  while (a) {
    ++n;
    a >>= 1;
  }
  return n;
}

struct Component {
  int id = 1;
  int h = 1, v = 1;
  int quantTable = 0;
  int dcTable = 0;
  int acTable = 0;
  uint32_t blocksPerLine = 0;
  uint32_t blocksPerCol = 0;
  std::vector<uint8_t> samples;   // padded plane
  uint32_t planeWidth = 0;
  uint32_t planeHeight = 0;
  std::vector<int16_t> coefs;     // blocksPerLine*blocksPerCol*64, zigzag order
};

void buildQuantTable(int quality, const int* base, uint16_t* table) {
  quality = std::min(100, std::max(1, quality));
  int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
  for (int i = 0; i < 64; ++i) {
    int v = (base[i] * scale + 50) / 100;
    table[i] = static_cast<uint16_t>(std::min(255, std::max(1, v)));
  }
}

Bytes makeSegment(uint8_t marker, const Bytes& payload) {
  Bytes s;
  s.push_back(0xff);
  s.push_back(marker);
  putU16BE(s, static_cast<uint16_t>(payload.size() + 2));
  putBytes(s, payload.data(), payload.size());
  return s;
}

}  // namespace

std::vector<Bytes> buildIccAppSegments(const Bytes& profile) {
  std::vector<Bytes> out;
  if (profile.empty()) return out;
  // 65533 - 2 (length) - 12 ("ICC_PROFILE\0") - 2 (chunk counters)
  const size_t kMaxChunk = 65519;
  size_t chunks = (profile.size() + kMaxChunk - 1) / kMaxChunk;
  if (chunks > 255) fail("ICC profile is too large to embed in JPEG");
  for (size_t i = 0; i < chunks; ++i) {
    Bytes payload;
    putString(payload, "ICC_PROFILE");
    payload.push_back(0);
    payload.push_back(static_cast<uint8_t>(i + 1));
    payload.push_back(static_cast<uint8_t>(chunks));
    size_t off = i * kMaxChunk;
    size_t n = std::min(kMaxChunk, profile.size() - off);
    putBytes(payload, profile.data() + off, n);
    out.push_back(makeSegment(0xe2, payload));
  }
  return out;
}

Bytes buildXmpAppSegment(const std::string& xmp) {
  Bytes payload;
  putString(payload, "http://ns.adobe.com/xap/1.0/");
  payload.push_back(0);
  putBytes(payload, xmp.data(), xmp.size());
  if (payload.size() + 2 > 65535)
    fail("XMP packet is too large for a single APP1 segment");
  return makeSegment(0xe1, payload);
}

Bytes buildJfifAppSegment() {
  Bytes payload;
  putString(payload, "JFIF");
  payload.push_back(0);
  payload.push_back(1);   // major
  payload.push_back(1);   // minor
  payload.push_back(0);   // units: none
  putU16BE(payload, 1);   // x density
  putU16BE(payload, 1);   // y density
  payload.push_back(0);   // thumbnail width
  payload.push_back(0);   // thumbnail height
  return makeSegment(0xe0, payload);
}

Bytes encodeJpeg(const JpegImage& image, const JpegOptions& options) {
  if (image.width == 0 || image.height == 0) fail("cannot encode an empty image");
  if (image.components != 1 && image.components != 3)
    fail("JPEG encoder supports 1 or 3 components only");
  if (!image.pixels) fail("JPEG encoder called without pixel data");

  const bool color = image.components == 3;
  const bool subsample = color && options.chromaSubsample;
  const int numComponents = color ? 3 : 1;

  std::vector<Component> comps(numComponents);
  comps[0].id = 1;
  comps[0].h = subsample ? 2 : 1;
  comps[0].v = subsample ? 2 : 1;
  comps[0].quantTable = 0;
  comps[0].dcTable = 0;
  comps[0].acTable = 0;
  if (color) {
    for (int c = 1; c < 3; ++c) {
      comps[c].id = c + 1;
      comps[c].h = 1;
      comps[c].v = 1;
      comps[c].quantTable = 1;
      comps[c].dcTable = 1;
      comps[c].acTable = 1;
    }
  }

  int hMax = 1, vMax = 1;
  for (const auto& c : comps) {
    hMax = std::max(hMax, c.h);
    vMax = std::max(vMax, c.v);
  }
  const uint32_t mcuWidth = 8u * hMax;
  const uint32_t mcuHeight = 8u * vMax;
  const uint32_t mcusX = (image.width + mcuWidth - 1) / mcuWidth;
  const uint32_t mcusY = (image.height + mcuHeight - 1) / mcuHeight;

  // ---- colour conversion into padded per-component planes -----------------
  for (auto& c : comps) {
    c.blocksPerLine = mcusX * c.h;
    c.blocksPerCol = mcusY * c.v;
    c.planeWidth = c.blocksPerLine * 8;
    c.planeHeight = c.blocksPerCol * 8;
    c.samples.assign(static_cast<size_t>(c.planeWidth) * c.planeHeight, 128);
  }

  const uint32_t w = image.width, h = image.height;
  const int nIn = image.components;
  parallelFor(h, options.threads, [&](size_t yy) {
    const uint32_t y = static_cast<uint32_t>(yy);
    const uint8_t* src = image.pixels + static_cast<size_t>(y) * w * nIn;
    if (!color) {
      uint8_t* dst = comps[0].samples.data() +
                     static_cast<size_t>(y) * comps[0].planeWidth;
      std::memcpy(dst, src, w);
      for (uint32_t x = w; x < comps[0].planeWidth; ++x) dst[x] = dst[w - 1];
      return;
    }
    uint8_t* yPlane =
        comps[0].samples.data() + static_cast<size_t>(y) * comps[0].planeWidth;
    for (uint32_t x = 0; x < w; ++x) {
      const int r = src[x * 3 + 0], g = src[x * 3 + 1], b = src[x * 3 + 2];
      // JFIF (BT.601) full-range YCbCr, fixed point 16.16.
      int yv = (19595 * r + 38470 * g + 7471 * b + 32768) >> 16;
      yPlane[x] = static_cast<uint8_t>(std::min(255, std::max(0, yv)));
    }
    for (uint32_t x = w; x < comps[0].planeWidth; ++x) yPlane[x] = yPlane[w - 1];
  });

  if (color) {
    const uint32_t cw = comps[1].planeWidth;
    const uint32_t chromaRows = subsample ? (h + 1) / 2 : h;
    parallelFor(chromaRows, options.threads, [&](size_t ry) {
      const uint32_t y0 = static_cast<uint32_t>(ry) * (subsample ? 2u : 1u);
      uint8_t* cbRow = comps[1].samples.data() + static_cast<size_t>(ry) * cw;
      uint8_t* crRow = comps[2].samples.data() + static_cast<size_t>(ry) * cw;
      const uint32_t outCols = subsample ? (w + 1) / 2 : w;
      for (uint32_t cx = 0; cx < outCols; ++cx) {
        int sumR = 0, sumG = 0, sumB = 0, n = 0;
        const uint32_t xSpan = subsample ? 2u : 1u;
        for (uint32_t dy = 0; dy < xSpan; ++dy) {
          uint32_t sy = y0 + dy;
          if (sy >= h) break;
          const uint8_t* src = image.pixels + static_cast<size_t>(sy) * w * 3;
          for (uint32_t dx = 0; dx < xSpan; ++dx) {
            uint32_t sx = cx * xSpan + dx;
            if (sx >= w) break;
            sumR += src[sx * 3 + 0];
            sumG += src[sx * 3 + 1];
            sumB += src[sx * 3 + 2];
            ++n;
          }
        }
        if (n == 0) n = 1;
        int r = sumR / n, g = sumG / n, b = sumB / n;
        int cb = ((-11059 * r - 21709 * g + 32768 * b + 8388608) >> 16);
        int cr = ((32768 * r - 27439 * g - 5329 * b + 8388608) >> 16);
        cbRow[cx] = static_cast<uint8_t>(std::min(255, std::max(0, cb)));
        crRow[cx] = static_cast<uint8_t>(std::min(255, std::max(0, cr)));
      }
      for (uint32_t x = outCols; x < cw; ++x) {
        cbRow[x] = cbRow[outCols - 1];
        crRow[x] = crRow[outCols - 1];
      }
    });
    // Replicate the last valid chroma row into the padding rows.
    for (int c = 1; c < 3; ++c) {
      const uint32_t validRows = subsample ? (h + 1) / 2 : h;
      for (uint32_t y = validRows; y < comps[c].planeHeight; ++y) {
        std::memcpy(comps[c].samples.data() + static_cast<size_t>(y) * cw,
                    comps[c].samples.data() +
                        static_cast<size_t>(validRows - 1) * cw,
                    cw);
      }
    }
  }
  // Replicate the last valid luma row into the padding rows.
  for (uint32_t y = h; y < comps[0].planeHeight; ++y) {
    std::memcpy(comps[0].samples.data() +
                    static_cast<size_t>(y) * comps[0].planeWidth,
                comps[0].samples.data() +
                    static_cast<size_t>(h - 1) * comps[0].planeWidth,
                comps[0].planeWidth);
  }

  // ---- quantisation tables and DCT ---------------------------------------
  uint16_t quant[2][64];
  buildQuantTable(options.quality, kQuantLuma, quant[0]);
  buildQuantTable(options.quality, kQuantChroma, quant[1]);

  double divisors[2][64];
  for (int t = 0; t < 2; ++t)
    for (int row = 0; row < 8; ++row)
      for (int col = 0; col < 8; ++col)
        divisors[t][row * 8 + col] =
            1.0 / (quant[t][row * 8 + col] * kAanScale[row] * kAanScale[col] * 8.0);

  for (auto& c : comps)
    c.coefs.assign(static_cast<size_t>(c.blocksPerLine) * c.blocksPerCol * 64, 0);

  for (auto& c : comps) {
    const double* div = divisors[c.quantTable];
    parallelFor(c.blocksPerCol, options.threads, [&](size_t by) {
      float block[64];
      for (uint32_t bx = 0; bx < c.blocksPerLine; ++bx) {
        const uint8_t* base = c.samples.data() +
                              (by * 8) * static_cast<size_t>(c.planeWidth) +
                              bx * 8;
        for (int r = 0; r < 8; ++r)
          for (int col = 0; col < 8; ++col)
            block[r * 8 + col] =
                static_cast<float>(base[r * c.planeWidth + col]) - 128.0f;
        forwardDct(block);
        int16_t* out = c.coefs.data() +
                       (by * c.blocksPerLine + bx) * 64;
        for (int i = 0; i < 64; ++i) {
          double v = block[kZigZag[i]] * div[kZigZag[i]];
          int q = static_cast<int>(std::nearbyint(v));
          out[i] = static_cast<int16_t>(std::min(32767, std::max(-32768, q)));
        }
      }
    });
    c.samples.clear();
    c.samples.shrink_to_fit();
  }

  // ---- Huffman tables -----------------------------------------------------
  HuffTable dcTables[2], acTables[2];
  const int numTables = color ? 2 : 1;
  if (options.optimizeHuffman) {
    std::vector<std::vector<uint32_t>> dcFreq(2, std::vector<uint32_t>(256, 0));
    std::vector<std::vector<uint32_t>> acFreq(2, std::vector<uint32_t>(256, 0));
    // Statistics pass: identical traversal to the encoding pass below.
    std::vector<int> pred(numComponents, 0);
    for (uint32_t my = 0; my < mcusY; ++my) {
      for (uint32_t mx = 0; mx < mcusX; ++mx) {
        for (int ci = 0; ci < numComponents; ++ci) {
          Component& c = comps[ci];
          for (int by = 0; by < c.v; ++by) {
            for (int bx = 0; bx < c.h; ++bx) {
              uint32_t blockY = my * c.v + by;
              uint32_t blockX = mx * c.h + bx;
              const int16_t* blk =
                  c.coefs.data() +
                  (static_cast<size_t>(blockY) * c.blocksPerLine + blockX) * 64;
              int diff = blk[0] - pred[ci];
              pred[ci] = blk[0];
              ++dcFreq[c.dcTable][magnitudeCategory(diff)];
              int run = 0;
              for (int k = 1; k < 64; ++k) {
                if (blk[k] == 0) {
                  ++run;
                  continue;
                }
                while (run > 15) {
                  ++acFreq[c.acTable][0xf0];
                  run -= 16;
                }
                ++acFreq[c.acTable][(run << 4) | magnitudeCategory(blk[k])];
                run = 0;
              }
              if (run > 0) ++acFreq[c.acTable][0x00];
            }
          }
        }
      }
    }
    for (int t = 0; t < numTables; ++t) {
      generateOptimalTable(dcFreq[t], &dcTables[t]);
      generateOptimalTable(acFreq[t], &acTables[t]);
    }
  } else {
    dcTables[0].setStandard(kStdDcLumaBits, kStdDcLumaVal, 12);
    acTables[0].setStandard(kStdAcLumaBits, kStdAcLumaVal, 162);
    if (color) {
      dcTables[1].setStandard(kStdDcChromaBits, kStdDcChromaVal, 12);
      acTables[1].setStandard(kStdAcChromaBits, kStdAcChromaVal, 162);
    }
  }

  // ---- file assembly ------------------------------------------------------
  Bytes out;
  out.reserve(static_cast<size_t>(w) * h / 4 + 65536);
  out.push_back(0xff);
  out.push_back(0xd8);  // SOI
  for (const auto& seg : options.appSegments)
    putBytes(out, seg.data(), seg.size());

  {  // DQT
    Bytes payload;
    for (int t = 0; t < numTables; ++t) {
      payload.push_back(static_cast<uint8_t>(t));  // 8-bit precision, table id
      for (int i = 0; i < 64; ++i)
        payload.push_back(static_cast<uint8_t>(quant[t][kZigZag[i]]));
    }
    Bytes seg = makeSegment(0xdb, payload);
    putBytes(out, seg.data(), seg.size());
  }

  {  // SOF0
    Bytes payload;
    payload.push_back(8);  // sample precision
    putU16BE(payload, static_cast<uint16_t>(h));
    putU16BE(payload, static_cast<uint16_t>(w));
    payload.push_back(static_cast<uint8_t>(numComponents));
    for (const auto& c : comps) {
      payload.push_back(static_cast<uint8_t>(c.id));
      payload.push_back(static_cast<uint8_t>((c.h << 4) | c.v));
      payload.push_back(static_cast<uint8_t>(c.quantTable));
    }
    Bytes seg = makeSegment(0xc0, payload);
    putBytes(out, seg.data(), seg.size());
  }

  {  // DHT
    Bytes payload;
    auto emit = [&](int cls, int id, const HuffTable& t) {
      payload.push_back(static_cast<uint8_t>((cls << 4) | id));
      for (int i = 1; i <= 16; ++i) payload.push_back(t.bits[i]);
      for (uint8_t v : t.values) payload.push_back(v);
    };
    for (int t = 0; t < numTables; ++t) {
      emit(0, t, dcTables[t]);
      emit(1, t, acTables[t]);
    }
    Bytes seg = makeSegment(0xc4, payload);
    putBytes(out, seg.data(), seg.size());
  }

  {  // SOS
    Bytes payload;
    payload.push_back(static_cast<uint8_t>(numComponents));
    for (const auto& c : comps) {
      payload.push_back(static_cast<uint8_t>(c.id));
      payload.push_back(static_cast<uint8_t>((c.dcTable << 4) | c.acTable));
    }
    payload.push_back(0);   // Ss
    payload.push_back(63);  // Se
    payload.push_back(0);   // Ah/Al
    Bytes seg = makeSegment(0xda, payload);
    putBytes(out, seg.data(), seg.size());
  }

  {  // Entropy-coded scan
    BitWriter bw(out);
    std::vector<int> pred(numComponents, 0);
    auto encodeValue = [&](int value, int category) {
      if (category == 0) return;
      int v = value;
      if (v < 0) v += (1 << category) - 1;  // one's complement for negatives
      bw.put(static_cast<uint16_t>(v & ((1 << category) - 1)), category);
    };
    for (uint32_t my = 0; my < mcusY; ++my) {
      for (uint32_t mx = 0; mx < mcusX; ++mx) {
        for (int ci = 0; ci < numComponents; ++ci) {
          Component& c = comps[ci];
          const HuffTable& dc = dcTables[c.dcTable];
          const HuffTable& ac = acTables[c.acTable];
          for (int by = 0; by < c.v; ++by) {
            for (int bx = 0; bx < c.h; ++bx) {
              uint32_t blockY = my * c.v + by;
              uint32_t blockX = mx * c.h + bx;
              const int16_t* blk =
                  c.coefs.data() +
                  (static_cast<size_t>(blockY) * c.blocksPerLine + blockX) * 64;
              int diff = blk[0] - pred[ci];
              pred[ci] = blk[0];
              int cat = magnitudeCategory(diff);
              if (dc.size[cat] == 0) fail("internal error: missing DC code");
              bw.put(dc.code[cat], dc.size[cat]);
              encodeValue(diff, cat);
              int run = 0;
              for (int k = 1; k < 64; ++k) {
                if (blk[k] == 0) {
                  ++run;
                  continue;
                }
                while (run > 15) {
                  if (ac.size[0xf0] == 0) fail("internal error: missing ZRL code");
                  bw.put(ac.code[0xf0], ac.size[0xf0]);
                  run -= 16;
                }
                int acCat = magnitudeCategory(blk[k]);
                int sym = (run << 4) | acCat;
                if (ac.size[sym] == 0) fail("internal error: missing AC code");
                bw.put(ac.code[sym], ac.size[sym]);
                encodeValue(blk[k], acCat);
                run = 0;
              }
              if (run > 0) {
                if (ac.size[0x00] == 0) fail("internal error: missing EOB code");
                bw.put(ac.code[0x00], ac.size[0x00]);
              }
            }
          }
        }
      }
    }
    bw.alignToByte();
  }

  out.push_back(0xff);
  out.push_back(0xd9);  // EOI
  return out;
}

}  // namespace iso21496
