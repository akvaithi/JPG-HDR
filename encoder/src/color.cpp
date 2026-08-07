#include "color.h"

#include <algorithm>
#include <cmath>

namespace iso21496 {
namespace {

// Bradford cone response, used to adapt D50-referred spaces (ProPhoto) to D65.
constexpr double kBradford[3][3] = {{0.8951, 0.2664, -0.1614},
                                    {-0.7502, 1.7135, 0.0367},
                                    {0.0389, -0.0685, 1.0296}};

Mat3 bradford() {
  Mat3 m;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) m.m[i][j] = kBradford[i][j];
  return m;
}

std::array<double, 3> whiteXyz(double x, double y) {
  return {x / y, 1.0, (1.0 - x - y) / y};
}

Mat3 adaptationMatrix(double srcX, double srcY, double dstX, double dstY) {
  Mat3 ma = bradford();
  auto src = ma.apply(whiteXyz(srcX, srcY));
  auto dst = ma.apply(whiteXyz(dstX, dstY));
  Mat3 scale;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) scale.m[i][j] = (i == j) ? dst[i] / src[i] : 0.0;
  return ma.inverse() * (scale * ma);
}

}  // namespace

Mat3 Mat3::operator*(const Mat3& o) const {
  Mat3 r;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      double s = 0;
      for (int k = 0; k < 3; ++k) s += m[i][k] * o.m[k][j];
      r.m[i][j] = s;
    }
  return r;
}

std::array<double, 3> Mat3::apply(const std::array<double, 3>& v) const {
  return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
          m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
          m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
}

Mat3 Mat3::inverse() const {
  double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
               m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  if (std::fabs(det) < 1e-12) fail("singular colour matrix");
  double inv = 1.0 / det;
  Mat3 r;
  r.m[0][0] = (m[1][1] * m[2][2] - m[1][2] * m[2][1]) * inv;
  r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * inv;
  r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * inv;
  r.m[1][0] = (m[1][2] * m[2][0] - m[1][0] * m[2][2]) * inv;
  r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * inv;
  r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * inv;
  r.m[2][0] = (m[1][0] * m[2][1] - m[1][1] * m[2][0]) * inv;
  r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * inv;
  r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * inv;
  return r;
}

bool Mat3::isIdentity(double eps) const {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      if (std::fabs(m[i][j] - (i == j ? 1.0 : 0.0)) > eps) return false;
  return true;
}

Primaries primariesFor(ColorPrimaries p) {
  switch (p) {
    case ColorPrimaries::DisplayP3:
      return {0.680, 0.320, 0.265, 0.690, 0.150, 0.060, 0.3127, 0.3290};
    case ColorPrimaries::Rec2020:
      return {0.708, 0.292, 0.170, 0.797, 0.131, 0.046, 0.3127, 0.3290};
    case ColorPrimaries::ProPhoto:
      return {0.734699, 0.265301, 0.159597, 0.840403,
              0.036598, 0.000105, 0.345704, 0.358540};  // D50
    case ColorPrimaries::AdobeRGB:
      return {0.640, 0.330, 0.210, 0.710, 0.150, 0.060, 0.3127, 0.3290};
    case ColorPrimaries::sRGB:
    default:
      return {0.640, 0.330, 0.300, 0.600, 0.150, 0.060, 0.3127, 0.3290};
  }
}

const char* primariesName(ColorPrimaries p) {
  switch (p) {
    case ColorPrimaries::DisplayP3: return "DisplayP3";
    case ColorPrimaries::Rec2020: return "Rec2020";
    case ColorPrimaries::ProPhoto: return "ProPhotoRGB";
    case ColorPrimaries::AdobeRGB: return "AdobeRGB";
    case ColorPrimaries::sRGB: return "sRGB";
    default: return "auto";
  }
}

const char* transferName(TransferFunction t) {
  switch (t) {
    case TransferFunction::Linear: return "linear";
    case TransferFunction::sRGB: return "srgb";
    case TransferFunction::Gamma18: return "gamma1.8";
    case TransferFunction::Gamma22: return "gamma2.2";
    case TransferFunction::ROMM: return "romm";
    case TransferFunction::PQ: return "pq";
    case TransferFunction::HLG: return "hlg";
    default: return "auto";
  }
}

bool parsePrimaries(const std::string& s, ColorPrimaries* out) {
  if (s == "auto") *out = ColorPrimaries::Auto;
  else if (s == "srgb" || s == "sRGB" || s == "rec709") *out = ColorPrimaries::sRGB;
  else if (s == "p3" || s == "DisplayP3" || s == "displayp3") *out = ColorPrimaries::DisplayP3;
  else if (s == "rec2020" || s == "Rec2020" || s == "bt2020") *out = ColorPrimaries::Rec2020;
  else if (s == "prophoto" || s == "ProPhotoRGB" || s == "romm") *out = ColorPrimaries::ProPhoto;
  else if (s == "adobergb" || s == "AdobeRGB") *out = ColorPrimaries::AdobeRGB;
  else return false;
  return true;
}

