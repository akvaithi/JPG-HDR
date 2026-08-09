#include "pipeline.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

#include "icc.h"
#include "threads.h"

namespace iso21496 {

float clampf(float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); }

namespace {

// Rec.2020 luminance weights; we do all tone mapping in the output space, so
// the weights are derived from that space's primaries at runtime instead.
std::array<double, 3> luminanceWeights(ColorPrimaries p) {
  Mat3 m = rgbToXyzD65(primariesFor(p));
  return {m.m[1][0], m.m[1][1], m.m[1][2]};
}

float toneMapLuminance(ToneMapOperator op, float l, float lmax) {
  if (l <= 0.0f) return 0.0f;
  switch (op) {
    case ToneMapOperator::Clip:
      return std::min(l, 1.0f);
    case ToneMapOperator::Filmic: {
      // Hable's filmic curve, normalised so that `lmax` maps to exactly 1.
      auto hable = [](float x) {
        constexpr float A = 0.15f, B = 0.50f, C = 0.10f, D = 0.20f, E = 0.02f,
                        F = 0.30f;
        return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) -
               E / F;
      };
      float denom = hable(lmax);
      if (denom <= 0.0f) return std::min(l, 1.0f);
      return std::min(1.0f, hable(l) / denom);
    }
    case ToneMapOperator::Reinhard:
    default: {
      // Extended Reinhard: identity-ish in the shadows, and l == lmax maps to
      // exactly 1.0 so nothing clips and nothing is left on the table.
      float k = 1.0f + l / (lmax * lmax);
      return std::min(1.0f, l * k / (1.0f + l));
    }
  }
}

// Everything that turns an HDR luminance into the SDR base image, in one
// place: the tone curve, the lift, and the contrast shaping. Used by the pixel
// loop and by the analytic gain-floor sweep, so the two cannot drift apart.
struct SdrShaper {
  ToneMapOperator op = ToneMapOperator::Reinhard;
  float toneCeiling = 16.0f;  // linear; the headroom the tone curve maps to 1.0
  float liftGain = 1.0f;      // 2^sdrLiftEV
  float contrast = 1.0f;
  float pivotScale = 1.0f;    // pivot^(1-contrast), precomputed

  // The tone-mapped, lifted, contrast-shaped luminance, before clamping.
  float shapedLuminance(float lHdr) const {
    float v = toneMapLuminance(op, lHdr, toneCeiling) * liftGain;
    if (std::fabs(contrast - 1.0f) <= 1e-4f) return v;
    if (v <= 0.0f) return 0.0f;
    return std::pow(v, contrast) * pivotScale;
  }

  // The uniform factor applied to linear RGB. Scaling all three channels by
  // the same number is what keeps chromaticity intact: a per-channel power law
  // would spread the channel ratios apart and silently raise saturation.
  float rgbScale(float lHdr) const {
    if (lHdr <= 1e-8f) return 0.0f;
    return shapedLuminance(lHdr) / lHdr;
  }
};

SdrShaper makeShaper(const PipelineOptions& o, float maxBoost, float liftEV,
                     float contrast) {
  SdrShaper s;
  s.op = o.toneMap;
  s.toneCeiling = std::max(1.0001f, std::pow(2.0f, maxBoost));
  // The lift is an explicit exposure gain on the tone-mapped result, not a
  // reduced tone-curve ceiling. Reducing the ceiling is how a Reinhard-with-
  // white-point curve is usually lifted, but its `1 + L/ceiling^2` term is
  // within a rounding error of 1 through the midtones: measured, shrinking the
  // ceiling by 0.43 EV moved mid grey by 0.001 EV. An exposure gain does what
  // it says — +0.43 EV means +0.43 EV at mid grey — and produces the same
  // end result, since highlights pushed past 1.0 clip in the 8-bit base and
  // are handed back by the gain map, which is measured per pixel against the
  // base as actually written.
  s.liftGain = std::pow(2.0f, std::max(0.0f, liftEV));
  s.contrast = contrast;
  constexpr float kPivot = 0.18f;  // linear-light mid grey
  s.pivotScale = std::pow(kPivot, 1.0f - contrast);
  return s;
}

// ---------------------------------------------------------------------------
// Solving the SDR base shaping from the image
//
// The lift and contrast exist to undo what the tone curve did on its way to an
// 8-bit base, and how much that is depends entirely on where this particular
// photo's tones sit. A fixed pair of numbers is therefore wrong for almost
// every picture: measured over the sweep in `solveShaping`, the lift a frame
// actually wants ranges from nothing at all on a night shot — where the curve
// is already the identity — to about a stop on a high-key one. That spread is
// why these were the two controls hardest to set by hand.
// ---------------------------------------------------------------------------

// A log2-luminance histogram of the block-averaged image. Block averages rather
// than raw pixels on purpose: it is the same softened downscale the headroom
// measurement uses, so noise cannot drag the percentiles around, and it costs
// one bin increment per block instead of one per pixel.
constexpr int kHistBins = 384;
constexpr float kHistLo = -14.0f;
constexpr float kHistHi = 10.0f;
constexpr float kHistScale = kHistBins / (kHistHi - kHistLo);

int histBin(float luminance) {
  const int bin = static_cast<int>(
      std::lround((std::log2(luminance) - kHistLo) * kHistScale));
  return std::min(kHistBins - 1, std::max(0, bin));
}

// Luminance below which the given fraction of the image lies.
float histPercentile(const std::vector<uint64_t>& hist, uint64_t total,
                     float fraction) {
  if (total == 0) return 0.0f;
  const uint64_t want = static_cast<uint64_t>(fraction * static_cast<double>(total));
  uint64_t seen = 0;
  for (int i = 0; i < kHistBins; ++i) {
    seen += hist[i];
    if (seen >= want)
      return std::pow(2.0f, kHistLo + static_cast<float>(i) / kHistScale);
  }
  return std::pow(2.0f, kHistHi);
}

// The share of the image above a given luminance. The inverse question to
// histPercentile, and the one worth reporting: how much of this frame did the
// edit put above SDR white.
double histFractionAbove(const std::vector<uint64_t>& hist, uint64_t total,
                         float luminance) {
  if (total == 0) return 0.0;
  const int from = histBin(luminance) + 1;
  uint64_t above = 0;
  for (int i = std::min(kHistBins, std::max(0, from)); i < kHistBins; ++i)
    above += hist[i];
  return static_cast<double>(above) / static_cast<double>(total);
}

struct AutoShape {
  float liftEV = 0.0f;
  float contrast = 1.0f;
  float anchor = 0.0f;
};


