#include "icc.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace iso21496 {
namespace {

constexpr double kD50x = 0.3457, kD50y = 0.3585;

int32_t toS15Fixed16(double v) {
  return static_cast<int32_t>(std::lround(v * 65536.0));
}

void putTag(Bytes& b, const char* sig) { putString(b, sig); }

struct TagRecord {
  char sig[5];
  Bytes data;
};

Bytes assembleProfile(const char* colorSpace, const char* pcs,
                      std::vector<TagRecord>& tags) {
  const uint32_t headerSize = 128;
  const uint32_t tagCount = static_cast<uint32_t>(tags.size());
  uint32_t tableSize = 4 + tagCount * 12;
  uint32_t offset = headerSize + tableSize;
  // Tag data is 4-byte aligned per the ICC spec.
  std::vector<uint32_t> offsets(tagCount), sizes(tagCount);
  for (uint32_t i = 0; i < tagCount; ++i) {
    offsets[i] = offset;
    sizes[i] = static_cast<uint32_t>(tags[i].data.size());
    offset += (sizes[i] + 3u) & ~3u;
  }
  const uint32_t totalSize = offset;

  Bytes p;
  p.reserve(totalSize);
  putU32BE(p, totalSize);          // profile size
  putString(p, "none");            // preferred CMM
  putU32BE(p, 0x02400000);         // version 2.4.0
  putString(p, "mntr");            // device class: display
  putString(p, colorSpace);        // data colour space
  putString(p, pcs);               // PCS
  // Creation date: fixed so builds are reproducible.
  putU16BE(p, 2025); putU16BE(p, 1); putU16BE(p, 1);
  putU16BE(p, 0); putU16BE(p, 0); putU16BE(p, 0);
  putString(p, "acsp");
  putString(p, "APPL");            // primary platform
  putU32BE(p, 0);                  // flags
  putU32BE(p, 0);                  // device manufacturer
  putU32BE(p, 0);                  // device model
  putU32BE(p, 0); putU32BE(p, 0);  // device attributes
  putU32BE(p, 0);                  // rendering intent: perceptual
  putS32BE(p, toS15Fixed16(kD50x / kD50y));
  putS32BE(p, toS15Fixed16(1.0));
  putS32BE(p, toS15Fixed16((1.0 - kD50x - kD50y) / kD50y));
  putString(p, "ISOG");            // creator
  p.resize(headerSize, 0);         // profile ID + reserved

  putU32BE(p, tagCount);
  for (uint32_t i = 0; i < tagCount; ++i) {
    putTag(p, tags[i].sig);
    putU32BE(p, offsets[i]);
    putU32BE(p, sizes[i]);
  }
  for (uint32_t i = 0; i < tagCount; ++i) {
    putBytes(p, tags[i].data.data(), tags[i].data.size());
    while (p.size() % 4) p.push_back(0);
  }
  return p;
}

Bytes xyzTag(double x, double y, double z) {
  Bytes t;
  putString(t, "XYZ ");
  putU32BE(t, 0);
  putS32BE(t, toS15Fixed16(x));
  putS32BE(t, toS15Fixed16(y));
  putS32BE(t, toS15Fixed16(z));
  return t;
}

// 'desc' in ICC v2 is a textDescriptionType: ASCII + Unicode + ScriptCode.
Bytes descTag(const std::string& s) {
  Bytes t;
  putString(t, "desc");
  putU32BE(t, 0);
  putU32BE(t, static_cast<uint32_t>(s.size() + 1));
  putBytes(t, s.data(), s.size());
  putU8(t, 0);
  putU32BE(t, 0);  // Unicode language code
  putU32BE(t, 0);  // Unicode count
  putU16BE(t, 0);  // ScriptCode code
  putU8(t, 0);     // ScriptCode count
  t.resize(t.size() + 67, 0);  // ScriptCode description (fixed 67 bytes)
  return t;
}

Bytes textTag(const std::string& s) {
  Bytes t;
  putString(t, "text");
  putU32BE(t, 0);
  putBytes(t, s.data(), s.size());
  putU8(t, 0);
  return t;
}

// Sampled sRGB tone curve; 1024 entries keeps the round trip below 1/65535.
Bytes srgbCurveTag() {
  Bytes t;
  putString(t, "curv");
  putU32BE(t, 0);
  const uint32_t n = 1024;
  putU32BE(t, n);
  for (uint32_t i = 0; i < n; ++i) {
    // A 'curv' table maps device value -> linear light, i.e. it samples the
    // EOTF, not its inverse.
    double device = static_cast<double>(i) / (n - 1);
    double linear = decodeSrgb(static_cast<float>(device));
    putU16BE(t, static_cast<uint16_t>(std::lround(linear * 65535.0)));
  }
  return t;
}

Bytes gammaCurveTag(double gamma) {
  Bytes t;
  putString(t, "curv");
  putU32BE(t, 0);
  putU32BE(t, 1);
  putU16BE(t, static_cast<uint16_t>(std::lround(gamma * 256.0)));
  return t;
}

Bytes chadTag(const Mat3& m) {
  Bytes t;
  putString(t, "sf32");
  putU32BE(t, 0);
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) putS32BE(t, toS15Fixed16(m.m[i][j]));
  return t;
}

}  // namespace

