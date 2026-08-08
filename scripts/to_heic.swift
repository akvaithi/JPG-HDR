// Re-encodes a gain map JPEG as an HDR HEIC, for testing what a transport does
// to the container as opposed to its contents.
//
//   swift scripts/to_heic.swift photo.jpg photo.heic
//
// Read this before trusting the output: ImageIO does **not** produce a gain map
// HEIC here. Asked to encode an HDR CGImage to HEIC it writes a native 10-bit
// PQ file — genuinely HDR, carrying Apple's adaptive gain curve in the ICC
// profile, but with no separate gain map auxiliary item and so no SDR rendition
// you chose. Handing it the parsed auxiliary dictionary instead produces a file
// with no gain map at all. Writing the iPhone's structure — an HEVC-coded
// auxiliary item alongside the primary — needs an HEVC encoder, which this
// project deliberately does not have.
//
// It is still the experiment worth running. In a JPEG the gain map is a second
// image appended after the primary's EOI, so anything that re-encodes the
// primary drops it; in HEIC the HDR is in the coded image itself and there is
// nothing to strip. Send the same photograph both ways and the difference tells
// you whether a transport is dropping a trailer or re-encoding wholesale.
import Foundation
import ImageIO
import CoreGraphics
import UniformTypeIdentifiers
import AVFoundation

guard CommandLine.arguments.count > 2 else {
    print("usage: swift to_heic.swift <in.jpg> <out.heic>")
    exit(2)
}
let inPath = CommandLine.arguments[1], outPath = CommandLine.arguments[2]

guard let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: inPath) as CFURL, nil) else {
    print("could not open \(inPath)"); exit(1)
}
// Decode the whole HDR rendition — base plus gain map applied — and let
// ImageIO derive a fresh gain map on the way out. Handing it the parsed
// auxiliary dictionary instead does not work: it writes the file without one.
guard let image = CGImageSourceCreateImageAtIndex(src, 0,
        [kCGImageSourceDecodeRequest: kCGImageSourceDecodeToHDR] as CFDictionary) else {
    print("could not decode the primary image"); exit(1)
}
guard CGImageSourceCopyAuxiliaryDataInfoAtIndex(
        src, 0, kCGImageAuxiliaryDataTypeISOGainMap) != nil else {
    print("no ISO 21496-1 gain map in \(inPath) — nothing to carry over"); exit(1)
}

var props = (CGImageSourceCopyPropertiesAtIndex(src, 0, nil) as? [CFString: Any]) ?? [:]
props[kCGImageDestinationLossyCompressionQuality] = 0.9

guard let dest = CGImageDestinationCreateWithURL(
        URL(fileURLWithPath: outPath) as CFURL,
        UTType.heic.identifier as CFString, 1,
        [kCGImageDestinationEncodeRequest: kCGImageDestinationEncodeToISOGainmap]
            as CFDictionary) else {
    print("could not create the HEIC destination"); exit(1)
}
CGImageDestinationAddImage(dest, image, props as CFDictionary)
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
    print("  ISO gain map present after the round trip: \(iso ? "yes" : "NO")")
    if let img = CGImageSourceCreateImageAtIndex(check, 0,
            [kCGImageSourceDecodeRequest: kCGImageSourceDecodeToHDR] as CFDictionary) {
        print(String(format: "  content headroom %.4f (%.4f EV)",
                     Double(img.contentHeadroom),
                     img.contentHeadroom > 0 ? log2(Double(img.contentHeadroom)) : 0))
    }
}