// How much of the midtone contrast the tone curve swallowed to hand back.
//
// Not 1.0, and this is the one number here that is a judgement rather than a
// measurement. Restoring the spread in full makes the base an identity below
// SDR white, which is the same thing as `--tone-map clip`: it removes the
// shoulder and drives highlights harder into the clip the gain map then has to
// undo. The value is pinned to the hand-tuned defaults this encoder shipped
// with, which were arrived at by eye on real photographs: on a frame with the
// median luminance those were chosen against, full restoration solves to 1.35
// and the shipped default was 1.14, i.e. (1.14 - 1) / (1.35 - 1) = 0.4. The
// lift carries no such factor — undoing a darkening has no shoulder to lose.
constexpr float kContrastRestore = 0.4f;

AutoShape solveShaping(const std::vector<uint64_t>& hist, uint64_t total,
                       const PipelineOptions& o, float maxBoost) {
  AutoShape s;
  if (total == 0) return s;

  // The bare tone curve, with no shaping on top: what the base would be if
  // nothing corrected it.
  const SdrShaper curve = makeShaper(o, maxBoost, 0.0f, 1.0f);

  // Contrast first. The curve flattens midtone separation — a Reinhard's
  // log-log slope is 1/(1+L), so it is compressing tones that are nowhere near
  // needing it — and the exponent that undoes that is the ratio of the two
  // interquartile spreads. In log space the exponent scales the spread
  // linearly, so this is exact rather than fitted, and it is independent of the
  // lift, which is a pure offset in the same space. That independence is why
  // this has to be solved before the lift and not after.
  const float lo = std::min(1.0f, histPercentile(hist, total, 0.25f));
  const float hi = std::min(1.0f, histPercentile(hist, total, 0.75f));
  if (lo > 0.0f && hi > lo) {
    const float sLo = curve.shapedLuminance(lo);
    const float sHi = curve.shapedLuminance(hi);
    if (sLo > 0.0f && sHi > sLo) {
      const float got = std::log2(sHi / sLo);
      if (got > 1e-4f) {
        const float ideal = std::log2(hi / lo) / got;
        s.contrast = 1.0f + kContrastRestore * (ideal - 1.0f);
      }
    }
  }
  s.contrast = clampf(s.contrast, 1.0f, 1.5f);

  // Lift, solved *through* that contrast so the two do not fight: the contrast
  // pivots about mid grey, which moves the anchor unless it is solved for
  // afterwards. Below SDR white the curve is darkening luminance that never
  // needed compressing, so putting the median back where it started corrects
  // exactly that error and nothing more — which is what makes it safe to apply
  // to a photograph nobody has looked at.
  //
  //   shaped(a) = (T(a) * lift)^c * pivot^(1-c) = a
  //     =>  lift = (a / pivot^(1-c))^(1/c) / T(a)
  const float median = histPercentile(hist, total, 0.5f);
  s.anchor = median;
  const float anchor = std::min(1.0f, median);  // above white it is clipped anyway
  const float toneMapped = anchor > 0.0f ? curve.shapedLuminance(anchor) : 0.0f;
  if (toneMapped > 0.0f) {
    constexpr float kPivot = 0.18f;
    const float pivotScale = std::pow(kPivot, 1.0f - s.contrast);
    s.liftEV = std::log2(std::pow(anchor / pivotScale, 1.0f / s.contrast) /
                         toneMapped);
  }

  // The base is a fallback, not the picture: it is never worth letting the
  // solve run away on an unusual histogram.
  s.liftEV = clampf(s.liftEV, 0.0f, 1.5f);
  s.contrast = clampf(s.contrast, 1.0f, 1.5f);
  return s;
}

struct Resolved {
  ColorPrimaries primaries;
  TransferFunction transfer;
};

Resolved resolveInputSpace(const TiffReader& tiff, const PipelineOptions& o) {
  Resolved r{ColorPrimaries::ProPhoto, TransferFunction::ROMM};
  IccSummary icc;
  if (!tiff.metadata().iccProfile.empty())
    icc = inspectIccProfile(tiff.metadata().iccProfile);

  if (o.inputPrimaries != ColorPrimaries::Auto) {
    r.primaries = o.inputPrimaries;
  } else if (icc.valid && icc.matched) {
    r.primaries = icc.primaries;
  } else if (tiff.isFloat()) {
    // 32-bit float intermediates from Lightroom are linear ProPhoto.
    r.primaries = ColorPrimaries::ProPhoto;
  }

  if (o.inputTransfer != TransferFunction::Auto) {
    r.transfer = o.inputTransfer;
  } else if (tiff.isFloat()) {
    r.transfer = TransferFunction::Linear;
  } else if (icc.valid && icc.transfer != TransferFunction::Auto) {
    // ProPhoto's ICC advertises gamma 1.8; the real curve has a linear toe.
    r.transfer = (icc.transfer == TransferFunction::Gamma18 &&
                  r.primaries == ColorPrimaries::ProPhoto)
                     ? TransferFunction::ROMM
                     : icc.transfer;
  } else if (r.primaries == ColorPrimaries::ProPhoto) {
    r.transfer = TransferFunction::ROMM;
  } else {
    r.transfer = TransferFunction::sRGB;
  }
  return r;
}

}  // namespace

bool parseToneMap(const std::string& s, ToneMapOperator* out) {
  if (s == "local") *out = ToneMapOperator::Local;
  else if (s == "reinhard") *out = ToneMapOperator::Reinhard;
  else if (s == "filmic") *out = ToneMapOperator::Filmic;
  else if (s == "clip" || s == "none") *out = ToneMapOperator::Clip;
  else return false;
  return true;
}

bool parsePeakDetect(const std::string& s, PeakDetect* out) {
  if (s == "softened" || s == "soft") *out = PeakDetect::Softened;
  else if (s == "exact" || s == "peak") *out = PeakDetect::Exact;
  else return false;
  return true;
}

const char* peakDetectName(PeakDetect p) {
  return p == PeakDetect::Exact ? "exact" : "softened";
}

// ---------------------------------------------------------------------------
// Local tone mapping
//
// Everything above is a curve, and a curve cannot add depth. Whatever tonal
// separation the compression takes out of the highlights is simply gone, which
// is why a curve-based base image reads as a brighter or darker version of the
// same flat picture rather than as the photograph.
//
// So: split log luminance into a smooth base and the detail that rides on it,
// compress only the base, and add the detail back untouched. Highlights are
// pulled into range while the separation inside them survives.
//
// The split is a self-guided filter (He, Sun, Tang) rather than a Gaussian
// blur, because a blur straddles high-contrast edges and leaves a halo along
// every one of them. It runs on a downscale of roughly a 1024-pixel long edge:
// the base layer is low frequency by definition, so computing it at full
// resolution would cost hundreds of megabytes on a 45 MP frame to produce the
// same answer.
// ---------------------------------------------------------------------------

