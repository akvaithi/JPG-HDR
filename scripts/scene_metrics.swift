// Candidate predictors, measured on the SOURCE render rather than on any
// export. The solver has to run before the base image exists, exactly as the
// lift and contrast solver does, so anything measured on an output is unusable
// as an input.
import Foundation
import ImageIO
import CoreGraphics
func hdrPixels(_ p: String, _ n: Int) -> [Float]? {
    guard let s = CGImageSourceCreateWithURL(URL(fileURLWithPath: p) as CFURL, nil),
          let img = CGImageSourceCreateImageAtIndex(s, 0,
              [kCGImageSourceDecodeRequest: kCGImageSourceDecodeToHDR] as CFDictionary) else { return nil }
    var b = [Float](repeating: 0, count: n*n*4)
    b.withUnsafeMutableBytes { q in
        let c = CGContext(data: q.baseAddress, width: n, height: n, bitsPerComponent: 32, bytesPerRow: n*16,
            space: CGColorSpace(name: CGColorSpace.extendedLinearDisplayP3)!,
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue | CGBitmapInfo.floatComponents.rawValue
                | CGBitmapInfo.byteOrder32Little.rawValue)!
        c.interpolationQuality = .high
        c.draw(img, in: CGRect(x: 0, y: 0, width: n, height: n))
    }
    return b
}
let n = 512
print("scene,median,p90,p99,frac_over_50,frac_over_80,frac_over_100,hi_sat,hi_bloblike,headroom_ev")
for name in CommandLine.arguments.dropFirst() {
    // Read the encoder's own HDR rendition of the scene: the exports all share
    // it, so it stands in for the render the solver would see.
    guard let b = hdrPixels("/Users/akvaithi/Desktop/HDR-knee-study/\(name)/1-k095.jpg", n) else { continue }
    var lum = [Double](); lum.reserveCapacity(n*n)
    var satSum = 0.0, satN = 0.0
    var mask = [Bool](repeating: false, count: n*n)
    for i in 0..<(n*n) {
        let r = Double(b[i*4]), g = Double(b[i*4+1]), bl = Double(b[i*4+2])
        let l = 0.2126*r + 0.7152*g + 0.0722*bl
        lum.append(l)
        if l > 0.5 {
            mask[i] = true
            let mx = max(r, max(g,bl)), mn = min(r, min(g,bl))
            if mx > 1e-6 { satSum += (mx-mn)/mx; satN += 1 }
        }
    }
    let sorted = lum.sorted()
    func pct(_ f: Double) -> Double { sorted[min(sorted.count-1, Int(Double(sorted.count)*f))] }
    let over50 = Double(lum.filter { $0 > 0.5 }.count) / Double(n*n)
    let over80 = Double(lum.filter { $0 > 0.8 }.count) / Double(n*n)
    let over100 = Double(lum.filter { $0 > 1.0 }.count) / Double(n*n)
    // "Blob-like": of the bright pixels, how many have bright neighbours. Large
    // flat highlight areas score high; scattered speculars score low. This is
    // the one metric aimed at the difference the eye reacted to.
    var neighbours = 0, brightTotal = 0
    for y in 1..<(n-1) {
        for x in 1..<(n-1) where mask[y*n+x] {
            brightTotal += 1
            var c = 0
            for dy in -1...1 { for dx in -1...1 where !(dx==0 && dy==0) {
                if mask[(y+dy)*n + x+dx] { c += 1 } } }
            if c >= 7 { neighbours += 1 }
        }
    }
    let blob = brightTotal > 0 ? Double(neighbours)/Double(brightTotal) : 0
    let peak = pct(0.9999)
    print(String(format: "%@,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f",
                 name, pct(0.5), pct(0.90), pct(0.99), over50, over80, over100,
                 satN > 0 ? satSum/satN : 0, blob, peak > 0 ? log2(peak) : 0))
}
