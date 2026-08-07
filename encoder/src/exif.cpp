#include "exif.h"

#include <algorithm>
#include <cstring>
#include <functional>

namespace iso21496 {
namespace {

struct OutEntry {
  uint16_t tag = 0;
  uint16_t type = 0;
  uint32_t count = 0;
  Bytes value;  // already big-endian
};

size_t componentSize(uint16_t type) {
  switch (type) {
    case 1: case 2: case 6: case 7: return 1;
    case 3: case 8: return 2;
    case 4: case 9: case 11: return 4;
    case 5: case 10: case 12: return 8;
    default: return 0;
  }
}

// IFD0 tags worth carrying into the JPEG. Everything else in IFD0 describes
// the TIFF container itself (strip layout, bit depth) and would be wrong.
bool ifd0Whitelisted(uint16_t tag) {
  switch (tag) {
    case 270:    // ImageDescription
    case 271:    // Make
    case 272:    // Model
    case 274:    // Orientation
    case 282:    // XResolution
    case 283:    // YResolution
    case 296:    // ResolutionUnit
    case 305:    // Software
    case 306:    // DateTime
    case 315:    // Artist
    case 33432:  // Copyright
      return true;
    default:
      return false;
  }
}

class SourceIfd {
 public:
  SourceIfd(const Bytes& data, bool littleEndian)
      : data_(data), le_(littleEndian) {}

  bool read(uint32_t offset, std::vector<OutEntry>* out,
            const std::function<bool(uint16_t)>& accept) const {
    if (offset + 2 > data_.size()) return false;
    uint16_t count = rd16(offset);
    if (count > 512 || offset + 2 + static_cast<size_t>(count) * 12 + 4 >
                           data_.size())
      return false;
    for (uint16_t i = 0; i < count; ++i) {
      size_t e = offset + 2 + static_cast<size_t>(i) * 12;
      uint16_t tag = rd16(e);
      uint16_t type = rd16(e + 2);
      uint32_t n = rd32(e + 4);
      size_t cs = componentSize(type);
      if (cs == 0 || !accept(tag)) continue;
      size_t total = cs * static_cast<size_t>(n);
      if (total > 65535) continue;
      const uint8_t* src;
      uint8_t inlineBuf[4];
      if (total <= 4) {
        std::memcpy(inlineBuf, &data_[e + 8], 4);
        src = inlineBuf;
      } else {
        uint32_t vo = rd32(e + 8);
        if (vo + total > data_.size()) continue;
        src = &data_[vo];
      }
      OutEntry oe;
      oe.tag = tag;
      oe.type = type;
      oe.count = n;
      oe.value = convert(type, n, src);
      out->push_back(std::move(oe));
    }
    return true;
  }

 private:
  uint16_t rd16(size_t o) const {
    return le_ ? readU16LE(&data_[o]) : readU16BE(&data_[o]);
  }
  uint32_t rd32(size_t o) const {
    return le_ ? readU32LE(&data_[o]) : readU32BE(&data_[o]);
  }

  // Re-emits a value in big-endian order, component by component.
  Bytes convert(uint16_t type, uint32_t count, const uint8_t* src) const {
    Bytes out;
    const size_t cs = componentSize(type);
    out.reserve(cs * count);
    for (uint32_t i = 0; i < count; ++i) {
      const uint8_t* p = src + i * cs;
      switch (type) {
        case 3: case 8:
          putU16BE(out, le_ ? readU16LE(p) : readU16BE(p));
          break;
        case 4: case 9: case 11:
          putU32BE(out, le_ ? readU32LE(p) : readU32BE(p));
          break;
        case 5: case 10:
          putU32BE(out, le_ ? readU32LE(p) : readU32BE(p));
          putU32BE(out, le_ ? readU32LE(p + 4) : readU32BE(p + 4));
          break;
        case 12:
          for (int b = 0; b < 8; ++b) out.push_back(le_ ? p[7 - b] : p[b]);
          break;
        default:
          out.push_back(*p);
          break;
      }
    }
    return out;
  }

