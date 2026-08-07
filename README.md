# ISO 21496-1 Gain Map HDR JPEG for Lightroom Classic

Export HDR photographs from Adobe Lightroom Classic as **ISO 21496-1:2025 gain
map JPEGs**: ordinary 8-bit JPEGs that look correct everywhere, and light up
specular highlights on HDR displays.

The package is two pieces, as set out in the [build spec](docs/BUILD_SPEC.md):

| | |
|---|---|
| **`plugin/iso21496.lrdevplugin`** | The Lightroom Classic plug-in: export dialog UI, render orchestration, progress and error reporting. |
| **`encoder/`** | `iso21496_encoder`, a self-contained native CLI that does the pixel work and writes the file. |

Lightroom renders each photo to an uncompressed 16-bit ProPhoto TIFF; the
encoder tone maps it to an SDR base image, derives a logarithmic gain map, and
writes both into one CIPA DC-007 multi-picture JPEG with the ISO 21496-1
metadata payload attached to the gain map.

```
  Lightroom Classic ──16-bit ProPhoto TIFF──▶ iso21496_encoder ──▶ photo.jpg
                                                                    ├── SDR baseline JPEG  (+ ICC, Exif, MPF index)
                                                                    └── gain map JPEG      (+ ISO 21496-1 APP2)
```

## Quick start

```bash
# Build the encoder and drop it into the plug-in bundle (macOS or Linux)
./scripts/build.sh

# Windows
pwsh scripts/build_windows.ps1

# Assemble a distributable bundle from both platform binaries
./scripts/package.sh --mac-binary <path> --windows-binary <path>
```

Then add `plugin/iso21496.lrdevplugin` in Lightroom's **File ▸ Plug-in
Manager** and pick **ISO 21496-1 HDR JPEG** in the Export dialog's *Export To*
menu. Full instructions, including the Gatekeeper step on macOS, are in
[docs/INSTALL.md](docs/INSTALL.md).

## Export settings

The five controls from section 3.1 of the spec, with their defaults:

| Setting | Options | Default |
|---|---|---|
| Target HDR headroom | +1.0 / +2.0 / +3.0 / +4.0 EV | **+4.0 EV** (~1280 nits) |
| Base colour space | Display P3 / sRGB / Rec. 2020 | **Display P3** |
| Gain map resolution | 1:1 / 1:2 / 1:4 | **1:2** |
| Gain map channels | Monochrome / RGB | **Monochrome** |
| Baseline JPEG quality | 60–100 | **90** |

The headroom setting is a *ceiling*, not a target: the encoder measures what
the photo actually needs and declares that, so displays with partial headroom
render it at full strength rather than scaling the effect down.

An *Advanced* section adds the SDR base look (brightness lift and contrast),
tone mapping operator, gain map quality and gamma, highlight measurement mode,
metadata copying, and overrides for how the rendered intermediate is
interpreted. Everything is also reachable from the command line —
`iso21496_encoder --help`.

### The SDR base look

A gain map file carries two renditions. The HDR one is fixed by the photo; the
SDR one — what everything without an HDR display shows — is a choice. A plain
tone curve lands about a quarter of a stop under where a hand-graded SDR edit
of the same photo would sit, and flatter, so the base is shaped by default:

| Control | Default | Effect |
|---|---|---|
| Brightness lift | **0.43 EV** | Exposure gain on the tone-mapped base; highlights pushed past white clip in the base and are handed back by the gain map |
| Contrast | **1.14** | Power law about a linear mid-grey pivot, applied as a luminance-derived scale on RGB so saturation is untouched |

Together these put mid grey at +0.21 EV relative to the source instead of
−0.24 EV. Neither affects the HDR rendition — the gain map is measured against
whatever base they produce, and the two cancel to within 0.008 EV (measured,
and asserted by the test suite). Set the lift to 0 and the contrast to 1.0, or
press *Neutral* in the dialog, for an unshaped base.

## Using the encoder directly

```bash
iso21496_encoder --input render.tif --output photo.jpg \
    --headroom 4.0 --color-space DisplayP3 --subsample 2 \
    --channels mono --quality 90 --json
```

It reads uncompressed, LZW or PackBits TIFFs with 8-bit, 16-bit or 32-bit float
samples, detects the colour space from the embedded ICC profile, and needs no
libraries beyond the C++ standard library.

## What the output contains

* A baseline JPEG in Display P3 (or sRGB / Rec. 2020) with an embedded ICC
  profile, the Exif and GPS metadata carried over from the render, and an MPF
  index describing both images.
* A second JPEG holding the gain map — single-channel and half resolution by
  default — carrying the `urn:iso:std:iso:ts:21496:-1` APP2 payload.
* Adobe `hdrgm:1.0` XMP on both images, so decoders that predate ISO 21496-1
  still find the gain map.

## Measured behaviour

From a 45 MP (8192 × 5464) synthetic HDR frame with fine grain, on four cores:

| | |
|---|---|
| Encode time | 3.8 s |
| Peak memory | 678 MB |
| Base JPEG | 3.20 MB |
| Gain map (mono, 1:2) | 0.39 MB — **+12.2%** |
| Gain map (RGB, 1:1) | 1.43 MB — +44.7% |
| HDR reconstruction error | 0.43% mean, 2.4% worst case in highlights |
| HDR shift from SDR shaping | 0.008 EV mean |

The +12.2% figure comes in under the 15–25% band the spec asks for, and the
RGB 1:1 comparison shows what the default is buying. A real photograph's gain
map is smoother than this deliberately grainy fixture, so the overhead in
practice tends to be lower still.

## Repository layout

```
encoder/          native CLI: sources, CMake build, test suite
  src/            one file per concern (TIFF, colour, JPEG, ISO metadata, ...)
  tests/          unit, structural and end-to-end tests
plugin/
  iso21496.lrdevplugin/   the Lightroom bundle
  tests/          headless Lua harness that runs the plug-in against the binary
scripts/          build and packaging
docs/             installation, architecture, acceptance criteria, build spec
```

## Development

```bash
cmake -S encoder -B encoder/build -DCMAKE_BUILD_TYPE=Release
cmake --build encoder/build --parallel
ctest --test-dir encoder/build --output-on-failure
```

The suite is dependency-free, and picks up two extra tests when the tools are
available: a decode round-trip through **libjpeg**, and the metadata compliance
audit through **exiftool**. See
[docs/ACCEPTANCE.md](docs/ACCEPTANCE.md) for how each acceptance criterion in
the build spec is verified, and which ones need real hardware.