struct Plane {
  uint32_t w = 0, h = 0;
  std::vector<float> v;
  Plane() = default;
  Plane(uint32_t width, uint32_t height) : w(width), h(height),
                                           v(static_cast<size_t>(width) * height, 0.0f) {}
  float& at(uint32_t x, uint32_t y) { return v[static_cast<size_t>(y) * w + x]; }
  float at(uint32_t x, uint32_t y) const { return v[static_cast<size_t>(y) * w + x]; }
};

// Separable box blur with a running sum: O(1) per pixel regardless of radius.
Plane boxBlur(const Plane& src, int radius) {
  Plane tmp(src.w, src.h), out(src.w, src.h);
  const int r = std::max(1, radius);
  for (uint32_t y = 0; y < src.h; ++y) {
    double sum = 0.0;
    int count = 0;
    for (int x = -r; x <= r; ++x) {
      sum += src.at(static_cast<uint32_t>(std::min<int>(src.w - 1, std::max(0, x))), y);
      ++count;
    }
    for (uint32_t x = 0; x < src.w; ++x) {
      tmp.at(x, y) = static_cast<float>(sum / count);
      const int add = std::min<int>(src.w - 1, static_cast<int>(x) + r + 1);
      const int drop = std::max(0, static_cast<int>(x) - r);
      sum += src.at(static_cast<uint32_t>(add), y);
      sum -= src.at(static_cast<uint32_t>(drop), y);
    }
  }
  for (uint32_t x = 0; x < src.w; ++x) {
    double sum = 0.0;
    int count = 0;
    for (int y = -r; y <= r; ++y) {
      sum += tmp.at(x, static_cast<uint32_t>(std::min<int>(src.h - 1, std::max(0, y))));
      ++count;
    }
    for (uint32_t y = 0; y < src.h; ++y) {
      out.at(x, y) = static_cast<float>(sum / count);
      const int add = std::min<int>(src.h - 1, static_cast<int>(y) + r + 1);
      const int drop = std::max(0, static_cast<int>(y) - r);
      sum += tmp.at(x, static_cast<uint32_t>(add));
      sum -= tmp.at(x, static_cast<uint32_t>(drop));
    }
  }
  return out;
}

// Self-guided filter: smooths within regions and holds still across edges.
// `eps` is in the units of the input squared — log2 luminance here — so it is
// literally "variance below this many stops counts as texture, not an edge".
Plane guidedFilter(const Plane& in, int radius, float eps) {
  Plane sq(in.w, in.h);
  for (size_t i = 0; i < in.v.size(); ++i) sq.v[i] = in.v[i] * in.v[i];

  const Plane mean = boxBlur(in, radius);
  const Plane meanSq = boxBlur(sq, radius);

  Plane a(in.w, in.h), b(in.w, in.h);
  for (size_t i = 0; i < in.v.size(); ++i) {
    const float var = std::max(0.0f, meanSq.v[i] - mean.v[i] * mean.v[i]);
    a.v[i] = var / (var + eps);       // ~1 across an edge, ~0 inside a flat area
    b.v[i] = mean.v[i] * (1.0f - a.v[i]);
  }
  const Plane meanA = boxBlur(a, radius);
  const Plane meanB = boxBlur(b, radius);

  Plane out(in.w, in.h);
  for (size_t i = 0; i < in.v.size(); ++i)
    out.v[i] = meanA.v[i] * in.v[i] + meanB.v[i];
  return out;
}

// Where compression starts, in stops below SDR white. Below this the base is
// left exactly alone, so the diffuse midtones of the picture come through at
// the luminance the render gave them; above it the remaining SDR range absorbs
// everything up to the peak.
float kneeStartFor(float maxBoost) {
  // Measured on Lightroom's own HDR export of a +2.30 EV edit: at -1.15 the
  // shoulder barely engages, because a photograph's midtones already sit below
  // SDR white and only the highlights need folding in. Taking it to about -2
  // puts the upper midtones through the shoulder too, which is what leaves room
  // for the detail layer to read as depth rather than as brightness.
  return -clampf(0.85f * maxBoost, 1.0f, 2.5f);
}

// Compresses the base layer into the SDR range: identity up to the knee, then a
// smooth shoulder that lands the peak exactly on white.
float compressBase(float b, float kneeStart, float maxLog) {
  if (b <= kneeStart) return b;
  const float span = -kneeStart;           // stops of SDR range above the knee
  const float range = maxLog - kneeStart;  // stops of HDR to fold into it
  if (span <= 1e-4f || range <= 1e-4f) return std::min(b, 0.0f);
  // Slope exactly 1 at the knee, decaying from there, so the shoulder joins the
  // identity smoothly and never rises above it.
  //
  // Deliberately *not* normalised to land the peak exactly on white. Scaling
  // this to reach white would push the slope above 1 just past the knee — the
  // average slope over the shoulder is span/range < 1, so hitting the endpoint
  // while starting at 1 requires exceeding 1 somewhere — and the base would
  // then be brighter than the HDR image there. That would cost the exactly-zero
  // gain floor this operator gets for free, to buy back the last 2.5% of
  // range. The peak lands a few hundredths of a stop under white instead.
  const float k = range / span;            // the compression ratio at the knee
  const float t = clampf((b - kneeStart) / range, 0.0f, 1.0f);
  return kneeStart + span * (1.0f - std::exp(-k * t));
}

float compressBaseForTest(float b, float kneeStart, float maxLog) {
  return compressBase(b, kneeStart, maxLog);
}

// A shoulder on the written value, per channel, so nothing reaches white.
//
// compressBase only promises that the *base layer* never rises above the
// identity. The detail layer is added on top of it untouched and at
// --sdr-detail 1.25 it is amplified, so the composite routinely lands above
// white and used to be hard clipped there. Measured across six frames that cost
// between 1.4% and 11.5% of the picture to flat white — all three channels at
// 255, no texture and no hue — where Lightroom's own export of the same renders
// clipped 0.00% on every one of them.
//
// Slope is exactly 1 at the knee and the curve approaches white
// asymptotically without arriving, so highlights keep their ordering and their
// texture instead of collapsing onto a single value. Below the knee this is the
// identity, which is what keeps the brighter midtones that make this base
// preferable to a flat SDR export in the first place.
//
// This has to happen before the gain is measured, and it does: the gain map is
// computed from `sdr` a few lines further down, so the HDR rendition is
// unaffected — a darker base simply means a larger gain. Invariant 1.
float softShoulder(float v, float knee) {
  if (!(knee < 1.0f) || v <= knee) return v;
  const float span = 1.0f - knee;
  return knee + span * (1.0f - std::exp(-(v - knee) / span));
}

