#include "encoder.h"

#include <algorithm>
#include <chrono>

#include "exif.h"
#include "icc.h"
#include "iso_metadata.h"
#include "jpeg_encoder.h"
#include "pipeline.h"
#include "tiff_reader.h"

namespace iso21496 {
namespace {

std::string profileDescription(ColorPrimaries p) {
  switch (p) {
    case ColorPrimaries::DisplayP3: return "Display P3";
    case ColorPrimaries::Rec2020: return "Rec. ITU-R BT.2020";
    default: return "sRGB IEC61966-2.1";
  }
}

}  // namespace

Bytes encodeToMemory(const EncoderOptions& opt, EncodeReport* report) {
  const auto start = std::chrono::steady_clock::now();

  TiffReader tiff(readFile(opt.inputPath));
  logf("input: %ux%u, %u channels, %u bit%s", tiff.width(), tiff.height(),
       tiff.channels(), tiff.bitsPerSample(), tiff.isFloat() ? " float" : "");

  PipelineOptions po;
  po.outputPrimaries = opt.outputPrimaries;
  po.inputPrimaries = opt.inputPrimaries;
  po.inputTransfer = opt.inputTransfer;
  po.pqDiffuseWhiteNits = opt.pqDiffuseWhiteNits;
  po.targetHeadroom = opt.targetHeadroom;
  po.gainMapSubsample = opt.gainMapSubsample;
  // Apple's pipeline only accepts a genuinely single channel gain map, so this
  // decides the channel count rather than sitting beside it.
  po.multiChannelGainMap = opt.multiChannelGainMap && !opt.appleCompatible;
  po.gainMapGamma = opt.gainMapGamma;
  po.offsetSdr = opt.offsetSdr;
  po.offsetHdr = opt.offsetHdr;
  po.toneMap = opt.toneMap;
  po.sdrDetail = opt.sdrDetail;
  po.sdrKnee = opt.sdrKnee;
  po.sdrEdge = opt.sdrEdge;
  po.autoMaxBoost = opt.autoMaxBoost;
  po.peakDetect = opt.peakDetect;
  po.sdrShape = opt.sdrShape;
  po.sdrLiftEV = opt.sdrLiftEV;
  po.sdrContrast = opt.sdrContrast;
  po.threads = opt.threads;

  // Lift the Exif out first: once the pipeline has run we can drop the whole
  // intermediate from memory, which matters for 45MP-and-up exports.
  Bytes exifSegment;
  if (opt.writeExif) {
    ExifOptions eo;
    eo.pixelWidth = tiff.width();
    eo.pixelHeight = tiff.height();
    eo.colorSpace = opt.outputPrimaries == ColorPrimaries::sRGB ? 1 : 0xFFFF;
    exifSegment = buildExifAppSegment(tiff, eo);
  }

  PipelineResult px = runPipeline(tiff, po);
  tiff.releaseFileData();
  logf("tone mapped with %s (%s lift %.2f EV, contrast %.2f); headroom %.2f "
       "stops softened / %.2f true; gain map %.2f to %.2f EV",
       toneMapName(opt.toneMap),
       opt.toneMap == ToneMapOperator::Local
           ? "local"
           : (opt.sdrShape == SdrShapeMode::Auto ? "solved" : "manual"),
       px.sdrLiftEV,
       px.sdrContrast, px.measuredHeadroom, px.truePeakHeadroom,
       px.minBoostLog2[0], px.maxBoostLog2[0]);

  GainMapMetadata meta;
  meta.multiChannel = opt.multiChannelGainMap && !opt.appleCompatible;
  meta.appleGainMap = opt.appleCompatible;
  meta.useBaseColorSpace = true;
  meta.baseHeadroom = 0.0f;
  // The headroom the image actually needs, not the user's ceiling. A decoder
  // applies the gain scaled by display_headroom / alternate_headroom, so
  // declaring the ceiling here would make every display with less headroom
  // than that ceiling render the photo dimmer than intended — even when it has
  // more than enough headroom for this particular image.
  meta.alternateHeadroom = std::max(px.declaredHeadroom, 0.05f);
  for (int c = 0; c < 3; ++c) {
    meta.minBoost[c] = px.minBoostLog2[c];
    meta.maxBoost[c] = px.maxBoostLog2[c];
    meta.gamma[c] = opt.gainMapGamma;
    meta.baseOffset[c] = opt.offsetSdr;
    meta.alternateOffset[c] = opt.offsetHdr;
  }

  // ---- gain map image (encoded first: the primary XMP needs its length) ---
  Bytes gainMapJpeg;
  {
    JpegOptions jo;
    jo.quality = opt.gainMapQuality;
    // The gain map is a smooth control signal; chroma subsampling it would
    // add ringing around specular edges for no meaningful size win.
    jo.chromaSubsample = false;
    jo.optimizeHuffman = opt.optimizeHuffman;
    jo.threads = opt.threads;
    // JFIF first. The ISO payload is what a decoder reads, but a gain map
    // image that opens SOI + APP2 is not recognised as a JPEG by scanners that
    // sniff the trailer for SOI followed by APP0/APP1/DQT — exiftool among
    // them, which reported "Error reading GainMap image/jpeg from trailer" on
    // every file this encoder wrote. Apple and Adobe both lead with APP0.
    jo.appSegments.push_back(buildJfifAppSegment());
    jo.appSegments.push_back(buildIsoGainMapSegment(meta));
    if (opt.writeXmp)
      jo.appSegments.push_back(buildXmpAppSegment(buildGainMapXmp(meta)));

    JpegImage img;
    img.width = px.gainWidth;
    img.height = px.gainHeight;
    img.components = px.gainChannels;
    img.pixels = px.gain.data();
    gainMapJpeg = encodeJpeg(img, jo);
    logf("gain map: %ux%u, %d channel(s), %zu bytes", px.gainWidth,
         px.gainHeight, px.gainChannels, gainMapJpeg.size());
  }
  px.gain.clear();
  px.gain.shrink_to_fit();

  // ---- primary baseline image --------------------------------------------
  Bytes primaryJpeg;
  {
    JpegOptions jo;
    jo.quality = opt.quality;
    jo.chromaSubsample = opt.chromaSubsample;
    jo.optimizeHuffman = opt.optimizeHuffman;
    jo.threads = opt.threads;

    jo.appSegments.push_back(buildJfifAppSegment());
    if (!exifSegment.empty()) jo.appSegments.push_back(std::move(exifSegment));
    jo.appSegments.push_back(buildIsoBaseImageSegment());
    if (opt.writeXmp)
      jo.appSegments.push_back(
          buildXmpAppSegment(buildPrimaryXmp(gainMapJpeg.size())));
    if (opt.writeIcc) {
      Bytes profile = buildRgbIccProfile(opt.outputPrimaries,
                                         profileDescription(opt.outputPrimaries));
      for (auto& seg : buildIccAppSegments(profile))
        jo.appSegments.push_back(std::move(seg));
    }
    // MPF must be the last APP segment we add, but its contents are patched
    // once both images are sized.
    jo.appSegments.push_back(buildMpfSegmentPlaceholder());

    JpegImage img;
    img.width = px.width;
    img.height = px.height;
    img.components = 3;
    img.pixels = px.sdr.data();
    primaryJpeg = encodeJpeg(img, jo);
    logf("primary: %ux%u, %zu bytes", px.width, px.height, primaryJpeg.size());
  }
  px.sdr.clear();
  px.sdr.shrink_to_fit();

  Bytes file = std::move(primaryJpeg);
  const size_t primarySize = file.size();
  file.insert(file.end(), gainMapJpeg.begin(), gainMapJpeg.end());
  patchMpfSegment(file, primarySize, gainMapJpeg.size());

  if (report) {
    report->width = px.width;
    report->height = px.height;
    report->gainWidth = px.gainWidth;
    report->gainHeight = px.gainHeight;
    report->gainChannels = px.gainChannels;
    report->primaryBytes = primarySize;
    report->gainMapBytes = gainMapJpeg.size();
    report->totalBytes = file.size();
    report->minBoostLog2 = px.minBoostLog2[0];
    report->maxBoostLog2 = px.maxBoostLog2[0];
    report->declaredHeadroom = meta.alternateHeadroom;
    report->measuredHeadroom = px.measuredHeadroom;
    report->truePeakHeadroom = px.truePeakHeadroom;
    // Only true when the lift/contrast solver actually ran. Local tone mapping
    // has nothing for it to solve, so reporting it there would be a lie.
    report->autoShaped = opt.sdrShape == SdrShapeMode::Auto &&
                         opt.toneMap != ToneMapOperator::Local;
    report->sdrLiftEV = px.sdrLiftEV;
    report->sdrContrast = px.sdrContrast;
    report->midtoneAnchor = px.midtoneAnchor;
    report->inputPrimaries = primariesName(px.resolvedInputPrimaries);
    report->inputTransfer = transferName(px.resolvedInputTransfer);
    report->seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
  }
  return file;
}

EncodeReport encodeFile(const EncoderOptions& opt) {
  EncodeReport report;
  Bytes file = encodeToMemory(opt, &report);
  writeFile(opt.outputPath, file);
  return report;
}

}  // namespace iso21496