Bytes buildRgbIccProfile(ColorPrimaries primaries, const std::string& desc) {
  const Primaries prim = primariesFor(primaries);
  // ICC v2 colorant tags live in PCS XYZ, i.e. adapted to D50.
  const Mat3 d65ToD50 = adaptD65ToD50();
  const Mat3 toD50 = d65ToD50 * rgbToXyzD65(prim);

  std::vector<TagRecord> tags;
  auto add = [&](const char* sig, Bytes data) {
    TagRecord r;
    std::snprintf(r.sig, sizeof(r.sig), "%s", sig);
    r.data = std::move(data);
    tags.push_back(std::move(r));
  };

  add("desc", descTag(desc));
  add("wtpt", xyzTag(kD50x / kD50y, 1.0, (1.0 - kD50x - kD50y) / kD50y));
  add("rXYZ", xyzTag(toD50.m[0][0], toD50.m[1][0], toD50.m[2][0]));
  add("gXYZ", xyzTag(toD50.m[0][1], toD50.m[1][1], toD50.m[2][1]));
  add("bXYZ", xyzTag(toD50.m[0][2], toD50.m[1][2], toD50.m[2][2]));
  Bytes curve = srgbCurveTag();
  add("rTRC", curve);
  add("gTRC", curve);
  add("bTRC", curve);
  add("chad", chadTag(d65ToD50));
  add("cprt", textTag("Generated by iso21496_encoder; no rights reserved."));
  return assembleProfile("RGB ", "XYZ ", tags);
}

Bytes buildGrayIccProfile(double gamma, const std::string& desc) {
  std::vector<TagRecord> tags;
  auto add = [&](const char* sig, Bytes data) {
    TagRecord r;
    std::snprintf(r.sig, sizeof(r.sig), "%s", sig);
    r.data = std::move(data);
    tags.push_back(std::move(r));
  };
  add("desc", descTag(desc));
  add("wtpt", xyzTag(kD50x / kD50y, 1.0, (1.0 - kD50x - kD50y) / kD50y));
  add("kTRC", gammaCurveTag(gamma));
  add("cprt", textTag("Generated by iso21496_encoder; no rights reserved."));
  return assembleProfile("GRAY", "XYZ ", tags);
}

