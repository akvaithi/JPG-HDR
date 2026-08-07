// TIFF reader: byte orders, bit depths, strip layouts and metadata pickup.
#include "test_support.h"
#include "tiff_reader.h"

using namespace iso21496;
using namespace iso21496::test;

namespace {

void roundTrip(bool littleEndian, int bits, bool isFloat, uint32_t rowsPerStrip) {
  TiffSpec spec;
  spec.width = 37;  // deliberately not a multiple of anything
  spec.height = 21;
  spec.channels = 3;
  spec.bitsPerSample = bits;
  spec.floatSamples = isFloat;
  spec.littleEndian = littleEndian;
  spec.rowsPerStrip = rowsPerStrip;
  auto pixels = makeHdrPattern(spec.width, spec.height, isFloat ? 8.0f : 1.0f);
  TiffReader r(makeTiff(spec, pixels));

  CHECK_EQ(r.width(), spec.width);
  CHECK_EQ(r.height(), spec.height);
  CHECK_EQ(r.channels(), 3u);
  CHECK_EQ(static_cast<int>(r.bitsPerSample()), bits);
  CHECK_EQ(r.isFloat(), isFloat);

  std::vector<float> got(static_cast<size_t>(spec.width) * spec.height * 3);
  r.readRows(0, spec.height, got.data());
  const double tol = isFloat ? 1e-6 : (bits == 16 ? 2e-5 : 3e-3);
  for (size_t i = 0; i < got.size(); ++i) {
    float expected = pixels[i];
    if (!isFloat) expected = std::min(1.0f, std::max(0.0f, expected));
    CHECK_NEAR(got[i], expected, tol);
  }
}

void partialReads() {
  TiffSpec spec;
  spec.width = 20;
  spec.height = 30;
  spec.rowsPerStrip = 7;
  auto pixels = makeHdrPattern(spec.width, spec.height, 1.0f);
  TiffReader r(makeTiff(spec, pixels));
  CHECK_EQ(r.suggestedBandRows(), 7u);

  std::vector<float> band(static_cast<size_t>(spec.width) * 5 * 3);
  r.readRows(11, 5, band.data());
  for (uint32_t y = 0; y < 5; ++y) {
    for (uint32_t x = 0; x < spec.width * 3; ++x) {
      size_t src = (static_cast<size_t>(11 + y) * spec.width) * 3 + x;
      size_t dst = (static_cast<size_t>(y) * spec.width) * 3 + x;
      CHECK_NEAR(band[dst], std::min(1.0f, pixels[src]), 2e-5);
    }
  }
}

void greyscaleAndMetadata() {
  TiffSpec spec;
  spec.width = 8;
  spec.height = 8;
  spec.channels = 1;
  spec.iccProfile = Bytes{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  std::vector<float> pixels(64, 0.5f);
  TiffReader r(makeTiff(spec, pixels));
  CHECK_EQ(r.channels(), 1u);
  CHECK_EQ(r.metadata().iccProfile.size(), size_t{10});
  CHECK_EQ(r.metadata().iccProfile[9], uint8_t{10});
  std::vector<float> got(64);
  r.readRows(0, 8, got.data());
  CHECK_NEAR(got[0], 0.5, 2e-5);
}

void rejectsGarbage() {
  CHECK_THROWS(TiffReader(Bytes{0, 1, 2, 3, 4, 5, 6, 7, 8}));
  CHECK_THROWS(TiffReader(Bytes{'I', 'I', 43, 0, 8, 0, 0, 0}));
}

void run() {
  roundTrip(true, 16, false, 0);
  roundTrip(false, 16, false, 0);
  roundTrip(true, 8, false, 4);
  roundTrip(true, 32, true, 3);
  roundTrip(false, 32, true, 0);
  partialReads();
  greyscaleAndMetadata();
  rejectsGarbage();
}

}  // namespace

TEST_MAIN(run)
