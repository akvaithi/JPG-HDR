// The pixel pipeline: linear HDR TIFF in, 8-bit SDR base image plus a
// quantised gain map out.
#pragma once

#include <vector>

#include "color.h"
#include "common.h"
#include "tiff_reader.h"

namespace iso21496 {

enum class ToneMapOperator { Reinhard, Filmic, Clip };
bool parseToneMap(const std::string& s, ToneMapOperator* out);
const char* toneMapName(ToneMapOperator t);

struct PipelineOptions {
  ColorPrimaries outputPrimaries = ColorPrimaries::DisplayP3;
  ColorPrimaries inputPrimaries = ColorPrimaries::Auto;
  TransferFunction inputTransfer = TransferFunction::Auto;
  float pqDiffuseWhiteNits = 203.0f;
  float targetHeadroom = 4.0f;   // log2 stops above SDR white
  int gainMapSubsample = 2;      // 1, 2 or 4
  bool multiChannelGainMap = false;
  float gainMapGamma = 2.2f;
  float offsetSdr = 1.0f / 64.0f;
  float offsetHdr = 1.0f / 64.0f;
  ToneMapOperator toneMap = ToneMapOperator::Reinhard;
  bool autoMaxBoost = true;      // shrink max boost to what the image needs
  unsigned threads = 0;
};

struct PipelineResult {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> sdr;       // width*height*3, sRGB-encoded
  uint32_t gainWidth = 0;
  uint32_t gainHeight = 0;
  int gainChannels = 1;
  std::vector<uint8_t> gain;      // gainWidth*gainHeight*gainChannels
  // Metadata values that end up in the ISO 21496-1 payload.
  float minBoostLog2[3] = {0, 0, 0};
  float maxBoostLog2[3] = {0, 0, 0};
  // Diagnostics for --verbose / --json.
  ColorPrimaries resolvedInputPrimaries = ColorPrimaries::sRGB;
  TransferFunction resolvedInputTransfer = TransferFunction::sRGB;
  float measuredHeadroom = 0.0f;
};

PipelineResult runPipeline(const TiffReader& tiff, const PipelineOptions& opts);

}  // namespace iso21496
