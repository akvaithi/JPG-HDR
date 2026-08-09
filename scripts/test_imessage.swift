// A faithful stand-in for what iMessage does to a gain map JPEG.
//
// The matched before/after pair shows it re-encodes both images and keeps the
// two-image structure, the MPF index and the APP10 AROT curve, while dropping
// every ISO 21496-1 segment. It cannot be doing that generically — it is
// reading the file as an *Apple* gain map and writing one back out. So the
// simulation is: ask ImageIO for the base image and the Apple gain map
// auxiliary, and write a JPEG from those two things alone. A file ImageIO
// cannot read that way is one iMessage would flatten.
import Foundation
import ImageIO
import CoreGraphics
import UniformTypeIdentifiers

for path in CommandLine.arguments.dropFirst() {
    let name = (path as NSString).lastPathComponent
    guard let src = CGImageSourceCreateWithURL(URL(fileURLWithPath: path) as CFURL, nil),
          let base = CGImageSourceCreateImageAtIndex(src, 0, nil) else {
        print("\(name): unreadable"); continue }
    guard let aux = CGImageSourceCopyAuxiliaryDataInfoAtIndex(
            src, 0, kCGImageAuxiliaryDataTypeHDRGainMap) as? [CFString: Any] else {
        print(String(format: "%-24@ no Apple gain map -> would be flattened to SDR", name as NSString))
        continue }
    let hasPixels = aux[kCGImageAuxiliaryDataInfoData] != nil
    let out = (path as NSString).deletingPathExtension + "_sent.jpg"
    var props = (CGImageSourceCopyPropertiesAtIndex(src, 0, nil) as? [CFString: Any]) ?? [:]
    props[kCGImageDestinationLossyCompressionQuality] = 0.85
    guard let d = CGImageDestinationCreateWithURL(URL(fileURLWithPath: out) as CFURL,
            UTType.jpeg.identifier as CFString, 1, nil) else { continue }
    CGImageDestinationAddImage(d, base, props as CFDictionary)
    CGImageDestinationAddAuxiliaryDataInfo(d, kCGImageAuxiliaryDataTypeHDRGainMap, aux as CFDictionary)
    guard CGImageDestinationFinalize(d) else {
        print("\(name): re-encode failed"); continue }
    let chk = CGImageSourceCreateWithURL(URL(fileURLWithPath: out) as CFURL, nil)!
    let apple = CGImageSourceCopyAuxiliaryDataInfoAtIndex(chk, 0, kCGImageAuxiliaryDataTypeHDRGainMap) != nil
    let img = CGImageSourceCreateImageAtIndex(chk, 0,
        [kCGImageSourceDecodeRequest: kCGImageSourceDecodeToHDR] as CFDictionary)
    let ev = (img?.contentHeadroom ?? 0) > 0 ? log2(Double(img!.contentHeadroom)) : 0
    // Two images means the SDR base you chose is still in there.
    let data = try! Data(contentsOf: URL(fileURLWithPath: out))
    var imgs = 0, i = 0
    while i + 1 < data.count { if data[i] == 0xFF && data[i+1] == 0xD8 { imgs += 1 }; i += 1 }
    print(String(format: "%-24@ aux-pixels:%@ -> gain map:%@ images:%d  %.4f EV",
                 name as NSString, hasPixels ? "yes" : "no ", apple ? "yes" : "no ", imgs, ev))
}
