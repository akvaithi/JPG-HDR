"""Writes a float32 TIFF test card whose message only appears above SDR white.

Takes an optional label, drawn in mid grey so it stays readable whether or not
the gain map is applied. Files get renamed by every transport worth testing, so
the card has to say which one it is from inside the picture.

Everything drawn at 1.0 clips to white on an SDR display, so a word painted at
4.0 inside a 1.0 field is invisible without a gain map and obvious with one.
The wedge underneath steps up in half stops, so it also shows how much headroom
the display is actually giving you rather than just whether it gives any.
"""
import struct

import sys

LABEL = sys.argv[1] if len(sys.argv) > 1 else ''
OUT = sys.argv[2] if len(sys.argv) > 2 else 'testcard.tif'

W, H = 1600, 1200

# 5x7 block font, enough for the few words on the card.
FONT = {
 'H': ["#   #","#   #","#   #","#####","#   #","#   #","#   #"],
 'D': ["#### ","#   #","#   #","#   #","#   #","#   #","#### "],
 'R': ["#### ","#   #","#   #","#### ","#  # ","#   #","#   #"],
 'O': [" ### ","#   #","#   #","#   #","#   #","#   #"," ### "],
 'N': ["#   #","##  #","# # #","#  ##","#   #","#   #","#   #"],
 'S': [" ####","#    ","#    "," ### ","    #","    #","#### "],
 'E': ["#####","#    ","#    ","#### ","#    ","#    ","#####"],
 'W': ["#   #","#   #","#   #","# # #","# # #","## ##","#   #"],
 'I': ["#####","  #  ","  #  ","  #  ","  #  ","  #  ","#####"],
 # J, K and Q were missing, and an unknown character renders as a blank rather
 # than raising. Two cards labelled "J0 control" and "K0 control" therefore came
 # out byte-identical, Photos deduplicated them into one asset, and a send round
 # lost a control. If you add a glyph-less character here, add the glyph.
 'J': ["    #","    #","    #","    #","#   #","#   #"," ### "],
 'K': ["#   #","#  # ","# #  ","##   ","# #  ","#  # ","#   #"],
 'Q': [" ### ","#   #","#   #","#   #","# # #","#  # "," ## #"],
 'T': ["#####","  #  ","  #  ","  #  ","  #  ","  #  ","  #  "],
 'F': ["#####","#    ","#    ","#### ","#    ","#    ","#    "],
 'A': [" ### ","#   #","#   #","#####","#   #","#   #","#   #"],
 'B': ["#### ","#   #","#   #","#### ","#   #","#   #","#### "],
 'C': [" ####","#    ","#    ","#    ","#    ","#    "," ####"],
 'G': [" ####","#    ","#    ","#  ##","#   #","#   #"," ### "],
 'L': ["#    ","#    ","#    ","#    ","#    ","#    ","#####"],
 'M': ["#   #","## ##","# # #","#   #","#   #","#   #","#   #"],
 'P': ["#### ","#   #","#   #","#### ","#    ","#    ","#    "],
 'U': ["#   #","#   #","#   #","#   #","#   #","#   #"," ### "],
 'V': ["#   #","#   #","#   #","#   #","#   #"," # # ","  #  "],
 'X': ["#   #","#   #"," # # ","  #  "," # # ","#   #","#   #"],
 'Y': ["#   #","#   #"," # # ","  #  ","  #  ","  #  ","  #  "],
 'Z': ["#####","    #","   # ","  #  "," #   ","#    ","#####"],
 '-': ["     ","     ","     ","#####","     ","     ","     "],
 '4': ["   # ","  ## "," # # ","#  # ","#####","   # ","   # "],
 '6': [" ### ","#    ","#    ","#### ","#   #","#   #"," ### "],
 '7': ["#####","    #","   # ","  #  "," #   "," #   "," #   "],
 '8': [" ### ","#   #","#   #"," ### ","#   #","#   #"," ### "],
 '9': [" ### ","#   #","#   #"," ####","    #","    #"," ### "],
 ' ': ["     "]*7,
 '.': ["     ","     ","     ","     ","     "," ##  "," ##  "],
 '0': [" ### ","#   #","#  ##","# # #","##  #","#   #"," ### "],
 '1': ["  #  "," ##  ","  #  ","  #  ","  #  ","  #  "," ### "],
 '2': [" ### ","#   #","    #","   # ","  #  "," #   ","#####"],
 '3': [" ### ","#   #","    #","  ## ","    #","#   #"," ### "],
 '5': ["#####","#    ","#### ","    #","    #","#   #"," ### "],
}

