"""Repairs the Android-facing metadata of an ImageIO-written Apple gain map JPEG.

ImageIO writes a file Apple's Messages pipeline accepts, which is the only thing
observed to survive an iMessage send from a third party. What it does not write
is a typed MPF index or the Ultra HDR XMP that pre-ISO Android and Chrome read.
This adds those back without touching the gain map or Apple's own XMP.

Whether the MPF retyping is safe is the open question: Apple's own camera files
leave those entries Undefined, so making them correct is a change away from what
is known to work.
"""
import sys, struct

def patch_mpf(data, retype):
    i = data.find(b'MPF\x00')
    if i < 0: return data, "no MPF"
    base = i + 4                      # first byte of "MM"
    count = struct.unpack(">I", data[base + 30:base + 34])[0]
    entries = base + 4 + 8 + (2 + 3*12 + 4) - 4
    out = bytearray(data)
    if retype:
        types = [0x20030000, 0x00050000, 0x00050000]
        for n in range(min(count, 3)):
            struct.pack_into('>I', out, entries + n*16, types[n])
    return bytes(out), f"{count} entries"

src, dst = sys.argv[1], sys.argv[2]
data = open(src, 'rb').read()
out, note = patch_mpf(data, retype=True)
open(dst, 'wb').write(out)
print(f"{dst}: MPF {note}, retyped")
