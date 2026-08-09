#include "options.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

#include "version.h"

namespace iso21496 {
namespace {

const char kUsage[] =
    "iso21496_encoder " ISO21496_ENCODER_VERSION "\n"
    "Encodes an ISO 21496-1 gain map HDR JPEG from a rendered TIFF.\n"
    "\n"
    "Usage:\n"
    "  iso21496_encoder --input <file.tif> --output <file.jpg> [options]\n"
    "\n"
    "Primary options (these map 1:1 onto the Lightroom export dialog):\n"
    "  --headroom <stops>       Target HDR headroom in stops: 1.0, 2.0, 3.0,\n"
    "                           4.0 (default 4.0 = +4 EV, about 1280 nits).\n"
    "  --color-space <name>     Base image colour space: DisplayP3 (default),\n"
    "                           sRGB, Rec2020.\n"
    "  --subsample <1|2|4>      Gain map scale: 1 = full (default), 2 = half,\n"
    "                           4 = quarter resolution.\n"
    "  --channels <mono|rgb>    Gain map channels: rgb (default) or mono.\n"
    "  --apple-compatible       Also describe the gain map the way Apple does,\n"
    "                           so the photo survives being sent over iMessage.\n"
    "                           Forces a single channel map, which Apple's\n"
    "                           pipeline requires: costs 0.25 EV in saturated\n"
    "                           highlights and 0.61 EV of hue drift.\n"
    "  --quality <60-100>       Baseline JPEG quality (default 90).\n"
    "\n"
    "SDR base image look (affects only the rendition seen without an HDR\n"
    "display; the HDR result is unchanged, because the gain map is measured\n"
    "against whatever base these produce). Both are solved from the image by\n"
    "default — how much the tone curve darkened a frame depends on where its\n"
    "tones sit, so one fixed pair of numbers cannot be right for every photo:\n"
    "  --auto-shape             Solve the lift and contrast from the image\n"
    "                           (the default; use this to override an earlier\n"
    "                           --sdr-lift or --sdr-contrast).\n"
    "  --sdr-lift <EV>          Brighten the base by this many stops, instead\n"
    "                           of solving it. Applied as an exposure gain on\n"
    "                           the tone-mapped image, so highlights pushed\n"
    "                           past white clip in the base and are handed back\n"
    "                           by the gain map. 0 disables.\n"
    "  --sdr-contrast <x>       Contrast about a linear mid-grey pivot, instead\n"
    "                           of solving it. Applied as a luminance-derived\n"
    "                           scale on RGB so saturation is untouched.\n"
    "                           1.0 disables.\n"
    "\n"
    "Gain map tuning:\n"
    "  --gainmap-quality <n>    JPEG quality of the gain map (default 50).\n"
    "  --gamma <g>              Gain map encoding gamma (default 2.2).\n"
    "  --offset-sdr <v>         SDR offset constant (default 0.015625).\n"
    "  --offset-hdr <v>         HDR offset constant (default 0.015625).\n"
    "  --tone-map <name>        local (default), reinhard, filmic or clip. Only\n"
    "                           local preserves highlight separation; the\n"
    "                           others are curves, and a curve cannot.\n"
    "  --sdr-detail <x>         Local tone mapping only: how much of the detail\n"
    "                           layer is put back over the compressed base\n"
    "                           (default 1.0 = in full, 0 = a plain shoulder).\n"
    "  --peak-detect <name>     softened (default) averages the image down\n"
    "                           before measuring its peak, so a few hot pixels\n"
    "                           cannot define the headroom; exact uses the\n"
    "                           true per-pixel maximum.\n"
    "  --no-auto-max-boost      Always store the full target headroom as the\n"
    "                           gain map maximum instead of measuring the\n"
    "                           headroom the image actually uses.\n"
    "\n"
    "Input interpretation:\n"
    "  --input-primaries <name> auto (default), prophoto, srgb, p3, rec2020,\n"
    "                           adobergb.\n"
    "  --input-transfer <name>  auto (default), linear, srgb, gamma1.8,\n"
    "                           gamma2.2, romm, pq, hlg.\n"
    "  --pq-diffuse-white <n>   Nits mapped to SDR white for PQ/HLG input\n"
    "                           (default 203).\n"
    "\n"
    "Output plumbing:\n"
    "  --no-icc                 Do not embed an ICC profile.\n"
    "  --no-exif                Do not copy Exif metadata from the input.\n"
    "  --no-xmp                 Do not write the Adobe hdrgm XMP blocks.\n"
    "  --no-optimize            Use the standard Huffman tables (faster, and\n"
    "                           roughly 5 percent larger).\n"
    "  --chroma-subsample       Encode the base image as 4:2:0. The default is\n"
    "                           4:4:4: the gain map is measured against the\n"
    "                           base as written, so chroma error in the base\n"
    "                           lands in the HDR rendition. Measured, 4:2:0\n"
    "                           costs more accuracy than it saves bytes.\n"
    "  --no-chroma-subsample    Force 4:4:4 (already the default).\n"
    "  --threads <n>            Worker threads (default: all cores).\n"
    "  --json                   Print a one-line JSON report on stdout.\n"
    "  --verbose                Log progress to stderr.\n"
    "  --version, --help\n";

bool needValue(int argc, char** argv, int i, const char* name,
               std::string* out) {
  if (i + 1 >= argc) fail(std::string("missing value for ") + name);
  *out = argv[i + 1];
  return true;
}

int toInt(const std::string& s, const char* name) {
  char* end = nullptr;
  long v = std::strtol(s.c_str(), &end, 10);
  if (end == s.c_str() || *end != '\0')
    fail(std::string("invalid integer for ") + name + ": " + s);
  return static_cast<int>(v);
}

float toFloat(const std::string& s, const char* name) {
  char* end = nullptr;
  double v = std::strtod(s.c_str(), &end);
  if (end == s.c_str() || *end != '\0')
    fail(std::string("invalid number for ") + name + ": " + s);
  return static_cast<float>(v);
}

}  // namespace

const char* usageText() { return kUsage; }

bool parseArguments(int argc, char** argv, EncoderOptions* out, bool* handled) {
  *handled = false;
  std::string v;
  bool autoShapeRequested = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      std::cout << kUsage;
      *handled = true;
      return true;
    }
    if (a == "--version") {
      std::cout << ISO21496_ENCODER_VERSION << "\n";
      *handled = true;
      return true;
    }
    if (a == "--input" || a == "-i") {
      needValue(argc, argv, i++, "--input", &v);
      out->inputPath = v;
    } else if (a == "--output" || a == "-o") {
      needValue(argc, argv, i++, "--output", &v);
      out->outputPath = v;
    } else if (a == "--headroom") {
      needValue(argc, argv, i++, "--headroom", &v);
      out->targetHeadroom = toFloat(v, "--headroom");
    } else if (a == "--color-space" || a == "--colour-space") {
      needValue(argc, argv, i++, "--color-space", &v);
      if (!parsePrimaries(v, &out->outputPrimaries) ||
          out->outputPrimaries == ColorPrimaries::Auto)
        fail("unknown --color-space: " + v + " (use DisplayP3, sRGB, Rec2020)");
      if (out->outputPrimaries == ColorPrimaries::ProPhoto)
        fail("ProPhoto RGB is not a valid base image colour space");
    } else if (a == "--subsample") {
      needValue(argc, argv, i++, "--subsample", &v);
      out->gainMapSubsample = toInt(v, "--subsample");
    } else if (a == "--channels") {
      needValue(argc, argv, i++, "--channels", &v);
      if (v == "mono" || v == "Monochrome" || v == "1") out->multiChannelGainMap = false;
      else if (v == "rgb" || v == "RGB" || v == "3") out->multiChannelGainMap = true;
      else fail("unknown --channels: " + v + " (use mono or rgb)");
    } else if (a == "--apple-compatible") {
      out->appleCompatible = true;
    } else if (a == "--quality" || a == "-q") {
      needValue(argc, argv, i++, "--quality", &v);
      out->quality = toInt(v, "--quality");
    } else if (a == "--gainmap-quality") {
      needValue(argc, argv, i++, "--gainmap-quality", &v);
      out->gainMapQuality = toInt(v, "--gainmap-quality");
    } else if (a == "--gamma") {
      needValue(argc, argv, i++, "--gamma", &v);
      out->gainMapGamma = toFloat(v, "--gamma");
    } else if (a == "--offset-sdr") {
      needValue(argc, argv, i++, "--offset-sdr", &v);
      out->offsetSdr = toFloat(v, "--offset-sdr");
    } else if (a == "--offset-hdr") {
      needValue(argc, argv, i++, "--offset-hdr", &v);
      out->offsetHdr = toFloat(v, "--offset-hdr");
    } else if (a == "--tone-map") {
      needValue(argc, argv, i++, "--tone-map", &v);
      if (!parseToneMap(v, &out->toneMap))
        fail("unknown --tone-map: " + v + " (use local, reinhard, filmic or clip)");
    } else if (a == "--peak-detect") {
      needValue(argc, argv, i++, "--peak-detect", &v);
      if (!parsePeakDetect(v, &out->peakDetect))
        fail("unknown --peak-detect: " + v + " (use softened or exact)");
    } else if (a == "--sdr-lift") {
      needValue(argc, argv, i++, "--sdr-lift", &v);
      out->sdrLiftEV = toFloat(v, "--sdr-lift");
      if (out->sdrLiftEV < 0.0f || out->sdrLiftEV > 3.0f)
        fail("--sdr-lift must be between 0 and 3 EV");
      out->sdrShape = SdrShapeMode::Manual;
    } else if (a == "--sdr-contrast") {
      needValue(argc, argv, i++, "--sdr-contrast", &v);
      out->sdrContrast = toFloat(v, "--sdr-contrast");
      if (out->sdrContrast < 0.5f || out->sdrContrast > 2.0f)
        fail("--sdr-contrast must be between 0.5 and 2.0");
      out->sdrShape = SdrShapeMode::Manual;
    } else if (a == "--sdr-detail") {
      needValue(argc, argv, i++, "--sdr-detail", &v);
      out->sdrDetail = toFloat(v, "--sdr-detail");
      if (out->sdrDetail < 0.0f || out->sdrDetail > 2.0f)
        fail("--sdr-detail must be between 0 and 2");
    } else if (a == "--sdr-knee") {
      needValue(argc, argv, i++, "--sdr-knee", &v);
      out->sdrKnee = toFloat(v, "--sdr-knee");
      if (out->sdrKnee > 0.0f || out->sdrKnee < -6.0f)
        fail("--sdr-knee must be between -6 and 0 EV");
    } else if (a == "--sdr-edge") {
      needValue(argc, argv, i++, "--sdr-edge", &v);
      out->sdrEdge = toFloat(v, "--sdr-edge");
      if (out->sdrEdge <= 0.0f) fail("--sdr-edge must be positive");
    } else if (a == "--auto-shape") {
      autoShapeRequested = true;
        } else if (a == "--no-auto-max-boost") {
      out->autoMaxBoost = false;
    } else if (a == "--input-primaries") {
      needValue(argc, argv, i++, "--input-primaries", &v);
      if (!parsePrimaries(v, &out->inputPrimaries))
        fail("unknown --input-primaries: " + v);
    } else if (a == "--input-transfer") {
      needValue(argc, argv, i++, "--input-transfer", &v);
      if (!parseTransfer(v, &out->inputTransfer))
        fail("unknown --input-transfer: " + v);
    } else if (a == "--pq-diffuse-white") {
      needValue(argc, argv, i++, "--pq-diffuse-white", &v);
      out->pqDiffuseWhiteNits = toFloat(v, "--pq-diffuse-white");
    } else if (a == "--no-icc") {
      out->writeIcc = false;
    } else if (a == "--no-exif") {
      out->writeExif = false;
    } else if (a == "--no-xmp") {
      out->writeXmp = false;
    } else if (a == "--no-optimize") {
      out->optimizeHuffman = false;
    } else if (a == "--no-chroma-subsample") {
      out->chromaSubsample = false;
    } else if (a == "--chroma-subsample") {
      // The positive form exists because the default flipped to 4:4:4; without
      // it there was no way to ask for 4:2:0 at all, and --no-chroma-subsample
      // had quietly become a no-op.
      out->chromaSubsample = true;
    } else if (a == "--threads") {
      needValue(argc, argv, i++, "--threads", &v);
      int n = toInt(v, "--threads");
      if (n < 0) fail("--threads must not be negative");
      out->threads = static_cast<unsigned>(n);
    } else if (a == "--json") {
      out->json = true;
    } else if (a == "--verbose") {
      g_verbose = true;
    } else {
      fail("unknown argument: " + a);
    }
  }

  // Applied after the loop so it wins regardless of order: a preset still
  // carrying an old --sdr-lift can be moved onto the solver by appending
  // one flag, without having to strip the old one out first.
  if (autoShapeRequested) out->sdrShape = SdrShapeMode::Auto;

  if (out->inputPath.empty()) fail("missing --input");
  if (out->outputPath.empty()) fail("missing --output");
  if (out->quality < 1 || out->quality > 100)
    fail("--quality must be between 1 and 100");
  if (out->gainMapQuality < 1 || out->gainMapQuality > 100)
    fail("--gainmap-quality must be between 1 and 100");
  return true;
}

}  // namespace iso21496
