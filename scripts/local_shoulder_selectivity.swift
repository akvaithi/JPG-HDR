// Measures what --local-shoulder actually does, which is a within-frame claim:
// large bright areas roll off, small bright things do not.
//
//   swift scripts/local_shoulder_selectivity.swift
//
// Expects a directory of <scene>/L000.jpg and <scene>/L100.jpg — the same
// export with the flag off and at full strength. It classifies the bright
// pixels of the off version into those inside a large bright region and those
// that are small and isolated, then reports how much each population moved.
//
// A ratio near 1 means the control is a global shoulder wearing a different
// name and should be abandoned. Measured across sixteen scenes it runs 1.75 to
// 5.04, median 2.59.
//
// The reason this exists as a script rather than a one-off: the claim was first
// measured on the single frame the idea came from, which is the frame least able
// to test it.
import Foundation
import ImageIO
import CoreGraphics
func px(_ p: String, _ n: Int) -> [Float]? {
    guard let s = CGImageSourceCreateWithURL(URL(fileURLWithPath: p) as CFURL, nil),
          let img = CGImageSourceCreateImageAtIndex(s, 0, nil) else { return nil }
    var b = [Float](repeating: 0, count: n*n*4)
    b.withUnsafeMutableBytes { q in
        let c = CGContext(data: q.baseAddress, width: n, height: n, bitsPerComponent: 32, bytesPerRow: n*16,
            space: CGColorSpace(name: CGColorSpace.displayP3)!,
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue | CGBitmapInfo.floatComponents.rawValue
                | CGBitmapInfo.byteOrder32Little.rawValue)!
        c.interpolationQuality = .none
        c.draw(img, in: CGRect(x: 0, y: 0, width: n, height: n))
    }
    return b
}
let n = 800, R = 6
let root = "/Users/akvaithi/Desktop/HDR-local-shoulder"
let scenes = (try! FileManager.default.contentsOfDirectory(atPath: root))
    .filter { $0.hasPrefix("_DSC") }.sorted()
print(String(format: "%-11@ %9@ %9@ %12@ %12@ %8@", "scene" as NSString, "large px" as NSString,
             "small px" as NSString, "large EV" as NSString, "small EV" as NSString, "ratio" as NSString))
var ratios = [Double]()
for s in scenes {
    guard let a = px("\(root)/\(s)/L000.jpg", n), let b = px("\(root)/\(s)/L100.jpg", n) else { continue }
    func lum(_ v: [Float], _ i: Int) -> Double {
        0.2126*Double(v[i*4]) + 0.7152*Double(v[i*4+1]) + 0.0722*Double(v[i*4+2])
    }
    var bright = [Bool](repeating: false, count: n*n)
    for i in 0..<(n*n) { bright[i] = lum(a,i) > 0.72 }
    var large = [Int](), small = [Int]()
    for y in R..<(n-R) {
        for x in R..<(n-R) where bright[y*n+x] {
            var c = 0
            for dy in stride(from: -R, through: R, by: 3) {
                for dx in stride(from: -R, through: R, by: 3) where bright[(y+dy)*n + x+dx] { c += 1 }
            }
            if c >= 22 { large.append(y*n+x) } else if c <= 12 { small.append(y*n+x) }
        }
    }
    if large.count < 400 || small.count < 400 {
        print(String(format: "%-11@ %9d %9d   too few of one kind to judge",
                     s as NSString, large.count, small.count)); continue
    }
    func meanEV(_ set: [Int]) -> Double {
        var t = 0.0
        for i in set { t += log2(max(lum(b,i),1e-4)) - log2(max(lum(a,i),1e-4)) }
        return t/Double(set.count)
    }
    let L = meanEV(large), S = meanEV(small)
    let r = S != 0 ? L/S : 0
    ratios.append(r)
    print(String(format: "%-11@ %9d %9d %+12.4f %+12.4f %8.2f",
                 s as NSString, large.count, small.count, L, S, r))
}
if !ratios.isEmpty {
    let sorted = ratios.sorted()
    print(String(format: "\nselectivity across %d scenes: median %.2f, min %.2f, max %.2f",
                 ratios.count, sorted[sorted.count/2], sorted.first!, sorted.last!))
}
