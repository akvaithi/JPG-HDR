#!/usr/bin/env python3
"""Check a gain map JPEG against what each ecosystem actually looks for.

    scripts/validate.py photo.jpg [photo2.jpg ...]
    scripts/validate.py --compare reference.jpg photo.jpg

Sharing a file through iMessage, Google Photos or a browser to find out whether
it survived is slow, and tells you *that* something broke rather than what. Each
of those pipelines is reading one of a small number of structures, so most of
the answer can be had offline: this checks them one at a time and names the
consumer that cares.

Structure and Ultra HDR conformance need nothing but the standard library.
The Apple check shells out to `scripts/probe_imageio.swift` when it is present,
and the validation check to `exiftool`; both are skipped with a note when the
tool is missing rather than failing the run.

Exit status is 1 if any REQUIRED check failed.
"""

import os
import re
import struct
import subprocess
import sys

RESET, BOLD = "\033[0m", "\033[1m"
RED, GREEN, YELLOW, GREY = "\033[31m", "\033[32m", "\033[33m", "\033[90m"
if not sys.stdout.isatty():
    RESET = BOLD = RED = GREEN = YELLOW = GREY = ""

ISO_URN = b"urn:iso:std:iso:ts:21496:-1\x00"

# MPF type codes, from CIPA DC-007 plus the gain map extension that Lightroom
# and Apple both write.
MP_PRIMARY = 0x030000
MP_GAIN_MAP = 0x050000
MP_REPRESENTATIVE = 1 << 29


class Report:
    def __init__(self):
        self.failed = 0
        self.warned = 0

    def section(self, title, who):
        print(f"\n{BOLD}{title}{RESET} {GREY}— {who}{RESET}")

    def ok(self, what, detail=""):
        print(f"  {GREEN}pass{RESET}  {what}" + (f" {GREY}({detail}){RESET}" if detail else ""))

    def fail(self, what, detail=""):
        self.failed += 1
        print(f"  {RED}FAIL{RESET}  {what}" + (f" {GREY}({detail}){RESET}" if detail else ""))

    def warn(self, what, detail=""):
        self.warned += 1
        print(f"  {YELLOW}warn{RESET}  {what}" + (f" {GREY}({detail}){RESET}" if detail else ""))

    def skip(self, what, why):
        print(f"  {GREY}skip  {what} ({why}){RESET}")

    def check(self, cond, what, detail="", required=True):
        (self.ok if cond else (self.fail if required else self.warn))(what, detail)
        return cond


def jpeg_segments(data, start):
    """Walk the marker segments of the JPEG at `start`. Returns (segments, end)."""
    segs, i = [], start + 2
    while i + 4 <= len(data):
        if data[i] != 0xFF:
            break
        marker = data[i + 1]
        if marker == 0xD8 or marker == 0x01 or 0xD0 <= marker <= 0xD7:
            i += 2
            continue
        if marker == 0xDA:  # start of scan: the entropy data runs to the EOI
            end = data.find(b"\xff\xd9", i)
            return segs, (end + 2 if end >= 0 else len(data))
        if marker == 0xD9:
            return segs, i + 2
        length = struct.unpack(">H", data[i + 2:i + 4])[0]
        segs.append((marker, i + 4, length - 2))
        i += 2 + length
    return segs, len(data)


def mpf_entries(data):
    at = data.find(b"MPF\x00")
    if at < 0:
        return None
    base = at + 4
    big = data[base:base + 2] == b"MM"
    u32 = lambda o: struct.unpack(">I" if big else "<I", data[o:o + 4])[0]
    u16 = lambda o: struct.unpack(">H" if big else "<H", data[o:o + 2])[0]
    ifd = base + u32(base + 4)
    count, entries = 0, None
    for i in range(u16(ifd)):
        e = ifd + 2 + i * 12
        tag = u16(e)
        if tag == 0xB001:
            count = u32(e + 8)
        elif tag == 0xB002:
            entries = base + u32(e + 8)
    if entries is None:
        return None
    out = []
    for i in range(count):
        o = entries + i * 16
        out.append((u32(o), u32(o + 4), u32(o + 8)))
    return {"base": base, "entries": out}


