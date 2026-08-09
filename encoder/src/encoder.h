// Top-level orchestration: TIFF in, finished ISO 21496-1 JPEG out.
#pragma once

#include <string>

#include "options.h"

namespace iso21496 {

struct EncodeReport {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t gainWidth = 0;
  uint32_t gainHeight = 0;
  int gainChannels = 1;
  size_t primaryBytes = 0;
  size_t gainMapBytes = 0;
  size_t totalBytes = 0;
  float minBoostLog2 = 0.0f;
  float maxBoostLog2 = 0.0f;
  float declaredHeadroom = 0.0f;
  float measuredHeadroom = 0.0f;
  float truePeakHeadroom = 0.0f;
  // The shaping that was applied, and whether it was solved or supplied.
  bool autoShaped = false;
  float sdrLiftEV = 0.0f;
  float sdrContrast = 1.0f;
  float midtoneAnchor = 0.0f;
  // How much of the render sits above SDR white. Reported for auditing a batch
  // rather than used: it is a property of the edit, not something to correct.
  float fractionAboveWhite = 0.0f;
  std::string inputPrimaries;
  std::string inputTransfer;
  double seconds = 0.0;
};

// Runs the whole pipeline and writes options.outputPath.
EncodeReport encodeFile(const EncoderOptions& options);
// Same, but returns the bytes instead of writing them (used by the tests).
Bytes encodeToMemory(const EncoderOptions& options, EncodeReport* report);

}  // namespace iso21496
