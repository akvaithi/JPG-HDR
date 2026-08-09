// Full pipeline: synthetic HDR TIFF in, ISO 21496-1 multi-picture JPEG out.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "encoder.h"
#include "iso_metadata.h"
#include "pipeline.h"
#include "test_support.h"
#include "tiff_reader.h"

using namespace iso21496;
using namespace iso21496::test;

namespace {

std::string writeTempTiff(const TiffSpec& spec, const std::vector<float>& px,
                          const char* name) {
  std::string path = std::string(ISO21496_TEST_TMPDIR) + "/" + name;
  writeFile(path, makeTiff(spec, px));
  return path;
}

struct Parsed {
  size_t primaryEnd = 0;
  bool hasIsoUrn = false;
  bool hasMpf = false;
  bool primaryIccPresent = false;
  int primaryComponents = 0;
  int gainComponents = 0;
  uint32_t primaryWidth = 0, primaryHeight = 0;
  uint32_t gainWidth = 0, gainHeight = 0;
  uint32_t mpfPrimarySize = 0, mpfSecondarySize = 0, mpfSecondaryOffset = 0;
  size_t mpfEndianOffset = 0;
  GainMapMetadata iso;
  bool isoMultiChannel = false;
};

float readRational(const uint8_t* p) {
  int32_t num = static_cast<int32_t>(readU32BE(p));
  uint32_t den = readU32BE(p + 4);
  return den ? static_cast<float>(num) / static_cast<float>(den) : 0.0f;
}

Parsed parseFile(const Bytes& file) {
  Parsed r;
  size_t end = 0;
  auto primary = walkJpeg(file, 0, &end);
  r.primaryEnd = end;
  CHECK(!primary.empty());

  for (const auto& s : primary) {
    if (s.marker == 0xc0) {
      r.primaryHeight = readU16BE(&file[s.payloadOffset + 1]);
      r.primaryWidth = readU16BE(&file[s.payloadOffset + 3]);
      r.primaryComponents = file[s.payloadOffset + 5];
    }
    if (s.marker != 0xe2) continue;
    const uint8_t* p = &file[s.payloadOffset];
    if (s.payloadSize >= 12 && std::memcmp(p, "ICC_PROFILE", 12) == 0)
      r.primaryIccPresent = true;
    if (s.payloadSize >= 4 && std::memcmp(p, "MPF\0", 4) == 0) {
      r.hasMpf = true;
      r.mpfEndianOffset = s.payloadOffset + 4;
      const size_t entries = s.payloadOffset + 4 + 8 + (2 + 3 * 12 + 4);
      r.mpfPrimarySize = readU32BE(&file[entries + 4]);
      r.mpfSecondarySize = readU32BE(&file[entries + 16 + 4]);
      r.mpfSecondaryOffset = readU32BE(&file[entries + 16 + 8]);
    }
  }

  size_t gainEnd = 0;
  auto gain = walkJpeg(file, end, &gainEnd);
  CHECK(!gain.empty());
  for (const auto& s : gain) {
    if (s.marker == 0xc0) {
      r.gainHeight = readU16BE(&file[s.payloadOffset + 1]);
      r.gainWidth = readU16BE(&file[s.payloadOffset + 3]);
      r.gainComponents = file[s.payloadOffset + 5];
    }
    if (s.marker != 0xe2) continue;
    const uint8_t* p = &file[s.payloadOffset];
    if (s.payloadSize >= kIsoGainMapUrnSize &&
        std::memcmp(p, kIsoGainMapUrn, 27) == 0 && p[27] == 0) {
      r.hasIsoUrn = true;
      const uint8_t* q = p + kIsoGainMapUrnSize;
      uint8_t flags = q[4];
      r.isoMultiChannel = (flags & 0x80) != 0;
      const uint8_t* hd = q + 5;
      r.iso.baseHeadroom = readRational(hd);
      r.iso.alternateHeadroom = readRational(hd + 8);
      const uint8_t* c = hd + 16;
      r.iso.minBoost[0] = readRational(c);
      r.iso.maxBoost[0] = readRational(c + 8);
      r.iso.gamma[0] = readRational(c + 16);
      r.iso.baseOffset[0] = readRational(c + 24);
      r.iso.alternateOffset[0] = readRational(c + 32);
    }
  }
  CHECK_EQ(gainEnd, file.size());
  return r;
}

void fullExport() {
  TiffSpec spec;
  spec.width = 200;
  spec.height = 140;
  spec.bitsPerSample = 32;
  spec.floatSamples = true;
  spec.rowsPerStrip = 16;
  auto px = makeHdrPattern(spec.width, spec.height, 6.0f);  // ~2.6 stops over
  std::string in = writeTempTiff(spec, px, "e2e_input.tif");

  EncoderOptions o;
  o.inputPath = in;
  o.outputPath = std::string(ISO21496_TEST_TMPDIR) + "/e2e_output.jpg";
  o.targetHeadroom = 4.0f;
  o.outputPrimaries = ColorPrimaries::DisplayP3;
  o.gainMapSubsample = 2;
  o.quality = 90;
  o.inputTransfer = TransferFunction::Linear;
  o.inputPrimaries = ColorPrimaries::ProPhoto;
  // This case pins the monochrome layout, so it has to ask for it: RGB is the
  // default. multiChannelAndSubsampling covers the three-channel form. It also
  // pins the curve path, because the assertions below are about the lift the
  // curve needs — the local operator has no lift and no negative floor.
  o.multiChannelGainMap = false;
  o.toneMap = ToneMapOperator::Reinhard;
  o.sdrShape = SdrShapeMode::Manual;

  EncodeReport report;
  Bytes file = encodeToMemory(o, &report);

  CHECK_EQ(report.width, 200u);
  CHECK_EQ(report.height, 140u);
  CHECK_EQ(report.gainWidth, 100u);
  CHECK_EQ(report.gainHeight, 70u);
  CHECK_EQ(report.gainChannels, 1);  // this case asks for mono explicitly
  CHECK_EQ(report.totalBytes, file.size());
  CHECK_EQ(report.primaryBytes + report.gainMapBytes, file.size());

  Parsed p = parseFile(file);
  CHECK(p.hasIsoUrn);
  CHECK(p.hasMpf);
  CHECK(p.primaryIccPresent);
  CHECK_EQ(p.primaryComponents, 3);
  CHECK_EQ(p.gainComponents, 1);
  CHECK_EQ(p.primaryWidth, 200u);
  CHECK_EQ(p.primaryHeight, 140u);
  CHECK_EQ(p.gainWidth, 100u);
  CHECK_EQ(p.gainHeight, 70u);
  CHECK(!p.isoMultiChannel);

  // MPF must describe the file exactly as it was written.
  CHECK_EQ(p.mpfPrimarySize, static_cast<uint32_t>(report.primaryBytes));
  CHECK_EQ(p.mpfSecondarySize, static_cast<uint32_t>(report.gainMapBytes));
  CHECK_EQ(p.mpfSecondaryOffset,
           static_cast<uint32_t>(report.primaryBytes - p.mpfEndianOffset));
  CHECK_EQ(p.primaryEnd, report.primaryBytes);

  // Metadata values must reflect the requested settings.
  CHECK_NEAR(p.iso.baseHeadroom, 0.0, 1e-6);
  // The gamma written must be the one the encoder actually applied, not its
  // reciprocal: decoders recover the gain as pow(stored, 1/gamma), so writing
  // the inverse silently rewrites every midtone. Asserted as the round trip
  // rather than as a number, because the number is the part that was wrong.
  CHECK_NEAR(p.iso.gamma[0], o.gainMapGamma, 1e-4);
  CHECK(p.iso.gamma[0] > 0.0f);
  CHECK_NEAR(p.iso.baseOffset[0], 0.015625, 1e-4);
  CHECK(p.iso.maxBoost[0] > 2.0f && p.iso.maxBoost[0] <= 4.0f);
  CHECK_NEAR(report.measuredHeadroom, std::log2(6.0), 0.15);

  // Reported for auditing a batch, not used by the pipeline: the share of the
  // render the edit put above SDR white. The synthetic pattern's specular patch
  // is about a tenth of the frame, so this must be a real fraction rather than
  // zero or everything — the failure worth catching is a statistic that is
  // silently never populated.
  CHECK(report.fractionAboveWhite > 0.0f);
  CHECK(report.fractionAboveWhite < 0.5f);

  // The declared alternate headroom must be what the image needs, not the
  // 4.0 EV ceiling that was asked for. Declaring the ceiling would make a
  // decoder scale the gain by display_headroom / 4.0 and render the photo
  // dim on any display with less than 4 stops of headroom.
  CHECK_NEAR(p.iso.alternateHeadroom, report.measuredHeadroom, 1e-3);
  CHECK(p.iso.alternateHeadroom < 3.5f);

  // The gain map's own maximum is a separate quantity, and is deliberately no
  // longer tied to it: the headroom says how much brighter than SDR white the
  // picture goes, while the gain range says what each channel needs to get
  // back there from the base. A saturated highlight needs more of the latter
  // than the former, and Lightroom's own exports write maxima up to 0.39 EV
  // above their declared headroom for exactly this reason. Tying the two
  // together is what was truncating saturated highlights toward neutral.
  CHECK(p.iso.maxBoost[0] > 0.0f);
  CHECK_NEAR(p.iso.maxBoost[0], report.maxBoostLog2, 1e-3);

  // Lifting the base means it is brighter than the HDR image through the
  // midtones, so the gain map has to be able to darken as well as brighten.
  CHECK(report.minBoostLog2 < 0.0f);
  CHECK(report.minBoostLog2 >= -1.0f);
  CHECK_NEAR(p.iso.minBoost[0], report.minBoostLog2, 1e-3);
}

// Nothing in the SDR base may reach flat white.
//
// compressBase only promises the *base layer* never rises above the identity.
// The detail layer is added over it untouched, and at the default --sdr-detail
// 1.25 it is amplified, so the composite used to land above white and be hard
// clipped there. Measured across six real frames that cost between 1.4% and
// 11.5% of the picture to all three channels at 255 — no texture, no hue —
// where Lightroom's export of the same renders clipped 0.00% on every one.
//
// A pixel with all three channels at 255 is the failure: the highlight has
// become a shape rather than a highlight, and no gain map can put back
// information the base does not carry.
void theBaseNeverReachesFlatWhite() {
  TiffSpec spec;
  spec.width = 160;
  spec.height = 120;
  spec.bitsPerSample = 32;
  spec.floatSamples = true;
  // A bright frame with detail sitting on top of already-bright areas, which is
  // the case that overshoots: dim scenes never approach white to begin with.
  auto px = makeDetailedHdrPattern(spec.width, spec.height, 8.0f);
  for (auto& v : px) v *= 3.0f;
  std::string in = writeTempTiff(spec, px, "e2e_clip.tif");

  EncoderOptions o;
  o.inputPath = in;
  o.outputPath = std::string(ISO21496_TEST_TMPDIR) + "/e2e_clip.jpg";
  EncodeReport report;
  Bytes file = encodeToMemory(o, &report);
  CHECK(!file.empty());

  PipelineOptions po;
  po.outputPrimaries = o.outputPrimaries;
  po.toneMap = o.toneMap;
  po.sdrDetail = o.sdrDetail;
  po.sdrHighlightKnee = o.sdrHighlightKnee;
  po.targetHeadroom = o.targetHeadroom;
  TiffReader tiff(readFile(in));
  PipelineResult px2 = runPipeline(tiff, po);

  size_t flat = 0;
  for (size_t i = 0; i + 2 < px2.sdr.size(); i += 3) {
    if (px2.sdr[i] == 255 && px2.sdr[i + 1] == 255 && px2.sdr[i + 2] == 255)
      ++flat;
  }
  CHECK_EQ(flat, size_t{0});

  // And the shoulder must not have bought that by darkening the picture: the
  // brighter midtones are the reason this base is worth having over a plain
  // SDR export, and folding the whole headroom below white would take them.
  double sum = 0;
  for (size_t i = 0; i < px2.sdr.size(); ++i) sum += px2.sdr[i];
  const double mean = sum / static_cast<double>(px2.sdr.size()) / 255.0;
  CHECK(mean > 0.35);
}

// The hdrgm properties must carry one value per channel when the map is
// multichannel, and in the form the gain map spec defines: an rdf:Seq of three
// rdf:li elements. This test used to assert a comma separated attribute, which
// is what the encoder wrote and what no decoder reads — the values were all
// there and all unparseable. Apple never noticed because ImageIO reads the ISO
// rationals instead, which is how it survived every measurement here.
void gainMapXmpCarriesEveryChannel() {
  TiffSpec spec;
  spec.width = 64;
  spec.height = 48;
  // Float samples, so the highlight survives as HDR: read as 16-bit the file
  // decodes through ROMM and everything above SDR white clips, which leaves
  // nothing for the three channels to disagree about.
  spec.bitsPerSample = 32;
  spec.floatSamples = true;
  // The shared pattern's specular highlight is neutral, so all three channels
  // measure the same ceiling and the spec's plain-attribute form is correct.
  // Warm it up: a coloured highlight above SDR white is what drives the three
  // maxima apart, and is the case the rdf:Seq form exists for.
  auto px = makeHdrPattern(spec.width, spec.height, 4.0f);
  for (size_t i = 0; i < px.size(); i += 3) {
    if (px[i] > 1.0f) {
      px[i + 1] *= 0.75f;
      px[i + 2] *= 0.45f;
    }
  }
  std::string in = writeTempTiff(spec, px, "e2e_xmp.tif");
  EncoderOptions o;
  o.inputPath = in;
  o.outputPath = std::string(ISO21496_TEST_TMPDIR) + "/e2e_xmp.jpg";
  EncodeReport report;
  Bytes file = encodeToMemory(o, &report);
  CHECK_EQ(report.gainChannels, 3);

  const std::string text(reinterpret_cast<const char*>(file.data()), file.size());
  // An element, not an attribute, and three <rdf:li> values inside it.
  const size_t at = text.find("<hdrgm:GainMapMax>");
  CHECK(at != std::string::npos);
  CHECK(text.find("hdrgm:GainMapMax=\"") == std::string::npos);
  const size_t end = text.find("</hdrgm:GainMapMax>", at);
  CHECK(end != std::string::npos);
  const std::string seq = text.substr(at, end - at);
  CHECK(seq.find("<rdf:Seq>") != std::string::npos);
  size_t items = 0, from = 0;
  while ((from = seq.find("<rdf:li>", from)) != std::string::npos) {
    ++items;
    from += 8;
  }
  CHECK_EQ(items, size_t{3});

  // A monochrome map has one value per channel and one channel, so the spec's
  // plain attribute form applies — which is also the only form libultrahdr's
  // XMP reader accepts.
  o.multiChannelGainMap = false;
  Bytes monoFile = encodeToMemory(o, &report);
  const std::string monoText(reinterpret_cast<const char*>(monoFile.data()),
                             monoFile.size());
  CHECK(monoText.find("hdrgm:GainMapMax=\"") != std::string::npos);
  CHECK(monoText.find("<hdrgm:GainMapMax>") == std::string::npos);
}

void multiChannelAndSubsampling() {
  TiffSpec spec;
  spec.width = 64;
  spec.height = 48;
  auto px = makeHdrPattern(spec.width, spec.height, 1.0f);
  std::string in = writeTempTiff(spec, px, "e2e_mc.tif");

  for (int sub : {1, 2, 4}) {
    EncoderOptions o;
    o.inputPath = in;
    o.outputPath = std::string(ISO21496_TEST_TMPDIR) + "/e2e_mc.jpg";
    o.gainMapSubsample = sub;
    o.multiChannelGainMap = true;
    o.autoMaxBoost = false;
    EncodeReport report;
    Bytes file = encodeToMemory(o, &report);
    Parsed p = parseFile(file);
    CHECK(p.isoMultiChannel);
    CHECK_EQ(p.gainComponents, 3);
    CHECK_EQ(p.gainWidth, static_cast<uint32_t>((64 + sub - 1) / sub));
    CHECK_EQ(p.gainHeight, static_cast<uint32_t>((48 + sub - 1) / sub));
    // With the measurement disabled the declared headroom is the ceiling that
    // was asked for. The gain range is still measured — it describes what the
    // base needs, which is a different question from what the file declares.
    CHECK_NEAR(p.iso.alternateHeadroom, 4.0, 1e-3);
    CHECK(p.iso.maxBoost[0] > 0.0f);
    CHECK(p.iso.maxBoost[0] <= 4.0f + 1e-3f);
  }
}

// A small specular highlight must survive: the headroom measurement averages
// the image down, and a grid-sampling scheme could step straight over a glint
// this size and clip it out of the HDR rendition entirely.
void smallHighlightsAreNotMissed() {
  const uint32_t w = 1024, h = 768;
  std::vector<float> px(static_cast<size_t>(w) * h * 3, 0.18f);
  // A 6x6 glint at 32x SDR white — 0.005% of the frame.
  for (uint32_t y = 400; y < 406; ++y)
    for (uint32_t x = 500; x < 506; ++x)
      for (int c = 0; c < 3; ++c) px[(static_cast<size_t>(y) * w + x) * 3 + c] = 32.0f;

  TiffSpec spec;
  spec.width = w;
  spec.height = h;
  spec.bitsPerSample = 32;
  spec.floatSamples = true;
  std::string in = writeTempTiff(spec, px, "e2e_glint.tif");

  EncoderOptions o;
  o.inputPath = in;
  o.outputPath = std::string(ISO21496_TEST_TMPDIR) + "/e2e_glint.jpg";
  o.inputTransfer = TransferFunction::Linear;
  o.inputPrimaries = ColorPrimaries::sRGB;
  o.outputPrimaries = ColorPrimaries::sRGB;
  o.targetHeadroom = 4.0f;
  EncodeReport report;
  encodeToMemory(o, &report);

  // The glint is 5 stops up, so the true peak must see all of it...
  CHECK_NEAR(report.truePeakHeadroom, 5.0, 0.05);
  // ...and the softened measurement must still register it as real headroom
  // rather than averaging it away into the 0.18 background.
  CHECK(report.measuredHeadroom > 1.0f);
  CHECK(report.maxBoostLog2 > 1.0f);
}

// The file-size benchmark from the spec: a mono 1:2 gain map must cost far
// less than a second full-size image.
void gainMapOverheadIsSmall() {
  TiffSpec spec;
  spec.width = 512;
  spec.height = 384;
  auto px = makeHdrPattern(spec.width, spec.height, 1.0f);
  std::string in = writeTempTiff(spec, px, "e2e_size.tif");

  EncoderOptions o;
  o.inputPath = in;
  o.outputPath = std::string(ISO21496_TEST_TMPDIR) + "/e2e_size.jpg";
  o.multiChannelGainMap = false;  // RGB is the default; ask for mono explicitly
  o.gainMapSubsample = 1;
  EncodeReport mono;
  encodeToMemory(o, &mono);

  o.multiChannelGainMap = true;
  EncodeReport rgbFull;
  encodeToMemory(o, &rgbFull);

  const double monoOverhead =
      static_cast<double>(mono.gainMapBytes) / mono.primaryBytes;
  const double rgbOverhead =
      static_cast<double>(rgbFull.gainMapBytes) / rgbFull.primaryBytes;
  // A monochrome map is smaller than a three-channel one at the same scale.
  // It is not offered in the plug-in — it cannot follow a highlight that
  // changes hue, and measured against Lightroom it left highlights 0.63 EV
  // duller — but the CLI keeps it, so it keeps its test.
  CHECK(monoOverhead < 0.6);
  CHECK(monoOverhead < rgbOverhead);
}

void reportsBadInputClearly() {
  EncoderOptions o;
  o.inputPath = std::string(ISO21496_TEST_TMPDIR) + "/does_not_exist.tif";
  o.outputPath = std::string(ISO21496_TEST_TMPDIR) + "/nope.jpg";
  CHECK_THROWS(encodeToMemory(o, nullptr));

  TiffSpec spec;
  auto px = makeHdrPattern(spec.width, spec.height, 1.0f);
  std::string in = writeTempTiff(spec, px, "e2e_bad.tif");
  EncoderOptions bad;
  bad.inputPath = in;
  bad.outputPath = std::string(ISO21496_TEST_TMPDIR) + "/nope.jpg";
  bad.gainMapSubsample = 3;
  CHECK_THROWS(encodeToMemory(bad, nullptr));
  bad.gainMapSubsample = 2;
  bad.targetHeadroom = 0.0f;
  CHECK_THROWS(encodeToMemory(bad, nullptr));
}

void run() {
  fullExport();
  theBaseNeverReachesFlatWhite();
  gainMapXmpCarriesEveryChannel();
  multiChannelAndSubsampling();
  smallHighlightsAreNotMissed();
  gainMapOverheadIsSmall();
  reportsBadInputClearly();
}

}  // namespace

TEST_MAIN(run)
