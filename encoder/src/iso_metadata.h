// ISO 21496-1 gain-map metadata, the CIPA DC-007 MPF index, and the Adobe
// gain-map XMP that older decoders still look for.
#pragma once

#include <string>

#include "common.h"

namespace iso21496 {

// The 28-byte identifier that opens the APP2 segment carrying the gain map
// metadata, including its terminating NUL.
extern const char kIsoGainMapUrn[];
constexpr size_t kIsoGainMapUrnSize = 28;

struct GainMapMetadata {
  bool multiChannel = false;
  bool useBaseColorSpace = true;
  float baseHeadroom = 0.0f;       // log2, 0 for an SDR base image
  float alternateHeadroom = 4.0f;  // log2, the user's target
  float minBoost[3] = {0, 0, 0};   // log2
  float maxBoost[3] = {4, 4, 4};   // log2
  float gamma[3] = {2.2f, 2.2f, 2.2f};
  float baseOffset[3] = {1.0f / 64, 1.0f / 64, 1.0f / 64};
  float alternateOffset[3] = {1.0f / 64, 1.0f / 64, 1.0f / 64};
};

// The APP2 payload (identifier + binary metadata), ready to be wrapped in a
// marker segment.
Bytes buildIsoGainMapPayload(const GainMapMetadata& m);
// The complete APP2 segment for the gain map image.
Bytes buildIsoGainMapSegment(const GainMapMetadata& m);
// The marker APP2 segment for the *base* image: the URN and the two version
// fields, no parameters. Tells a decoder the image has a gain map without
// making it walk MPF first.
Bytes buildIsoBaseImageSegment();

// A fixed-size APP2 MPF segment for a two-image file. Sizes and the offset of
// the second image are zero until patchMpfSegment fills them in.
Bytes buildMpfSegmentPlaceholder();
// Patches an already-assembled two-image file in place. `file` must start with
// the primary image and contain exactly one "MPF\0" identifier in its header.
void patchMpfSegment(Bytes& file, size_t primarySize, size_t secondarySize);

// XMP for the primary image: a GContainer directory naming the two images plus
// the hdrgm version marker.
std::string buildPrimaryXmp(size_t gainMapLength);
// XMP for the gain map image: the Adobe hdrgm:1.0 parameter block.
std::string buildGainMapXmp(const GainMapMetadata& m);

}  // namespace iso21496
