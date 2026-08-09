// The pixel pipeline: linear HDR TIFF in, 8-bit SDR base image plus a
// quantised gain map out.
#pragma once

#include <vector>

#include "color.h"
#include "common.h"
#include "tiff_reader.h"

namespace iso21496 {

// `Local` is the default and the only one that is not a curve. A curve maps
// every pixel of a given luminance to the same output, so the SDR base it makes
// can differ from the HDR image by brightness and contrast but never by
// *depth*: whatever separation the compression takes out of the highlights is
// gone. `Local` splits the image into a smooth base and a detail layer,
// compresses only the base, and puts the detail back at full strength — so the
// tonal separation inside the highlights survives being fitted into 8 bits.
enum class ToneMapOperator { Local, Reinhard, Filmic, Clip };
bool parseToneMap(const std::string& s, ToneMapOperator* out);
const char* toneMapName(ToneMapOperator t);

// How the image's peak highlight is measured. `Softened` averages the image
// down first, so a handful of hot pixels cannot define the headroom for the
// whole frame; `Exact` uses the true per-pixel maximum.
enum class PeakDetect { Softened, Exact };
bool parsePeakDetect(const std::string& s, PeakDetect* out);
const char* peakDetectName(PeakDetect p);

// Where the SDR base image's brightness lift and contrast come from. `Auto`
// solves both from the image's own luminance distribution, which is what the
// right answer depends on: how much the tone curve darkened *this* photo.
// `Manual` uses the numbers the caller supplied.
enum class SdrShapeMode { Auto, Manual };

struct PipelineOptions {
  ColorPrimaries outputPrimaries = ColorPrimaries::DisplayP3;
  ColorPrimaries inputPrimaries = ColorPrimaries::Auto;
  TransferFunction inputTransfer = TransferFunction::Auto;
  float pqDiffuseWhiteNits = 203.0f;
  float targetHeadroom = 4.0f;   // log2 stops above SDR white
  int gainMapSubsample = 1;      // 1, 2 or 4
  bool multiChannelGainMap = true;
  float gainMapGamma = 1.0f;
  float offsetSdr = 1.0f / 64.0f;
  float offsetHdr = 1.0f / 64.0f;
  ToneMapOperator toneMap = ToneMapOperator::Local;
  // Local tone mapping only. How much of the detail layer is put back on top of
  // the compressed base. 1.0 restores it exactly; above that is a deliberate
  // local-contrast boost, and 1.25 is what it takes for the base to carry more
  // tonal separation than a straight SDR export of the same edit rather than
  // merely as much — measured against Lightroom's, 0.396 against 0.343 RMS of
  // log2 detail through the upper midtones and highlights.
  float sdrDetail = 1.25f;
  // Where the shoulder starts, in stops below SDR white. 0 means "derive it
  // from the headroom". More negative compresses more of the picture, which is
  // what makes room for the detail layer to read as depth.
  float sdrKnee = 0.0f;
  // Where the shoulder on the composite SDR value starts. 1.0 disables it and
  // restores the hard clip.
  //
  // A constant, and that is a result rather than a default left unexamined.
  // Sixteen scenes were laddered from 0.95 to 0.35 and rated by eye; the picks
  // spanned 0.65 to 0.95 with none below, and 0.80 is where they sit best.
  //
  // A per-image solver was fitted and rejected. Scene brightness does correlate
  // with the preferred knee — median luminance gives r = -0.55 across the
  // sixteen, in the sensible direction — but held four scenes out and refitted
  // three thousand times, no model of any one or two of the nine measured
  // predictors beat this constant: the best managed 0.0912 mean absolute error
  // against 0.0889 for simply using 0.80.
  //
  // The reason is in the data rather than in the fit. _DSC5343 and _DSC5651
  // have median luminances of 0.642 and 0.666 and were rated at opposite ends
  // of the ladder, 0.95 and 0.65. No global statistic separates those, and the
  // rating that explains why was "parts of one are better and parts of another
  // are better" — the preference varies within a frame, not just between
  // frames. A shoulder driven by the local base layer could act on that; one
  // number chosen per image cannot, however it is chosen.
  float sdrHighlightKnee = 0.80f;
  // Guided-filter edge threshold, in stops squared. Larger smooths through more
  // texture, leaving more of it in the detail layer to be handed back.
  float sdrEdge = 4.0f;
  bool autoMaxBoost = true;      // shrink max boost to what the image needs
  PeakDetect peakDetect = PeakDetect::Softened;

  // Shaping applied to the SDR base image only. The gain map is measured
  // against whatever base these produce, so the HDR rendition is unaffected;
  // what changes is the fallback seen without an HDR display.
  SdrShapeMode sdrShape = SdrShapeMode::Auto;
  float sdrLiftEV = 0.43f;     // Manual only; 0 disables
  float sdrContrast = 1.14f;   // Manual only; 1.0 disables


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
  // The headroom the file declares it needs. This is the measured requirement,
  // not the user's ceiling: a decoder scales the gain it applies by
  // display_headroom / declared_headroom, so declaring more than the image
  // needs makes it render dim on any display with partial headroom.
  float declaredHeadroom = 0.0f;
  // Diagnostics for --verbose / --json.
  ColorPrimaries resolvedInputPrimaries = ColorPrimaries::sRGB;
  TransferFunction resolvedInputTransfer = TransferFunction::sRGB;
  float measuredHeadroom = 0.0f;   // softened peak, in stops
  float truePeakHeadroom = 0.0f;   // true per-pixel peak, in stops
  // The shaping actually applied, whether it was solved or supplied.
  float sdrLiftEV = 0.0f;
  float sdrContrast = 1.0f;
  float midtoneAnchor = 0.0f;      // luminance the lift was solved at
  float fractionAboveWhite = 0.0f;  // share of the render above SDR white
};

PipelineResult runPipeline(const TiffReader& tiff, const PipelineOptions& opts);

// The solved SDR base shaping. Exposed so the solver can be driven with a
// known luminance distribution rather than a synthesised image: what it does
// is a function of where a photo's tones sit, and that is far easier to state
// as a histogram than to arrange in pixels.
struct SdrShaping {
  float liftEV = 0.0f;
  float contrast = 1.0f;
};
SdrShaping solveSdrShaping(const float* luminances, size_t count,
                           const PipelineOptions& opts, float maxBoost);

// The local operator's shoulder, exposed for the same reason: its shape is what
// has to be asserted, and stating it as a function is far clearer than
// reconstructing it from a rendered image. `b` and the result are log2
// luminance relative to SDR white.
float compressBaseForTest(float b, float kneeStart, float maxLog);
// The highlight shoulder, as the adjusted uniform scale it produces. Exposed so
// a test can assert the property that matters — that it is a scale, and so
// cannot move the channels relative to each other — without depending on a
// rendered frame happening to contain a saturated highlight.
float highlightScaleForTest(const float rgb[3], float scale, float knee);
float clampf(float v, float lo, float hi);

}  // namespace iso21496
