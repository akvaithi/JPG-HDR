// Self-contained baseline (ITU-T T.81 sequential DCT, Huffman) JPEG encoder.
//
// Deliberately not libjpeg: the plugin ships a single static binary per
// platform, and the feature set we need is small — 8-bit, 1 or 3 components,
// 4:4:4 or 4:2:0, optimised Huffman tables, and arbitrary APPn segments that
// the caller pre-builds (ICC, MPF, Exif, XMP, ISO 21496-1).
#pragma once

#include <vector>

#include "common.h"

namespace iso21496 {

struct JpegImage {
  uint32_t width = 0;
  uint32_t height = 0;
  int components = 3;        // 1 (greyscale) or 3 (RGB input, YCbCr stored)
  const uint8_t* pixels = nullptr;  // interleaved, stride = width*components
};

struct JpegOptions {
  int quality = 90;             // 1..100
  bool chromaSubsample = true;  // 4:2:0 when true and components == 3
  bool optimizeHuffman = true;  // two-pass optimal tables (smaller files)
  unsigned threads = 0;         // 0 = hardware concurrency
  // Complete APPn segments (marker, length, payload) written directly after
  // SOI in the order given.
  std::vector<Bytes> appSegments;
};

Bytes encodeJpeg(const JpegImage& image, const JpegOptions& options);

// Builds the APP2 ICC_PROFILE segment chain for a profile of any size.
std::vector<Bytes> buildIccAppSegments(const Bytes& profile);
// Builds an APP1 segment holding an XMP packet.
Bytes buildXmpAppSegment(const std::string& xmp);
// Builds the JFIF APP0 segment.
Bytes buildJfifAppSegment();

}  // namespace iso21496
