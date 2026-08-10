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

Bytes buildIsoBaseImageSegment() {
  Bytes p;
  putBytes(p, kIsoGainMapUrn, kIsoGainMapUrnSize - 1);
  p.push_back(0);
  // The same two version fields the gain map's payload opens with, and nothing
  // else: this segment says only "a gain map belongs to this image". Both
  // Lightroom and the Pixel camera write it, byte for byte, and without it a
  // decoder has to discover the gain map by walking MPF to the second image
  // before it knows the file is HDR at all.
  putU16BE(p, 0);  // minimum_version
  putU16BE(p, 0);  // writer_version

  Bytes seg;
  seg.push_back(0xff);
  seg.push_back(0xe2);  // APP2
  putU16BE(seg, static_cast<uint16_t>(p.size() + 2));
  putBytes(seg, p.data(), p.size());
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

  // Entry 0: the primary baseline image. The low 24 bits are the MP type code
  // — 0x030000, "Baseline MP Primary Image" — and bit 29 is the Representative
  // Image flag, which says this is the one to display. Lightroom sets it; not
  // setting it leaves a reader to guess.
  putU32BE(p, 0x20030000);
  putU32BE(p, 0);  // size, patched later
  putU32BE(p, 0);  // offset
  putU16BE(p, 0);
  putU16BE(p, 0);

  // Entry 1: the gain map. Type code 0x050000 is "Gain Map Image".
  //
  // This was 0x000000, "Undefined", on the assumption that decoders locate the
  // gain map through the ISO 21496-1 APP2 marker alone. Apple's do. Lightroom
  // writes 0x050000, and a pipeline that reads the MPF index rather than
  // parsing every trailing image — which is the cheap way to do it, and what
  // Android and the iOS share sheet appear to do — has nothing to go on
  // without it.
  putU32BE(p, 0x00050000);
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

// A per-channel hdrgm property. When the three channels agree it is a plain
// attribute; when they differ the Adobe gain map spec — and Lightroom, and the
// Pixel camera — write an rdf:Seq of three rdf:li elements, *not* one attribute
// holding a comma separated list. This encoder wrote the comma form, which is
// what an XMP-reading decoder sees: Android and Chrome read these properties
// rather than the ISO payload, so Google Photos recognised the file as Ultra
// HDR (MPF and hdrgm:Version parse fine) and then rendered it flat, because
// every gain parameter failed to parse as a number. Apple was unaffected
// throughout — ImageIO reads the ISO rationals and never looks at this block —
// which is why the file measured correct on every tool here.
bool uniform(const float (&v)[3]) { return v[0] == v[1] && v[1] == v[2]; }

std::string attrIfUniform(const char* name, const float (&v)[3],
                          bool multiChannel) {
  if (multiChannel && !uniform(v)) return std::string();
  return std::string(" hdrgm:") + name + "=\"" + fmt(v[0]) + "\"";
}

std::string seqIfPerChannel(const char* name, const float (&v)[3],
                            bool multiChannel) {
  if (!multiChannel || uniform(v)) return std::string();
  std::ostringstream os;
  os << "<hdrgm:" << name << "><rdf:Seq>";
  for (int c = 0; c < 3; ++c) os << "<rdf:li>" << fmt(v[c]) << "</rdf:li>";
  os << "</rdf:Seq></hdrgm:" << name << ">";
  return os.str();
}

// Apple's own description of the same gain map, in its own namespaces, as a
// second rdf:Description inside the same XMP packet.
//
// This is what makes a file survive an iMessage send. iMessage does not carry a
// gain map JPEG across intact: it re-encodes, strips every ISO 21496-1 segment,
// and rebuilds the file from whatever it recognises. What it recognises is this
// block. Without it the photo arrives as a single flattened SDR image; with it
// the gain map and the chosen SDR base both come through at the original
// headroom. Verified by sending the files both ways: 2.3001 EV and 9.5% of the
// frame above SDR white either side, against 0.00 EV and 0.0% without.
//
// The gain map image has to be *genuinely* single channel for Apple to accept
// it. Declaring StoredFormat as L008 over a three channel map is not enough —
// that file was tested and flattened. Hence the guard here rather than a
// per-channel Seq: Apple's schema would express three channels happily, but the
// pipeline that reads it will not.
constexpr uint32_t kPixelFormatL008 = 0x4C303038;  // 'L008'
constexpr uint32_t kPixelFormat444f = 0x34343466;  // '444f'

// The other Apple convention for describing the same map. An iPhone 17 camera
// JPEG writes these two fields beside the three apdi ones and no HDRToneMap at
// all; ImageIO's own JPEG writer does the reverse — HDRToneMap exactly as
// below, no HDRGainMap. Both are Apple, both name the same 2020 aux type, and
// they disagree, so writing both covers whichever a given reader looks for.
//
// This was added on the theory that its absence was why iMessage flattened our
// files. It is not: a card carrying it and one without it were sent 27 -> 26
// together and both arrived at 0.00 EV. It stays because it is free — 0.0000 EV
// median and max drift through ImageIO's HDR decode against the same file
// without it — and because matching the camera costs nothing. Do not cite it
// as an iMessage fix.
//
// 0x20000 is what the camera writes; exiftool renders it "0.2.0.0".
constexpr uint32_t kAppleGainMapVersion = 0x20000;

std::string buildAppleGainMapBlock(const GainMapMetadata& m) {
  const int channels = m.multiChannel ? 3 : 1;
  std::ostringstream os;
  os << "<rdf:Description rdf:about=\"\""
     << " xmlns:apdi=\"http://ns.apple.com/pixeldatainfo/1.0/\""
     << " xmlns:HDRGainMap=\"http://ns.apple.com/HDRGainMap/1.0/\""
     << " xmlns:HDRToneMap=\"http://ns.apple.com/HDRToneMap/1.0/\">"
     << "<apdi:AuxiliaryImageType>"
     << "urn:com:apple:photo:2020:aux:hdrgainmap"
     << "</apdi:AuxiliaryImageType>"
     << "<apdi:NativeFormat>"
     << (m.multiChannel ? kPixelFormat444f : kPixelFormatL008)
     << "</apdi:NativeFormat>"
     << "<apdi:StoredFormat>"
     << (m.multiChannel ? kPixelFormat444f : kPixelFormatL008)
     << "</apdi:StoredFormat>"
     << "<HDRGainMap:HDRGainMapVersion>" << kAppleGainMapVersion
     << "</HDRGainMap:HDRGainMapVersion>"
     // Linear, not log2: the camera writes 4.560482 where ImageIO reports a
     // content headroom of 4.5605, i.e. the multiplier itself.
     << "<HDRGainMap:HDRGainMapHeadroom>"
     << fmt(std::exp2(m.alternateHeadroom))
     << "</HDRGainMap:HDRGainMapHeadroom>"
     << "<HDRToneMap:AlternateHeadroom>" << fmt(m.alternateHeadroom)
     << "</HDRToneMap:AlternateHeadroom>"
     << "<HDRToneMap:ChannelMetadata><rdf:Seq>";
  for (int c = 0; c < channels; ++c) {
    os << "<rdf:li rdf:parseType=\"Resource\">"
       << "<HDRToneMap:GainMapMin>" << fmt(m.minBoost[c])
       << "</HDRToneMap:GainMapMin>"
       << "<HDRToneMap:GainMapMax>" << fmt(m.maxBoost[c])
       << "</HDRToneMap:GainMapMax>"
       << "<HDRToneMap:Gamma>" << fmt(m.gamma[c]) << "</HDRToneMap:Gamma>"
       << "<HDRToneMap:BaseOffset>" << fmt(m.baseOffset[c])
       << "</HDRToneMap:BaseOffset>"
       << "<HDRToneMap:AlternateOffset>" << fmt(m.alternateOffset[c])
       << "</HDRToneMap:AlternateOffset>"
       << "</rdf:li>";
  }
  os << "</rdf:Seq></HDRToneMap:ChannelMetadata>"
     << "<HDRToneMap:BaseHeadroom>" << fmt(m.baseHeadroom)
     << "</HDRToneMap:BaseHeadroom>"
     << "<HDRToneMap:BaseColorIsWorkingColor>True"
     << "</HDRToneMap:BaseColorIsWorkingColor>"
     << "<HDRToneMap:Version>1</HDRToneMap:Version>"
     << "</rdf:Description>";
  return os.str();
}

std::string buildGainMapXmp(const GainMapMetadata& m) {
  static const char* kNames[] = {"GainMapMin", "GainMapMax", "Gamma",
                                 "OffsetSDR", "OffsetHDR"};
  const float(*values[])[3] = {&m.minBoost, &m.maxBoost, &m.gamma,
                               &m.baseOffset, &m.alternateOffset};

  std::ostringstream os;
  os << "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>"
     << "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">"
     << "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
     << "<rdf:Description rdf:about=\"\""
     << " xmlns:hdrgm=\"http://ns.adobe.com/hdr-gain-map/1.0/\""
     << " hdrgm:Version=\"1.0\""
     << " hdrgm:HDRCapacityMin=\"" << fmt(m.baseHeadroom) << "\""
     << " hdrgm:HDRCapacityMax=\"" << fmt(m.alternateHeadroom) << "\""
     << " hdrgm:BaseRenditionIsHDR=\"False\"";
  for (int i = 0; i < 5; ++i)
    os << attrIfUniform(kNames[i], *values[i], m.multiChannel);
  os << ">";
  for (int i = 0; i < 5; ++i)
    os << seqIfPerChannel(kNames[i], *values[i], m.multiChannel);
  os << "</rdf:Description>";
  if (m.appleGainMap) os << buildAppleGainMapBlock(m);
  os << "</rdf:RDF></x:xmpmeta><?xpacket end=\"w\"?>";
  return os.str();
}

}  // namespace iso21496
