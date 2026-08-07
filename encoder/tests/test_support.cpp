#include "test_support.h"

#include <algorithm>
#include <cstring>

namespace iso21496 {
namespace test {
namespace {

struct Writer {
  Bytes out;
  bool le = true;

  void u16(uint16_t v) {
    if (le) {
      out.push_back(static_cast<uint8_t>(v));
      out.push_back(static_cast<uint8_t>(v >> 8));
    } else {
      putU16BE(out, v);
    }
  }
  void u32(uint32_t v) {
    if (le) {
      out.push_back(static_cast<uint8_t>(v));
      out.push_back(static_cast<uint8_t>(v >> 8));
      out.push_back(static_cast<uint8_t>(v >> 16));
      out.push_back(static_cast<uint8_t>(v >> 24));
    } else {
      putU32BE(out, v);
    }
  }
  void patch32(size_t at, uint32_t v) {
    if (le) {
      out[at] = static_cast<uint8_t>(v);
      out[at + 1] = static_cast<uint8_t>(v >> 8);
      out[at + 2] = static_cast<uint8_t>(v >> 16);
      out[at + 3] = static_cast<uint8_t>(v >> 24);
    } else {
      writeU32BE(&out[at], v);
    }
  }
};

struct Field {
  uint16_t tag;
  uint16_t type;
  uint32_t count;
  Bytes value;  // in the writer's byte order
};

}  // namespace

Bytes makeTiff(const TiffSpec& spec, const std::vector<float>& pixels) {
  Writer w;
  w.le = spec.littleEndian;
  const uint32_t rowsPerStrip =
      spec.rowsPerStrip ? spec.rowsPerStrip : spec.height;
  const uint32_t stripCount = (spec.height + rowsPerStrip - 1) / rowsPerStrip;
  const uint32_t bytesPerSample = spec.bitsPerSample / 8;
  const size_t rowBytes =
      static_cast<size_t>(spec.width) * spec.channels * bytesPerSample;

  // Pixel data first; the IFD then points back at it.
  Bytes pixelData;
  pixelData.reserve(rowBytes * spec.height);
  for (size_t i = 0; i < pixels.size(); ++i) {
    float v = pixels[i];
    if (spec.floatSamples) {
      uint32_t bits;
      std::memcpy(&bits, &v, 4);
      if (spec.littleEndian) {
        pixelData.push_back(static_cast<uint8_t>(bits));
        pixelData.push_back(static_cast<uint8_t>(bits >> 8));
        pixelData.push_back(static_cast<uint8_t>(bits >> 16));
        pixelData.push_back(static_cast<uint8_t>(bits >> 24));
      } else {
        putU32BE(pixelData, bits);
      }
    } else if (spec.bitsPerSample == 16) {
      uint16_t q = static_cast<uint16_t>(
          std::lround(std::min(1.0f, std::max(0.0f, v)) * 65535.0f));
      if (spec.littleEndian) {
        pixelData.push_back(static_cast<uint8_t>(q));
        pixelData.push_back(static_cast<uint8_t>(q >> 8));
      } else {
        putU16BE(pixelData, q);
      }
    } else {
      pixelData.push_back(static_cast<uint8_t>(
          std::lround(std::min(1.0f, std::max(0.0f, v)) * 255.0f)));
    }
  }

  std::vector<Field> fields;
  auto shortField = [&](uint16_t tag, uint16_t v) {
    Field f{tag, 3, 1, {}};
    Writer t;
    t.le = w.le;
    t.u16(v);
    f.value = t.out;
    fields.push_back(f);
  };
  auto longField = [&](uint16_t tag, uint32_t v) {
    Field f{tag, 4, 1, {}};
    Writer t;
    t.le = w.le;
    t.u32(v);
    f.value = t.out;
    fields.push_back(f);
  };

  longField(256, spec.width);
  longField(257, spec.height);
  {  // BitsPerSample, one SHORT per channel
    Field f{258, 3, static_cast<uint32_t>(spec.channels), {}};
    Writer t;
    t.le = w.le;
    for (int c = 0; c < spec.channels; ++c)
      t.u16(static_cast<uint16_t>(spec.bitsPerSample));
    f.value = t.out;
    fields.push_back(f);
  }
  shortField(259, 1);  // no compression
  shortField(262, spec.channels == 1 ? 1 : 2);
  // 273 StripOffsets and 279 StripByteCounts are added below.
  shortField(274, 1);
  shortField(277, static_cast<uint16_t>(spec.channels));
  longField(278, rowsPerStrip);
  shortField(284, 1);
  shortField(339, spec.floatSamples ? 3 : 1);
  if (!spec.iccProfile.empty()) {
    Field f{34675, 7, static_cast<uint32_t>(spec.iccProfile.size()),
            spec.iccProfile};
    fields.push_back(f);
  }
  if (spec.withExif) {
    Field make{271, 2, 6, {}};
    make.value = Bytes{'A', 'C', 'M', 'E', '\0', '\0'};
    fields.push_back(make);
    Field artist{315, 2, 12, {}};
    const char* a = "Test Author";
    artist.value.assign(a, a + 12);
    fields.push_back(artist);
  }

  {  // Strip offsets / byte counts (patched after layout is known)
    Field so{273, 4, stripCount, {}};
    Field sc{279, 4, stripCount, {}};
    Writer t1, t2;
    t1.le = t2.le = w.le;
    for (uint32_t s = 0; s < stripCount; ++s) {
      t1.u32(0);
      uint32_t rows = std::min(rowsPerStrip, spec.height - s * rowsPerStrip);
      t2.u32(static_cast<uint32_t>(rowBytes * rows));
    }
    so.value = t1.out;
    sc.value = t2.out;
    fields.push_back(so);
    fields.push_back(sc);
  }

  std::sort(fields.begin(), fields.end(),
            [](const Field& a, const Field& b) { return a.tag < b.tag; });

  const uint32_t ifdOffset = 8;
  const uint32_t ifdBytes =
      2 + static_cast<uint32_t>(fields.size()) * 12 + 4;
  uint32_t cursor = ifdOffset + ifdBytes;
  std::vector<uint32_t> valueOffsets(fields.size(), 0);
  for (size_t i = 0; i < fields.size(); ++i) {
    if (fields[i].value.size() > 4) {
      valueOffsets[i] = cursor;
      cursor += static_cast<uint32_t>((fields[i].value.size() + 1) & ~size_t{1});
    }
  }
  const uint32_t pixelOffset = cursor;

  w.out.push_back(spec.littleEndian ? 'I' : 'M');
  w.out.push_back(spec.littleEndian ? 'I' : 'M');
  w.u16(42);
  w.u32(ifdOffset);
  w.u16(static_cast<uint16_t>(fields.size()));
  // Where the strip offsets end up depends on whether they fit inline in the
  // IFD entry (a single strip) or spill into the value area.
  size_t stripOffsetPatchPos = 0;
  for (size_t i = 0; i < fields.size(); ++i) {
    w.u16(fields[i].tag);
    w.u16(fields[i].type);
    w.u32(fields[i].count);
    const bool inlineValue = fields[i].value.size() <= 4;
    if (fields[i].tag == 273)
      stripOffsetPatchPos = inlineValue ? w.out.size() : valueOffsets[i];
    if (inlineValue) {
      Bytes padded = fields[i].value;
      padded.resize(4, 0);
      putBytes(w.out, padded.data(), 4);
    } else {
      w.u32(valueOffsets[i]);
    }
  }
  w.u32(0);  // no next IFD
  for (size_t i = 0; i < fields.size(); ++i) {
    if (fields[i].value.size() > 4) {
      putBytes(w.out, fields[i].value.data(), fields[i].value.size());
      if (w.out.size() & 1) w.out.push_back(0);
    }
  }
  putBytes(w.out, pixelData.data(), pixelData.size());

  // Fill in the real strip offsets now that the pixel block has a home.
  for (uint32_t s = 0; s < stripCount; ++s)
    w.patch32(stripOffsetPatchPos + s * 4,
              pixelOffset + static_cast<uint32_t>(rowBytes * rowsPerStrip * s));
  return w.out;
}

std::vector<float> makeHdrPattern(uint32_t w, uint32_t h, float peak) {
  std::vector<float> px(static_cast<size_t>(w) * h * 3);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      float* p = &px[(static_cast<size_t>(y) * w + x) * 3];
      const float u = w > 1 ? static_cast<float>(x) / (w - 1) : 0.0f;
      const float v = h > 1 ? static_cast<float>(y) / (h - 1) : 0.0f;
      if (u > 0.7f && v > 0.7f) {
        // Specular highlight well above SDR white.
        p[0] = p[1] = p[2] = peak;
      } else if (v < 0.3f) {
        p[0] = 0.18f + 0.6f * u;  // gradient
        p[1] = 0.18f;
        p[2] = 0.5f - 0.4f * u;
      } else {
        p[0] = p[1] = p[2] = 0.18f + 0.5f * u;
      }
    }
  }
  return px;
}

