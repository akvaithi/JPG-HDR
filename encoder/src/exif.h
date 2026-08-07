// Rebuilds an Exif APP1 segment from the IFDs Lightroom wrote into the
// intermediate TIFF, so capture data, GPS and copyright survive the export.
#pragma once

#include "common.h"
#include "tiff_reader.h"

namespace iso21496 {

struct ExifOptions {
  uint32_t pixelWidth = 0;
  uint32_t pixelHeight = 0;
  // Exif ColorSpace tag: 1 = sRGB, 0xFFFF = uncalibrated (anything else; the
  // embedded ICC profile is then authoritative).
  uint16_t colorSpace = 0xFFFF;
};

// Returns an empty vector when there is nothing worth carrying over, or when
// the result would not fit in a single APP1 segment.
Bytes buildExifAppSegment(const TiffReader& tiff, const ExifOptions& options);

}  // namespace iso21496