px = [[0.05, 0.05, 0.05] for _ in range(W*H)]

def rect(x0, y0, x1, y1, v):
    for y in range(max(0,y0), min(H,y1)):
        for x in range(max(0,x0), min(W,x1)):
            px[y*W + x] = [v, v, v]

def text(s, x0, y0, scale, v):
    cx = x0
    for ch in s.upper():
        glyph = FONT.get(ch, FONT[' '])
        for r, row in enumerate(glyph):
            for c, bit in enumerate(row):
                if bit == '#':
                    rect(cx + c*scale, y0 + r*scale,
                         cx + (c+1)*scale, y0 + (r+1)*scale, v)
        cx += 6*scale

# The panel: a field at exactly SDR white, with the word four times brighter.
rect(80, 90, W-80, 560, 1.0)
text("HDR ON", 300, 210, 34, 4.0)

# Half-stop wedge, each patch labelled with its stops above white.
labels = ["0.0", "0.5", "1.0", "1.5", "2.0", "2.5", "3.0"]
n = len(labels)
w = (W - 160) // n
for i, lab in enumerate(labels):
    v = 2.0 ** (i * 0.5)
    x = 80 + i*w
    rect(x, 660, x + w - 12, 980, v)
    text(lab, x + 18, 1010, 9, 0.85)

# A mid grey strip so the base image has something that is not clipped, which
# keeps the tone mapper honest rather than solving a frame that is all white.
rect(80, 1080, W-80, 1140, 0.18)

# The label, in grey well below white so it survives with or without HDR. This
# is the only thing on the card that is legible on an SDR display.
if LABEL:
    text(LABEL, 90, 1082, 8, 0.62)

rows = b''.join(struct.pack('<%df' % (W*3), *[c for p in px[y*W:(y+1)*W] for c in p])
                for y in range(H))

def ifd_entry(tag, typ, count, value):
    return struct.pack('<HHI', tag, typ, count) + (
        value if isinstance(value, bytes) else struct.pack('<I', value))

header_len = 8
n_entries = 12
ifd_len = 2 + n_entries*12 + 4
extra_off = header_len + ifd_len
bits_off, fmt_off = extra_off, extra_off + 6
data_off = fmt_off + 6

entries = b''.join([
    ifd_entry(256, 3, 1, struct.pack('<HH', W, 0)),
    ifd_entry(257, 3, 1, struct.pack('<HH', H, 0)),
    ifd_entry(258, 3, 3, bits_off),
    ifd_entry(259, 3, 1, struct.pack('<HH', 1, 0)),
    ifd_entry(262, 3, 1, struct.pack('<HH', 2, 0)),
    ifd_entry(273, 4, 1, data_off),
    ifd_entry(277, 3, 1, struct.pack('<HH', 3, 0)),
    ifd_entry(278, 3, 1, struct.pack('<HH', H, 0)),
    ifd_entry(279, 4, 1, len(rows)),
    ifd_entry(284, 3, 1, struct.pack('<HH', 1, 0)),
    ifd_entry(339, 3, 3, fmt_off),
    ifd_entry(34675, 1, 0, 0),
])
out = (struct.pack('<2sHI', b'II', 42, header_len)
       + struct.pack('<H', n_entries) + entries + struct.pack('<I', 0)
       + struct.pack('<3H', 32, 32, 32) + struct.pack('<3H', 3, 3, 3) + rows)
open(OUT, 'wb').write(out)
print(f"wrote {OUT}  {W}x{H}  label={LABEL!r}")
