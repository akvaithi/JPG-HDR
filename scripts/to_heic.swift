// Repackages a gain map JPEG as a gain map HEIC, keeping the SDR rendition the
// encoder chose and the gain map it measured.
//
//   swift scripts/to_heic.swift photo.jpg photo.heic
//
// Why this exists: in a JPEG the gain map is a *second* image appended after
// the primary's EOI, so anything that decodes and re-encodes the primary drops
// it — the pixels the transport keeps are only ever the SDR base. In HEIC the
// gain map is an auxiliary item inside the container, and it survives. Measured
// here, re-encoding through ImageIO:
//
//   gain map JPEG   2.3001 EV -> 0.0000 EV   lost
//   gain map HEIC   2.3001 EV -> 2.3001 EV   survives
//   iPhone HEIC     2.1855 EV -> 2.1855 EV   survives
//
// The one non-obvious part: ImageIO will not give you the gain map's pixels
// back from a JPEG source. CGImageSourceCopyAuxiliaryDataInfoAtIndex returns
// the description, colour space and metadata but no
// kCGImageAuxiliaryDataInfoData, and handing that dictionary straight to a
// destination writes a file with no gain map at all — which is what an earlier
// version of this script did, and why it concluded HEIC could not carry one.
// The map has to be decoded from the second image and supplied as a planar
// buffer.
//
// macOS only, and deliberately not part of the encoder: HEIC needs an HEVC
// encoder, and a single static binary with no runtime dependencies is the point
// of that program.
import Foundation
import ImageIO
import CoreGraphics
import UniformTypeIdentifiers

let args = CommandLine.arguments
guard args.count > 2 else {
    print("usage: swift to_heic.swift <in.jpg> <out.heic> [--quality 0.9] "
          + "[--full] [--iso-only]")
    exit(2)
}
let inPath = args[1], outPath = args[2]
// Half resolution by default: it is what both an iPhone and a Pixel write, and
// on the reference frame it costs nothing measurable (highlight error 0.2314 EV
// against 0.2351 for a full resolution map — both dominated by the single
// channel conversion below) while taking the file from 9.27 MB to 7.38 MB.
let half = !args.contains("--full")
// Apple's own gain map auxiliary alongside the ISO one. Apple's HDR path
// predates ISO 21496-1 and its own files still carry both; a shipping
// third-party plug-in reports needing the Apple form specifically for iMessage.
let dual = !args.contains("--iso-only")
var quality = 0.9
if let q = args.firstIndex(of: "--quality"), q + 1 < args.count {
    quality = Double(args[q + 1]) ?? 0.9
}

guard let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: inPath) as CFURL, nil) else {
    print("could not open \(inPath)"); exit(1)
}
// The base image as written — not the HDR rendition. Decoding to HDR here and
// letting ImageIO derive a fresh map produces a native PQ file instead, which
// is HDR but throws away the SDR rendition the encoder solved for.
guard let base = CGImageSourceCreateImageAtIndex(src, 0, nil) else {
    print("could not decode the base image"); exit(1)
}
guard var aux = CGImageSourceCopyAuxiliaryDataInfoAtIndex(
        src, 0, kCGImageAuxiliaryDataTypeISOGainMap) as? [CFString: Any] else {
    print("no ISO 21496-1 gain map in \(inPath) — nothing to carry over"); exit(1)
}

// The gain map image itself: the second image of the MPF file. ImageIO reports
// a count of 1 for these files, so it is sliced out at the primary's EOI.
var gainMap: CGImage?
if CGImageSourceGetCount(src) > 1 {
    gainMap = CGImageSourceCreateImageAtIndex(src, 1, nil)
}
if gainMap == nil {
    let data = try! Data(contentsOf: URL(fileURLWithPath: inPath))
    var i = 2, cut = -1
    while i + 1 < data.count {
        if data[i] == 0xFF && data[i + 1] == 0xD9 { cut = i + 2; break }
        i += 1
    }
    guard cut > 0,
          let tail = CGImageSourceCreateWithData(
              data.subdata(in: cut..<data.count) as CFData, nil),
          let image = CGImageSourceCreateImageAtIndex(tail, 0, nil) else {
        print("could not decode the gain map image"); exit(1)
    }
    gainMap = image
}
let gm = gainMap!

