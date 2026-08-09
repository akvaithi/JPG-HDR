"""Adds Apple's gain map XMP to the gain map image of one of our JPEGs.

Apple identifies a gain map by XMP on the auxiliary image, not by the APP10
AROT curve: apdi:AuxiliaryImageType naming the 2020 hdrgainmap URN, the stored
pixel format, and an HDRToneMap block whose ChannelMetadata is an rdf:Seq with
one entry per channel. That last part is the interesting one — Apple's own
schema is per-channel, so it can describe the three-channel map this encoder
writes even though ImageIO will only ever *write* a single channel one.

Both descriptions live in one XMP packet, as separate rdf:Description elements:
hdrgm for Android and Chrome, apdi/HDRToneMap for Apple.
"""
import sys, struct, re

FMT_444F = 875836518   # '444f', three channel
FMT_L008 = 1278226488  # 'L008', one channel

def apple_block(headroom, channels, fmt):
    li = ""
    for (lo, hi, gamma, bo, ao) in channels:
        li += (f"""
               <rdf:li rdf:parseType="Resource">
                  <HDRToneMap:GainMapMin>{lo:.6f}</HDRToneMap:GainMapMin>
                  <HDRToneMap:GainMapMax>{hi:.6f}</HDRToneMap:GainMapMax>
                  <HDRToneMap:Gamma>{gamma:.6f}</HDRToneMap:Gamma>
                  <HDRToneMap:BaseOffset>{bo:.6f}</HDRToneMap:BaseOffset>
                  <HDRToneMap:AlternateOffset>{ao:.6f}</HDRToneMap:AlternateOffset>
               </rdf:li>""")
    return f"""      <rdf:Description rdf:about=""
            xmlns:apdi="http://ns.apple.com/pixeldatainfo/1.0/"
            xmlns:HDRToneMap="http://ns.apple.com/HDRToneMap/1.0/">
         <apdi:AuxiliaryImageType>urn:com:apple:photo:2020:aux:hdrgainmap</apdi:AuxiliaryImageType>
         <apdi:NativeFormat>{fmt}</apdi:NativeFormat>
         <apdi:StoredFormat>{fmt}</apdi:StoredFormat>
         <HDRToneMap:AlternateHeadroom>{headroom:.6f}</HDRToneMap:AlternateHeadroom>
         <HDRToneMap:ChannelMetadata>
            <rdf:Seq>{li}
            </rdf:Seq>
         </HDRToneMap:ChannelMetadata>
         <HDRToneMap:BaseHeadroom>0.000000</HDRToneMap:BaseHeadroom>
         <HDRToneMap:BaseColorIsWorkingColor>True</HDRToneMap:BaseColorIsWorkingColor>
         <HDRToneMap:Version>1</HDRToneMap:Version>
      </rdf:Description>
"""

def segments(data, start):
    i, out = start + 2, []
    while i < len(data) - 1:
        if data[i] != 0xFF: i += 1; continue
        m = data[i+1]
        if m in (0xD8, 0x01) or 0xD0 <= m <= 0xD7: i += 2; continue
        if m in (0xDA, 0xD9): break
        ln = struct.unpack('>H', data[i+2:i+4])[0]
        out.append((m, i, ln))
        i += 2 + ln
    return out

def second_image(data):
    i = data.find(b'\xff\xd9')
    return i + 2

def build(src, dst, headroom, channels, fmt):
    data = open(src, 'rb').read()
    s2 = second_image(data)
    xmp_seg = None
    for m, off, ln in segments(data, s2):
        if m == 0xE1 and data[off+4:off+4+28] == b'http://ns.adobe.com/xap/1.0/':
            xmp_seg = (off, ln)
    if xmp_seg is None:
        raise SystemExit("no XMP on the gain map image")
    off, ln = xmp_seg
    old = data[off+4+29 : off+2+ln].decode('utf-8', 'replace')
    # Splice Apple's description in beside the hdrgm one, inside the same RDF.
    merged = old.replace("</rdf:RDF>", apple_block(headroom, channels, fmt) + "</rdf:RDF>")
    payload = b"http://ns.adobe.com/xap/1.0/\x00" + merged.encode('utf-8')
    seg = b"\xff\xe1" + struct.pack('>H', len(payload) + 2) + payload
    out = data[:off] + seg + data[off+2+ln:]
    open(dst, 'wb').write(out)
    print(f"{dst}: XMP {ln} -> {len(seg)-2} bytes, file {len(data)} -> {len(out)}")

if __name__ == "__main__":
    ch3 = [(0.0, 2.466447, 1.0, 0.015625, 0.015625),
           (0.0, 2.292490, 1.0, 0.015625, 0.015625),
           (0.0, 2.283278, 1.0, 0.015625, 0.015625)]
    build("fix1.jpg", "ultimate_444f.jpg", 2.300056, ch3, FMT_444F)
    build("fix1.jpg", "ultimate_l008.jpg", 2.300056, ch3, FMT_L008)