std::vector<float> makeDetailedHdrPattern(uint32_t w, uint32_t h, float peak) {
  std::vector<float> px = makeHdrPattern(w, h, peak);
  // A cheap deterministic hash gives repeatable grain without <random>'s cost.
  auto noise = [](uint32_t x, uint32_t y, uint32_t c) {
    uint32_t v = x * 374761393u + y * 668265263u + c * 2246822519u;
    v = (v ^ (v >> 13)) * 1274126177u;
    return static_cast<float>((v ^ (v >> 16)) & 0xffff) / 65535.0f - 0.5f;
  };
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      for (uint32_t c = 0; c < 3; ++c) {
        float& v = px[(static_cast<size_t>(y) * w + x) * 3 + c];
        // Texture at two scales plus grain, scaled with the local level so
        // shadows stay clean the way a real raw file does.
        float texture = 0.10f * std::sin(x * 0.31f + y * 0.17f) *
                        std::cos(x * 0.07f - y * 0.23f);
        v = std::max(0.0f, v * (1.0f + texture) + 0.03f * v * noise(x, y, c));
      }
    }
  }
  return px;
}

std::vector<JpegSegment> walkJpeg(const Bytes& data, size_t start,
                                  size_t* endOffset) {
  std::vector<JpegSegment> out;
  size_t i = start;
  if (i + 2 > data.size() || data[i] != 0xff || data[i + 1] != 0xd8) {
    if (endOffset) *endOffset = start;
    return out;
  }
  out.push_back({0xd8, i, i + 2, 0});
  i += 2;
  while (i + 1 < data.size()) {
    if (data[i] != 0xff) break;
    uint8_t marker = data[i + 1];
    if (marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7) || marker == 0x01) {
      out.push_back({marker, i, i + 2, 0});
      i += 2;
      continue;
    }
    if (marker == 0xd9) {  // EOI
      out.push_back({marker, i, i + 2, 0});
      i += 2;
      break;
    }
    if (i + 4 > data.size()) break;
    size_t len = readU16BE(&data[i + 2]);
    out.push_back({marker, i, i + 4, len - 2});
    i += 2 + len;
    if (marker == 0xda) {
      // Skip the entropy-coded data: scan for the next non-RST marker.
      while (i + 1 < data.size()) {
        if (data[i] == 0xff && data[i + 1] != 0x00 &&
            !(data[i + 1] >= 0xd0 && data[i + 1] <= 0xd7)) {
          break;
        }
        ++i;
      }
    }
  }
  if (endOffset) *endOffset = i;
  return out;
}

}  // namespace test
}  // namespace iso21496
