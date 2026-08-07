#include "tiff_reader.h"

#include <cstring>
#include <limits>

namespace iso21496 {
namespace {

constexpr uint16_t kTagImageWidth = 256;
constexpr uint16_t kTagImageLength = 257;
constexpr uint16_t kTagBitsPerSample = 258;
constexpr uint16_t kTagCompression = 259;
constexpr uint16_t kTagPhotometric = 262;
constexpr uint16_t kTagStripOffsets = 273;
constexpr uint16_t kTagOrientation = 274;
constexpr uint16_t kTagSamplesPerPixel = 277;
constexpr uint16_t kTagRowsPerStrip = 278;
constexpr uint16_t kTagStripByteCounts = 279;
constexpr uint16_t kTagPlanarConfig = 284;
constexpr uint16_t kTagPredictor = 317;
constexpr uint16_t kTagTileWidth = 322;
constexpr uint16_t kTagTileLength = 323;
constexpr uint16_t kTagTileOffsets = 324;
constexpr uint16_t kTagTileByteCounts = 325;
constexpr uint16_t kTagSampleFormat = 339;
constexpr uint16_t kTagXmp = 700;
constexpr uint16_t kTagExifIfd = 34665;
constexpr uint16_t kTagIccProfile = 34675;
constexpr uint16_t kTagGpsIfd = 34853;

size_t typeSize(uint16_t type) {
  switch (type) {
    case 1:   // BYTE
    case 2:   // ASCII
    case 6:   // SBYTE
    case 7:   // UNDEFINED
      return 1;
    case 3:   // SHORT
    case 8:   // SSHORT
      return 2;
    case 4:   // LONG
    case 9:   // SLONG
    case 11:  // FLOAT
      return 4;
    case 5:   // RATIONAL
    case 10:  // SRATIONAL
    case 12:  // DOUBLE
      return 8;
    default:
      return 0;
  }
}

// TIFF LZW (Rev 6.0) with the customary "early change" of the code width.
void decodeLzw(const uint8_t* src, size_t srcLen, Bytes& out, size_t expected) {
  constexpr int kClear = 256;
  constexpr int kEoi = 257;
  // Dictionary entries are (prefix, suffix) pairs; 4096 max codes.
  std::vector<int32_t> prefix(4096, -1);
  std::vector<uint8_t> suffix(4096, 0);
  std::vector<uint8_t> stack(4096);

  out.clear();
  out.reserve(expected);

  int nextCode = 258;
  int codeWidth = 9;
  int32_t oldCode = -1;
  uint32_t bitBuf = 0;
  int bitCount = 0;
  size_t pos = 0;

  auto readCode = [&](int& code) -> bool {
    while (bitCount < codeWidth) {
      if (pos >= srcLen) return false;
      bitBuf = (bitBuf << 8) | src[pos++];
      bitCount += 8;
    }
    bitCount -= codeWidth;
    code = static_cast<int>((bitBuf >> bitCount) & ((1u << codeWidth) - 1u));
    return true;
  };

  int code = 0;
  while (readCode(code)) {
    if (code == kEoi) break;
    if (code == kClear) {
      nextCode = 258;
      codeWidth = 9;
      oldCode = -1;
      continue;
    }
    if (oldCode == -1) {
      if (code >= 256) fail("corrupt LZW stream in TIFF (bad first code)");
      out.push_back(static_cast<uint8_t>(code));
      oldCode = code;
      continue;
    }
    int inCode = code;
    size_t sp = 0;
    if (code >= nextCode) {
      // KwKwK case: emit the previous string plus its own first byte.
      if (oldCode < 0 || oldCode >= 4096) fail("corrupt LZW stream in TIFF");
      int c = oldCode;
      // Walk to the root, whose code is literally its first character.
      while (prefix[c] >= 0) c = prefix[c];
      stack[sp++] = static_cast<uint8_t>(c);
      code = oldCode;
    }
    while (code >= 256) {
      if (sp >= stack.size() || code >= 4096) fail("corrupt LZW stream in TIFF");
      stack[sp++] = suffix[code];
      code = prefix[code];
    }
    stack[sp++] = static_cast<uint8_t>(code);
    uint8_t firstByte = static_cast<uint8_t>(code);
    while (sp > 0) out.push_back(stack[--sp]);

    if (nextCode < 4096) {
      prefix[nextCode] = oldCode;
      suffix[nextCode] = firstByte;
      ++nextCode;
    }
    oldCode = inCode;
    // Early change: widen one code before the dictionary is actually full.
    if (nextCode + 1 >= (1 << codeWidth) && codeWidth < 12) ++codeWidth;
    if (out.size() > expected + 4096) break;  // runaway guard
  }
  out.resize(expected, 0);
}

void decodePackBits(const uint8_t* src, size_t srcLen, Bytes& out, size_t expected) {
  out.clear();
  out.reserve(expected);
  size_t i = 0;
  while (i < srcLen && out.size() < expected) {
    int8_t n = static_cast<int8_t>(src[i++]);
    if (n >= 0) {
      size_t run = static_cast<size_t>(n) + 1;
      if (i + run > srcLen) run = srcLen - i;
      out.insert(out.end(), src + i, src + i + run);
      i += run;
    } else if (n != -128) {
      if (i >= srcLen) break;
      out.insert(out.end(), static_cast<size_t>(-n) + 1, src[i]);
      ++i;
    }
  }
  out.resize(expected, 0);
}

}  // namespace

TiffReader::TiffReader(Bytes fileData) : data_(std::move(fileData)) {
  if (data_.size() < 8) fail("input is not a TIFF file (too small)");
  if (data_[0] == 'I' && data_[1] == 'I') {
    littleEndian_ = true;
  } else if (data_[0] == 'M' && data_[1] == 'M') {
    littleEndian_ = false;
  } else {
    fail("input is not a TIFF file (bad byte order mark)");
  }
  uint16_t magic = rdU16(2);
  if (magic == 43) fail("BigTIFF is not supported; export a classic TIFF");
  if (magic != 42) fail("input is not a TIFF file (bad magic)");
  parseIfd0();
}

uint16_t TiffReader::rdU16(size_t off) const {
  requireRange(off, 2, "TIFF header");
  return littleEndian_ ? readU16LE(&data_[off]) : readU16BE(&data_[off]);
}

uint32_t TiffReader::rdU32(size_t off) const {
  requireRange(off, 4, "TIFF header");
  return littleEndian_ ? readU32LE(&data_[off]) : readU32BE(&data_[off]);
}

void TiffReader::requireRange(size_t off, size_t len, const char* what) const {
  if (off > data_.size() || len > data_.size() - off)
    fail(std::string("truncated TIFF: out-of-range read in ") + what);
}

std::vector<uint32_t> TiffReader::readValues(const Entry& e) const {
  size_t esize = typeSize(e.type);
  if (esize == 0) fail("unsupported TIFF field type");
  std::vector<uint32_t> out;
  out.reserve(e.count);
  size_t total = esize * static_cast<size_t>(e.count);
  size_t base;
  uint8_t inlineBuf[4];
  const uint8_t* src;
  if (total <= 4) {
    // Value is stored inline in the offset field, in file byte order.
    if (littleEndian_) {
      writeU32BE(inlineBuf, 0);
      inlineBuf[0] = static_cast<uint8_t>(e.valueOffset);
      inlineBuf[1] = static_cast<uint8_t>(e.valueOffset >> 8);
      inlineBuf[2] = static_cast<uint8_t>(e.valueOffset >> 16);
      inlineBuf[3] = static_cast<uint8_t>(e.valueOffset >> 24);
    } else {
      writeU32BE(inlineBuf, e.valueOffset);
    }
    src = inlineBuf;
    base = 0;
  } else {
    requireRange(e.valueOffset, total, "TIFF field value");
    src = data_.data();
    base = e.valueOffset;
  }
  for (uint32_t i = 0; i < e.count; ++i) {
    const uint8_t* p = src + base + i * esize;
    switch (e.type) {
      case 1:
      case 2:
      case 6:
      case 7:
        out.push_back(*p);
        break;
      case 3:
      case 8:
        out.push_back(littleEndian_ ? readU16LE(p) : readU16BE(p));
        break;
      default:
        out.push_back(littleEndian_ ? readU32LE(p) : readU32BE(p));
        break;
    }
  }
  return out;
}

uint32_t TiffReader::readValue(const Entry& e, uint32_t index) const {
  auto v = readValues(e);
  if (index >= v.size()) fail("TIFF field has fewer values than expected");
  return v[index];
}

void TiffReader::parseIfd0() {
  uint32_t ifdOff = rdU32(4);
  meta_.ifd0Offset = ifdOff;
  uint16_t count = rdU16(ifdOff);
  requireRange(ifdOff + 2, static_cast<size_t>(count) * 12 + 4, "IFD0");

  std::vector<uint16_t> bitsPerSampleList;
  bool haveStrip = false, haveTile = false;

  for (uint16_t i = 0; i < count; ++i) {
    size_t off = ifdOff + 2 + static_cast<size_t>(i) * 12;
    Entry e;
    e.tag = rdU16(off);
    e.type = rdU16(off + 2);
    e.count = rdU32(off + 4);
    e.valueOffset = rdU32(off + 8);

    switch (e.tag) {
      case kTagImageWidth: width_ = readValue(e); break;
      case kTagImageLength: height_ = readValue(e); break;
      case kTagBitsPerSample: {
        auto v = readValues(e);
        for (uint32_t x : v) bitsPerSampleList.push_back(static_cast<uint16_t>(x));
        break;
      }
      case kTagCompression: compression_ = static_cast<uint16_t>(readValue(e)); break;
      case kTagPhotometric: photometric_ = static_cast<uint16_t>(readValue(e)); break;
      case kTagOrientation: meta_.orientation = static_cast<uint16_t>(readValue(e)); break;
      case kTagSamplesPerPixel: samplesPerPixel_ = static_cast<uint16_t>(readValue(e)); break;
      case kTagRowsPerStrip: rowsPerStrip_ = readValue(e); break;
      case kTagPlanarConfig: planarConfig_ = static_cast<uint16_t>(readValue(e)); break;
      case kTagPredictor: predictor_ = static_cast<uint16_t>(readValue(e)); break;
      case kTagSampleFormat: sampleFormat_ = static_cast<uint16_t>(readValue(e)); break;
      case kTagTileWidth: tileWidth_ = readValue(e); break;
      case kTagTileLength: tileHeight_ = readValue(e); break;
      case kTagStripOffsets: segOffsets_ = readValues(e); haveStrip = true; break;
      case kTagStripByteCounts: segByteCounts_ = readValues(e); break;
      case kTagTileOffsets: segOffsets_ = readValues(e); haveTile = true; break;
      case kTagTileByteCounts: segByteCounts_ = readValues(e); break;
      case kTagIccProfile: {
        requireRange(e.valueOffset, e.count, "ICC profile");
        meta_.iccProfile.assign(data_.begin() + e.valueOffset,
                                data_.begin() + e.valueOffset + e.count);
        break;
      }
      case kTagXmp: {
        if (e.count > 4) {
          requireRange(e.valueOffset, e.count, "XMP packet");
          meta_.xmpPacket.assign(data_.begin() + e.valueOffset,
                                 data_.begin() + e.valueOffset + e.count);
        }
        break;
      }
      case kTagExifIfd:
        meta_.hasExif = true;
        meta_.exifIfdOffset = readValue(e);
        break;
      case kTagGpsIfd:
        meta_.hasGps = true;
        meta_.gpsIfdOffset = readValue(e);
        break;
      default:
        break;
    }
  }

  if (width_ == 0 || height_ == 0) fail("TIFF has no image dimensions");
  if (samplesPerPixel_ == 0) samplesPerPixel_ = bitsPerSampleList.empty() ? 1
                             : static_cast<uint16_t>(bitsPerSampleList.size());
  if (bitsPerSampleList.empty()) bitsPerSampleList.assign(samplesPerPixel_, 1);
  bitsPerSample_ = bitsPerSampleList[0];
  for (uint16_t b : bitsPerSampleList)
    if (b != bitsPerSample_) fail("TIFF with mixed sample widths is not supported");
  if (bitsPerSample_ != 8 && bitsPerSample_ != 16 && bitsPerSample_ != 32)
    fail("unsupported TIFF bit depth: " + std::to_string(bitsPerSample_) +
         " (need 8, 16 or 32)");
  if (sampleFormat_ == 3 && bitsPerSample_ != 32)
    fail("floating point TIFF must be 32 bits per sample");
  if (sampleFormat_ != 1 && sampleFormat_ != 3)
    fail("unsupported TIFF sample format (need unsigned integer or IEEE float)");
  if (bitsPerSample_ == 32 && sampleFormat_ != 3)
    fail("32-bit integer TIFF samples are not supported");

  if (photometric_ == 0 || photometric_ == 1) {
    colorChannels_ = 1;
  } else if (photometric_ == 2) {
    colorChannels_ = 3;
    if (samplesPerPixel_ < 3) fail("RGB TIFF with fewer than 3 samples per pixel");
  } else {
    fail("unsupported TIFF photometric interpretation: " +
         std::to_string(photometric_) + " (need greyscale or RGB)");
  }

  switch (compression_) {
    case 1:
    case 5:
    case 32773:
      break;
    case 8:
    case 32946:
      fail("Deflate/ZIP-compressed TIFF is not supported; re-export with "
           "compression set to None");
    case 7:
      fail("JPEG-compressed TIFF is not supported; re-export with compression "
           "set to None");
    default:
      fail("unsupported TIFF compression: " + std::to_string(compression_));
  }
  if (predictor_ != 1 && predictor_ != 2 && predictor_ != 3)
    fail("unsupported TIFF predictor: " + std::to_string(predictor_));
  if (planarConfig_ != 1 && planarConfig_ != 2)
    fail("unsupported TIFF planar configuration");

  tiled_ = haveTile;
  if (!haveTile && !haveStrip) fail("TIFF has neither strip nor tile offsets");
  if (tiled_) {
    if (tileWidth_ == 0 || tileHeight_ == 0) fail("TIFF tile dimensions missing");
    tilesAcross_ = (width_ + tileWidth_ - 1) / tileWidth_;
    tilesDown_ = (height_ + tileHeight_ - 1) / tileHeight_;
  } else {
    if (rowsPerStrip_ == 0) rowsPerStrip_ = height_;
  }
  if (segByteCounts_.size() != segOffsets_.size())
    fail("TIFF strip/tile offset and byte-count arrays disagree");
}

void TiffReader::decodeSegment(uint32_t index, size_t expectedBytes,
                               Bytes& out) const {
  if (index >= segOffsets_.size()) fail("TIFF strip/tile index out of range");
  uint32_t off = segOffsets_[index];
  uint32_t len = segByteCounts_[index];
  requireRange(off, len, "TIFF pixel data");
  const uint8_t* src = data_.data() + off;
  switch (compression_) {
    case 1:
      out.assign(src, src + len);
      out.resize(expectedBytes, 0);
      break;
    case 5:
      decodeLzw(src, len, out, expectedBytes);
      break;
    case 32773:
      decodePackBits(src, len, out, expectedBytes);
      break;
    default:
      fail("unsupported TIFF compression");
  }
}

void TiffReader::unpredict(Bytes& buf, uint32_t pixelsPerRow, uint32_t rows,
                           uint32_t samplesPerPixel) const {
  if (predictor_ == 1) return;
  const uint32_t bytesPerSample = bitsPerSample_ / 8;
  const size_t rowBytes =
      static_cast<size_t>(pixelsPerRow) * samplesPerPixel * bytesPerSample;
  if (rowBytes == 0) return;

  if (predictor_ == 2) {
    for (uint32_t r = 0; r < rows; ++r) {
      uint8_t* row = buf.data() + static_cast<size_t>(r) * rowBytes;
      const size_t samples = static_cast<size_t>(pixelsPerRow) * samplesPerPixel;
      if (bitsPerSample_ == 8) {
        for (size_t i = samplesPerPixel; i < samples; ++i)
          row[i] = static_cast<uint8_t>(row[i] + row[i - samplesPerPixel]);
      } else if (bitsPerSample_ == 16) {
        for (size_t i = samplesPerPixel; i < samples; ++i) {
          uint8_t* cur = row + i * 2;
          const uint8_t* prev = row + (i - samplesPerPixel) * 2;
          uint16_t v = static_cast<uint16_t>(
              (littleEndian_ ? readU16LE(cur) : readU16BE(cur)) +
              (littleEndian_ ? readU16LE(prev) : readU16BE(prev)));
          if (littleEndian_) {
            cur[0] = static_cast<uint8_t>(v);
            cur[1] = static_cast<uint8_t>(v >> 8);
          } else {
            writeU16BE(cur, v);
          }
        }
      } else {
        fail("horizontal predictor is not defined for 32-bit samples");
      }
    }
    return;
  }

  // Predictor 3: bytes are stored in per-significance planes and delta coded.
  Bytes tmp(rowBytes);
  for (uint32_t r = 0; r < rows; ++r) {
    uint8_t* row = buf.data() + static_cast<size_t>(r) * rowBytes;
    for (size_t i = 1; i < rowBytes; ++i)
      row[i] = static_cast<uint8_t>(row[i] + row[i - 1]);
    const size_t samples = static_cast<size_t>(pixelsPerRow) * samplesPerPixel;
    for (size_t s = 0; s < samples; ++s) {
      for (uint32_t b = 0; b < bytesPerSample; ++b) {
        // Big-endian sample assembly; swapped below for LE files.
        tmp[s * bytesPerSample + b] = row[b * samples + s];
      }
    }
    if (littleEndian_) {
      for (size_t s = 0; s < samples; ++s) {
        uint8_t* p = tmp.data() + s * bytesPerSample;
        for (uint32_t b = 0; b < bytesPerSample / 2; ++b)
          std::swap(p[b], p[bytesPerSample - 1 - b]);
      }
    }
    std::memcpy(row, tmp.data(), rowBytes);
  }
}

void TiffReader::expandSegment(const Bytes& seg, uint32_t segFirstRow,
                               uint32_t segRows, uint32_t segFirstCol,
                               uint32_t segCols, uint32_t plane,
                               uint32_t firstRow, uint32_t rowCount,
                               float* dst) const {
  const uint32_t samplesInSeg = (planarConfig_ == 1) ? samplesPerPixel_ : 1;
  const uint32_t bytesPerSample = bitsPerSample_ / 8;
  const size_t segRowBytes =
      static_cast<size_t>(segCols) * samplesInSeg * bytesPerSample;
  const float scale =
      (bitsPerSample_ == 8) ? 1.0f / 255.0f : 1.0f / 65535.0f;
  const bool invert = (photometric_ == 0);

  for (uint32_t r = 0; r < segRows; ++r) {
    uint32_t imageRow = segFirstRow + r;
    if (imageRow < firstRow || imageRow >= firstRow + rowCount) continue;
    if (imageRow >= height_) break;
    const uint8_t* srcRow = seg.data() + static_cast<size_t>(r) * segRowBytes;
    float* dstRow = dst + static_cast<size_t>(imageRow - firstRow) * width_ *
                              colorChannels_;
    uint32_t cols = segCols;
    if (segFirstCol + cols > width_) cols = width_ - segFirstCol;
    for (uint32_t c = 0; c < cols; ++c) {
      float* out = dstRow + static_cast<size_t>(segFirstCol + c) * colorChannels_;
      for (uint32_t ch = 0; ch < colorChannels_; ++ch) {
        uint32_t srcChannel;
        if (planarConfig_ == 1) {
          srcChannel = ch;
        } else {
          if (plane != ch) continue;
          srcChannel = 0;
        }
        const uint8_t* p =
            srcRow + (static_cast<size_t>(c) * samplesInSeg + srcChannel) *
                         bytesPerSample;
        float v;
        if (bitsPerSample_ == 8) {
          v = static_cast<float>(*p) * scale;
        } else if (bitsPerSample_ == 16) {
          v = static_cast<float>(littleEndian_ ? readU16LE(p) : readU16BE(p)) *
              scale;
        } else {
          uint32_t bits = littleEndian_ ? readU32LE(p) : readU32BE(p);
          std::memcpy(&v, &bits, 4);
        }
        if (invert) v = 1.0f - v;
        out[ch] = v;
      }
    }
  }
}

void TiffReader::readRows(uint32_t firstRow, uint32_t rowCount,
                          float* dst) const {
  if (firstRow >= height_) fail("readRows past end of image");
  if (firstRow + rowCount > height_) rowCount = height_ - firstRow;
  const uint32_t planes = (planarConfig_ == 2) ? colorChannels_ : 1;
  const uint32_t bytesPerSample = bitsPerSample_ / 8;
  const uint32_t samplesInSeg = (planarConfig_ == 1) ? samplesPerPixel_ : 1;
  Bytes seg;

  if (tiled_) {
    const uint32_t tilesPerPlane = tilesAcross_ * tilesDown_;
    const size_t tileBytes = static_cast<size_t>(tileWidth_) * tileHeight_ *
                             samplesInSeg * bytesPerSample;
    uint32_t firstTileRow = firstRow / tileHeight_;
    uint32_t lastTileRow = (firstRow + rowCount - 1) / tileHeight_;
    for (uint32_t p = 0; p < planes; ++p) {
      for (uint32_t ty = firstTileRow; ty <= lastTileRow; ++ty) {
        for (uint32_t tx = 0; tx < tilesAcross_; ++tx) {
          uint32_t index = p * tilesPerPlane + ty * tilesAcross_ + tx;
          decodeSegment(index, tileBytes, seg);
          unpredict(seg, tileWidth_, tileHeight_, samplesInSeg);
          expandSegment(seg, ty * tileHeight_, tileHeight_, tx * tileWidth_,
                        tileWidth_, p, firstRow, rowCount, dst);
        }
      }
    }
    return;
  }

  const uint32_t stripsPerPlane = (height_ + rowsPerStrip_ - 1) / rowsPerStrip_;
  uint32_t firstStrip = firstRow / rowsPerStrip_;
  uint32_t lastStrip = (firstRow + rowCount - 1) / rowsPerStrip_;
  for (uint32_t p = 0; p < planes; ++p) {
    for (uint32_t s = firstStrip; s <= lastStrip && s < stripsPerPlane; ++s) {
      uint32_t stripFirstRow = s * rowsPerStrip_;
      uint32_t stripRows = rowsPerStrip_;
      if (stripFirstRow + stripRows > height_) stripRows = height_ - stripFirstRow;
      const size_t stripBytes = static_cast<size_t>(width_) * stripRows *
                                samplesInSeg * bytesPerSample;
      decodeSegment(p * stripsPerPlane + s, stripBytes, seg);
      unpredict(seg, width_, stripRows, samplesInSeg);
      expandSegment(seg, stripFirstRow, stripRows, 0, width_, p, firstRow,
                    rowCount, dst);
    }
  }
}

}  // namespace iso21496
