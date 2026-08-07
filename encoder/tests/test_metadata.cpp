// ISO 21496-1 payload layout, MPF index patching and Exif reconstruction.
#include <cstring>

#include "exif.h"
#include "iso_metadata.h"
#include "test_support.h"

using namespace iso21496;
using namespace iso21496::test;

namespace {

void isoPayloadLayout() {
  GainMapMetadata m;
  m.multiChannel = false;
  m.alternateHeadroom = 4.0f;
  m.maxBoost[0] = 3.5f;
  m.gamma[0] = 2.2f;
  m.baseOffset[0] = 0.015625f;
  m.alternateOffset[0] = 0.015625f;

  Bytes p = buildIsoGainMapPayload(m);
  CHECK_EQ(std::memcmp(p.data(), kIsoGainMapUrn, 27), 0);
  CHECK_EQ(p[27], uint8_t{0});
  // 28 URN + 2 + 2 + 1 + 16 headroom + 40 per channel
  CHECK_EQ(p.size(), size_t{28 + 2 + 2 + 1 + 16 + 40});

  const uint8_t* q = p.data() + 28;
  CHECK_EQ(readU16BE(q), uint16_t{0});      // minimum version
  CHECK_EQ(readU16BE(q + 2), uint16_t{1});  // writer version
  CHECK_EQ(q[4], uint8_t{0x40});            // single channel, base colour space

  const uint8_t* h = q + 5;
  CHECK_EQ(readU32BE(h), 0u);                        // base headroom numerator
  CHECK_EQ(readU32BE(h + 4), 1000000u);              // denominator
  CHECK_EQ(readU32BE(h + 8), 4000000u);              // alternate headroom
  CHECK_EQ(readU32BE(h + 12), 1000000u);

  const uint8_t* c = h + 16;
  CHECK_EQ(static_cast<int32_t>(readU32BE(c)), 0);          // min boost
  CHECK_EQ(static_cast<int32_t>(readU32BE(c + 8)), 3500000);  // max boost
  CHECK_EQ(readU32BE(c + 16), 2200000u);                    // gamma
  CHECK_EQ(static_cast<int32_t>(readU32BE(c + 24)), 15625);   // base offset
  CHECK_EQ(static_cast<int32_t>(readU32BE(c + 32)), 15625);   // alternate offset

  m.multiChannel = true;
  Bytes p3 = buildIsoGainMapPayload(m);
  CHECK_EQ(p3.size(), size_t{28 + 2 + 2 + 1 + 16 + 120});
  CHECK_EQ(p3[28 + 4], uint8_t{0xC0});  // multichannel | base colour space

  Bytes seg = buildIsoGainMapSegment(m);
  CHECK_EQ(seg[0], uint8_t{0xff});
  CHECK_EQ(seg[1], uint8_t{0xe2});
  CHECK_EQ(readU16BE(&seg[2]), static_cast<uint16_t>(p3.size() + 2));
}

void mpfPatching() {
  Bytes mpf = buildMpfSegmentPlaceholder();
  CHECK_EQ(mpf.size(), size_t{2 + 2 + 86});

  // Fake a two-image file: a header containing the MPF segment, then bodies.
  Bytes file;
  file.push_back(0xff);
  file.push_back(0xd8);
  putBytes(file, mpf.data(), mpf.size());
  const size_t primarySize = file.size() + 1234;
  file.resize(primarySize, 0x20);
  const size_t secondarySize = 567;
  file.resize(primarySize + secondarySize, 0x30);

  patchMpfSegment(file, primarySize, secondarySize);

  // Locate the identifier the same way the patcher does and re-read the index.
  size_t id = 0;
  for (size_t i = 0; i + 4 <= file.size(); ++i)
    if (std::memcmp(&file[i], "MPF\0", 4) == 0) {
      id = i;
      break;
    }
  CHECK(id > 0);
  const size_t endian = id + 4;
  const size_t entries = id + 4 + 8 + (2 + 3 * 12 + 4);
  CHECK_EQ(readU32BE(&file[entries]), 0x030000u);
  CHECK_EQ(readU32BE(&file[entries + 4]), static_cast<uint32_t>(primarySize));
  CHECK_EQ(readU32BE(&file[entries + 8]), 0u);
  CHECK_EQ(readU32BE(&file[entries + 16 + 4]),
           static_cast<uint32_t>(secondarySize));
  CHECK_EQ(readU32BE(&file[entries + 16 + 8]),
           static_cast<uint32_t>(primarySize - endian));

  // MP Index IFD sanity: version tag then image count.
  CHECK_EQ(readU16BE(&file[endian]), uint16_t{0x4d4d});  // "MM"
  const size_t ifd = endian + 8;
  CHECK_EQ(readU16BE(&file[ifd]), uint16_t{3});
  CHECK_EQ(readU16BE(&file[ifd + 2]), uint16_t{0xb000});
  CHECK_EQ(readU16BE(&file[ifd + 2 + 12]), uint16_t{0xb001});
  CHECK_EQ(readU32BE(&file[ifd + 2 + 12 + 8]), 2u);
}

void xmpBlocks() {
  std::string primary = buildPrimaryXmp(4242);
  CHECK(primary.find("Item:Semantic=\"GainMap\"") != std::string::npos);
  CHECK(primary.find("Item:Length=\"4242\"") != std::string::npos);
  CHECK(primary.find("hdrgm:Version=\"1.0\"") != std::string::npos);

  GainMapMetadata m;
  m.maxBoost[0] = 2.5f;
  std::string gm = buildGainMapXmp(m);
  CHECK(gm.find("hdrgm:GainMapMax=\"2.5\"") != std::string::npos);
  CHECK(gm.find("hdrgm:BaseRenditionIsHDR=\"False\"") != std::string::npos);
}

void exifPassthrough() {
  TiffSpec spec;
  spec.width = 12;
  spec.height = 9;
  spec.withExif = true;
  auto pixels = makeHdrPattern(spec.width, spec.height, 1.0f);
  TiffReader tiff(makeTiff(spec, pixels));

  ExifOptions eo;
  eo.pixelWidth = 12;
  eo.pixelHeight = 9;
  eo.colorSpace = 0xFFFF;
  Bytes seg = buildExifAppSegment(tiff, eo);
  CHECK(!seg.empty());
  CHECK_EQ(seg[0], uint8_t{0xff});
  CHECK_EQ(seg[1], uint8_t{0xe1});
  CHECK_EQ(std::memcmp(&seg[4], "Exif\0\0", 6), 0);
  CHECK_EQ(std::memcmp(&seg[10], "MM", 2), 0);
  CHECK_EQ(readU16BE(&seg[12]), uint16_t{42});
  CHECK_EQ(readU32BE(&seg[14]), 8u);

  // Walk IFD0 and confirm Make survived and Orientation was normalised.
  const uint8_t* tiffBase = &seg[10];
  uint16_t count = readU16BE(tiffBase + 8);
  CHECK(count >= 3);
  bool sawMake = false, sawOrientation = false, sawExifPointer = false;
  for (uint16_t i = 0; i < count; ++i) {
    const uint8_t* e = tiffBase + 10 + i * 12;
    uint16_t tag = readU16BE(e);
    if (tag == 271) {
      sawMake = true;
      uint32_t n = readU32BE(e + 4);
      CHECK_EQ(n, 6u);
      uint32_t off = readU32BE(e + 8);
      CHECK_EQ(std::memcmp(tiffBase + off, "ACME", 4), 0);
    }
    if (tag == 274) {
      sawOrientation = true;
      CHECK_EQ(readU16BE(e + 8), uint16_t{1});
    }
    if (tag == 34665) sawExifPointer = true;
  }
  CHECK(sawMake);
  CHECK(sawOrientation);
  CHECK(sawExifPointer);  // PixelXDimension et al. live in the Exif sub-IFD

  // The Exif sub-IFD must carry the output dimensions, not the TIFF's.
  for (uint16_t i = 0; i < count; ++i) {
    const uint8_t* e = tiffBase + 10 + i * 12;
    if (readU16BE(e) != 34665) continue;
    uint32_t sub = readU32BE(e + 8);
    uint16_t n = readU16BE(tiffBase + sub);
    bool sawWidth = false;
    for (uint16_t j = 0; j < n; ++j) {
      const uint8_t* se = tiffBase + sub + 2 + j * 12;
      if (readU16BE(se) == 40962) {
        sawWidth = true;
        CHECK_EQ(readU32BE(se + 8), 12u);
      }
    }
    CHECK(sawWidth);
  }
}

void run() {
  isoPayloadLayout();
  mpfPatching();
  xmpBlocks();
  exifPassthrough();
}

}  // namespace

TEST_MAIN(run)