IccSummary inspectIccProfile(const Bytes& profile) {
  IccSummary s;
  if (profile.size() < 132) return s;
  const uint32_t size = readU32BE(&profile[0]);
  if (size > profile.size()) return s;
  const char* space = reinterpret_cast<const char*>(&profile[16]);
  s.isGray = std::strncmp(space, "GRAY", 4) == 0;
  const uint32_t tagCount = readU32BE(&profile[128]);
  if (tagCount > 256 || 132 + tagCount * 12 > profile.size()) return s;

  auto findTag = [&](const char* sig, uint32_t* off, uint32_t* len) -> bool {
    for (uint32_t i = 0; i < tagCount; ++i) {
      const uint8_t* e = &profile[132 + i * 12];
      if (std::memcmp(e, sig, 4) == 0) {
        *off = readU32BE(e + 4);
        *len = readU32BE(e + 8);
        return *off + *len <= profile.size();
      }
    }
    return false;
  };

  uint32_t off = 0, len = 0;
  if (findTag("desc", &off, &len) && len > 12) {
    uint32_t n = readU32BE(&profile[off + 8]);
    if (n > 0 && off + 12 + n <= profile.size()) {
      s.description.assign(reinterpret_cast<const char*>(&profile[off + 12]),
                           n - 1);
    }
  }

  // ICC v4.4 CICP tag pins the transfer function exactly; prefer it.
  if (findTag("cicp", &off, &len) && len >= 12) {
    uint8_t transferChar = profile[off + 9];
    uint8_t colourPrim = profile[off + 8];
    switch (transferChar) {
      case 8: s.transfer = TransferFunction::Linear; break;
      case 13: s.transfer = TransferFunction::sRGB; break;
      case 16: s.transfer = TransferFunction::PQ; break;
      case 18: s.transfer = TransferFunction::HLG; break;
      case 1: case 6: case 14: case 15: s.transfer = TransferFunction::Gamma22; break;
      default: break;
    }
    switch (colourPrim) {
      case 1: s.primaries = ColorPrimaries::sRGB; s.matched = true; break;
      case 9: s.primaries = ColorPrimaries::Rec2020; s.matched = true; break;
      case 12: s.primaries = ColorPrimaries::DisplayP3; s.matched = true; break;
      default: break;
    }
  }

  if (s.transfer == TransferFunction::Auto) {
    const char* trcSig = s.isGray ? "kTRC" : "rTRC";
    if (findTag(trcSig, &off, &len) && len >= 12 &&
        std::memcmp(&profile[off], "curv", 4) == 0) {
      uint32_t n = readU32BE(&profile[off + 8]);
      if (n == 0) {
        s.transfer = TransferFunction::Linear;
      } else if (n == 1) {
        double g = readU16BE(&profile[off + 12]) / 256.0;
        if (std::fabs(g - 1.0) < 0.02) s.transfer = TransferFunction::Linear;
        else if (std::fabs(g - 1.8) < 0.05) s.transfer = TransferFunction::Gamma18;
        else s.transfer = TransferFunction::Gamma22;
      } else if (n >= 3 && off + 12 + 2 * n <= profile.size()) {
        // Sampled curve: compare the midpoint against known candidates.
        uint32_t mid = n / 2;
        double v = readU16BE(&profile[off + 12 + 2 * mid]) / 65535.0;
        double x = static_cast<double>(mid) / (n - 1);
        double srgbErr = std::fabs(v - decodeSrgb(static_cast<float>(x)));
        double g18Err = std::fabs(v - std::pow(x, 1.8));
        double g22Err = std::fabs(v - std::pow(x, 2.2));
        double linErr = std::fabs(v - x);
        double best = std::min({srgbErr, g18Err, g22Err, linErr});
        if (best == linErr) s.transfer = TransferFunction::Linear;
        else if (best == g18Err) s.transfer = TransferFunction::Gamma18;
        else if (best == g22Err) s.transfer = TransferFunction::Gamma22;
        else s.transfer = TransferFunction::sRGB;
      }
    } else if (findTag(trcSig, &off, &len) && len >= 12 &&
               std::memcmp(&profile[off], "para", 4) == 0) {
      uint16_t fn = readU16BE(&profile[off + 8]);
      // Type 0 is a pure gamma; types 3/4 are the sRGB-shaped piecewise forms.
      if (fn == 0 && len >= 14) {
        double g = static_cast<int32_t>(readU32BE(&profile[off + 12])) / 65536.0;
        if (std::fabs(g - 1.0) < 0.02) s.transfer = TransferFunction::Linear;
        else if (std::fabs(g - 1.8) < 0.05) s.transfer = TransferFunction::Gamma18;
        else s.transfer = TransferFunction::Gamma22;
      } else {
        s.transfer = TransferFunction::sRGB;
      }
    }
  }

  if (!s.isGray && !s.matched) {
    uint32_t ro, rl, go, gl, bo, bl;
    if (findTag("rXYZ", &ro, &rl) && findTag("gXYZ", &go, &gl) &&
        findTag("bXYZ", &bo, &bl) && rl >= 20 && gl >= 20 && bl >= 20) {
      auto readXyz = [&](uint32_t o) {
        std::array<double, 3> v{};
        for (int i = 0; i < 3; ++i)
          v[i] = static_cast<int32_t>(readU32BE(&profile[o + 8 + i * 4])) /
                 65536.0;
        return v;
      };
      auto r = readXyz(ro), g = readXyz(go), b = readXyz(bo);
      const ColorPrimaries candidates[] = {
          ColorPrimaries::ProPhoto, ColorPrimaries::Rec2020,
          ColorPrimaries::DisplayP3, ColorPrimaries::AdobeRGB,
          ColorPrimaries::sRGB};
      double bestErr = 1e9;
      ColorPrimaries best = ColorPrimaries::Auto;
      const Mat3 d65ToD50 = adaptD65ToD50();
      for (ColorPrimaries c : candidates) {
        // Reference colorants, D50-adapted exactly the way we write them.
        const Mat3 ref = d65ToD50 * rgbToXyzD65(primariesFor(c));
        double err = 0;
        for (int i = 0; i < 3; ++i) {
          err += std::fabs(ref.m[i][0] - r[i]) + std::fabs(ref.m[i][1] - g[i]) +
                 std::fabs(ref.m[i][2] - b[i]);
        }
        if (err < bestErr) {
          bestErr = err;
          best = c;
        }
      }
      if (bestErr < 0.05) {
        s.primaries = best;
        s.matched = true;
      }
    }
  }

  s.valid = true;
  return s;
}

}  // namespace iso21496
