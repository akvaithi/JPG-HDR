"""Diffs two JPEGs segment by segment: name, length and a hash of the contents.

  python3 scripts/diffprim.py ours.jpg reference.jpg

Only the primary image is compared — it stops at the first EOI that an SOI
follows. SOF0 is decoded so sampling factors are readable rather than hex.

This exists because eight rounds of iMessage tests compared segment *lengths*
and only opened the segments that looked interesting. The JFIF APP0 was 16
bytes against Apple's 20, which reads like padding; the four extra bytes were
"AMPF" and the entire bug. Hash every segment, including the boring ones.
"""
import struct, sys, hashlib

NAMES = {0xC0: "SOF0", 0xC4: "DHT", 0xDB: "DQT", 0xDD: "DRI", 0xDA: "SOS"}


def split(d):
    i = 2
    while i + 1 < len(d):
        if d[i] == 0xFF and d[i + 1] == 0xD9 and d[i + 2:i + 4] == b"\xff\xd8":
            return d[:i + 2]
        i += 1
    return d


def segs(d):
    out, i = [], 2
    while i + 1 < len(d):
        if d[i] != 0xFF:
            break
        m = d[i + 1]
        ln = struct.unpack(">H", d[i + 2:i + 4])[0]
        body = d[i + 4:i + 2 + ln]
        if 0xE0 <= m <= 0xEF:
            name = "APP%d:%s" % (m - 0xE0, body[:12].split(b"\x00")[0].decode("latin1", "replace"))
        else:
            name = NAMES.get(m, "%02X" % m)
        out.append((name, ln, hashlib.md5(body).hexdigest()[:8], body))
        if m == 0xDA:
            break
        i += 2 + ln
    return out


def sof(body):
    prec = body[0]
    h, w = struct.unpack(">HH", body[1:5])
    n = body[5]
    comps = []
    for c in range(n):
        cid, samp, q = body[6 + c * 3:9 + c * 3]
        comps.append("id%d %dx%d q%d" % (cid, samp >> 4, samp & 15, q))
    return "%dbit %dx%d %dcomp  %s" % (prec, w, h, n, ", ".join(comps))


a, b = split(open(sys.argv[1], "rb").read()), split(open(sys.argv[2], "rb").read())
sa, sb = segs(a), segs(b)
print("%-28s %-26s %s" % ("segment", sys.argv[1].split("/")[-1], sys.argv[2].split("/")[-1]))
print("-" * 90)
names = []
for s in sa + sb:
    if s[0] not in names:
        names.append(s[0])
for n in names:
    fa = [s for s in sa if s[0] == n]
    fb = [s for s in sb if s[0] == n]
    da = ", ".join("%d/%s" % (s[1], s[2]) for s in fa) or "-- absent --"
    db = ", ".join("%d/%s" % (s[1], s[2]) for s in fb) or "-- absent --"
    flag = "" if da == db else "   <-- differs"
    print("%-28s %-26s %s%s" % (n, da, db, flag))
    if n == "SOF0":
        if fa: print("%-28s %s" % ("", sof(fa[0][3])))
        if fb: print("%-28s %s" % ("", sof(fb[0][3])))
print()
print("entropy-coded bytes: %d vs %d" % (len(a), len(b)))
