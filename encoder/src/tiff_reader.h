// Minimal, dependency-free baseline TIFF reader for the intermediate file that
// Lightroom Classic renders. Supports exactly the shapes Lightroom can produce
// (and a bit more, so hand-made test files work):
//
//   * little- and big-endian classic TIFF (no BigTIFF)
//   * strips and tiles
//   * 8 / 16 bit unsigned integer and 32 bit IEEE float samples
//   * 1 (grey) / 3 (RGB) / 4 (RGB + alpha or extra) samples per pixel
//   * planar configuration 1 (chunky) and 2 (planar)
//   * compression: none (1), LZW (5), PackBits (32773)
//   * horizontal differencing predictor (2) and floating-point predictor (3)
//
// Deflate/ZIP (8/32946) and JPEG-in-TIFF are rejected with an explicit message
// rather than silently producing garbage; the plugin always asks Lightroom for
// uncompressed output.
#pragma once

#include <string>
#include <vector>

#include "common.h"

namespace iso21496 {

// Raw byte ranges lifted out of the TIFF so the encoder can carry colour and
// capture metadata through to the JPEG.
struct TiffMetadata {
  Bytes iccProfile;             // tag 34675
  Bytes xmpPacket;              // tag 700
  uint16_t orientation = 1;     // tag 274
  bool hasExif = false;         // tag 34665 present
  uint32_t exifIfdOffset = 0;   // absolute offset of the Exif IFD
  bool hasGps = false;          // tag 34853 present
  uint32_t gpsIfdOffset = 0;
  uint32_t ifd0Offset = 0;
};

class TiffReader {
 public:
  // Takes ownership of the whole file image; the reader is a view over it.
  explicit TiffReader(Bytes fileData);

  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  uint32_t channels() const { return colorChannels_; }  // 1 or 3, alpha dropped
  uint16_t bitsPerSample() const { return bitsPerSample_; }
  bool isFloat() const { return sampleFormat_ == 3; }
  bool littleEndian() const { return littleEndian_; }
  const TiffMetadata& metadata() const { return meta_; }
  const Bytes& fileData() const { return data_; }

  // Decodes rows [firstRow, firstRow + rowCount) into `dst`, which must hold
  // rowCount * width() * channels() floats. Values are normalised to [0,1] for
  // integer samples and passed through unchanged for float samples. Thread
  // safe: concurrent calls for disjoint row ranges are fine.
  void readRows(uint32_t firstRow, uint32_t rowCount, float* dst) const;

  // Natural decode granularity (strip height / tile height). Reading in
  // multiples of this avoids decompressing the same segment twice.
  uint32_t suggestedBandRows() const {
    return tiled_ ? tileHeight_ : rowsPerStrip_;
  }

 private:
  struct Entry {
    uint16_t tag = 0;
    uint16_t type = 0;
    uint32_t count = 0;
    uint32_t valueOffset = 0;  // raw field, may be inline data
  };

  uint16_t rdU16(size_t off) const;
  uint32_t rdU32(size_t off) const;
  void requireRange(size_t off, size_t len, const char* what) const;
  std::vector<uint32_t> readValues(const Entry& e) const;
  uint32_t readValue(const Entry& e, uint32_t index = 0) const;
  void parseIfd0();
  // Decompresses one strip/tile into `out` (which is resized to expectedBytes).
  void decodeSegment(uint32_t index, size_t expectedBytes, Bytes& out) const;
  void unpredict(Bytes& buf, uint32_t pixelsPerRow, uint32_t rows,
                 uint32_t samplesPerPixel) const;
  // Scatters one decoded segment's samples into the float destination.
  void expandSegment(const Bytes& seg, uint32_t segFirstRow, uint32_t segRows,
                     uint32_t segFirstCol, uint32_t segCols, uint32_t plane,
                     uint32_t firstRow, uint32_t rowCount, float* dst) const;

  Bytes data_;
  bool littleEndian_ = true;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint16_t samplesPerPixel_ = 0;
  uint16_t colorChannels_ = 0;
  uint16_t bitsPerSample_ = 0;
  uint16_t sampleFormat_ = 1;  // 1 = uint, 3 = float
  uint16_t compression_ = 1;
  uint16_t photometric_ = 2;
  uint16_t planarConfig_ = 1;
  uint16_t predictor_ = 1;
  bool tiled_ = false;
  uint32_t rowsPerStrip_ = 0;
  uint32_t tileWidth_ = 0;
  uint32_t tileHeight_ = 0;
  uint32_t tilesAcross_ = 0;
  uint32_t tilesDown_ = 0;
  std::vector<uint32_t> segOffsets_;
  std::vector<uint32_t> segByteCounts_;
  TiffMetadata meta_;
};

}  // namespace iso21496
