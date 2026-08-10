// Rewrites a gain map JPEG as a gain map JPEG written entirely by ImageIO —
// Apple's own framework, its own container layout, its own segment order.
//
//   swift scripts/to_apple_jpeg.swift photo.jpg apple.jpg [--quality 0.9]
//
// Why this exists: when a transport loses our gain map, the first answer is
// always "your file is malformed". This produces a file we did not lay out,
// carrying the same picture and the same gain map, so that answer can be
// tested instead of argued about. If Apple's own writer's output is lost on
// the same path, the fault is not in how we write JPEGs.
//
// The gain map is supplied as a planar buffer rather than by handing the
// source's auxiliary dictionary straight to the destination: that dictionary
// is accepted and silently writes a file with no gain map in it.
import Foundation
import ImageIO
import CoreGraphics
import UniformTypeIdentifiers

let args = CommandLine.arguments
guard args.count > 2 else {
    print("usage: swift to_apple_jpeg.swift <in.jpg> <out.jpg> [--quality 0.9]")
    exit(2)
}
let inPath = args[1], outPath = args[2]
var quality = 0.9
if let q = args.firstIndex(of: "--quality"), q + 1 < args.count {
    quality = Double(args[q + 1]) ?? 0.9
}

guard let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: inPath) as CFURL, nil) else {
    print("could not open \(inPath)"); exit(1)
}
guard let base = CGImageSourceCreateImageAtIndex(src, 0, nil) else {
    print("could not decode the base image"); exit(1)
}
guard var aux = CGImageSourceCopyAuxiliaryDataInfoAtIndex(
        src, 0, kCGImageAuxiliaryDataTypeISOGainMap) as? [CFString: Any] else {
    print("no ISO 21496-1 gain map in \(inPath)"); exit(1)
}

// The gain map image is the second image of the MPF file. ImageIO reports a
// count of 1 for these, so it is sliced out at the primary's EOI.
var gainMap = CGImageSourceGetCount(src) > 1
    ? CGImageSourceCreateImageAtIndex(src, 1, nil) : nil
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

// Single channel, matching what Apple's own files carry. A three channel map
// is not an option through this path anyway — ImageIO's writer crashes inside
// VideoToolbox on a '444f' auxiliary.
let w = gm.width, h = gm.height
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
        UTType.jpeg.identifier as CFString, 1, nil) else {
    print("could not create the JPEG destination"); exit(1)
}
CGImageDestinationAddImage(dest, base, props as CFDictionary)
CGImageDestinationAddAuxiliaryDataInfo(dest, kCGImageAuxiliaryDataTypeISOGainMap,
                                       aux as CFDictionary)
CGImageDestinationAddAuxiliaryDataInfo(dest, kCGImageAuxiliaryDataTypeHDRGainMap,
                                       aux as CFDictionary)
guard CGImageDestinationFinalize(dest) else {
    print("JPEG encode failed"); exit(1)
}

let size = ((try? FileManager.default.attributesOfItem(atPath: outPath))?[.size] as? Int) ?? 0
print("wrote \(outPath) (\(size) bytes)")

// Read it back, so "ImageIO wrote a gain map" is checked rather than assumed.
if let check = CGImageSourceCreateWithURL(URL(fileURLWithPath: outPath) as CFURL, nil) {
    let iso = CGImageSourceCopyAuxiliaryDataInfoAtIndex(
        check, 0, kCGImageAuxiliaryDataTypeISOGainMap) != nil
    let apple = CGImageSourceCopyAuxiliaryDataInfoAtIndex(
        check, 0, kCGImageAuxiliaryDataTypeHDRGainMap) != nil
    print("  ISO gain map present:   \(iso ? "yes" : "NO")")
    print("  Apple gain map present: \(apple ? "yes" : "NO")")
    if let img = CGImageSourceCreateImageAtIndex(check, 0,
            [kCGImageSourceDecodeRequest: kCGImageSourceDecodeToHDR] as CFDictionary) {
        print(String(format: "  content headroom %.4f (%.4f EV)",
                     Double(img.contentHeadroom),
                     img.contentHeadroom > 0 ? log2(Double(img.contentHeadroom)) : 0))
    }
}
