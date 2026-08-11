// Writes a synthetic HDR TIFF for the exiftool compliance test and for manual
// experimentation:  make_fixture <out.tif> [width] [height] [peak]
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>

#include "test_support.h"

int ::iso21496::test::g_failures = 0;

int main(int argc, char** argv) {
  using namespace iso21496;
  using namespace iso21496::test;
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: make_fixture <out.tif> [w] [h] [peak] [detail] "
                 "[bits]\n");
    return 2;
  }
  TiffSpec spec;
  spec.width = argc > 2 ? std::atoi(argv[2]) : 640;
  spec.height = argc > 3 ? std::atoi(argv[3]) : 480;
  // 32-bit float by default. Pass 16 for the layout Lightroom actually hands
  // us — its HDR intermediate is 16-bit, so a 32-bit fixture overstates both
  // the file size and the read cost by two, which made an earlier round of
  // export benchmarking measure the wrong thing. Values above 1.0 clip at 16
  // bits, so pair it with a peak of 1.0 unless you want the clipping.
  const int bits = argc > 6 ? std::atoi(argv[6]) : 32;
  spec.bitsPerSample = bits;
  spec.floatSamples = bits == 32;
  spec.rowsPerStrip = 32;
  const float peak = argc > 4 ? static_cast<float>(std::atof(argv[4])) : 8.0f;
  const bool detailed = argc > 5 && std::atoi(argv[5]) != 0;
  auto pixels = detailed ? makeDetailedHdrPattern(spec.width, spec.height, peak)
                         : makeHdrPattern(spec.width, spec.height, peak);
  writeFile(argv[1], makeTiff(spec, pixels));
  return 0;
}