// One 8-bit channel, which is the layout Apple's own HEICs use — and the only
// one available: handed a three channel '444f' map, ImageIO's HEIC writer
// crashes inside VideoToolbox (vt_Copy_444v_Crop), so this is a hard limit of
// the framework rather than a choice.
//
// It is not free. Against the three channel JPEG this file came from, measured
// on the reference frame:
//
//   shadows and midtones   0.0522 EV
//   highlights             0.2314 EV
//   highlight hue drift    0.5665 EV
//
// which is the desaturation of warm highlights that the per-channel gain map
// exists to prevent. The cost is inherent to a single channel map, not to this
// conversion — encoding the JPEG with --channels mono and repackaging that
// measures 0.2522 EV in the highlights, slightly worse. Averaging the stored
// per-channel gains, as here, is the better of the two.
let w = half ? gm.width / 2 : gm.width
let h = half ? gm.height / 2 : gm.height
let bytesPerRow = (w + 63) / 64 * 64
var plane = [UInt8](repeating: 0, count: bytesPerRow * h)
plane.withUnsafeMutableBytes { buffer in
    let ctx = CGContext(data: buffer.baseAddress, width: w, height: h,
                        bitsPerComponent: 8, bytesPerRow: bytesPerRow,
                        space: CGColorSpaceCreateDeviceGray(),
                        bitmapInfo: CGImageAlphaInfo.none.rawValue)!
    ctx.interpolationQuality = .high
    ctx.draw(gm, in: CGRect(x: 0, y: 0, width: w, height: h))
}
aux[kCGImageAuxiliaryDataInfoData] = Data(plane) as CFData
aux[kCGImageAuxiliaryDataInfoDataDescription] = [
    "Width" as CFString: w,
    "Height" as CFString: h,
    "BytesPerRow" as CFString: bytesPerRow,
    "PixelFormat" as CFString: 0x4C303038,  // 'L008'
] as CFDictionary

var props = (CGImageSourceCopyPropertiesAtIndex(src, 0, nil) as? [CFString: Any]) ?? [:]
props[kCGImageDestinationLossyCompressionQuality] = quality

guard let dest = CGImageDestinationCreateWithURL(
        URL(fileURLWithPath: outPath) as CFURL,
        UTType.heic.identifier as CFString, 1, nil) else {
    print("could not create the HEIC destination"); exit(1)
}
CGImageDestinationAddImage(dest, base, props as CFDictionary)
CGImageDestinationAddAuxiliaryDataInfo(dest, kCGImageAuxiliaryDataTypeISOGainMap,
                                       aux as CFDictionary)
if dual {
    CGImageDestinationAddAuxiliaryDataInfo(dest, kCGImageAuxiliaryDataTypeHDRGainMap,
                                           aux as CFDictionary)
}
guard CGImageDestinationFinalize(dest) else {
    print("HEIC encode failed"); exit(1)
}

let size = ((try? FileManager.default.attributesOfItem(atPath: outPath))?[.size] as? Int) ?? 0
print("wrote \(outPath) (\(String(format: "%.2f", Double(size) / 1e6)) MB)")

// Read it back, so the claim that the gain map survived is checked rather than
// assumed.
if let check = CGImageSourceCreateWithURL(URL(fileURLWithPath: outPath) as CFURL, nil) {
    let iso = CGImageSourceCopyAuxiliaryDataInfoAtIndex(
        check, 0, kCGImageAuxiliaryDataTypeISOGainMap) != nil
    let apple = CGImageSourceCopyAuxiliaryDataInfoAtIndex(
        check, 0, kCGImageAuxiliaryDataTypeHDRGainMap) != nil
    print("  ISO gain map present after the round trip:   \(iso ? "yes" : "NO")")
    print("  Apple gain map present after the round trip: \(apple ? "yes" : "no")")
    if let img = CGImageSourceCreateImageAtIndex(check, 0,
            [kCGImageSourceDecodeRequest: kCGImageSourceDecodeToHDR] as CFDictionary) {
        print(String(format: "  content headroom %.4f (%.4f EV)",
                     Double(img.contentHeadroom),
                     img.contentHeadroom > 0 ? log2(Double(img.contentHeadroom)) : 0))
    }
}