// The shoulder for a whole pixel, returned as an adjustment to the uniform
// scale rather than as three values.
//
// A saturated highlight can put one channel over white while its luminance
// sits under it, so the brightest channel needs a shoulder of its own. Running
// that shoulder per channel is the obvious implementation and it is a hue
// shift: each channel moves by a different amount, so the ratios between them
// change. On a frame with bright foliage it read as a green cast, and measured
// against the render it doubled the green bias of the highlight region — a
// chromaticity error of +0.0030 where this leaves -0.0018. It is the same rule
// the contrast shaping follows, for the same reason.
//
// Scaling all three by whatever the brightest one needed leaves chromaticity
// exactly untouched, and costs a little brightness in precisely the pixels
// that were about to clip.
float highlightScale(const float rgb[3], float scale, float knee) {
  const float peak = std::max(rgb[0], std::max(rgb[1], rgb[2])) * scale;
  if (!(peak > knee) || peak <= 1e-6f) return scale;
  return scale * (softShoulder(peak, knee) / peak);
}

float highlightScaleForTest(const float rgb[3], float scale, float knee) {
  return highlightScale(rgb, scale, knee);
}

// Where the highlight shoulder starts for one pixel, given how bright that
// pixel's surroundings are.
//
// A single global knee cannot serve a frame that contains both a white garment
// under even light and a small specular on a dark ground. Rated across sixteen
// scenes, the preferred global knee could not be predicted from any measurement
// of the whole frame — two scenes with median luminances of 0.642 and 0.666 were
// rated at opposite ends of the ladder — and the rating that explained why
// refused to give a number at all: parts of one rendering better and parts of
// another better, within a single picture.
//
// So take the knee from the guided filter's base layer, which is exactly the
// "how bright is it around here" signal that distinguishes the two. `b` is the
// base layer's log2 luminance: a white garment has a high b and little detail
// riding on it, a specular has a low b and a large one. Where b is high the
// shoulder starts earlier and the area rolls off; where it is low the pixel
// keeps the punch a global knee would also have given it.
//
// strength 0 is exactly the global behaviour, which is what ships until this is
// shown to beat it on frames it has not seen.
float localShoulderKneeLog(float b, float globalKneeLog, float strength) {
  if (strength <= 0.0f) return globalKneeLog;
  // Ramp over the stop and a half below SDR white: below that the surroundings
  // are dim enough that nothing here is a large bright area.
  const float t = clampf((b + 1.5f) / 1.3f, 0.0f, 1.0f);
  // A full stop earlier at the top of the ramp, scaled by strength.
  return globalKneeLog - strength * t * 1.0f;
}

float localShoulderKneeLogForTest(float b, float globalKneeLog, float strength) {
  return localShoulderKneeLog(b, globalKneeLog, strength);
}

SdrShaping solveSdrShaping(const float* luminances, size_t count,
                           const PipelineOptions& opts, float maxBoost) {
  std::vector<uint64_t> hist(kHistBins, 0);
  uint64_t total = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!(luminances[i] > 0.0f)) continue;
    ++hist[histBin(luminances[i])];
    ++total;
  }
  const AutoShape s = solveShaping(hist, total, opts, maxBoost);
  return {s.liftEV, s.contrast};
}

const char* toneMapName(ToneMapOperator t) {
  switch (t) {
    case ToneMapOperator::Filmic: return "filmic";
    case ToneMapOperator::Clip: return "clip";
    case ToneMapOperator::Reinhard: return "reinhard";
    default: return "local";
  }
}