  const Bytes& data_;
  bool le_;
};

void setShort(std::vector<OutEntry>* entries, uint16_t tag, uint16_t value) {
  for (auto& e : *entries) {
    if (e.tag == tag) {
      e.type = 3;
      e.count = 1;
      e.value.clear();
      putU16BE(e.value, value);
      return;
    }
  }
  OutEntry e;
  e.tag = tag;
  e.type = 3;
  e.count = 1;
  putU16BE(e.value, value);
  entries->push_back(std::move(e));
}

void setLong(std::vector<OutEntry>* entries, uint16_t tag, uint32_t value) {
  for (auto& e : *entries) {
    if (e.tag == tag) {
      e.type = 4;
      e.count = 1;
      e.value.clear();
      putU32BE(e.value, value);
      return;
    }
  }
  OutEntry e;
  e.tag = tag;
  e.type = 4;
  e.count = 1;
  putU32BE(e.value, value);
  entries->push_back(std::move(e));
}

bool hasTag(const std::vector<OutEntry>& entries, uint16_t tag) {
  for (const auto& e : entries)
    if (e.tag == tag) return true;
  return false;
}

void addIfAbsent(std::vector<OutEntry>* entries, uint16_t tag, uint16_t type,
                 uint32_t count, Bytes value) {
  if (hasTag(*entries, tag)) return;
  OutEntry e;
  e.tag = tag;
  e.type = type;
  e.count = count;
  e.value = std::move(value);
  entries->push_back(std::move(e));
}

void addRationalIfAbsent(std::vector<OutEntry>* entries, uint16_t tag,
                         uint32_t num, uint32_t den) {
  Bytes v;
  putU32BE(v, num);
  putU32BE(v, den);
  addIfAbsent(entries, tag, 5, 1, std::move(v));
}

// exiftool -validate (and some strict readers) expect these to be present in
// any Exif-bearing JPEG, so fill in conventional defaults when the source TIFF
// did not carry them.
void addRequiredExifTags(std::vector<OutEntry>* ifd0,
                         std::vector<OutEntry>* exifIfd) {
  addRationalIfAbsent(ifd0, 282, 72, 1);  // XResolution
  addRationalIfAbsent(ifd0, 283, 72, 1);  // YResolution
  if (!hasTag(*ifd0, 296)) {              // ResolutionUnit: inches
    Bytes v;
    putU16BE(v, 2);
    addIfAbsent(ifd0, 296, 3, 1, std::move(v));
  }
  if (!hasTag(*ifd0, 531)) {  // YCbCrPositioning: centered
    Bytes v;
    putU16BE(v, 1);
    addIfAbsent(ifd0, 531, 3, 1, std::move(v));
  }
  addIfAbsent(exifIfd, 36864, 7, 4, Bytes{'0', '2', '3', '2'});  // ExifVersion
  addIfAbsent(exifIfd, 37121, 7, 4, Bytes{1, 2, 3, 0});  // ComponentsConfig
  addIfAbsent(exifIfd, 40960, 7, 4, Bytes{'0', '1', '0', '0'});  // Flashpix
}

size_t ifdSize(size_t entryCount) { return 2 + entryCount * 12 + 4; }

size_t valueAreaSize(const std::vector<OutEntry>& entries) {
  size_t total = 0;
  for (const auto& e : entries)
    if (e.value.size() > 4) total += (e.value.size() + 1) & ~size_t{1};
  return total;
}

// Writes one IFD plus its out-of-line values. `ifdOffset` is relative to the
// start of the TIFF header (the "MM" bytes).
void writeIfd(Bytes& out, std::vector<OutEntry>& entries, uint32_t ifdOffset,
              uint32_t nextIfd) {
  std::sort(entries.begin(), entries.end(),
            [](const OutEntry& a, const OutEntry& b) { return a.tag < b.tag; });
  uint32_t valueCursor =
      ifdOffset + static_cast<uint32_t>(ifdSize(entries.size()));
  Bytes values;
  putU16BE(out, static_cast<uint16_t>(entries.size()));
  for (auto& e : entries) {
    putU16BE(out, e.tag);
    putU16BE(out, e.type);
    putU32BE(out, e.count);
    if (e.value.size() <= 4) {
      Bytes padded = e.value;
      padded.resize(4, 0);
      putBytes(out, padded.data(), 4);
    } else {
      putU32BE(out, valueCursor + static_cast<uint32_t>(values.size()));
      putBytes(values, e.value.data(), e.value.size());
      if (values.size() & 1) values.push_back(0);
    }
  }
  putU32BE(out, nextIfd);
  putBytes(out, values.data(), values.size());
}

}  // namespace

Bytes buildExifAppSegment(const TiffReader& tiff, const ExifOptions& options) {
  const TiffMetadata& meta = tiff.metadata();
  SourceIfd source(tiff.fileData(), tiff.littleEndian());

  std::vector<OutEntry> ifd0, exifIfd, gpsIfd;
  source.read(meta.ifd0Offset, &ifd0, ifd0Whitelisted);
  if (meta.hasExif) {
    source.read(meta.exifIfdOffset, &exifIfd, [](uint16_t tag) {
      // MakerNote is riddled with absolute offsets into the source file and
      // cannot be relocated safely; the interop pointer would dangle.
      return tag != 37500 && tag != 40965;
    });
  }
  if (meta.hasGps) source.read(meta.gpsIfdOffset, &gpsIfd, [](uint16_t) {
    return true;
  });

  // Lightroom bakes rotation into the rendered pixels, so the JPEG is upright.
  setShort(&ifd0, 274, 1);
  if (!exifIfd.empty() || options.pixelWidth) {
    setLong(&exifIfd, 40962, options.pixelWidth);
    setLong(&exifIfd, 40963, options.pixelHeight);
    setShort(&exifIfd, 40961, options.colorSpace);
  }
  if (ifd0.empty() && exifIfd.empty() && gpsIfd.empty()) return {};
  addRequiredExifTags(&ifd0, &exifIfd);

  // Reserve the pointer entries before measuring, so the sizes stay valid.
  if (!exifIfd.empty()) setLong(&ifd0, 34665, 0);
  if (!gpsIfd.empty()) setLong(&ifd0, 34853, 0);

  const uint32_t ifd0Off = 8;
  const uint32_t ifd0End =
      ifd0Off + static_cast<uint32_t>(ifdSize(ifd0.size()) + valueAreaSize(ifd0));
  const uint32_t exifOff = ifd0End;
  const uint32_t exifEnd =
      exifIfd.empty()
          ? exifOff
          : exifOff + static_cast<uint32_t>(ifdSize(exifIfd.size()) +
                                            valueAreaSize(exifIfd));
  const uint32_t gpsOff = exifEnd;

  if (!exifIfd.empty()) setLong(&ifd0, 34665, exifOff);
  if (!gpsIfd.empty()) setLong(&ifd0, 34853, gpsOff);

  Bytes tiffBlock;
  putString(tiffBlock, "MM");
  putU16BE(tiffBlock, 42);
  putU32BE(tiffBlock, ifd0Off);
  writeIfd(tiffBlock, ifd0, ifd0Off, 0);
  if (!exifIfd.empty()) {
    if (tiffBlock.size() != exifOff)
      fail("internal error: Exif IFD offset mismatch");
    writeIfd(tiffBlock, exifIfd, exifOff, 0);
  }
  if (!gpsIfd.empty()) {
    if (tiffBlock.size() != gpsOff)
      fail("internal error: GPS IFD offset mismatch");
    writeIfd(tiffBlock, gpsIfd, gpsOff, 0);
  }

  Bytes payload;
  putString(payload, "Exif");
  payload.push_back(0);
  payload.push_back(0);
  putBytes(payload, tiffBlock.data(), tiffBlock.size());
  if (payload.size() + 2 > 65535) return {};  // too big; drop it rather than
                                              // write a malformed segment

  Bytes seg;
  seg.push_back(0xff);
  seg.push_back(0xe1);
  putU16BE(seg, static_cast<uint16_t>(payload.size() + 2));
  putBytes(seg, payload.data(), payload.size());
  return seg;
}

}  // namespace iso21496