def parse_iso_payload(data, segs=None):
    at = -1
    if segs:
        # Look inside APP2 only. The URN turns up in XMP text in some writers'
        # files, and scanning the whole file finds that copy first.
        for marker, off, size in segs:
            if marker == 0xE2 and data[off:off + len(ISO_URN)] == ISO_URN:
                at = off
                break
    if at < 0:
        at = data.find(ISO_URN)
    if at < 0:
        return None
    q = at + len(ISO_URN)
    rat_s = lambda o: (struct.unpack(">i", data[o:o + 4])[0],
                       struct.unpack(">I", data[o + 4:o + 8])[0])
    flags = data[q + 4]
    multi = bool(flags & 0x80)
    head = q + 5
    ch = head + 16
    channels = []
    for c in range(3 if multi else 1):
        o = ch + c * 40
        vals = []
        for k in range(5):
            n, d = rat_s(o + k * 8)
            vals.append(n / d if d else 0.0)
        channels.append(vals)  # min, max, gamma, baseOffset, altOffset
    n, d = rat_s(head + 8)
    return {
        "multichannel": multi,
        "use_base_colour_space": bool(flags & 0x40),
        "alt_headroom": n / d if d else 0.0,
        "channels": channels,
        "writer_version": struct.unpack(">H", data[q + 2:q + 4])[0],
    }


def xmp_blocks(data, segs):
    out = []
    for marker, off, size in segs:
        if marker == 0xE1 and data[off:off + 29].startswith(b"http://ns.adobe.com/xap/1.0/\x00"):
            out.append(data[off + 29:off + size].decode("utf-8", "replace"))
    return out


def attr(name, xml):
    """An hdrgm value's text, however it was written. See attr_form for syntax."""
    value, _ = attr_form(name, xml)
    return value


def attr_form(name, xml):
    """(text, form) for an hdrgm property, where form is "attribute" or "seq".

    The distinction is load-bearing. The gain map spec writes a single value as
    an XML attribute and a per-channel value as an rdf:Seq of rdf:li elements;
    it never writes three values inside one attribute. This encoder did exactly
    that for a while — hdrgm:GainMapMax="2.46645, 2.29249, 2.28328" — which
    contains all the right numbers and is not the per-channel form any decoder
    looks for. Returning both values and syntax is what lets the caller tell
    "three channels declared" from "three channels declared legibly".
    """
    m = re.search(r'%s\s*=\s*"([^"]*)"' % re.escape(name), xml)
    if m:
        return m.group(1), "attribute"
    m = re.search(r'<%s>(.*?)</%s>' % (re.escape(name), re.escape(name)), xml, re.S)
    if m:
        items = re.findall(r'<rdf:li[^>]*>([^<]*)</rdf:li>', m.group(1), re.S)
        if items:
            return ", ".join(v.strip() for v in items), "seq"
        return m.group(1).strip(), "seq"
    return None, None


