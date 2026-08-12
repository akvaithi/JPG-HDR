"""Splices the primary of one gain map JPEG onto the trailer of another, so the
two halves of the file can be tested separately.

  splice.py <primary-from> <trailer-from> <out>

The GContainer Item:Length in the primary's XMP is rewritten to the spliced
trailer's real size and padded back to the same byte count, so the segment
length stays valid and Android is not handed a lie. MPF is repatched by the
caller.
"""
import re, sys

SOI = b"\xff\xd8"


def split_images(d):
    i = 2
    while i + 1 < len(d):
        if d[i] == 0xFF and d[i + 1] == 0xD9 and d[i + 2:i + 4] == SOI:
            return d[:i + 2], d[i + 2:]
        i += 1
    raise SystemExit("no trailer image found")


if len(sys.argv) < 4:
    raise SystemExit(__doc__)

pa = open(sys.argv[1], "rb").read()
pb = open(sys.argv[2], "rb").read()
primary, _ = split_images(pa)
_, trailer = split_images(pb)

m = re.search(rb'Item:Length="(\d+)"', primary)
if m:
    old = m.group(1)
    new = str(len(trailer)).encode()
    if len(new) > len(old):
        raise SystemExit("new length needs more digits than the old one")
    # Pad with spaces after the closing quote's attribute so the XMP packet,
    # and therefore the APP1 segment length, keeps its byte count.
    pad = b" " * (len(old) - len(new))
    primary = primary[:m.start(1)] + new + b'"' + pad + primary[m.end(1) + 1:]
    print("GContainer Item:Length %s -> %s" % (old.decode(), new.decode()))

open(sys.argv[3], "wb").write(primary + trailer)
print("spliced: primary %d + trailer %d = %d bytes"
      % (len(primary), len(trailer), len(primary) + len(trailer)))
