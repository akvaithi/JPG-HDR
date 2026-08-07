// Colour matrices, transfer functions and ICC generation/inspection.
#include <cstring>

#include "color.h"
#include "icc.h"
#include "test_support.h"

using namespace iso21496;
using namespace iso21496::test;

namespace {

void whitePointsMapToWhite() {
  for (ColorPrimaries p : {ColorPrimaries::sRGB, ColorPrimaries::DisplayP3,
                           ColorPrimaries::Rec2020, ColorPrimaries::ProPhoto,
                           ColorPrimaries::AdobeRGB}) {
    Mat3 m = rgbToXyzD65(primariesFor(p));
    auto xyz = m.apply({1.0, 1.0, 1.0});
    // Every space, after Bradford adaptation, has to land on D65 white.
    CHECK_NEAR(xyz[0], 0.3127 / 0.3290, 2e-3);
    CHECK_NEAR(xyz[1], 1.0, 2e-3);
    CHECK_NEAR(xyz[2], (1.0 - 0.3127 - 0.3290) / 0.3290, 2e-3);
  }
}

void conversionsRoundTrip() {
  Mat3 there = conversionMatrix(ColorPrimaries::ProPhoto, ColorPrimaries::DisplayP3);
  Mat3 back = conversionMatrix(ColorPrimaries::DisplayP3, ColorPrimaries::ProPhoto);
  CHECK((there * back).isIdentity(1e-9));
  CHECK(conversionMatrix(ColorPrimaries::sRGB, ColorPrimaries::sRGB).isIdentity());

  // sRGB is inside P3, so its primaries stay in [0,1] after conversion.
  Mat3 srgbToP3 = conversionMatrix(ColorPrimaries::sRGB, ColorPrimaries::DisplayP3);
  auto red = srgbToP3.apply({1.0, 0.0, 0.0});
  CHECK(red[0] > 0.7 && red[0] <= 1.0001);
  CHECK(red[1] >= -1e-6 && red[1] < 0.3);
}

void transferFunctions() {
  CHECK_NEAR(decodeSrgb(encodeSrgb(0.0f)), 0.0, 1e-6);
  CHECK_NEAR(decodeSrgb(encodeSrgb(1.0f)), 1.0, 1e-6);
  for (float v : {0.001f, 0.02f, 0.18f, 0.5f, 0.9f})
    CHECK_NEAR(decodeSrgb(encodeSrgb(v)), v, 1e-5);

  CHECK_NEAR(decodeTransfer(TransferFunction::Linear, 0.42f, 203.0f), 0.42, 1e-6);
  CHECK_NEAR(decodeTransfer(TransferFunction::Gamma22, 1.0f, 203.0f), 1.0, 1e-6);
  CHECK_NEAR(decodeTransfer(TransferFunction::ROMM, 1.0f, 203.0f), 1.0, 1e-6);
  CHECK_NEAR(decodeTransfer(TransferFunction::ROMM, 0.0f, 203.0f), 0.0, 1e-6);

  // PQ: code value 0.5081 is roughly 100 nits; diffuse white at 203 nits must
  // therefore land near 1.0 for the matching code value.
  float pqWhite = decodeTransfer(TransferFunction::PQ, 0.58f, 203.0f);
  CHECK(pqWhite > 0.7f && pqWhite < 1.4f);
  CHECK_NEAR(decodeTransfer(TransferFunction::PQ, 0.0f, 203.0f), 0.0, 1e-6);
  // Monotonic and unbounded above white.
  CHECK(decodeTransfer(TransferFunction::PQ, 0.9f, 203.0f) >
        decodeTransfer(TransferFunction::PQ, 0.7f, 203.0f));
}

void iccGenerationAndInspection() {
  for (ColorPrimaries p : {ColorPrimaries::sRGB, ColorPrimaries::DisplayP3,
                           ColorPrimaries::Rec2020}) {
    Bytes profile = buildRgbIccProfile(p, "test profile");
    CHECK(profile.size() > 300);
    CHECK_EQ(readU32BE(&profile[0]), static_cast<uint32_t>(profile.size()));
    CHECK(std::memcmp(&profile[36], "acsp", 4) == 0);
    CHECK(std::memcmp(&profile[16], "RGB ", 4) == 0);

    IccSummary s = inspectIccProfile(profile);
    CHECK(s.valid);
    CHECK(s.matched);
    CHECK(s.primaries == p);
    CHECK(s.transfer == TransferFunction::sRGB);
    CHECK_EQ(s.description, std::string("test profile"));
  }

  Bytes gray = buildGrayIccProfile(2.2, "gain map");
  CHECK(std::memcmp(&gray[16], "GRAY", 4) == 0);
  IccSummary gs = inspectIccProfile(gray);
  CHECK(gs.valid);
  CHECK(gs.isGray);

  CHECK(!inspectIccProfile(Bytes{1, 2, 3}).valid);
}

void run() {
  whitePointsMapToWhite();
  conversionsRoundTrip();
  transferFunctions();
  iccGenerationAndInspection();
}

}  // namespace

TEST_MAIN(run)
