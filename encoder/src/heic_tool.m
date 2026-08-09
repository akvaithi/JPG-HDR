// iso21496_heic — repackages a gain map JPEG as a gain map HEIC. macOS only.
//
//   iso21496_heic <in.jpg> <out.heic> [--quality 0.9] [--full] [--iso-only]
//
// Why this is a separate program: HEIC needs an HEVC encoder, and the point of
// iso21496_encoder is a single static binary with no runtime dependencies on
// any platform. This one leans entirely on ImageIO, so it exists only where
// ImageIO does, and the plug-in offers it only there.
//
// Why it exists at all: in a JPEG the gain map is a second image after the
// primary's EOI, so anything that re-encodes the primary can drop it. Measured
// against a real iMessage send from iOS 27, the JPEG arrives as a single
// flattened SDR image while the HEIC arrives intact at its original headroom.
// HEIC is also about a third the size. What it costs is Android: libultrahdr
// has HEIF gain map support written but it needs four libheif entry points
// (heif_image_handle_get_gain_map_metadata and friends) that no released
// libheif has, so nothing on Android can read a gain map HEIC yet.
//
// Objective-C rather than Swift so it builds with the C toolchain already in
// use and needs no Swift runtime on the photographer's machine.
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

static int fail(const char *message) {
  fprintf(stderr, "error: %s\n", message);
  return 1;
}

// The gain map image is the second image of the multi-picture file. ImageIO
// reports a count of 1 for these, so it is sliced out at the primary's EOI.
static CGImageRef copyGainMapImage(NSString *path, CGImageSourceRef source) {
  if (CGImageSourceGetCount(source) > 1) {
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 1, NULL);
    if (image) return image;
  }
  NSData *data = [NSData dataWithContentsOfFile:path];
  if (!data) return NULL;
  const uint8_t *bytes = data.bytes;
  NSUInteger cut = 0;
  for (NSUInteger i = 2; i + 1 < data.length; ++i) {
    if (bytes[i] == 0xFF && bytes[i + 1] == 0xD9) { cut = i + 2; break; }
  }
  if (cut == 0 || cut >= data.length) return NULL;
  NSData *tail = [data subdataWithRange:NSMakeRange(cut, data.length - cut)];
  CGImageSourceRef tailSource =
      CGImageSourceCreateWithData((__bridge CFDataRef)tail, NULL);
  if (!tailSource) return NULL;
  CGImageRef image = CGImageSourceCreateImageAtIndex(tailSource, 0, NULL);
  CFRelease(tailSource);
  return image;
}

