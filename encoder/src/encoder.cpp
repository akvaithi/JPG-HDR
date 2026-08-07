#include "encoder.h"

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
  po.multiChannelGainMap = opt.multiChannelGainMap;
  po.gainMapGamma = opt.gainMapGamma;
  po.offsetSdr = opt.offsetSdr;
  po.offsetHdr = opt.offsetHdr;
  po.toneMap = opt.toneMap;
  po.autoMaxBoost = opt.autoMaxBoost;
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
  logf("tone mapped with %s; measured headroom %.2f stops, gain map max %.2f",
       toneMapName(opt.toneMap), px.measuredHeadroom, px.maxBoostLog2[0]);

  GainMapMetadata meta;
  meta.multiChannel = opt.multiChannelGainMap;
  meta.useBaseColorSpace = true;
  meta.baseHeadroom = 0.0f;
  meta.alternateHeadroom = opt.targetHeadroom;
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
    report->maxBoostLog2 = px.maxBoostLog2[0];
    report->measuredHeadroom = px.measuredHeadroom;
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