def validate(path, r):
    data = open(path, "rb").read()
    print(f"\n{BOLD}{'=' * 72}{RESET}\n{BOLD}{os.path.basename(path)}{RESET}"
          f" {GREY}({len(data) / 1e6:.2f} MB){RESET}")

    # ---------------------------------------------------------------- structure
    r.section("Container", "everything downstream depends on this")
    r.check(data[:2] == b"\xff\xd8", "starts with a JPEG SOI marker")
    psegs, primary_end = jpeg_segments(data, 0)
    r.check(primary_end < len(data), "a second image follows the primary",
            f"primary {primary_end} bytes, trailer {len(data) - primary_end}")

    mpf = mpf_entries(data)
    if not r.check(mpf is not None, "the primary carries an MPF index"):
        return
    entries = mpf["entries"]
    r.check(len(entries) == 2, "MPF declares exactly two images", f"declares {len(entries)}")

    if len(entries) == 2:
        (a0, s0, o0), (a1, s1, o1) = entries
        r.check(a0 & 0xFFFFFF == MP_PRIMARY, "image 1 is the Baseline MP Primary Image",
                f"type 0x{a0 & 0xFFFFFF:06X}")
        r.check(bool(a0 & MP_REPRESENTATIVE), "image 1 is flagged Representative",
                "the flag that says which image to display")
        r.check(a1 & 0xFFFFFF == MP_GAIN_MAP, "image 2 is typed Gain Map Image",
                f"type 0x{a1 & 0xFFFFFF:06X}; 0x000000 (Undefined) leaves a reader "
                f"that trusts the index with nothing to go on")
        r.check(s0 == primary_end, "the declared primary size matches the file",
                f"declared {s0}, actual {primary_end}")
        r.check(o0 == 0, "the primary's offset is zero")
        # MPF offsets are measured from the first byte of the MP Endian field.
        want = primary_end - mpf["base"]
        r.check(o1 == want, "the gain map's offset is measured from the MP Endian field",
                f"declared {o1}, expected {want}")
        r.check(s1 == len(data) - primary_end, "the declared gain map size matches",
                f"declared {s1}, actual {len(data) - primary_end}")

    # ------------------------------------------------------------- the gain map
    r.section("Gain map image", "iMessage, and anything scanning the trailer")
    gm = data[primary_end:]
    r.check(gm[:2] == b"\xff\xd8", "the trailer starts with SOI")
    nxt = gm[2:4]
    r.check(nxt in (b"\xff\xe0", b"\xff\xe1", b"\xff\xdb"),
            "SOI is followed by APP0, APP1 or DQT",
            f"found {nxt.hex()}; scanners sniff for these three and skip anything else")
    r.check(gm.rstrip(b"\x00")[-2:] == b"\xff\xd9", "the gain map ends with EOI")

    gsegs, gend = jpeg_segments(data, primary_end)
    sof = next((s for s in gsegs if s[0] in (0xC0, 0xC1, 0xC2)), None)
    if sof:
        comps = data[sof[1] + 5]
        h = struct.unpack(">H", data[sof[1] + 1:sof[1] + 3])[0]
        w = struct.unpack(">H", data[sof[1] + 3:sof[1] + 5])[0]
        r.check(comps in (1, 3), "the gain map is 1 or 3 channel", f"{comps} channel, {w}x{h}")
        r.check(comps == 3, "the gain map is three channel", required=False,
                detail="one gain cannot follow a highlight that changes hue")

    # -------------------------------------------------------------- ISO payload
    r.section("ISO 21496-1", "Apple: Photos, Preview, Safari, iMessage previews")
    # The base image carries a marker APP2 — the URN and two version fields,
    # no parameters — saying a gain map belongs to it. Lightroom and the Pixel
    # camera both write exactly these 34 bytes. Without it a decoder has to walk
    # MPF to the second image before it knows the file is HDR at all.
    base_marker = any(
        marker == 0xE2 and data[off:off + len(ISO_URN)] == ISO_URN and size == 32
        for marker, off, size in psegs)
    r.check(base_marker, "the base image carries the ISO 21496-1 marker APP2",
            "" if base_marker else "34-byte APP2 holding the URN and versions")

    iso = parse_iso_payload(data, gsegs)
    if r.check(iso is not None, "the ISO 21496-1 URN and payload are present"):
        r.check(iso["alt_headroom"] > 0, "a positive alternate headroom is declared",
                f"{iso['alt_headroom']:.4f} EV")
        for i, ch in enumerate(iso["channels"]):
            lo, hi, gamma = ch[0], ch[1], ch[2]
            r.check(hi > lo, f"channel {i} has a real range", f"{lo:+.4f} to {hi:+.4f} EV")
            r.check(gamma > 0, f"channel {i} gamma is positive", f"{gamma:.4f}")
        r.check(iso["multichannel"] == (sof is not None and data[sof[1] + 5] == 3),
                "the multichannel flag matches the image's channel count")

    # --------------------------------------------------------------- Ultra HDR
    r.section("Ultra HDR / hdrgm XMP", "Android, Google Photos, Chrome")
    pxmp = xmp_blocks(data, psegs)
    gxmp = xmp_blocks(data, gsegs)
    joined_p = "\n".join(pxmp)
    joined_g = "\n".join(gxmp)

    has_container = "http://ns.google.com/photos/1.0/container/" in joined_p
    r.check(has_container, "the primary carries a GContainer directory", required=False,
            detail="Lightroom writes none and Google's own decoder accepts its files, "
                   "so this is belt and braces rather than required")
    length = attr("Item:Length", joined_p) if has_container else None
    if has_container:
        r.check("Semantic=\"Primary\"" in joined_p, "the directory names a Primary item")
        r.check("GainMap" in joined_p, "the directory names a GainMap item")
    if length is not None:
        actual = len(data) - primary_end
        r.check(int(length) == actual,
                "the declared length matches the gain map on disk",
                f"declared {length}, actual {actual}; Android reads the gain map "
                f"at this length and a mismatch is silently dropped")

    r.check("http://ns.adobe.com/hdr-gain-map/1.0/" in joined_g,
            "the gain map image carries hdrgm XMP", required=False,
            detail="decoders that predate ISO 21496-1 read this instead")
    if "hdr-gain-map" in joined_g:
        for name in ("hdrgm:GainMapMin", "hdrgm:GainMapMax", "hdrgm:Gamma",
                     "hdrgm:HDRCapacityMax"):
            v = attr(name, joined_g)
            r.check(v is not None, f"{name} is present", v or "")
        if iso and iso["multichannel"]:
            for name in ("hdrgm:GainMapMin", "hdrgm:GainMapMax", "hdrgm:Gamma"):
                v, form = attr_form(name, joined_g)
                v = v or ""
                if v.count(",") != 2:
                    # One value for three channels: an XMP-only decoder applies
                    # the red range to green and blue too. Legal when the three
                    # genuinely agree, which is why this is not fatal.
                    r.check(v.count(",") == 0, f"{name} is a single coherent value",
                            f'"{v}"')
                    continue
                r.check(form == "seq",
                        f"{name} uses the rdf:Seq form for its three channels",
                        f'"{v}" written as an {form}; three values inside one '
                        f"attribute is not a form the spec defines, and an "
                        f"XMP-reading decoder gets either the red channel or "
                        f"nothing")

    # ------------------------------------------------------------ external tools
    r.section("External validators", "independent of anything above")
    exiftool = which("exiftool")
    if exiftool:
        out = subprocess.run([exiftool, "-validate", "-warning", "-a", path],
                             capture_output=True, text=True).stdout
        warnings = [l for l in out.splitlines() if re.match(r"\s*Warning\s*:", l)]
        gainmap_related = [w for w in warnings
                           if re.search(r"gain ?map|trailer|MPF|APP2", w, re.I)]
        r.check(not gainmap_related, "exiftool reports no gain map warnings",
                "; ".join(w.split(":", 1)[1].strip() for w in gainmap_related))
        other = [w for w in warnings if w not in gainmap_related]
        r.check(not other, "exiftool reports no other warnings", required=False,
                detail="; ".join(w.split(":", 1)[1].strip() for w in other))
    else:
        r.skip("exiftool", "not installed")

    uhdr = which("ultrahdr_app")
    if uhdr:
        out = subprocess.run([uhdr, "-m", "1", "-j", path, "-P"],
                             capture_output=True, text=True).stdout
        detected = "Ultra HDR Image: Yes" in out
        r.check(detected, "libultrahdr accepts it as an Ultra HDR image",
                "Google's reference codec, and what Android reads")
        if detected:
            import tempfile
            td = tempfile.mkdtemp()
            raw = os.path.join(td, "out.raw")
            dec = subprocess.run([uhdr, "-m", "1", "-j", os.path.abspath(path),
                                  "-o", "1", "-O", "5", "-z", raw],
                                 capture_output=True, text=True)
            size = os.path.getsize(raw) if os.path.exists(raw) else 0
            r.check(size > 0, "libultrahdr decodes it end to end",
                    f"{size / 1e6:.0f} MB of RGBA1010102" if size
                    else (dec.stdout + dec.stderr).strip()[:150])
            if os.path.exists(raw):
                os.remove(raw)
            os.rmdir(td)
    else:
        r.skip("libultrahdr", "brew install libultrahdr")

    probe = os.path.join(os.path.dirname(os.path.abspath(__file__)), "probe_imageio.swift")
    if sys.platform == "darwin" and os.path.exists(probe) and which("swift"):
        out = subprocess.run(["swift", probe, path], capture_output=True, text=True).stdout
        r.check("ISO gain map: FOUND" in out,
                "macOS ImageIO reads it as an ISO 21496-1 gain map",
                out.strip().replace("\n", "; "))
    else:
        r.skip("macOS ImageIO", "needs macOS with swift")


def which(name):
    for d in os.environ.get("PATH", "").split(os.pathsep):
        p = os.path.join(d, name)
        if os.path.isfile(p) and os.access(p, os.X_OK):
            return p
    return None


def main(argv):
    paths = [a for a in argv[1:] if not a.startswith("-")]
    if not paths:
        print(__doc__)
        return 2
    r = Report()
    for p in paths:
        if not os.path.exists(p):
            print(f"{RED}no such file: {p}{RESET}")
            r.failed += 1
            continue
        validate(p, r)
    print(f"\n{BOLD}{'=' * 72}{RESET}")
    if r.failed:
        print(f"{RED}{r.failed} required check(s) failed{RESET}"
              + (f", {r.warned} warning(s)" if r.warned else ""))
        return 1
    print(f"{GREEN}all required checks passed{RESET}"
          + (f", {YELLOW}{r.warned} warning(s){RESET}" if r.warned else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