int main(int argc, const char *argv[]) {
  @autoreleasepool {
    if (argc < 3) {
      fprintf(stderr,
              "usage: iso21496_heic <in.jpg> <out.heic> [--quality 0.9] "
              "[--full] [--iso-only]\n");
      return 2;
    }
    NSString *inPath = [NSString stringWithUTF8String:argv[1]];
    NSString *outPath = [NSString stringWithUTF8String:argv[2]];
    double quality = 0.9;
    // Half resolution by default: it is what both an iPhone and a Pixel write,
    // and measured it costs nothing against a full resolution map while taking
    // roughly a fifth off the file.
    BOOL half = YES;
    // Apple's own gain map auxiliary alongside the ISO one. Apple's HDR path
    // predates ISO 21496-1 and its own files still carry both.
    BOOL dual = YES;
    for (int i = 3; i < argc; ++i) {
      if (strcmp(argv[i], "--quality") == 0 && i + 1 < argc)
        quality = atof(argv[++i]);
      else if (strcmp(argv[i], "--full") == 0)
        half = NO;
      else if (strcmp(argv[i], "--iso-only") == 0)
        dual = NO;
    }

    // The ISO 21496-1 auxiliary type is macOS 15 and newer, and the deployment
    // target is 11.0, so the symbol is weakly linked and null on anything
    // older. Reading it through a local resolved inside the availability check
    // is what makes the guard cover the uses rather than just precede them.
    CFStringRef isoGainMapType = NULL;
    if (@available(macOS 15.0, *)) {
      isoGainMapType = kCGImageAuxiliaryDataTypeISOGainMap;
    } else {
      return fail("reading an ISO 21496-1 gain map needs macOS 15 or newer");
    }

    NSURL *inURL = [NSURL fileURLWithPath:inPath];
    CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)inURL, NULL);
    if (!source) return fail("cannot open the input file");

    // The base image as written, not the HDR rendition: decoding to HDR and
    // letting ImageIO derive a fresh map writes a native PQ file instead and
    // throws away the SDR rendition the encoder solved for.
    CGImageRef base = CGImageSourceCreateImageAtIndex(source, 0, NULL);
    if (!base) { CFRelease(source); return fail("cannot decode the base image"); }

    NSMutableDictionary *aux = [(__bridge_transfer NSDictionary *)
        CGImageSourceCopyAuxiliaryDataInfoAtIndex(
            source, 0, isoGainMapType) mutableCopy];
    if (!aux) {
      CGImageRelease(base); CFRelease(source);
      return fail("no ISO 21496-1 gain map in the input");
    }

    CGImageRef gainMap = copyGainMapImage(inPath, source);
    if (!gainMap) {
      CGImageRelease(base); CFRelease(source);
      return fail("cannot decode the gain map image");
    }

    // One 8-bit channel, which is the layout Apple's own HEICs use and the only
    // one available: handed a three channel map, ImageIO's HEIC writer crashes
    // inside VideoToolbox. The plug-in's default export is already single
    // channel for the same reason, so nothing is lost going through here.
    size_t w = half ? CGImageGetWidth(gainMap) / 2 : CGImageGetWidth(gainMap);
    size_t h = half ? CGImageGetHeight(gainMap) / 2 : CGImageGetHeight(gainMap);
    size_t bytesPerRow = ((w + 63) / 64) * 64;
    NSMutableData *plane = [NSMutableData dataWithLength:bytesPerRow * h];
    CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
    CGContextRef ctx = CGBitmapContextCreate(plane.mutableBytes, w, h, 8,
                                             bytesPerRow, gray, (CGBitmapInfo)kCGImageAlphaNone);
    CGColorSpaceRelease(gray);
    if (!ctx) {
      CGImageRelease(gainMap); CGImageRelease(base); CFRelease(source);
      return fail("cannot render the gain map");
    }
    CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), gainMap);
    CGContextRelease(ctx);

    aux[(__bridge NSString *)kCGImageAuxiliaryDataInfoData] = plane;
    aux[(__bridge NSString *)kCGImageAuxiliaryDataInfoDataDescription] = @{
      @"Width": @(w),
      @"Height": @(h),
      @"BytesPerRow": @(bytesPerRow),
      @"PixelFormat": @(0x4C303038),  // 'L008'
    };

    NSMutableDictionary *props = [(__bridge_transfer NSDictionary *)
        CGImageSourceCopyPropertiesAtIndex(source, 0, NULL) mutableCopy];
    if (!props) props = [NSMutableDictionary dictionary];
    props[(__bridge NSString *)kCGImageDestinationLossyCompressionQuality] = @(quality);

    NSURL *outURL = [NSURL fileURLWithPath:outPath];
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(
        (__bridge CFURLRef)outURL, CFSTR("public.heic"), 1, NULL);
    if (!dest) {
      CGImageRelease(gainMap); CGImageRelease(base); CFRelease(source);
      return fail("cannot create the HEIC destination");
    }
    CGImageDestinationAddImage(dest, base, (__bridge CFDictionaryRef)props);
    CGImageDestinationAddAuxiliaryDataInfo(dest, isoGainMapType,
                                           (__bridge CFDictionaryRef)aux);
    if (dual)
      CGImageDestinationAddAuxiliaryDataInfo(dest, kCGImageAuxiliaryDataTypeHDRGainMap,
                                             (__bridge CFDictionaryRef)aux);
    BOOL ok = CGImageDestinationFinalize(dest);
    CFRelease(dest);
    CGImageRelease(gainMap);
    CGImageRelease(base);
    CFRelease(source);
    if (!ok) return fail("HEIC encode failed");

    // Read it back, so success is checked rather than assumed: the failure this
    // guards against is a file that writes cleanly and carries no gain map,
    // which is what happens if the auxiliary is handed over without pixel data.
    CGImageSourceRef check = CGImageSourceCreateWithURL((__bridge CFURLRef)outURL, NULL);
    if (!check) return fail("wrote a file that cannot be reopened");
    CFDictionaryRef iso =
        CGImageSourceCopyAuxiliaryDataInfoAtIndex(check, 0, isoGainMapType);
    if (!iso) { CFRelease(check); return fail("the written HEIC has no gain map"); }
    CFRelease(iso);
    unsigned long long size =
        [[NSFileManager.defaultManager attributesOfItemAtPath:outPath error:NULL]
            fileSize];
    printf("{\"ok\":true,\"totalBytes\":%llu,\"gainWidth\":%zu,\"gainHeight\":%zu}\n",
           size, w, h);
    CFRelease(check);
    return 0;
  }
}
