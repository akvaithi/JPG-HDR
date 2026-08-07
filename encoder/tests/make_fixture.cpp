// Writes a synthetic HDR TIFF for the exiftool compliance test and for manual
// experimentation:  make_fixture <out.tif> [width] [height] [peak]
#include <cstdlib>
#include <cstdio>
#include <string>

#include "test_support.h"

int ::iso21496::test::g_failures = 0;

int main(int argc, char** argv) {
  using namespace iso21496;
  using namespace iso21496::test;
  if (argc < 2) {
    std::fprintf(stderr, "usage: make_fixture <out.tif> [w] [h] [peak]\n");
    return 2;
  }
  TiffSpec spec;
  spec.width = argc > 2 ? std::atoi(argv[2]) : 640;
  spec.height = argc > 3 ? std::atoi(argv[3]) : 480;
  spec.bitsPerSample = 32;
  spec.floatSamples = true;
  spec.rowsPerStrip = 32;
  const float peak = argc > 4 ? static_cast<float>(std::atof(argv[4])) : 8.0f;
  writeFile(argv[1], makeTiff(spec, makeHdrPattern(spec.width, spec.height, peak)));
  return 0;
}
