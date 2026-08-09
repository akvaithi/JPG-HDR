// The SDR base shaping solver.
//
// The point of solving rather than fixing these is that the right answer moves
// with the picture, so most of what is worth asserting is how the answer
// changes between distributions, not any single number.
#include <algorithm>
#include <cmath>
#include <vector>

#include "pipeline.h"
#include "test_support.h"

using namespace iso21496;
using namespace iso21496::test;

namespace {

// A lognormal spread of luminances about `median`, which is what a photograph's
// tone distribution broadly looks like and, more to the point, lets a test say
// "a darker picture" without rendering one.
std::vector<float> scene(float median, float spreadStops = 1.6f,
                         int count = 20000) {
  std::vector<float> out;
  out.reserve(count);
  // Deterministic: a test that samples a generator is a test that eventually
  // fails on someone else's machine.
  uint32_t state = 0x9e3779b9u;
  for (int i = 0; i < count; ++i) {
    // Sum of three uniforms, an adequate and cheap normal for this purpose.
    double u = 0.0;
    for (int k = 0; k < 3; ++k) {
      state = state * 1664525u + 1013904223u;
      u += static_cast<double>(state >> 8) / static_cast<double>(1 << 24);
    }
    const double z = (u - 1.5) * 2.0;  // roughly [-1.5, 1.5] stops
    out.push_back(static_cast<float>(median * std::pow(2.0, z * spreadStops)));
  }
  return out;
}

SdrShaping solve(const std::vector<float>& lum, const PipelineOptions& o,
                 float maxBoost = 3.0f) {
  return solveSdrShaping(lum.data(), lum.size(), o, maxBoost);
}

// A dark frame needs almost no correction and a bright one needs a lot: the
// Reinhard's log-log slope is 1/(1+L), which is the identity in the shadows and
// only bites as the tones approach SDR white. Getting this backwards would
// brighten night shots and leave high-key ones muddy.
void liftTracksSceneBrightness() {
  PipelineOptions o;
  const SdrShaping dark = solve(scene(0.02f), o);
  const SdrShaping mid = solve(scene(0.18f), o);
  const SdrShaping bright = solve(scene(0.55f), o);

  CHECK(dark.liftEV < 0.10f);
  CHECK(mid.liftEV > dark.liftEV);
  CHECK(bright.liftEV > mid.liftEV);
  CHECK(bright.liftEV < 1.5f);

  // Same ordering for contrast, and it never darkens or inverts.
  CHECK(dark.contrast >= 1.0f);
  CHECK(mid.contrast >= dark.contrast);
  CHECK(bright.contrast >= mid.contrast);
  CHECK(bright.contrast <= 1.5f);
}

// The lift is solved so the median lands back where the tone curve found it.
// This is the whole claim the solver makes, so it is worth checking against the
// shaping as the pixel loop will actually apply it rather than against the
// formula that produced it.
void theMedianLandsWhereItStarted() {
  PipelineOptions o;
  for (float median : {0.08f, 0.18f, 0.35f, 0.60f}) {
    const std::vector<float> lum = scene(median);
    const SdrShaping s = solve(lum, o);

    // Reproduce shapedLuminance(): tone curve, lift, then contrast about mid
    // grey. Reinhard with the ceiling the solve was given.
    const float ceiling = std::pow(2.0f, 3.0f);
    std::vector<float> sorted = lum;
    std::sort(sorted.begin(), sorted.end());
    const float actualMedian = sorted[sorted.size() / 2];

    const float k = 1.0f + actualMedian / (ceiling * ceiling);
    const float toneMapped =
        std::min(1.0f, actualMedian * k / (1.0f + actualMedian));
    const float lifted = toneMapped * std::pow(2.0f, s.liftEV);
    const float shaped =
        std::pow(lifted, s.contrast) * std::pow(0.18f, 1.0f - s.contrast);

    // Within a twentieth of a stop of the luminance it started at. The gap is
    // the histogram's bin width, not an error in the solve.
    CHECK_NEAR(std::log2(shaped / actualMedian), 0.0, 0.05);
  }
}

// The solver has to reproduce the numbers this encoder shipped with on the kind
// of frame they were tuned against, or it is not a refinement of them.
void agreesWithTheHandTunedDefaultsOnATypicalFrame() {
  PipelineOptions o;
  const SdrShaping s = solve(scene(0.34f), o, 2.9f);
  CHECK_NEAR(s.contrast, 1.14, 0.04);
  CHECK(s.liftEV > 0.15f);
  CHECK(s.liftEV < 0.50f);
}

// Degenerate inputs must not produce a shaping that mangles the base.
void degenerateDistributions() {
  PipelineOptions o;

  const SdrShaping empty = solveSdrShaping(nullptr, 0, o, 3.0f);
  CHECK_EQ(empty.liftEV, 0.0f);
  CHECK_EQ(empty.contrast, 1.0f);

  // Every pixel identical: no spread to restore, so no contrast change.
  const std::vector<float> flat(5000, 0.2f);
  const SdrShaping s = solve(flat, o);
  CHECK_NEAR(s.contrast, 1.0, 1e-3);
  CHECK(s.liftEV >= 0.0f);

  // All black, and all far above SDR white.
  const std::vector<float> black(500, 0.0f);
  CHECK_EQ(solve(black, o).liftEV, 0.0f);
  const SdrShaping blown = solve(std::vector<float>(500, 40.0f), o);
  CHECK(blown.liftEV >= 0.0f);
  CHECK(blown.liftEV <= 1.5f);
  CHECK(blown.contrast >= 1.0f);
}

// Nothing the solver produces may push the base outside what the gain map can
// describe: the contrast must never invert tones and the lift must never
// darken, or the base stops being a plausible SDR rendition.
void solvedShapingStaysInRange() {
  PipelineOptions o;
  for (float median : {0.005f, 0.05f, 0.18f, 0.5f, 0.9f, 2.0f}) {
    for (float spread : {0.4f, 1.6f, 3.0f}) {
      const SdrShaping s = solve(scene(median, spread), o);
      CHECK(s.liftEV >= 0.0f);
      CHECK(s.liftEV <= 1.5f);
      CHECK(s.contrast >= 1.0f);
      CHECK(s.contrast <= 1.5f);
      CHECK(std::isfinite(s.liftEV));
      CHECK(std::isfinite(s.contrast));
    }
  }
}

// The local operator's two load-bearing properties, checked against the shaping
// it actually applies rather than against a rendered image.
//
// 1. It never brightens. That is what makes the gain map floor exactly zero
//    rather than something that has to be swept, and the sweep would not be
//    valid here anyway — it assumes one output per input luminance, which is
//    precisely the property this operator exists to break.
// 2. Below the knee it is the identity, so the diffuse midtones of the picture
//    arrive at the luminance the render gave them instead of being darkened and
//    corrected back.
void localOperatorCompressesWithoutBrightening() {
  PipelineOptions o;
  CHECK(o.toneMap == ToneMapOperator::Local);  // the default, and the point

  for (float maxBoost : {0.5f, 1.5f, 2.3f, 4.0f}) {
    const float knee = -clampf(0.85f * maxBoost, 1.0f, 2.5f);
    CHECK(knee < 0.0f);
    CHECK(knee >= -2.5f);

    // Identity below the knee.
    for (float b : {-12.0f, -6.0f, knee - 0.01f, knee}) {
      const float out = compressBaseForTest(b, knee, maxBoost);
      CHECK_NEAR(out, b, 1e-4);
    }
    // Monotonic, never brightening, and the peak lands on white.
    float previous = -1e9f;
    for (int i = 0; i <= 200; ++i) {
      const float b = knee + (maxBoost - knee) * (i / 200.0f);
      const float out = compressBaseForTest(b, knee, maxBoost);
      CHECK(out <= b + 1e-4f);       // compresses downward, never up
      CHECK(out >= previous - 1e-4f);  // and never folds tones over each other
      previous = out;
    }
    // The peak lands under white rather than exactly on it: see compressBase
    // for why buying that last sliver would cost the zero floor. The shortfall
    // is span*exp(-k), about 0.22 EV at the usual headrooms, and it does not
    // show up in the finished base — the detail layer rides on top of this and
    // takes the brightest pixels back to white and past it.
    const float peak = compressBaseForTest(maxBoost, knee, maxBoost);
    CHECK(peak <= 0.0f);
    CHECK(peak > -0.35f);
  }
}

// The highlight shoulder is a scale on the whole pixel, never a curve applied
// to each channel.
//
// Per channel is the obvious implementation and it is a hue shift: each channel
// is compressed by a different amount, so the ratios between them change. On a
// real frame with bright foliage that read as a green cast — measured against
// the render it left a chromaticity bias of +0.0030 toward green where this
// leaves -0.0018, and a larger overall hue error than Lightroom's own export.
// It is the same rule the contrast shaping follows, for the same reason.
//
// Asserted on the operator rather than on a rendered frame: an earlier version
// of this test rendered a synthetic pattern and passed with the per-channel
// implementation still in place, because the pattern never drove enough
// saturated pixels past the knee to show it.
void theHighlightShoulderIsAUniformScale() {
  const float knee = 0.75f;
  const float cases[][3] = {
      {4.0f, 2.2f, 0.8f},   // warm specular, all three over the knee
      {1.2f, 3.0f, 0.9f},   // green-dominant, which is the case that showed
      {0.9f, 0.9f, 0.9f},   // neutral and just under: untouched
      {6.0f, 0.2f, 0.05f},  // extreme, one channel far over
  };
  for (const auto& rgb : cases) {
    const float scale = highlightScaleForTest(rgb, 1.0f, knee);
    CHECK(scale > 0.0f);
    CHECK(scale <= 1.0f + 1e-6f);  // it may darken, never brighten

    // The defining property: applying it cannot change any channel ratio.
    const float sum = rgb[0] + rgb[1] + rgb[2];
    const float outSum = sum * scale;
    for (int c = 0; c < 3; ++c)
      CHECK_NEAR(rgb[c] * scale / outSum, rgb[c] / sum, 1e-5);

    // And the brightest channel must land under white.
    const float peak = std::max(rgb[0], std::max(rgb[1], rgb[2])) * scale;
    CHECK(peak <= 1.0f);
  }

  // Below the knee it is exactly the identity, so ordinary midtones are not
  // quietly darkened by a control that is meant to touch highlights only.
  const float dim[3] = {0.3f, 0.25f, 0.2f};
  CHECK_NEAR(highlightScaleForTest(dim, 1.0f, knee), 1.0f, 1e-6);

  // A knee of 1.0 disables it, which is what restores the old hard clip.
  const float hot[3] = {5.0f, 4.0f, 3.0f};
  CHECK_NEAR(highlightScaleForTest(hot, 1.0f, 1.0f), 1.0f, 1e-6);
}

// The local shoulder takes its knee from the base layer, so a large bright
// area rolls off where a specular on a dark ground does not.
//
// Asserted on the mapping rather than on a rendered frame, because a rendered
// frame is exactly where this kind of test goes quiet: an earlier attempt to
// check the hue property through a synthetic pattern passed with the broken
// implementation still in place.
void theLocalShoulderFollowsTheBaseLayer() {
  const float global = std::log2(0.80f);

  // Off is the shipped behaviour, and must be the global knee exactly — not
  // approximately, since that is what keeps every existing export identical.
  for (float b : {-6.0f, -1.5f, -0.5f, 0.0f, 2.0f})
    CHECK_EQ(localShoulderKneeLogForTest(b, global, 0.0f), global);

  // Dim surroundings: a specular sitting on a dark ground keeps the global
  // knee, which is the whole point of doing this locally.
  CHECK_EQ(localShoulderKneeLogForTest(-3.0f, global, 1.0f), global);
  CHECK_EQ(localShoulderKneeLogForTest(-1.5f, global, 1.0f), global);

  // Bright surroundings: the shoulder starts earlier, by a full stop at the
  // top of the ramp.
  CHECK_NEAR(localShoulderKneeLogForTest(-0.2f, global, 1.0f), global - 1.0f, 1e-5);
  CHECK_NEAR(localShoulderKneeLogForTest(1.0f, global, 1.0f), global - 1.0f, 1e-5);

  // Monotonic in between, and never above the global knee: this may pull the
  // shoulder earlier, never later.
  float previous = global + 1.0f;
  for (int i = 0; i <= 100; ++i) {
    const float b = -2.0f + 3.0f * (i / 100.0f);
    const float k = localShoulderKneeLogForTest(b, global, 1.0f);
    CHECK(k <= global + 1e-6f);
    CHECK(k <= previous + 1e-6f);
    previous = k;
  }

  // Strength scales the effect rather than switching it on: half the strength
  // is half the shift, so a sweep over it is a sweep over something continuous.
  CHECK_NEAR(localShoulderKneeLogForTest(0.0f, global, 0.5f), global - 0.5f, 1e-5);
}

void run() {
  localOperatorCompressesWithoutBrightening();
  theLocalShoulderFollowsTheBaseLayer();
  theHighlightShoulderIsAUniformScale();
  liftTracksSceneBrightness();
  theMedianLandsWhereItStarted();
  agreesWithTheHandTunedDefaultsOnATypicalFrame();
  degenerateDistributions();
  solvedShapingStaysInRange();
}

}  // namespace

TEST_MAIN(run)
