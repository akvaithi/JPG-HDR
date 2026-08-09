"""Builds a three-image MPF file: primary, three-channel gain map, mono gain map.

The idea: Apple's stack only handles single channel gain maps end to end, but
Android and Chrome read the three-channel one happily. MPF is not limited to two
images, so carry both — the three-channel map for the ISO and Ultra HDR readers,
and a mono one carrying Apple's apdi/HDRToneMap XMP for Apple.
"""
import sys, struct
from apple_xmp import apple_block, FMT_L008, segments

def images(d):
    out, i = [], 0
    while True:
        j = d.find(b'\xff\xd8', i)
        if j < 0: break
        if j == 0 or d[j-2:j] == b'\xff\xd9': out.append(j)
        i = j + 2
    return out

def build_mpf(count, sizes, offsets):
    p = bytearray(b'MPF\x00MM')
    p += struct.pack('>HI', 42, 8)
    p += struct.pack('>H', 3)
    p += struct.pack('>HHI', 0xb000, 7, 4) + b'0100'
    p += struct.pack('>HHII', 0xb001, 4, 1, count)
    entries_off = 4 + 8 + (2 + 3*12 + 4)      # from "MPF\0"; relative to MM
    p += struct.pack('>HHII', 0xb002, 7, count*16, entries_off - 4)
    p += struct.pack('>I', 0)
    types = [0x20030000, 0x00050000, 0x00050000]
    for n in range(count):
        p += struct.pack('>IIIHH', types[n], sizes[n], offsets[n], 0, 0)
    return bytes(p)

src = open(sys.argv[1], 'rb').read()          # primary + 3-channel gain map
mono_src = open(sys.argv[2], 'rb').read()     # donor for the mono gain map
out_path = sys.argv[3]

s = images(src)
primary, gm3 = src[:s[1]], src[s[1]:]
m = images(mono_src)
gm1 = mono_src[m[1]:]

# Apple's identification goes on the mono image.
ch3 = [(0.0, 2.466447, 1.0, 0.015625, 0.015625),
       (0.0, 2.292490, 1.0, 0.015625, 0.015625),
       (0.0, 2.283278, 1.0, 0.015625, 0.015625)]
xmp = ('<?xpacket begin="﻿" id="W5M0MpCehiHzreSzNTczkc9d"?>'
       '<x:xmpmeta xmlns:x="adobe:ns:meta/">'
       '<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">'
       + apple_block(2.300056, ch3, FMT_L008) +
       '</rdf:RDF></x:xmpmeta><?xpacket end="w"?>')
payload = b'http://ns.adobe.com/xap/1.0/\x00' + xmp.encode('utf-8')
seg = b'\xff\xe1' + struct.pack('>H', len(payload) + 2) + payload
# Insert after the mono image's SOI, replacing any XMP it already has.
keep = bytearray(gm1[:2])
i, rest = 2, None
while i < len(gm1) - 1:
    if gm1[i] != 0xFF: break
    mk = gm1[i+1]
    if mk in (0xD8, 0x01) or 0xD0 <= mk <= 0xD7: i += 2; continue
    if mk in (0xDA, 0xD9) or not (0xE0 <= mk <= 0xEF): rest = i; break
    ln = struct.unpack('>H', gm1[i+2:i+4])[0]
    if not (mk == 0xE1 and gm1[i+4:i+32] == b'http://ns.adobe.com/xap/1.0/'):
        keep += gm1[i:i+2+ln]
    i += 2 + ln
gm1 = bytes(keep) + seg + gm1[rest:]

# The MPF segment must be rebuilt for three entries, which changes the primary's
# length, which changes the offsets inside it — so solve it by iterating twice.
def assemble(mpf_len_guess):
    idx = primary.find(b'\xff\xe2')
    while idx > 0:
        ln = struct.unpack('>H', primary[idx+2:idx+4])[0]
        if primary[idx+4:idx+8] == b'MPF\x00': break
        idx = primary.find(b'\xff\xe2', idx + 2 + ln)
    old_len = struct.unpack('>H', primary[idx+2:idx+4])[0]
    head, tail = primary[:idx], primary[idx+2+old_len:]
    delta = mpf_len_guess - old_len
    p_size = len(primary) + delta
    sizes = [p_size, len(gm3), len(gm1)]
    offsets = [0, p_size, p_size + len(gm3)]
    endian = len(head) + 4 + 4                # first byte of "MM"
    offsets = [0, p_size - endian, p_size + len(gm3) - endian]
    body = build_mpf(3, sizes, offsets)
    seg = b'\xff\xe2' + struct.pack('>H', len(body) + 2) + body
    return head + seg + tail, len(body) + 2

guess = 88
for _ in range(4):
    newp, guess = assemble(guess)
open(out_path, 'wb').write(newp + gm3 + gm1)
print(f"wrote {out_path}: {len(newp)+len(gm3)+len(gm1)} bytes, 3 images")