bool parseTransfer(const std::string& s, TransferFunction* out) {
  if (s == "auto") *out = TransferFunction::Auto;
  else if (s == "linear") *out = TransferFunction::Linear;
  else if (s == "srgb") *out = TransferFunction::sRGB;
  else if (s == "gamma1.8" || s == "gamma18") *out = TransferFunction::Gamma18;
  else if (s == "gamma2.2" || s == "gamma22") *out = TransferFunction::Gamma22;
  else if (s == "romm" || s == "prophoto") *out = TransferFunction::ROMM;
  else if (s == "pq" || s == "rec2100pq") *out = TransferFunction::PQ;
  else if (s == "hlg" || s == "rec2100hlg") *out = TransferFunction::HLG;
  else return false;
  return true;
}

Mat3 rgbToXyzD65(const Primaries& p) {
  Mat3 xyz;
  const double zr = 1.0 - p.rx - p.ry;
  const double zg = 1.0 - p.gx - p.gy;
  const double zb = 1.0 - p.bx - p.by;
  Mat3 base;
  base.m[0][0] = p.rx; base.m[0][1] = p.gx; base.m[0][2] = p.bx;
  base.m[1][0] = p.ry; base.m[1][1] = p.gy; base.m[1][2] = p.by;
  base.m[2][0] = zr;   base.m[2][1] = zg;   base.m[2][2] = zb;
  auto w = whiteXyz(p.wx, p.wy);
  auto s = base.inverse().apply(w);
  for (int i = 0; i < 3; ++i) {
    xyz.m[i][0] = base.m[i][0] * s[0];
    xyz.m[i][1] = base.m[i][1] * s[1];
    xyz.m[i][2] = base.m[i][2] * s[2];
  }
  const double kD65x = 0.3127, kD65y = 0.3290;
  if (std::fabs(p.wx - kD65x) > 1e-6 || std::fabs(p.wy - kD65y) > 1e-6)
    xyz = adaptationMatrix(p.wx, p.wy, kD65x, kD65y) * xyz;
  return xyz;
}

Mat3 adaptD65ToD50() {
  return adaptationMatrix(0.3127, 0.3290, 0.3457, 0.3585);
}

Mat3 conversionMatrix(ColorPrimaries from, ColorPrimaries to) {
  if (from == to) return Mat3::identity();
  return rgbToXyzD65(primariesFor(to)).inverse() *
         rgbToXyzD65(primariesFor(from));
}

float decodeSrgb(float v) {
  return v <= 0.04045f ? v / 12.92f
                       : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

float encodeSrgb(float v) {
  if (v <= 0.0f) return 0.0f;
  if (v >= 1.0f) return 1.0f;
  return v <= 0.0031308f ? v * 12.92f
                         : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

float decodeTransfer(TransferFunction t, float v, float pqDiffuseWhiteNits) {
  switch (t) {
    case TransferFunction::Linear:
      return v;
    case TransferFunction::Gamma18:
      return v <= 0.0f ? 0.0f : std::pow(v, 1.8f);
    case TransferFunction::Gamma22:
      return v <= 0.0f ? 0.0f : std::pow(v, 2.2f);
    case TransferFunction::ROMM:
      // ROMM RGB (ProPhoto): linear toe below 16/512, then gamma 1.8.
      return v < 0.031248f ? v / 16.0f : std::pow(v, 1.8f);
    case TransferFunction::PQ: {
      // SMPTE ST 2084 EOTF, normalised so diffuse white lands on 1.0.
      constexpr float m1 = 2610.0f / 16384.0f;
      constexpr float m2 = 2523.0f / 4096.0f * 128.0f;
      constexpr float c1 = 3424.0f / 4096.0f;
      constexpr float c2 = 2413.0f / 4096.0f * 32.0f;
      constexpr float c3 = 2392.0f / 4096.0f * 32.0f;
      float e = std::max(0.0f, v);
      float p = std::pow(e, 1.0f / m2);
      float num = std::max(p - c1, 0.0f);
      float den = c2 - c3 * p;
      float y = den <= 0.0f ? 0.0f : std::pow(num / den, 1.0f / m1);
      float nits = y * 10000.0f;
      return nits / std::max(1.0f, pqDiffuseWhiteNits);
    }
    case TransferFunction::HLG: {
      // ARIB STD-B67 inverse OETF plus the 1000-nit reference OOTF.
      constexpr float a = 0.17883277f;
      constexpr float b = 1.0f - 4.0f * a;
      constexpr float c = 0.55991073f;  // 0.5 - a*ln(4a)
      float e = std::max(0.0f, v);
      float s = e <= 0.5f ? (e * e) / 3.0f
                          : (std::exp((e - c) / a) + b) / 12.0f;
      // Scene light -> display light for a 1000 nit peak, diffuse white at 203.
      float peak = 1000.0f;
      float scaled = std::pow(s, 1.2f) * peak;
      return scaled / std::max(1.0f, pqDiffuseWhiteNits);
    }
    case TransferFunction::sRGB:
    default:
      return decodeSrgb(v);
  }
}

}  // namespace iso21496