PipelineResult runPipeline(const TiffReader& tiff, const PipelineOptions& opts) {
  if (opts.gainMapSubsample != 1 && opts.gainMapSubsample != 2 &&
      opts.gainMapSubsample != 4)
    fail("gain map subsample factor must be 1, 2 or 4");
  if (!(opts.targetHeadroom > 0.0f) || opts.targetHeadroom > 10.0f)
    fail("target headroom must be greater than 0 and at most 10 stops");
  if (!(opts.gainMapGamma > 0.0f)) fail("gain map gamma must be positive");

  const Resolved in = resolveInputSpace(tiff, opts);
  const uint32_t w = tiff.width();
  const uint32_t h = tiff.height();
  const uint32_t inChannels = tiff.channels();
  const int sub = opts.gainMapSubsample;
  const int gainChannels = opts.multiChannelGainMap ? 3 : 1;

  PipelineResult res;
  res.width = w;
  res.height = h;
  res.gainWidth = (w + sub - 1) / sub;
  res.gainHeight = (h + sub - 1) / sub;
  res.gainChannels = gainChannels;
  res.resolvedInputPrimaries = in.primaries;
  res.resolvedInputTransfer = in.transfer;
  res.sdr.assign(static_cast<size_t>(w) * h * 3, 0);

  const Mat3 toOutput = conversionMatrix(in.primaries, opts.outputPrimaries);
  const bool identityMatrix = toOutput.isIdentity();
  const auto lw = luminanceWeights(opts.outputPrimaries);

  // Band height: a multiple of the subsample factor that also lines up with
  // the TIFF's own strip/tile height so nothing is decompressed twice.
  uint32_t band = tiff.suggestedBandRows();
  if (band == 0) band = 64;
  band = ((band + sub - 1) / sub) * sub;
  band = std::max<uint32_t>(band, static_cast<uint32_t>(sub));
  const uint32_t bandCount = (h + band - 1) / band;

  // Pass 1: how much headroom does the content actually use?
  //
  // Every pixel is folded into a box-averaged downscale, and the peak is taken
  // from that. Averaging cuts both ways deliberately: a lone hot pixel or a
  // speck of sensor noise cannot define the headroom for the whole frame, and
  // — unlike sampling a grid of pixels, which this replaces — a small specular
  // highlight can never be skipped over entirely, because it always raises the
  // block that contains it.
  float maxBoost = opts.targetHeadroom;
  float measured = 0.0f;
  float truePeak = 0.0f;
  std::vector<uint64_t> hist(kHistBins, 0);
  uint64_t histTotal = 0;
  uint32_t guideBlock = 1, guideW = 0, guideH = 0;
  Plane guideLog;
  float channelCeiling[3] = {0.0f, 0.0f, 0.0f};
  float channelTruePeak[3] = {0.0f, 0.0f, 0.0f};
  {
    const uint32_t longEdge = std::max(w, h);
    // Roughly a 2048-pixel long edge, and always at least 2x2 on anything
    // bigger than a thumbnail, so there is some averaging even when small.
    uint32_t blockSize = (longEdge + 2047u) / 2048u;
    if (longEdge > 512) blockSize = std::max<uint32_t>(blockSize, 2);
    blockSize = std::max<uint32_t>(blockSize, 1);
    if (opts.peakDetect == PeakDetect::Exact) blockSize = 1;

    const uint32_t blocksX = (w + blockSize - 1) / blockSize;

    // A second, coarser downscale for the local operator's guide. Kept
    // independent of the peak-detection blocks because --peak-detect exact
    // drives those to 1x1, and a full-resolution guide would cost 180 MB on a
    // 45 MP frame to compute a layer that is low frequency by construction.
    guideBlock = std::max<uint32_t>(1, (longEdge + 1023u) / 1024u);
    guideW = (w + guideBlock - 1) / guideBlock;
    guideH = (h + guideBlock - 1) / guideBlock;
    std::vector<double> guideSum(static_cast<size_t>(guideW) * guideH, 0.0);
    std::vector<uint32_t> guideCount(static_cast<size_t>(guideW) * guideH, 0);

    // Output-space luminance as a function of the *input* primaries, so the
    // measurement pass never has to build a converted RGB triple.
    double inputLumWeights[3];
    for (int c = 0; c < 3; ++c) {
      inputLumWeights[c] = identityMatrix
                               ? lw[c]
                               : lw[0] * toOutput.m[0][c] +
                                     lw[1] * toOutput.m[1][c] +
                                     lw[2] * toOutput.m[2][c];
    }
    const double inputLumWeightSum =
        inputLumWeights[0] + inputLumWeights[1] + inputLumWeights[2];

    // Read in whole strips or tiles, not one block row at a time: readRows
    // decodes every segment its range touches, so a 4-row read against a
    // 32-row strip layout would decode each strip eight times over.
    uint32_t readRows = tiff.suggestedBandRows();
    if (readRows == 0) readRows = blockSize * 16;
    readRows = ((readRows + blockSize - 1) / blockSize) * blockSize;
    readRows = std::max(readRows, blockSize);
    const uint32_t readBands = (h + readRows - 1) / readRows;

    std::vector<float> blockPeak(readBands, 0.0f);
    std::vector<float> pixelPeak(readBands, 0.0f);
    // Per-channel peaks, in the output space, so each channel can be held at
    // its own ceiling instead of at the luminance one.
    std::vector<float> bandChanBlock(readBands * 3, 0.0f);
    std::vector<float> bandChanPixel(readBands * 3, 0.0f);
    // One histogram per band, merged afterwards: a shared one would need a
    // lock on the hottest loop in the encoder.
    std::vector<std::vector<uint32_t>> bandHist(readBands);
    // Guide accumulators are per band and merged afterwards for the same
    // reason: a band boundary can land inside a guide row.
    std::vector<std::vector<double>> bandGuideSum(readBands);
    std::vector<std::vector<uint32_t>> bandGuideCount(readBands);
    std::vector<uint32_t> bandGuideRow0(readBands, 0);

    parallelFor(readBands, opts.threads, [&](size_t bi) {
      const uint32_t y0 = static_cast<uint32_t>(bi) * readRows;
      const uint32_t rows = std::min(readRows, h - y0);
      std::vector<float> src(static_cast<size_t>(rows) * w * inChannels);
      tiff.readRows(y0, rows, src.data());

      const uint32_t blockRowsHere = (rows + blockSize - 1) / blockSize;
      std::vector<double> sums(static_cast<size_t>(blockRowsHere) * blocksX, 0.0);
      std::vector<uint32_t> counts(static_cast<size_t>(blockRowsHere) * blocksX, 0);
      float localPixelPeak = 0.0f;
      std::vector<double> chanSums(static_cast<size_t>(blockRowsHere) * blocksX * 3, 0.0);
      float localChanPixel[3] = {0.0f, 0.0f, 0.0f};

      const uint32_t gRow0 = y0 / guideBlock;
      const uint32_t gRowsHere = (y0 + rows - 1) / guideBlock - gRow0 + 1;
      std::vector<double> gSum(static_cast<size_t>(gRowsHere) * guideW, 0.0);
      std::vector<uint32_t> gCount(static_cast<size_t>(gRowsHere) * guideW, 0);

      for (uint32_t r = 0; r < rows; ++r) {
        const float* srow = src.data() + static_cast<size_t>(r) * w * inChannels;
        double* sumRow = sums.data() + static_cast<size_t>(r / blockSize) * blocksX;
        uint32_t* cntRow =
            counts.data() + static_cast<size_t>(r / blockSize) * blocksX;
        for (uint32_t x = 0; x < w; ++x) {
          // Only luminance is needed here, so the primaries matrix is folded
          // into the weights: one dot product instead of a 3x3 multiply.
          float l = 0.0f;
          if (inChannels == 1) {
            l = static_cast<float>(inputLumWeightSum) *
                decodeTransfer(in.transfer, srow[x], opts.pqDiffuseWhiteNits);
          } else {
            const float* p = srow + static_cast<size_t>(x) * inChannels;
            for (int c = 0; c < 3; ++c)
              l += static_cast<float>(inputLumWeights[c]) *
                   decodeTransfer(in.transfer, p[c], opts.pqDiffuseWhiteNits);
          }
          // Per-channel values in the output space, for the channel ceilings.
          {
            float lin[3];
            for (int c = 0; c < 3; ++c) {
              const float raw = inChannels == 1
                                    ? srow[x]
                                    : srow[static_cast<size_t>(x) * inChannels + c];
              lin[c] = decodeTransfer(in.transfer, raw, opts.pqDiffuseWhiteNits);
            }
            float outRgb[3];
            if (identityMatrix) {
              outRgb[0] = lin[0]; outRgb[1] = lin[1]; outRgb[2] = lin[2];
            } else {
              auto v = toOutput.apply({lin[0], lin[1], lin[2]});
              outRgb[0] = static_cast<float>(v[0]);
              outRgb[1] = static_cast<float>(v[1]);
              outRgb[2] = static_cast<float>(v[2]);
            }
            double* cs = chanSums.data() +
                         (static_cast<size_t>(r / blockSize) * blocksX +
                          x / blockSize) * 3;
            for (int c = 0; c < 3; ++c) {
              const float v = std::max(0.0f, outRgb[c]);
              cs[c] += v;
              localChanPixel[c] = std::max(localChanPixel[c], v);
            }
          }
          if (l < 0.0f) l = 0.0f;
          sumRow[x / blockSize] += l;
          ++cntRow[x / blockSize];
          localPixelPeak = std::max(localPixelPeak, l);
          const size_t gi =
              static_cast<size_t>((y0 + r) / guideBlock - gRow0) * guideW +
              x / guideBlock;
          gSum[gi] += l;
          ++gCount[gi];
        }
      }

      float bandPeak = 0.0f;
      std::vector<uint32_t> localHist(kHistBins, 0);
      for (size_t b = 0; b < sums.size(); ++b) {
        if (counts[b] == 0) continue;
        const float avg = static_cast<float>(sums[b] / counts[b]);
        bandPeak = std::max(bandPeak, avg);
        if (avg > 0.0f) ++localHist[histBin(avg)];
      }
      for (size_t b = 0; b < counts.size(); ++b) {
        if (counts[b] == 0) continue;
        for (int c = 0; c < 3; ++c) {
          const float avg = static_cast<float>(chanSums[b * 3 + c] / counts[b]);
          bandChanBlock[bi * 3 + c] = std::max(bandChanBlock[bi * 3 + c], avg);
        }
      }
      for (int c = 0; c < 3; ++c) bandChanPixel[bi * 3 + c] = localChanPixel[c];

      blockPeak[bi] = bandPeak;
      pixelPeak[bi] = localPixelPeak;
      bandHist[bi] = std::move(localHist);
      bandGuideRow0[bi] = gRow0;
      bandGuideSum[bi] = std::move(gSum);
      bandGuideCount[bi] = std::move(gCount);
    });

    for (uint32_t bi = 0; bi < readBands; ++bi) {
      const size_t n = bandGuideSum[bi].size();
      const size_t offset = static_cast<size_t>(bandGuideRow0[bi]) * guideW;
      for (size_t i = 0; i < n; ++i) {
        guideSum[offset + i] += bandGuideSum[bi][i];
        guideCount[offset + i] += bandGuideCount[bi][i];
      }
    }

    // The guide lives in log2 luminance, which is the space the split, the
    // knee and the detail layer all work in.
    guideLog = Plane(guideW, guideH);
    for (size_t i = 0; i < guideLog.v.size(); ++i) {
      const float avg = guideCount[i]
                            ? static_cast<float>(guideSum[i] / guideCount[i])
                            : 0.0f;
      guideLog.v[i] = std::max(-20.0f, std::log2(std::max(avg, 1e-7f)));
    }

    float peak = 0.0f;
    for (uint32_t bi = 0; bi < readBands; ++bi) {
      peak = std::max(peak, blockPeak[bi]);
      truePeak = std::max(truePeak, pixelPeak[bi]);
      for (int c = 0; c < 3; ++c) {
        channelCeiling[c] = std::max(channelCeiling[c], bandChanBlock[bi * 3 + c]);
        channelTruePeak[c] = std::max(channelTruePeak[c], bandChanPixel[bi * 3 + c]);
      }
      if (bandHist[bi].empty()) continue;
      for (int i = 0; i < kHistBins; ++i) {
        hist[i] += bandHist[bi][i];
        histTotal += bandHist[bi][i];
      }
    }
    measured = peak > 1.0f ? std::log2(peak) : 0.0f;
    res.measuredHeadroom = measured;
    // Not used by anything in the pipeline: reported so a batch export can be
    // audited afterwards. A frame whose edit pushed a large part of the picture
    // above SDR white is the one whose SDR base will look bright and flat, and
    // no encoder setting puts back the separation the edit spent.
    res.fractionAboveWhite = static_cast<float>(
        histFractionAbove(hist, histTotal, 1.0f));
    res.truePeakHeadroom = truePeak > 1.0f ? std::log2(truePeak) : 0.0f;
    if (opts.autoMaxBoost) {
      // Exactly what was measured, with no slack. A sixth of a stop used to be
      // added here "for interpolation and rounding", but nothing interpolates
      // above the stored maximum, and the cost is real: declaring 2.47 where
      // the photo needs 2.30 makes a display with 2.30 stops apply only
      // 2.30/2.47 of the gain. Lightroom declares its measurement exactly, and
      // on the reference frame this now agrees with it to four decimals.
      maxBoost = std::min(opts.targetHeadroom, measured);
      maxBoost = std::max(maxBoost, 0.0f);
    }
  }
  if (maxBoost <= 0.0f) maxBoost = 1e-4f;  // keep the encoding well defined

  // The HDR clamp and the gain map share one ceiling, so every highlight the
  // base image gives up is one the gain map can hand back.

  // The shaping has to be settled before the shaper is built, because the gain
  // map is measured against the base the shaper produces.
  const bool localToneMap = opts.toneMap == ToneMapOperator::Local;

  // The base layer, and the knee that will compress it. A radius of about a
  // sixteenth of the guide's long edge is wide enough to separate the
  // illumination from the texture riding on it without reaching across the
  // frame; eps is 0.25, i.e. a quarter of a stop of local variance is texture
  // to be preserved and anything larger is an edge to hold still across.
  Plane baseLog;
  const float kneeStart =
      opts.sdrKnee < 0.0f ? opts.sdrKnee : kneeStartFor(maxBoost);
  const float detailStrength = opts.sdrDetail;
  // Where the shoulder on the composite starts, in stops relative to white.
  const float highlightKneeLog =
      opts.sdrHighlightKnee >= 1.0f ? 0.0f : std::log2(opts.sdrHighlightKnee);
  if (localToneMap && guideLog.w > 0) {
    const int radius = std::max<int>(
        2, static_cast<int>(std::max(guideLog.w, guideLog.h) / 16));
    baseLog = guidedFilter(guideLog, radius, std::max(1e-3f, opts.sdrEdge));
    logf("local tone map: guide %ux%u (1:%u), radius %d, knee %.2f EV, detail %.2f",
         guideLog.w, guideLog.h, guideBlock, radius, kneeStart, detailStrength);
  }

  // A curve needs the lift and contrast to undo its own midtone darkening. The
  // local operator has nothing to undo — below the knee it is the identity — so
  // shaping it would only push the base away from the render.
  float liftEV = localToneMap ? 0.0f : opts.sdrLiftEV;
  float contrast = localToneMap ? 1.0f : opts.sdrContrast;
  if (!localToneMap && opts.sdrShape == SdrShapeMode::Auto) {
    const AutoShape solved = solveShaping(hist, histTotal, opts, maxBoost);
    liftEV = solved.liftEV;
    contrast = solved.contrast;
    res.midtoneAnchor = solved.anchor;
    logf("auto shaping: median luminance %.4f -> lift %.3f EV, contrast %.3f",
         solved.anchor, liftEV, contrast);
  }
  res.sdrLiftEV = liftEV;
  res.sdrContrast = contrast;

  const SdrShaper shaper = makeShaper(opts, maxBoost, liftEV, contrast);
  // The local operator never brightens: the compressed base is
  // compressBase(B) <= B and the detail layer cancels exactly, so the base
  // image is everywhere at or below the HDR image and the gain map only ever
  // has to brighten. No sweep needed, and none would be valid — the sweep
  // assumes one output per input luminance, which is the property the local
  // operator exists to break.
  //
  // Both ends of the gain range are measured rather than bounded, because
  // neither can be bounded honestly. The local operator has no closed form to
  // sweep, and with the detail layer above unit strength the base can come out
  // brighter than the HDR image wherever the detail is positive, so the floor
  // is not reliably zero either. Anything a bound missed would be clamped at
  // quantisation and reconstruct wrong.

  res.declaredHeadroom = maxBoost;

  // The gains are held as floats for one pass so their true extremes can be
  // read off, then quantised against those. The buffer is released immediately
  // afterwards; it is four times the finished gain map and nothing else needs
  // it.
  std::vector<float> gainF(static_cast<size_t>(res.gainWidth) * res.gainHeight *
                               gainChannels,
                           0.0f);
  std::vector<float> bandGainLo(static_cast<size_t>(bandCount) * 3, 1e30f);
  std::vector<float> bandGainHi(static_cast<size_t>(bandCount) * 3, -1e30f);

  // Decoders recover the normalised gain as pow(stored, 1/gamma) — Ultra HDR
  // and ISO 21496-1 agree on this — so the encoder stores pow(norm, gamma).
  // This was inverted, and because the decode test encoded and decoded through
  // the same inverted convention it round-tripped perfectly while every real
  // decoder read the file 1.26 EV hot in the midtones. Measured against
  // Lightroom's own export of the same edit: +1.30 EV at the median, with 55%
  // of the frame pushed above SDR white where Lightroom put 11%. Any test for
  // this has to decode the way the standard says, not the way we wrote it.
  const float encodeGamma = opts.gainMapGamma;

  parallelFor(bandCount, opts.threads, [&](size_t bi) {
    const uint32_t y0 = static_cast<uint32_t>(bi) * band;
    const uint32_t rows = std::min(band, h - y0);
    std::vector<float> src(static_cast<size_t>(rows) * w * inChannels);
    tiff.readRows(y0, rows, src.data());

    // Gain accumulators for the (partial) gain-map rows this band covers.
    const uint32_t gy0 = y0 / sub;
    const uint32_t gRows = (rows + sub - 1) / sub;
    std::vector<float> accum(static_cast<size_t>(gRows) * res.gainWidth *
                                 gainChannels,
                             0.0f);
    std::vector<uint16_t> counts(static_cast<size_t>(gRows) * res.gainWidth, 0);

    for (uint32_t r = 0; r < rows; ++r) {
      const float* srow = src.data() + static_cast<size_t>(r) * w * inChannels;
      uint8_t* drow = res.sdr.data() + (static_cast<size_t>(y0 + r) * w) * 3;
      const uint32_t gr = r / sub;
      for (uint32_t x = 0; x < w; ++x) {
        float lin[3];
        for (int c = 0; c < 3; ++c) {
          float v = srow[static_cast<size_t>(x) * inChannels +
                         (inChannels == 1 ? 0 : c)];
          lin[c] = decodeTransfer(in.transfer, v, opts.pqDiffuseWhiteNits);
        }
        float hdr[3];
        if (identityMatrix) {
          hdr[0] = lin[0]; hdr[1] = lin[1]; hdr[2] = lin[2];
        } else {
          auto v = toOutput.apply({lin[0], lin[1], lin[2]});
          hdr[0] = static_cast<float>(v[0]);
          hdr[1] = static_cast<float>(v[1]);
          hdr[2] = static_cast<float>(v[2]);
        }
        // Out-of-gamut negatives are clipped. Each channel is then held at
        // *its own* measured ceiling rather than at the luminance ceiling: a
        // saturated highlight puts far more into one channel than into the
        // luminance it contributes to, and clamping every channel at the
        // luminance peak truncates the strong one while leaving the weak ones
        // alone. That is a hue shift, not a clip — measured on a Lightroom
        // render whose luminance peaked at 2.30 EV, red reached 2.59 EV and
        // 32,523 pixels were being desaturated toward neutral by it.
        for (int c = 0; c < 3; ++c)
          hdr[c] = std::min(channelCeiling[c], std::max(0.0f, hdr[c]));

        const float lHdr = std::max(0.0f, static_cast<float>(
                                              lw[0] * hdr[0] + lw[1] * hdr[1] +
                                              lw[2] * hdr[2]));
        // Tone curve, lift and contrast collapse into one scale on RGB, so
        // the base image's chromaticity matches the HDR image's exactly. The
        // local operator produces a scale the same way — one number for all
        // three channels — so it inherits that property unchanged.
        float scale;
        // The knee this pixel's own surroundings ask for. Only the local
        // operator has a base layer to read it from; the curve modes keep the
        // global one.
        float pixelKnee = opts.sdrHighlightKnee;
        if (localToneMap && baseLog.w > 0) {
          // Bilinear sample of the base layer at this pixel. Guide sample gx
          // covers source columns [gx*block, (gx+1)*block), so its centre sits
          // at (gx+0.5)*block.
          const float fx =
              (static_cast<float>(x) + 0.5f) / guideBlock - 0.5f;
          const float fy =
              (static_cast<float>(y0 + r) + 0.5f) / guideBlock - 0.5f;
          const int x0 = static_cast<int>(std::floor(fx));
          const int y0i = static_cast<int>(std::floor(fy));
          const float tx = fx - x0, ty = fy - y0i;
          const uint32_t xa = std::min<uint32_t>(baseLog.w - 1, std::max(0, x0));
          const uint32_t xb = std::min<uint32_t>(baseLog.w - 1, std::max(0, x0 + 1));
          const uint32_t ya = std::min<uint32_t>(baseLog.h - 1, std::max(0, y0i));
          const uint32_t yb = std::min<uint32_t>(baseLog.h - 1, std::max(0, y0i + 1));
          const float top = baseLog.at(xa, ya) * (1.0f - tx) + baseLog.at(xb, ya) * tx;
          const float bot = baseLog.at(xa, yb) * (1.0f - tx) + baseLog.at(xb, yb) * tx;
          const float b = top * (1.0f - ty) + bot * ty;

          const float logL = std::max(-20.0f, std::log2(std::max(lHdr, 1e-7f)));
          const float detail = logL - b;
          float logS =
              compressBase(b, kneeStart, maxBoost) + detailStrength * detail;
          // The detail layer is added over the compressed base with no ceiling,
          // and above unit strength it is amplified, so the composite lands
          // above white wherever bright texture sits on a bright base. Folding
          // it through the same shoulder puts it back under white with its
          // ordering intact, which is the difference between a highlight that
          // is bright and one that is a flat white shape.
          //
          // Below the knee this is the identity, and for a value the first
          // shoulder already produced it is very nearly so — the shoulder's
          // range is the whole headroom while its span is only the stops
          // between the knee and white, so the slope there is close to 1. What
          // it changes is the part that used to be clipped away.
          //
          // Its range is deliberately *not* the headroom. Folding the whole
          // headroom below white is what a global operator does, and measured
          // it costs 0.04 of mean luminance and drops p99 to 0.818 — darker
          // than Lightroom's own export of the same render, and the opposite of
          // why this base is worth having. The composite only ever overshoots
          // by what the detail layer adds, so two stops is the whole budget.
          const float kneeLog = localShoulderKneeLog(b, highlightKneeLog,
                                                     opts.sdrLocalShoulder);
          logS = compressBase(logS, kneeLog, kneeLog + 2.0f);
          pixelKnee = std::exp2(kneeLog);
          // Back to a ratio. Expressed as scale rather than an absolute value
          // so the chromaticity argument above still holds.
          scale = lHdr > 1e-8f ? std::exp2(logS) / lHdr : 0.0f;
          scale = std::min(scale, 1.0f);  // the operator must never brighten
        } else {
          scale = shaper.rgbScale(lHdr);
        }

        scale = highlightScale(hdr, scale, pixelKnee);

        float sdr[3];
        for (int c = 0; c < 3; ++c)
          sdr[c] = std::min(1.0f, std::max(0.0f, hdr[c] * scale));

        uint8_t* out = drow + static_cast<size_t>(x) * 3;
        for (int c = 0; c < 3; ++c)
          out[c] = static_cast<uint8_t>(
              std::lround(encodeSrgb(sdr[c]) * 255.0f));

        const uint32_t gx = x / sub;
        float* acc = accum.data() +
                     (static_cast<size_t>(gr) * res.gainWidth + gx) *
                         gainChannels;
        if (gainChannels == 1) {
          // Single achromatic channel: the luminance ratio the decoder needs,
          // measured against the base image as actually written — shaping and
          // per-channel clipping included.
          const float lSdr = std::min(1.0f, std::max(0.0f,
              static_cast<float>(lw[0] * sdr[0] + lw[1] * sdr[1] +
                                 lw[2] * sdr[2])));
          float g = std::log2((lHdr + opts.offsetHdr) /
                              (lSdr + opts.offsetSdr));
          acc[0] += g;
        } else {
          for (int c = 0; c < 3; ++c)
            acc[c] += std::log2((hdr[c] + opts.offsetHdr) /
                                (sdr[c] + opts.offsetSdr));
        }
        ++counts[static_cast<size_t>(gr) * res.gainWidth + gx];
      }
    }

    // Box-average into the float buffer, and note the extremes this band saw.
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t gr = 0; gr < gRows; ++gr) {
      for (uint32_t gx = 0; gx < res.gainWidth; ++gx) {
        const size_t idx = static_cast<size_t>(gr) * res.gainWidth + gx;
        const float n = counts[idx] ? static_cast<float>(counts[idx]) : 1.0f;
        float* dst = gainF.data() +
                     (static_cast<size_t>(gy0 + gr) * res.gainWidth + gx) *
                         gainChannels;
        for (int c = 0; c < gainChannels; ++c) {
          const float g = accum[idx * gainChannels + c] / n;
          dst[c] = g;
          lo[c] = std::min(lo[c], g);
          hi[c] = std::max(hi[c], g);
        }
      }
    }
    for (int c = 0; c < gainChannels; ++c) {
      bandGainLo[bi * 3 + c] = lo[c];
      bandGainHi[bi * 3 + c] = hi[c];
    }
  });

  // The range every channel actually needs, end to end.
  float gainLo[3] = {0.0f, 0.0f, 0.0f}, gainHi[3] = {0.0f, 0.0f, 0.0f};
  for (int c = 0; c < gainChannels; ++c) {
    float lo = 1e30f, hi = -1e30f;
    for (uint32_t bi = 0; bi < bandCount; ++bi) {
      lo = std::min(lo, bandGainLo[bi * 3 + c]);
      hi = std::max(hi, bandGainHi[bi * 3 + c]);
    }
    if (!(lo <= hi)) { lo = 0.0f; hi = 0.0f; }
    // A floor above zero would waste the bottom of the range, and a ceiling
    // below it would be meaningless.
    gainLo[c] = std::min(0.0f, lo);
    gainHi[c] = std::max(gainLo[c] + 1e-4f, hi);
  }
  // A monochrome map has one channel but three metadata slots to fill.
  for (int c = gainChannels; c < 3; ++c) {
    gainLo[c] = gainLo[0];
    gainHi[c] = gainHi[0];
  }
  for (int c = 0; c < 3; ++c) {
    res.minBoostLog2[c] = gainLo[c];
    res.maxBoostLog2[c] = gainHi[c];
  }
  logf("gain range: R %.3f..%.3f  G %.3f..%.3f  B %.3f..%.3f (declared headroom %.3f)",
       gainLo[0], gainHi[0], gainLo[1], gainHi[1], gainLo[2], gainHi[2], maxBoost);

  res.gain.assign(gainF.size(), 0);
  parallelFor(res.gainHeight, opts.threads, [&](size_t gy) {
    for (uint32_t gx = 0; gx < res.gainWidth; ++gx) {
      const size_t i = (gy * res.gainWidth + gx) * gainChannels;
      for (int c = 0; c < gainChannels; ++c) {
        const float range = gainHi[c] - gainLo[c];
        float norm = (gainF[i + c] - gainLo[c]) / range;
        norm = std::min(1.0f, std::max(0.0f, norm));
        res.gain[i + c] = static_cast<uint8_t>(
            std::lround(std::pow(norm, encodeGamma) * 255.0f));
      }
    }
  });
  gainF.clear();
  gainF.shrink_to_fit();

  return res;
}

}  // namespace iso21496
