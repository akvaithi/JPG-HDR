// Generation of the small matrix/TRC ICC v2 profiles we embed, plus just
// enough parsing to recognise the profile Lightroom put on the intermediate.
#pragma once

#include "color.h"
#include "common.h"

namespace iso21496 {

struct IccSummary {
  bool valid = false;
  bool isGray = false;
  ColorPrimaries primaries = ColorPrimaries::Auto;  // nearest match
  TransferFunction transfer = TransferFunction::Auto;
  std::string description;
  bool matched = false;  // primaries matched a known space within tolerance
};

// Best-effort identification of an embedded profile. Never throws: an
// unrecognisable profile just yields valid == false.
IccSummary inspectIccProfile(const Bytes& profile);

// A v2 matrix/TRC RGB display profile with a 1024-entry sampled sRGB curve.
Bytes buildRgbIccProfile(ColorPrimaries primaries, const std::string& desc);
// A v2 greyscale profile (used for the single-channel gain map image).
Bytes buildGrayIccProfile(double gamma, const std::string& desc);

}  // namespace iso21496
