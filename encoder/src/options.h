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
  // Every one of these is the measured optimum against Lightroom's own export
  // of the same edit, swept one factor at a time on a fixed render. None is a
  // taste question, which is why none of them is offered in the plug-in.
  //   1:1 vs 1:2       highlight hue drift 0.051 vs 0.099 EV
  //   quality 85       0.051, against 0.055 at 70 and 0.056 at 95
  //   gamma 1.0        0.051, and smaller than any gamma below it
  //   4:4:4 base       0.051 vs 0.078 — the single largest factor, because the
  //                    gain map is measured against the base as written, so
  //                    chroma error in the base lands in the HDR rendition
  int gainMapSubsample = 1;
  // RGB by default. A monochrome map applies one gain to all three channels,
  // so it cannot follow highlights that change hue as they brighten: measured
  // against Lightroom's own export, mono leaves 0.33 EV of channel drift at p95
  // and 0.51 at p99, where RGB leaves 0.12 and 0.21. It is close to free —
  // the chroma planes of a gain map are nearly flat, so on the reference frame
  // three channels cost 1.8% more file than one.
  bool multiChannelGainMap = true;

  // Describe the gain map in Apple's XMP namespaces as well as the standard
  // ones, which is what lets it survive an iMessage send — that pipeline
  // re-encodes the file, discards every ISO 21496-1 segment, and rebuilds from
  // what it recognises. Only works on a single channel map: a three channel one
  // declared as single channel was tested and arrived flattened, so this turns
  // multiChannelGainMap off rather than pretending. That costs 0.25 EV in
  // saturated highlights and 0.61 EV of hue drift, on 10% of the reference
  // frame, and buys the photo arriving as HDR at all.
  bool appleCompatible = false;
  int quality = 90;
  int gainMapQuality = 85;

  // 1.0, i.e. no curve, which is also Ultra HDR's default. An exponent below 1
  // spends codes on the low gains, but the gain map is itself a JPEG, and the
  // extra contrast that exponent creates costs more in compression artefacts
  // than the code distribution wins back. Measured, not assumed.
  float gainMapGamma = 1.0f;
  float offsetSdr = 1.0f / 64.0f;
  float offsetHdr = 1.0f / 64.0f;

  ColorPrimaries inputPrimaries = ColorPrimaries::Auto;
  TransferFunction inputTransfer = TransferFunction::Auto;
  float pqDiffuseWhiteNits = 203.0f;
  ToneMapOperator toneMap = ToneMapOperator::Local;
  float sdrDetail = 1.25f;
  float sdrKnee = 0.0f;  // 0 = derive from the headroom
  float sdrEdge = 4.0f;
  bool autoMaxBoost = true;
  PeakDetect peakDetect = PeakDetect::Softened;
  // Solved from the image unless --sdr-lift or --sdr-contrast is given, which
  // switches to Manual: passing a number means you want that number.
  SdrShapeMode sdrShape = SdrShapeMode::Auto;
  float sdrLiftEV = 0.43f;
  float sdrContrast = 1.14f;

  bool writeIcc = true;
  bool writeExif = true;
  bool writeXmp = true;
  bool optimizeHuffman = true;
  bool chromaSubsample = false;  // 4:4:4; see gainMapSubsample
  unsigned threads = 0;
  bool json = false;
};

// Returns false and prints to stdout when --help or --version was requested.
bool parseArguments(int argc, char** argv, EncoderOptions* out, bool* handled);
const char* usageText();

}  // namespace iso21496
