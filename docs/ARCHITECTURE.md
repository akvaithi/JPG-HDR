# Architecture

## Why two components

Lightroom Classic's Lua SDK can render pixels and write files, but it cannot
control gain map channel counts, subsampling factors, or arbitrary APP2 binary
payloads, and it will not let a plug-in near its own gain map writer. So the
plug-in does what the SDK is good at — dialog integration, render
orchestration, progress, error surfacing — and hands the pixels to a native
binary that does what Lua cannot.

The interface between them is a command line and an exit code. That keeps the
Lua side small and testable, and lets the encoder be used, benchmarked and
debugged entirely on its own.

## The pipeline

```
 Lightroom render                iso21496_encoder
 ─────────────────               ─────────────────────────────────────────────
 16-bit ProPhoto TIFF  ────▶  TiffReader        strips/tiles, 8/16/32f, LZW
                                   │
                              resolveInputSpace  ICC inspection or CLI override
                                   │
                              decodeTransfer     ROMM / sRGB / linear / PQ / HLG
                                   │
                              conversionMatrix   → Display P3 / sRGB / Rec.2020
                                   │
                     ┌─────────────┴─────────────┐
                     ▼                           ▼
              tone map + lift +            log2 gain per pixel,
              contrast → SDR base          measured against that base
              (8-bit)                      → gain map (8-bit, subsampled)
                     │                           │
                     ▼                           ▼
              encodeJpeg + ICC             encodeJpeg + ISO 21496-1 APP2
              + Exif + XMP + MPF           + hdrgm XMP
                     │                           │
                     └──────────┬────────────────┘
                                ▼
                        patchMpfSegment  →  photo.jpg
```

### Reading the intermediate

`TiffReader` (`tiff_reader.cpp`) is a baseline TIFF reader covering exactly
what Lightroom can emit plus a little more: both byte orders, strips and tiles,
8/16-bit unsigned and 32-bit float samples, planar and chunky layouts, LZW and
PackBits, and both predictors. Deflate and JPEG-in-TIFF are rejected with an
explicit message rather than mis-decoded — the plug-in always asks for
uncompressed output, so this only ever fires if someone hand-feeds the CLI.

It also lifts out the ICC profile, the XMP packet, and the Exif and GPS IFD
offsets, which the encoder needs later.

Rows are decoded on demand through `readRows(firstRow, count, dst)`, which is
safe to call concurrently for disjoint ranges. Nothing ever materialises the
whole image as floats.

### Deciding what the pixels mean

`resolveInputSpace` (`pipeline.cpp`) works out the input's primaries and
transfer function, preferring, in order: an explicit `--input-primaries` /
`--input-transfer`; an ICC `cicp` tag, which names the transfer characteristic
exactly; the ICC colorant tags matched against known spaces; and finally the
shape of the file (32-bit float means linear).

One special case: ProPhoto's ICC profile advertises a pure 1.8 gamma, but the
real ROMM curve has a linear toe below 16/512. When the primaries are ProPhoto
and the profile claims gamma 1.8, the encoder uses the ROMM curve.

### Tone mapping and the gain map

Everything happens in the output colour space, in one streaming pass over
bands of rows sized to the TIFF's own strip height so nothing is decompressed
twice. Each band is processed on its own thread.

For each pixel: decode to linear, convert primaries, clamp to
`[0, 2^maxBoost]`, shape the luminance, scale RGB by the resulting ratio, and
encode to 8-bit sRGB. The gain is `log2((hdr + offsetHdr) / (sdr +
offsetSdr))` — on luminance for a monochrome map, per channel for an RGB one —
box-averaged over the subsampling block, normalised against the boost range,
and raised to `1/gamma` before quantising to 8 bits.

The default tone curve is extended Reinhard, which maps the maximum HDR value
to exactly 1.0. That matters: the HDR clamp and the gain map's maximum boost
are the same number, so every highlight the base image gives up is one the gain
map can hand back, with nothing clipped and no range wasted.

