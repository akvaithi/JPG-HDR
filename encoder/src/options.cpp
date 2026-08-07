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
    "  --subsample <1|2|4>      Gain map scale: 1 = full, 2 = half (default),\n"
    "                           4 = quarter resolution.\n"
    "  --channels <mono|rgb>    Gain map channels: mono (default) or rgb.\n"
    "  --quality <60-100>       Baseline JPEG quality (default 90).\n"
    "\n"
    "Gain map tuning:\n"
    "  --gainmap-quality <n>    JPEG quality of the gain map (default 85).\n"
    "  --gamma <g>              Gain map encoding gamma (default 2.2).\n"
    "  --offset-sdr <v>         SDR offset constant (default 0.015625).\n"
    "  --offset-hdr <v>         HDR offset constant (default 0.015625).\n"
    "  --tone-map <name>        reinhard (default), filmic or clip.\n"
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
    "  --no-chroma-subsample    Encode the base image as 4:4:4.\n"
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
        fail("unknown --tone-map: " + v + " (use reinhard, filmic or clip)");
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

  if (out->inputPath.empty()) fail("missing --input");
  if (out->outputPath.empty()) fail("missing --output");
  if (out->quality < 1 || out->quality > 100)
    fail("--quality must be between 1 and 100");
  if (out->gainMapQuality < 1 || out->gainMapQuality > 100)
    fail("--gainmap-quality must be between 1 and 100");
  return true;
}

}  // namespace iso21496
