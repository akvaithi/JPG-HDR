// Command-line surface. The Lightroom plugin builds these arguments, so the
// names here are part of the contract between the two components.
#pragma once

#include <string>

#include "color.h"
#include "pipeline.h"

namespace iso21496 {

struct EncoderOptions {
  std::string inputPath;
  std::string outputPath;

  float targetHeadroom = 4.0f;
  ColorPrimaries outputPrimaries = ColorPrimaries::DisplayP3;
  int gainMapSubsample = 2;
  bool multiChannelGainMap = false;
  int quality = 90;
  // Gain maps are smooth luminance ratios, so JPEG artefacts in them are not
  // visually significant the way they are in the picture; 50 is what current
  // camera pipelines use and it costs roughly a third the bytes of 85.
  int gainMapQuality = 50;

  float gainMapGamma = 2.2f;
  float offsetSdr = 1.0f / 64.0f;
  float offsetHdr = 1.0f / 64.0f;

  ColorPrimaries inputPrimaries = ColorPrimaries::Auto;
  TransferFunction inputTransfer = TransferFunction::Auto;
  float pqDiffuseWhiteNits = 203.0f;
  ToneMapOperator toneMap = ToneMapOperator::Reinhard;
  bool autoMaxBoost = true;
  PeakDetect peakDetect = PeakDetect::Softened;
  float sdrLiftEV = 0.43f;
  float sdrContrast = 1.14f;

  bool writeIcc = true;
  bool writeExif = true;
  bool writeXmp = true;
  bool optimizeHuffman = true;
  bool chromaSubsample = true;
  unsigned threads = 0;
  bool json = false;
};

// Returns false and prints to stdout when --help or --version was requested.
bool parseArguments(int argc, char** argv, EncoderOptions* out, bool* handled);
const char* usageText();

}  // namespace iso21496
