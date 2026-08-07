// Structural checks on the baseline JPEG encoder.
#include <cstring>
#include <map>

#include "jpeg_encoder.h"
#include "test_support.h"

using namespace iso21496;
using namespace iso21496::test;

namespace {

std::vector<uint8_t> gradient(uint32_t w, uint32_t h, int channels) {
  std::vector<uint8_t> px(static_cast<size_t>(w) * h * channels);
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x)
      for (int c = 0; c < channels; ++c)
        px[(static_cast<size_t>(y) * w + x) * channels + c] =
            static_cast<uint8_t>((x * 7 + y * 3 + c * 40) & 0xff);
  return px;
}

void checkStructure(const Bytes& jpeg, uint32_t w, uint32_t h, int components) {
  size_t end = 0;
  auto segs = walkJpeg(jpeg, 0, &end);
  CHECK(!segs.empty());
  CHECK_EQ(end, jpeg.size());

  std::map<uint8_t, int> counts;
  for (const auto& s : segs) ++counts[s.marker];
  CHECK_EQ(counts[0xd8], 1);  // SOI
  CHECK_EQ(counts[0xd9], 1);  // EOI
  CHECK_EQ(counts[0xc0], 1);  // SOF0 (baseline)
  CHECK_EQ(counts[0xda], 1);  // SOS
  CHECK(counts[0xdb] >= 1);   // DQT
  CHECK(counts[0xc4] >= 1);   // DHT

  for (const auto& s : segs) {
    if (s.marker != 0xc0) continue;
    const uint8_t* p = &jpeg[s.payloadOffset];
    CHECK_EQ(static_cast<int>(p[0]), 8);
    CHECK_EQ(readU16BE(p + 1), static_cast<uint16_t>(h));
    CHECK_EQ(readU16BE(p + 3), static_cast<uint16_t>(w));
    CHECK_EQ(static_cast<int>(p[5]), components);
  }

  // The entropy-coded stream must contain no unescaped 0xFF bytes.
  size_t sosEnd = 0;
  for (const auto& s : segs)
    if (s.marker == 0xda) sosEnd = s.payloadOffset + s.payloadSize;
  CHECK(sosEnd > 0);
  for (size_t i = sosEnd; i + 1 < jpeg.size() - 2; ++i) {
    if (jpeg[i] == 0xff) {
      bool ok = jpeg[i + 1] == 0x00 ||
                (jpeg[i + 1] >= 0xd0 && jpeg[i + 1] <= 0xd7) ||
                jpeg[i + 1] == 0xd9;
      CHECK(ok);
    }
  }
}

void encodesColorAndGrey() {
  for (bool optimize : {false, true}) {
    for (bool subsample : {false, true}) {
      JpegOptions o;
      o.quality = 90;
      o.optimizeHuffman = optimize;
      o.chromaSubsample = subsample;
      auto px = gradient(53, 41, 3);
      JpegImage img{53, 41, 3, px.data()};
      Bytes out = encodeJpeg(img, o);
      checkStructure(out, 53, 41, 3);
    }
  }

  JpegOptions o;
  o.quality = 85;
  auto px = gradient(64, 64, 1);
  JpegImage img{64, 64, 1, px.data()};
  Bytes out = encodeJpeg(img, o);
  checkStructure(out, 64, 64, 1);
}

void qualityAffectsSize() {
  auto px = gradient(128, 128, 3);
  JpegImage img{128, 128, 3, px.data()};
  JpegOptions low, high;
  low.quality = 60;
  high.quality = 98;
  Bytes a = encodeJpeg(img, low);
  Bytes b = encodeJpeg(img, high);
  CHECK(a.size() < b.size());
}

void optimizedTablesAreSmaller() {
  auto px = gradient(160, 120, 3);
  JpegImage img{160, 120, 3, px.data()};
  JpegOptions plain, optimized;
  plain.optimizeHuffman = false;
  optimized.optimizeHuffman = true;
  CHECK(encodeJpeg(img, optimized).size() <= encodeJpeg(img, plain).size());
}

void appSegmentsAreEmitted() {
  auto px = gradient(16, 16, 3);
  JpegImage img{16, 16, 3, px.data()};
  JpegOptions o;
  o.appSegments.push_back(buildJfifAppSegment());
  o.appSegments.push_back(buildXmpAppSegment("<x:xmpmeta/>"));
  Bytes profile(700, 0x5a);
  for (auto& s : buildIccAppSegments(profile)) o.appSegments.push_back(s);
  Bytes out = encodeJpeg(img, o);

  size_t end = 0;
  auto segs = walkJpeg(out, 0, &end);
  int app0 = 0, app1 = 0, app2 = 0;
  for (const auto& s : segs) {
    if (s.marker == 0xe0) ++app0;
    if (s.marker == 0xe1) ++app1;
    if (s.marker == 0xe2) ++app2;
  }
  CHECK_EQ(app0, 1);
  CHECK_EQ(app1, 1);
  CHECK_EQ(app2, 1);

  // A profile larger than one segment must be split into a numbered chain.
  Bytes big(140000, 0x11);
  auto chunks = buildIccAppSegments(big);
  CHECK_EQ(chunks.size(), size_t{3});
  CHECK_EQ(chunks[1][2 + 2 + 12], uint8_t{2});      // chunk number
  CHECK_EQ(chunks[1][2 + 2 + 13], uint8_t{3});      // chunk count
}

void rejectsBadInput() {
  auto px = gradient(8, 8, 3);
  JpegImage img{8, 8, 3, px.data()};
  JpegOptions o;
  JpegImage empty{0, 8, 3, px.data()};
  CHECK_THROWS(encodeJpeg(empty, o));
  JpegImage twoChannel{8, 8, 2, px.data()};
  CHECK_THROWS(encodeJpeg(twoChannel, o));
  JpegImage noPixels{8, 8, 3, nullptr};
  CHECK_THROWS(encodeJpeg(noPixels, o));
}

void run() {
  encodesColorAndGrey();
  qualityAffectsSize();
  optimizedTablesAreSmaller();
  appSegmentsAreEmitted();
  rejectsBadInput();
}

}  // namespace

TEST_MAIN(run)
