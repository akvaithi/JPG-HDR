#include <cstdio>
#include <iostream>

#include "encoder.h"
#include "options.h"

int main(int argc, char** argv) {
  using namespace iso21496;
  try {
    if (argc <= 1) {
      std::cout << usageText();
      return 2;
    }
    EncoderOptions options;
    bool handled = false;
    parseArguments(argc, argv, &options, &handled);
    if (handled) return 0;

    EncodeReport r = encodeFile(options);

    if (options.json) {
      std::printf(
          "{\"ok\":true,\"width\":%u,\"height\":%u,\"gainWidth\":%u,"
          "\"gainHeight\":%u,\"gainChannels\":%d,\"primaryBytes\":%zu,"
          "\"gainMapBytes\":%zu,\"totalBytes\":%zu,\"minBoostLog2\":%.4f,"
          "\"maxBoostLog2\":%.4f,\"declaredHeadroom\":%.4f,"
          "\"measuredHeadroom\":%.4f,\"truePeakHeadroom\":%.4f,"
          "\"autoShaped\":%s,\"sdrLiftEV\":%.4f,\"sdrContrast\":%.4f,"
          "\"midtoneAnchor\":%.5f,\"inputPrimaries\":\"%s\","
          "\"inputTransfer\":\"%s\",\"seconds\":%.3f}\n",
          r.width, r.height, r.gainWidth, r.gainHeight, r.gainChannels,
          r.primaryBytes, r.gainMapBytes, r.totalBytes, r.minBoostLog2,
          r.maxBoostLog2, r.declaredHeadroom, r.measuredHeadroom,
          r.truePeakHeadroom, r.autoShaped ? "true" : "false", r.sdrLiftEV,
          r.sdrContrast, r.midtoneAnchor, r.inputPrimaries.c_str(),
          r.inputTransfer.c_str(), r.seconds);
    }
    return 0;
  } catch (const Error& e) {
    std::fprintf(stderr, "iso21496_encoder: error: %s\n", e.what());
    return 1;
  } catch (const std::bad_alloc&) {
    std::fprintf(stderr,
                 "iso21496_encoder: error: out of memory (try exporting at a "
                 "smaller size, or close other applications)\n");
    return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "iso21496_encoder: internal error: %s\n", e.what());
    return 1;
  }
}