**The SDR base look.** The tone curve alone is not a good SDR rendition. Plain
extended Reinhard puts mid grey at −0.24 EV relative to the source — measured,
and near-constant across headrooms, because its `1 + L/ceiling^2` term is
within a rounding error of 1 through the midtones. So the base is shaped:

1. **Lift** — an exposure gain on the tone-mapped luminance, 0.43 EV by
   default. Highlights pushed past 1.0 clip in the 8-bit base and are restored
   by the gain map. (The usual way to lift a Reinhard-family curve is to shrink
   its white point, which is what the reference implementation this borrows
   from does with Apple's tone map. That does not transfer: measured against
   this curve, shrinking the ceiling by 0.43 EV moved mid grey by 0.001 EV. An
   explicit exposure gain does what it says.)
2. **Contrast** — a power law about a 0.18 linear pivot, 1.14 by default,
   applied as a luminance-derived *uniform scale on RGB*. A per-channel power
   law would spread the channel ratios apart — for R > G, (R/G)^c > R/G — and
   silently raise saturation along with contrast. Scaling all three channels by
   one number leaves chromaticity untouched.

Both shape the SDR base only. The gain map is measured against the base as
actually written, shaping and per-channel clipping included, so the two cancel:
measured, shaping moves the reconstructed HDR rendition by 0.008 EV on average.

**Auto max boost.** Every pixel is folded into a box-averaged downscale
(roughly a 2048-pixel long edge, always at least 2×2), and the peak is taken
from that. Averaging cuts both ways deliberately: a lone hot pixel or a speck
of sensor noise cannot define the headroom for the whole frame, and a small
specular highlight can never be skipped over, because it always raises the
block that contains it. `--peak-detect exact` uses the true per-pixel maximum
instead. Only luminance is needed, so the primaries matrix is folded into the
weights and the pass costs one dot product per pixel.

Unless disabled, the gain map's maximum boost is that measurement plus a sixth
of a stop of slack, rather than the user's target.

