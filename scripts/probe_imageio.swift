// Asks macOS ImageIO what it makes of a gain map JPEG — the same framework
// Photos, Preview, Safari and Quick Look all sit on, so its answer is the one
// that decides whether a file shows HDR on an Apple device.
//
//   swift scripts/probe_imageio.swift photo.jpg
//
// Used by scripts/validate.py, and useful on its own.
import Foundation
import ImageIO
import CoreGraphics

guard CommandLine.arguments.count > 1 else {
    print("usage: swift probe_imageio.swift <file.jpg>")
    exit(2)
}
let path = CommandLine.arguments[1]
guard let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: path) as CFURL, nil) else {
    print("ImageIO could not open the file")
    exit(1)
}

var found = false
for i in 0..<CGImageSourceGetCount(src) {
    if let info = CGImageSourceCopyAuxiliaryDataInfoAtIndex(
            src, i, kCGImageAuxiliaryDataTypeISOGainMap) as? [String: Any] {
        found = true
        let desc = info["kCGImageAuxiliaryDataInfoDataDescription"] as? [String: Any] ?? [:]
        let w = desc["Width"] ?? "?", h = desc["Height"] ?? "?"
        // The pixel format is a FourCC: L008 is single channel, 444f is three.
        var fourcc = "?"
        if let pf = desc["PixelFormat"] as? UInt32 {
            fourcc = String(bytes: [UInt8((pf >> 24) & 0xff), UInt8((pf >> 16) & 0xff),
                                    UInt8((pf >> 8) & 0xff), UInt8(pf & 0xff)],
                            encoding: .ascii) ?? "?"
        }
        print("ISO gain map: FOUND at index \(i), \(w)x\(h), format \(fourcc)")
    }
    if CGImageSourceCopyAuxiliaryDataInfoAtIndex(
            src, i, kCGImageAuxiliaryDataTypeHDRGainMap) != nil {
        print("Apple HDR gain map: FOUND at index \(i) (the pre-ISO Apple format)")
    }
}
if !found { print("ISO gain map: NOT FOUND — this file will render SDR on Apple devices") }

if let img = CGImageSourceCreateImageAtIndex(src, 0,
        [kCGImageSourceDecodeRequest: kCGImageSourceDecodeToHDR] as CFDictionary) {
    let headroom = img.contentHeadroom
    print(String(format: "decoded HDR: %dx%d, content headroom %.4f (%.4f EV)",
                 img.width, img.height, Double(headroom),
                 headroom > 0 ? log2(Double(headroom)) : 0))
}
