#include "iso_metadata.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <string>

namespace iso21496 {
namespace {

// ISO 21496-1 stores every parameter as a rational, not as a float. One fixed
// denominator keeps the payload byte-identical for a given set of settings.
constexpr uint32_t kDenominator = 1000000;

void putRationalS(Bytes& b, float v) {
  double scaled = static_cast<double>(v) * kDenominator;
  if (scaled > 2147483647.0) scaled = 2147483647.0;
  if (scaled < -2147483648.0) scaled = -2147483648.0;
  putS32BE(b, static_cast<int32_t>(std::llround(scaled)));
  putU32BE(b, kDenominator);
}

void putRationalU(Bytes& b, float v) {
  double scaled = static_cast<double>(v) * kDenominator;
  if (scaled < 0.0) scaled = 0.0;
  if (scaled > 4294967295.0) scaled = 4294967295.0;
  putU32BE(b, static_cast<uint32_t>(std::llround(scaled)));
  putU32BE(b, kDenominator);
}

std::string fmt(float v) {
  std::ostringstream os;
  os.precision(6);
  os << std::defaultfloat << v;
  return os.str();
}

// Byte layout of the MPF payload, relative to the start of "MPF\0".
constexpr size_t kMpfEndianOffset = 4;
constexpr size_t kMpfIfdOffset = kMpfEndianOffset + 8;
constexpr size_t kMpfEntryCount = 2;
// count(2) + 3 entries * 12 + next-IFD(4)
constexpr size_t kMpfIfdBytes = 2 + 3 * 12 + 4;
constexpr size_t kMpfEntriesOffset = kMpfIfdOffset + kMpfIfdBytes;
constexpr size_t kMpfPayloadSize = kMpfEntriesOffset + kMpfEntryCount * 16;

}  // namespace

const char kIsoGainMapUrn[] = "urn:iso:std:iso:ts:21496:-1";

Bytes buildIsoGainMapPayload(const GainMapMetadata& m) {
  Bytes p;
  putBytes(p, kIsoGainMapUrn, kIsoGainMapUrnSize - 1);
  p.push_back(0);  // the URN is NUL terminated: 28 bytes in total
  static_assert(sizeof(kIsoGainMapUrn) == kIsoGainMapUrnSize,
                "the ISO gain map URN must be 28 bytes including its NUL");

  putU16BE(p, 0);  // minimum_version: decoders gate on this
  putU16BE(p, 1);  // writer_version

  uint8_t flags = 0;
  if (m.multiChannel) flags |= 0x80;
  if (m.useBaseColorSpace) flags |= 0x40;
  p.push_back(flags);

  putRationalU(p, m.baseHeadroom);
  putRationalU(p, m.alternateHeadroom);

  const int channels = m.multiChannel ? 3 : 1;
  for (int c = 0; c < channels; ++c) {
    putRationalS(p, m.minBoost[c]);
    putRationalS(p, m.maxBoost[c]);
    putRationalU(p, m.gamma[c]);
    putRationalS(p, m.baseOffset[c]);
    putRationalS(p, m.alternateOffset[c]);
  }
  return p;
}

Bytes buildIsoGainMapSegment(const GainMapMetadata& m) {
  Bytes payload = buildIsoGainMapPayload(m);
  Bytes seg;
  seg.push_back(0xff);
  seg.push_back(0xe2);  // APP2
  putU16BE(seg, static_cast<uint16_t>(payload.size() + 2));
  putBytes(seg, payload.data(), payload.size());
  return seg;
}

Bytes buildMpfSegmentPlaceholder() {
  Bytes p;
  putString(p, "MPF");
  p.push_back(0);
  // MP Endian field: big endian, first IFD 8 bytes in.
  putString(p, "MM");
  putU16BE(p, 42);
  putU32BE(p, 8);

  putU16BE(p, 3);  // three tags in the MP Index IFD

  putU16BE(p, 0xb000);  // MPFVersion
  putU16BE(p, 7);       // UNDEFINED
  putU32BE(p, 4);
  putString(p, "0100");

  putU16BE(p, 0xb001);  // NumberOfImages
  putU16BE(p, 4);       // LONG
  putU32BE(p, 1);
  putU32BE(p, static_cast<uint32_t>(kMpfEntryCount));

  putU16BE(p, 0xb002);  // MPEntry
  putU16BE(p, 7);       // UNDEFINED
  putU32BE(p, static_cast<uint32_t>(kMpfEntryCount * 16));
  putU32BE(p, static_cast<uint32_t>(kMpfEntriesOffset - kMpfEndianOffset));

  putU32BE(p, 0);  // no next IFD

  // Entry 0: the primary baseline image. Attribute 0x030000 marks it as the
  // "Baseline MP Primary Image"; its offset is always zero.
  putU32BE(p, 0x030000);
  putU32BE(p, 0);  // size, patched later
  putU32BE(p, 0);  // offset
  putU16BE(p, 0);
  putU16BE(p, 0);

  // Entry 1: the gain map. Type "undefined" is what current gain-map writers
  // use; decoders locate it through the ISO 21496-1 APP2 marker.
  putU32BE(p, 0x000000);
  putU32BE(p, 0);  // size, patched later
  putU32BE(p, 0);  // offset, patched later
  putU16BE(p, 0);
  putU16BE(p, 0);

  if (p.size() != kMpfPayloadSize) fail("internal error: MPF payload size");

  Bytes seg;
  seg.push_back(0xff);
  seg.push_back(0xe2);
  putU16BE(seg, static_cast<uint16_t>(p.size() + 2));
  putBytes(seg, p.data(), p.size());
  return seg;
}

void patchMpfSegment(Bytes& file, size_t primarySize, size_t secondarySize) {
  // Walk the primary image's marker segments rather than scanning for the
  // identifier: Exif or an ICC profile could contain the same four bytes.
  const size_t limit = std::min(primarySize, file.size());
  size_t idPos = std::string::npos;
  size_t i = 2;  // skip SOI
  while (i + 4 <= limit) {
    if (file[i] != 0xff) break;
    const uint8_t marker = file[i + 1];
    if (marker == 0xd8 || marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) {
      i += 2;
      continue;
    }
    if (marker == 0xda || marker == 0xd9) break;  // start of scan: past the headers
    const size_t length = readU16BE(&file[i + 2]);
    if (length < 2 || i + 2 + length > limit) break;
    const size_t payload = i + 4;
    if (marker == 0xe2 && length - 2 >= 4 &&
        std::memcmp(&file[payload], "MPF\0", 4) == 0) {
      idPos = payload;
      break;
    }
    i += 2 + length;
  }
  if (idPos == std::string::npos)
    fail("internal error: MPF segment not found in the primary image");
  const size_t endian = idPos + kMpfEndianOffset;
  const size_t entries = idPos + kMpfEntriesOffset;
  if (entries + kMpfEntryCount * 16 > file.size())
    fail("internal error: truncated MPF segment");

  writeU32BE(&file[entries + 4], static_cast<uint32_t>(primarySize));
  writeU32BE(&file[entries + 8], 0);
  writeU32BE(&file[entries + 16 + 4], static_cast<uint32_t>(secondarySize));
  // Offsets in the MP Index IFD are measured from the MP Endian field.
  writeU32BE(&file[entries + 16 + 8],
             static_cast<uint32_t>(primarySize - endian));
}

std::string buildPrimaryXmp(size_t gainMapLength) {
  std::ostringstream os;
  os << "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>"
     << "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
     << "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
     << "<rdf:Description rdf:about=\"\""
     << " xmlns:Container=\"http://ns.google.com/photos/1.0/container/\""
     << " xmlns:Item=\"http://ns.google.com/photos/1.0/container/item/\""
     << " xmlns:hdrgm=\"http://ns.adobe.com/hdr-gain-map/1.0/\""
     << " hdrgm:Version=\"1.0\">"
     << "<Container:Directory><rdf:Seq>"
     << "<rdf:li rdf:parseType=\"Resource\">"
     << "<Container:Item Item:Semantic=\"Primary\" Item:Mime=\"image/jpeg\"/>"
     << "</rdf:li>"
     << "<rdf:li rdf:parseType=\"Resource\">"
     << "<Container:Item Item:Semantic=\"GainMap\" Item:Mime=\"image/jpeg\""
     << " Item:Length=\"" << gainMapLength << "\"/>"
     << "</rdf:li>"
     << "</rdf:Seq></Container:Directory>"
     << "</rdf:Description></rdf:RDF></x:xmpmeta><?xpacket end=\"w\"?>";
  return os.str();
}

// The hdrgm attributes take one value per channel, comma separated, when the
// map is multichannel — Lightroom writes "2.366608, 2.507721, 2.68985". Writing
// only the first channel's number would tell an XMP-reading decoder (Android,
// Chrome — the ones that do not read the ISO payload) to apply the red range to
// all three, which is a hue shift in exactly the highlights the three channels
// exist to keep apart.
std::string perChannel(const float (&v)[3], bool multiChannel) {
  if (!multiChannel) return fmt(v[0]);
  return fmt(v[0]) + ", " + fmt(v[1]) + ", " + fmt(v[2]);
}

std::string buildGainMapXmp(const GainMapMetadata& m) {
  std::ostringstream os;
  os << "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>"
     << "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
     << "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
     << "<rdf:Description rdf:about=\"\""
     << " xmlns:hdrgm=\"http://ns.adobe.com/hdr-gain-map/1.0/\""
     << " hdrgm:Version=\"1.0\""
     << " hdrgm:GainMapMin=\"" << perChannel(m.minBoost, m.multiChannel) << "\""
     << " hdrgm:GainMapMax=\"" << perChannel(m.maxBoost, m.multiChannel) << "\""
     << " hdrgm:Gamma=\"" << perChannel(m.gamma, m.multiChannel) << "\""
     << " hdrgm:OffsetSDR=\"" << perChannel(m.baseOffset, m.multiChannel) << "\""
     << " hdrgm:OffsetHDR=\"" << perChannel(m.alternateOffset, m.multiChannel) << "\""
     << " hdrgm:HDRCapacityMin=\"" << fmt(m.baseHeadroom) << "\""
     << " hdrgm:HDRCapacityMax=\"" << fmt(m.alternateHeadroom) << "\""
     << " hdrgm:BaseRenditionIsHDR=\"False\"/>"
     << "</rdf:RDF></x:xmpmeta><?xpacket end=\"w\"?>";
  return os.str();
}

}  // namespace iso21496