**The gain floor.** Once the base is lifted it is brighter than the HDR image
through the midtones, so the gain map has to darken as well as brighten; a
floor pinned at 0 would clamp those pixels and reconstruct them too bright. The
floor is found by sweeping the shaping function analytically — the mapping from
HDR luminance to shaped SDR luminance is one-dimensional, so a dense sweep
finds the true extreme with no extra pass over the pixels and no dependence on
whether a downscale happened to catch the right pixel. It is clamped to −1 EV,
keeping it in the territory real captures occupy (an iPhone 17 writes about
−0.49 EV; Lightroom's own export −0.39 to −0.46 EV).

### Writing the JPEGs

`jpeg_encoder.cpp` is a baseline sequential encoder: AAN float DCT, Annex K
quantisation tables scaled by quality, 4:4:4 or 4:2:0, one or three components,
and optimal Huffman tables built with libjpeg's length-limiting construction.
Colour conversion and the DCT run in parallel over rows and block rows; entropy
coding is a single serial pass over the stored coefficients.

It is not libjpeg because the plug-in ships one static binary per platform and
the required feature set is this small. It is validated against libjpeg in the
test suite whenever libjpeg is present at build time.

### Assembling the file

The gain map image is encoded first, because the primary image's XMP has to
state its length. The primary then gets JFIF, Exif, XMP, ICC and a
fixed-size MPF placeholder. Once both images exist, they are concatenated and
`patchMpfSegment` fills in the two image sizes and the offset of the second
image, measured — as CIPA DC-007 requires — from the first byte of the MP
Endian field.

## Metadata

### ISO 21496-1 payload

The APP2 segment on the gain map image opens with the 28-byte
`urn:iso:std:iso:ts:21496:-1\0` identifier, followed by:

| Field | Encoding |
|---|---|
| `minimum_version`, `writer_version` | `uint16` each |
| flags | `uint8`; bit 7 multichannel, bit 6 use-base-colour-space |
| base / alternate HDR headroom | unsigned rational pairs |
| per channel: min boost, max boost, gamma, base offset, alternate offset | signed or unsigned rational pairs |

**Alternate headroom is the measured requirement, not the user's ceiling.** A
decoder applies the gain scaled by
`(display_headroom − base_headroom) / (alternate_headroom − base_headroom)`,
clamped to 1. Writing the 4 EV ceiling into a photo that needs 1.6 EV would
make a display with 2 EV of headroom apply half the gain — dim, despite having
more headroom than the image needs. So the encoder declares what it measured;
the user's setting remains a ceiling that caps it.

**A deliberate deviation from the build spec.** Section 5.2 of the spec
describes these values as `float32`. The published standard encodes them as
rational pairs (numerator and denominator, big-endian), and that is what
shipping decoders — Apple's, Android's, Chrome's — parse. Writing float32
fields would produce a file no current decoder could read, so the encoder
writes rationals with a fixed denominator of 1,000,000. Every value the spec
lists is present with the semantics it describes; only the on-the-wire encoding
differs.

Two smaller choices, both within what the spec leaves open: the offset
constants default to 1/64 rather than 0.01, because 1/64 is exactly
representable and is the value other gain map writers use; and gamma defaults
to 2.2 as the spec specifies, exposed as `--gamma` for anyone who wants 1.0.

### MPF

A CIPA DC-007 MP Index IFD in the primary image, big-endian, with three tags
(`MPFVersion`, `NumberOfImages = 2`, `MPEntry`) and two 16-byte entries. The
primary is attribute `0x030000` — Baseline MP Primary Image — at offset 0; the
gain map is attribute `0x000000`, the convention current gain map writers use,
with decoders locating it through the ISO 21496-1 marker rather than the MPF
type code.

### Exif, XMP and ICC

Exif is rebuilt rather than copied: the source IFDs are little- or big-endian
TIFF structures full of absolute offsets, so `exif.cpp` re-serialises a
whitelist of IFD0 tags plus the whole Exif and GPS sub-IFDs into a fresh
big-endian block with corrected offsets. MakerNote is dropped — it is riddled
with offsets into the original file and cannot be relocated safely.
Orientation is forced to 1 because Lightroom bakes rotation into the render,
the pixel dimensions are corrected to the output size, and the tags
`exiftool -validate` insists on are filled in with conventional defaults.

ICC profiles are generated, not shipped as blobs: `icc.cpp` builds a v2
matrix/TRC display profile with D50-adapted colorants, a 1024-entry sampled
sRGB tone curve and a chromatic adaptation tag, in about 3 KB.

XMP carries the Adobe `hdrgm:1.0` parameters on the gain map and a GContainer
directory on the primary, so decoders that predate ISO 21496-1 still recognise
the file.

## Memory and threading

Peak memory on a 45 MP export is 678 MB, dominated by the intermediate TIFF
held in memory. The encoder drops that buffer the moment the pipeline finishes
— Exif is extracted beforehand specifically so it can — leaving only the SDR
base image and the JPEG coefficient arrays alive during encoding. Memory
mapping the input would cut the peak further and is the obvious next
optimisation.

Threading is plain `std::thread` through a small `parallelFor`, deliberately
not OpenMP: an OpenMP runtime would have to be shipped alongside the plug-in or
found on the photographer's machine, and the whole point of this binary is that
it depends on nothing.

## Testing

| Test | What it covers |
|---|---|
| `tiff` | Byte orders, bit depths, strip layouts, partial reads, metadata, malformed input |
| `color` | White point adaptation, matrix round trips, transfer functions, ICC generation and detection |
| `jpeg` | Marker structure, quality response, optimal tables, byte stuffing, APP segment chains |
| `metadata` | ISO payload byte layout, MPF patching, XMP contents, Exif reconstruction |
| `endtoend` | A full export parsed back apart: both images, MPF consistency, declared headroom, gain floor, small-highlight survival, error paths |
| `decode` *(needs libjpeg)* | Our bitstream decoded by libjpeg, the HDR image reconstructed from the result, and SDR shaping shown not to move it |
| `exiftool_compliance` *(needs exiftool)* | The metadata audit from the build spec, run the way a reviewer would |
| `plugin/tests/test_plugin.lua` *(needs Lua)* | The plug-in's settings, argument construction, shell quoting and report parsing, against the real binary |
