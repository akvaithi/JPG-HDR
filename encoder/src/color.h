// Colour primaries, transfer functions and the 3x3 plumbing between them.
#pragma once

#include <array>
#include <string>

#include "common.h"

namespace iso21496 {

struct Mat3 {
  double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

  static Mat3 identity() { return Mat3{}; }
  Mat3 operator*(const Mat3& o) const;
  std::array<double, 3> apply(const std::array<double, 3>& v) const;
  Mat3 inverse() const;
  bool isIdentity(double eps = 1e-6) const;
};

// CIE xy chromaticities of the three primaries plus the white point.
struct Primaries {
  double rx, ry, gx, gy, bx, by, wx, wy;
};

enum class ColorPrimaries { Auto, sRGB, DisplayP3, Rec2020, ProPhoto, AdobeRGB };
enum class TransferFunction { Auto, Linear, sRGB, Gamma18, Gamma22, ROMM, PQ, HLG };

// Working space for all internal maths: linear Rec.2020 primaries, D65 white.
// Wide enough to hold ProPhoto and P3 gamuts without clipping.
Primaries primariesFor(ColorPrimaries p);
const char* primariesName(ColorPrimaries p);
const char* transferName(TransferFunction t);
bool parsePrimaries(const std::string& s, ColorPrimaries* out);
bool parseTransfer(const std::string& s, TransferFunction* out);

// RGB->XYZ for the given primaries, then Bradford-adapted to D65.
Mat3 rgbToXyzD65(const Primaries& p);
Mat3 conversionMatrix(ColorPrimaries from, ColorPrimaries to);
// Bradford adaptation from the D65 working white to the ICC PCS white (D50).
Mat3 adaptD65ToD50();

// Decodes an encoded value to linear light where 1.0 is SDR diffuse white.
// `pqDiffuseWhiteNits` only matters for PQ/HLG.
float decodeTransfer(TransferFunction t, float v, float pqDiffuseWhiteNits);
// Encodes linear [0,1] to the display-referred value for the output profile.
float encodeSrgb(float linear);
float decodeSrgb(float encoded);

}  // namespace iso21496
