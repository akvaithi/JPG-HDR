"""Segment surgery on a two-image gain map JPEG, so one variable can be moved
at a time without re-encoding any pixels.

  segedit.py add-primary-xmp <target> <donor> <out>
  segedit.py drop-gainmap-icc <target> <out>

The primary/trailer boundary is the first EOI followed by an SOI. MPF is
repatched afterwards by the caller, since every edit shifts the offsets.
"""
import struct, sys

SOI = b"\xff\xd8"
EOI = b"\xff\xd9"


def split_images(d):
    """(primary, trailer) at the first EOI followed by SOI."""
    i = 2
    while i + 1 < len(d):
        if d[i] == 0xFF and d[i + 1] == 0xD9 and d[i + 2:i + 4] == SOI:
            return d[:i + 2], d[i + 2:]
        i += 1
    raise SystemExit("no trailer image found")


def segments(d):
    """[(marker, start, total_len)] for the APPn/DQT/... run before SOS."""
    out, i = [], 2
    while i + 1 < len(d):
        if d[i] != 0xFF:
            break
        m = d[i + 1]
        if m in (0xD8, 0xD9) or 0xD0 <= m <= 0xD7:
            i += 2
            continue
        ln = struct.unpack(">H", d[i + 2:i + 4])[0]
        out.append((m, i, 2 + ln))
        if m == 0xDA:
            break
        i += 2 + ln
    return out


def payload(d, s):
    _, start, total = s
    return d[start + 4:start + total]


def find(d, marker, prefix):
    for s in segments(d):
        if s[0] == marker and payload(d, s).startswith(prefix):
            return s
    return None


if len(sys.argv) < 2:
    raise SystemExit(__doc__)

cmd = sys.argv[1]

if cmd == "add-primary-xmp":
    target, donor, out = sys.argv[2], sys.argv[3], sys.argv[4]
    td = open(target, "rb").read()
    dd = open(donor, "rb").read()
    dprim, _ = split_images(dd)
    xmp = find(dprim, 0xE1, b"http://ns.adobe.com/xap/1.0/")
    if not xmp:
        raise SystemExit("donor has no XMP APP1 on its primary")
    blob = dprim[xmp[1]:xmp[1] + xmp[2]]
    tprim, ttrail = split_images(td)
    # After the ISO marker if there is one, else after MPF, matching our own
    # layout — the point is to add the segment, not to move anything else.
    anchor = (find(tprim, 0xE2, b"urn:iso:std:iso:ts:21496")
              or find(tprim, 0xE2, b"MPF\x00"))
    at = anchor[1] + anchor[2]
    open(out, "wb").write(tprim[:at] + blob + tprim[at:] + ttrail)
    print("added %d-byte XMP to the primary" % len(blob))

elif cmd == "drop-gainmap-icc":
    target, out = sys.argv[2], sys.argv[3]
    td = open(target, "rb").read()
    tprim, ttrail = split_images(td)
    icc = find(ttrail, 0xE2, b"ICC_PROFILE")
    if not icc:
        raise SystemExit("the gain map image has no ICC profile")
    open(out, "wb").write(tprim + ttrail[:icc[1]] + ttrail[icc[1] + icc[2]:])
    print("dropped %d-byte ICC from the gain map image" % icc[2])

else:
    raise SystemExit(__doc__)
