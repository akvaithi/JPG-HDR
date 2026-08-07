#include "pipeline.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

#include "icc.h"
#include "threads.h"

namespace iso21496 {
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
  if (s == "reinhard") *out = ToneMapOperator::Reinhard;
  else if (s == "filmic") *out = ToneMapOperator::Filmic;
  else if (s == "clip" || s == "none") *out = ToneMapOperator::Clip;
  else return false;
  return true;
}

const char* toneMapName(ToneMapOperator t) {
  switch (t) {
    case ToneMapOperator::Filmic: return "filmic";
    case ToneMapOperator::Clip: return "clip";
    default: return "reinhard";
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

  // Pass 1 (optional): sample the image on a coarse grid to learn how much
  // headroom the content actually uses, so the gain map spends its 8 bits on
  // the range in use rather than on the user's worst-case target.
  float maxBoost = opts.targetHeadroom;
  float measured = 0.0f;
  {
    const uint32_t step = std::max<uint32_t>(1, h / 512);
    std::atomic<uint32_t> maxScaled{0};  // fixed point x1000, for a lock-free max
    const uint32_t sampleRows = (h + step - 1) / step;
    parallelFor(sampleRows, opts.threads, [&](size_t i) {
      const uint32_t y = static_cast<uint32_t>(i) * step;
      std::vector<float> row(static_cast<size_t>(w) * inChannels);
      tiff.readRows(y, 1, row.data());
      float localMax = 0.0f;
      const uint32_t xStep = std::max<uint32_t>(1, w / 512);
      for (uint32_t x = 0; x < w; x += xStep) {
        float rgb[3];
        for (uint32_t c = 0; c < 3; ++c) {
          float v = row[static_cast<size_t>(x) * inChannels +
                        (inChannels == 1 ? 0 : c)];
          rgb[c] = decodeTransfer(in.transfer, v, opts.pqDiffuseWhiteNits);
        }
        double o[3];
        if (identityMatrix) {
          o[0] = rgb[0]; o[1] = rgb[1]; o[2] = rgb[2];
        } else {
          auto v = toOutput.apply({rgb[0], rgb[1], rgb[2]});
          o[0] = v[0]; o[1] = v[1]; o[2] = v[2];
        }
        double l = std::max(0.0, lw[0] * o[0] + lw[1] * o[1] + lw[2] * o[2]);
        localMax = std::max(localMax, static_cast<float>(l));
      }
      uint32_t scaled = static_cast<uint32_t>(
          std::lround(std::min(1000.0f, localMax) * 1000.0f));
      uint32_t prev = maxScaled.load(std::memory_order_relaxed);
      while (scaled > prev &&
             !maxScaled.compare_exchange_weak(prev, scaled,
                                              std::memory_order_relaxed)) {
      }
    });
    float peak = static_cast<float>(maxScaled.load()) / 1000.0f;
    measured = peak > 1.0f ? std::log2(peak) : 0.0f;
    res.measuredHeadroom = measured;
    if (opts.autoMaxBoost) {
      // Leave a sixth of a stop of slack for interpolation and rounding.
      maxBoost = std::min(opts.targetHeadroom, measured + 1.0f / 6.0f);
      maxBoost = std::max(maxBoost, 0.0f);
    }
  }
  if (maxBoost <= 0.0f) maxBoost = 1e-4f;  // keep the encoding well defined

  // The tone curve and the gain map share one ceiling, so the highlights the
  // curve compresses are exactly the ones the gain map can restore.
  const float lmax = std::pow(2.0f, maxBoost);

  for (int c = 0; c < 3; ++c) {
    res.minBoostLog2[c] = 0.0f;
    res.maxBoostLog2[c] = maxBoost;
  }

  res.gain.assign(static_cast<size_t>(res.gainWidth) * res.gainHeight *
                      gainChannels,
                  0);

  const float invGamma = 1.0f / opts.gainMapGamma;
  const float boostRange = maxBoost;  // min boost is 0 by construction

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
        // Out-of-gamut negatives are clipped; values above the target
        // headroom are held at the ceiling the metadata promises.
        for (int c = 0; c < 3; ++c)
          hdr[c] = std::min(lmax, std::max(0.0f, hdr[c]));

        const float lHdr = std::max(0.0f, static_cast<float>(
                                              lw[0] * hdr[0] + lw[1] * hdr[1] +
                                              lw[2] * hdr[2]));
        const float lSdr = toneMapLuminance(opts.toneMap, lHdr, lmax);
        const float scale = lHdr > 1e-8f ? lSdr / lHdr : 0.0f;

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
          // Single achromatic channel: the luminance ratio the decoder needs.
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

    // Box-average, normalise into [0,1], then apply the encoding gamma.
    for (uint32_t gr = 0; gr < gRows; ++gr) {
      for (uint32_t gx = 0; gx < res.gainWidth; ++gx) {
        const size_t idx = static_cast<size_t>(gr) * res.gainWidth + gx;
        const float n = counts[idx] ? static_cast<float>(counts[idx]) : 1.0f;
        uint8_t* dst = res.gain.data() +
                       (static_cast<size_t>(gy0 + gr) * res.gainWidth + gx) *
                           gainChannels;
        for (int c = 0; c < gainChannels; ++c) {
          float g = accum[idx * gainChannels + c] / n;
          float norm = boostRange > 0.0f ? g / boostRange : 0.0f;
          norm = std::min(1.0f, std::max(0.0f, norm));
          dst[c] = static_cast<uint8_t>(
              std::lround(std::pow(norm, invGamma) * 255.0f));
        }
      }
    }
  });

  return res;
}

}  // namespace iso21496
