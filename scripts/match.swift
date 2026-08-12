// Matches received files to sent files by picture content, so a batch result
// is attributed by what the card says rather than by assumed send order.
import Foundation
import ImageIO
import CoreGraphics

func thumb(_ p: String) -> [Float]? {
    guard let s = CGImageSourceCreateWithURL(URL(fileURLWithPath: p) as CFURL, nil),
          let img = CGImageSourceCreateImageAtIndex(s, 0, nil) else { return nil }
    let n = 64
    var b = [UInt8](repeating: 0, count: n * n)
    b.withUnsafeMutableBytes { q in
        let c = CGContext(data: q.baseAddress, width: n, height: n,
                          bitsPerComponent: 8, bytesPerRow: n,
                          space: CGColorSpaceCreateDeviceGray(),
                          bitmapInfo: CGImageAlphaInfo.none.rawValue)!
        c.interpolationQuality = .high
        c.draw(img, in: CGRect(x: 0, y: 0, width: n, height: n))
    }
    let v = b.map { Float($0) }
    let mean = v.reduce(0, +) / Float(v.count)
    return v.map { $0 - mean }
}

func corr(_ a: [Float], _ b: [Float]) -> Float {
    var num: Float = 0, da: Float = 0, db: Float = 0
    for i in 0..<min(a.count, b.count) { num += a[i]*b[i]; da += a[i]*a[i]; db += b[i]*b[i] }
    return num / (sqrt(da) * sqrt(db) + 1e-9)
}

func headroomEV(_ p: String) -> Double {
    guard let s = CGImageSourceCreateWithURL(URL(fileURLWithPath: p) as CFURL, nil),
          let img = CGImageSourceCreateImageAtIndex(s, 0,
              [kCGImageSourceDecodeRequest: kCGImageSourceDecodeToHDR] as CFDictionary)
    else { return -1 }
    let h = Double(img.contentHeadroom)
    return h > 0 ? log2(h) : 0
}

let sep = CommandLine.arguments.firstIndex(of: "--")!
let sent = Array(CommandLine.arguments[1..<sep])
let recv = Array(CommandLine.arguments[(sep+1)...])
let sentT = sent.map { thumb($0) }

for r in recv {
    guard let rt = thumb(r) else { continue }
    var best = -2 as Float, bestI = -1
    for (i, st) in sentT.enumerated() {
        guard let st = st else { continue }
        let c = corr(rt, st)
        if c > best { best = c; bestI = i }
    }
    let name = (sent[bestI] as NSString).lastPathComponent
    let ev = headroomEV(r)
    let verdict = ev > 0.5 ? "SURVIVED" : "lost"
    print(String(format: "%-18@ -> %-30@ r=%.3f   %.4f EV   %@",
                 (r as NSString).lastPathComponent as NSString, name as NSString,
                 Double(best), ev, verdict as NSString))
}
