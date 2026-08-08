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

Three controls, with their defaults:

| Setting | Options | Default |
|---|---|---|
| HDR headroom | Match the render / cap at +1.0…+4.0 EV | **Match the render** |
| Depth | 0–2 | **1.25** |
| Baseline JPEG quality | 60–100 | **90** |

*Match the render* declares the headroom the photo actually turned out to need,
which is the same figure Lightroom shows for it — both are reading the same
render. A decoder scales the gain it applies by
`display_headroom / declared_headroom`, so a ceiling above what the image uses
only ever makes it render dim; cap it to hold a set down deliberately, not by
default.

*Baseline JPEG quality* is also the size control. The gain map is always written
at full precision, because every way of shrinking it costs more accuracy than
the bytes it saves. Measured on a 24 MP frame:

| Quality | File | Highlight colour error |
|---|---|---|
| 70 | 8.0 MB | 0.063 EV |
| 80 | 9.2 MB | 0.056 EV |
| **90** | **11.8 MB** | **0.051 EV** |
| 95 | 15.2 MB | 0.051 EV — no measurable gain |

Everything else that used to be a setting — gain map resolution, channels,
quality, encoding gamma, base chroma subsampling, the tone curve — was swept
against a Lightroom reference export and each turned out to have one answer that
is simply better. They are the encoder's defaults now. An *Advanced* section
keeps base colour space, the rendered-encoding overrides for troubleshooting,
and the housekeeping checkboxes. Everything remains reachable from the command
line — `iso21496_encoder --help`.

### The SDR base look

A gain map file carries two renditions, and the SDR one is a choice. It is also
not only a fallback: a decoder renders
`(SDR + offset) · 2^(gain · w) − offset` where `w` is how much of the file's
headroom your display has, so **every display short of full headroom shows a
blend anchored on the base image**. Measured against Lightroom's export of the
same edit, at 0.5 EV of display headroom our rendition carries 12.6% more
highlight separation; at full headroom the two agree to 0.008 EV.

That is why the base is built by a local operator rather than a curve. A curve
maps every pixel of a given luminance to the same output, so it can make the
picture lighter or darker but never less flat — whatever separation the
compression takes out of the highlights is gone. Instead a self-guided filter
splits log luminance into a smooth base and the detail riding on it, only the
base goes through the shoulder, and the detail is added back at *Depth*
strength. Highlights are pulled into range while the separation inside them
survives.

None of it moves the HDR rendition: the gain map is measured against whatever
base it produces, and the two cancel to within 0.017 EV (measured, and asserted
by the test suite).

## Using the encoder directly

```bash
iso21496_encoder --input render.tif --output photo.jpg \
    --headroom 10 --color-space DisplayP3 --quality 90 \
    --tone-map local --sdr-detail 1.25 --json
```

It reads uncompressed, LZW or PackBits TIFFs with 8-bit, 16-bit or 32-bit float
samples, detects the colour space from the embedded ICC profile, and needs no
libraries beyond the C++ standard library.

## What the output contains

* A baseline JPEG in Display P3 (or sRGB / Rec. 2020) with an embedded ICC
  profile, the Exif and GPS metadata carried over from the render, and an MPF
  index describing both images.
* A second JPEG holding the gain map — three channels at full resolution,
  carrying the `urn:iso:std:iso:ts:21496:-1` APP2 payload with a measured
  per-channel range.
* Adobe `hdrgm:1.0` XMP on both images, so decoders that predate ISO 21496-1
  still find the gain map, with per-channel values written the way Lightroom
  writes them.

## Measured behaviour

Against Lightroom Classic's own HDR export of the same edit — a 24 MP frame,
both files decoded through macOS ImageIO and compared in linear Display P3:

| | |
|---|---|
| HDR agreement, whole frame | **0.006 EV** mean absolute |
| Highlight colour (hue drift, p95) | **0.051 EV** |
| Highlight saturation | **+0.002 EV** |
| Declared headroom | 2.3001 against Lightroom's 2.2999 |
| File size | 11.8 MB against Lightroom's 15.5 MB |
| HDR shift from the SDR base | 0.017 EV mean |

On a 45 MP (8192 × 5464) synthetic frame with fine grain: 1.6 s and 1.7 GB
peak. The memory is dominated by the float buffer the measured gain range needs
and by holding the uncompressed TIFF; both scale linearly with pixel count.

## Status

Running in Lightroom Classic, with output verified on an HDR display and
measured against Lightroom's own gain map export.

The tuning — the shoulder position, the depth strength, the gain map settings —
was swept against **one photograph**. The physics generalises; the taste may
not. A reference set spanning low-key, backlit and strongly saturated frames is
the next thing this needs, and haloing on a hard backlit edge is the specific
failure to look for, since the one test frame has no such edge.

## Repository layout

```
encoder/          native CLI: sources, CMake build, test suite
  src/            one file per concern (TIFF, colour, JPEG, ISO metadata, ...)
  tests/          unit, structural and end-to-end tests
plugin/
  iso21496.lrdevplugin/   the Lightroom bundle
  tests/          headless Lua harness that runs the plug-in against the binary
scripts/          build and packaging
docs/             handoff, architecture, acceptance criteria, install, build spec
CLAUDE.md         invariants and gotchas for anyone changing the code
```

## Development

```bash
cmake -S encoder -B encoder/build -DCMAKE_BUILD_TYPE=Release
cmake --build encoder/build --parallel
ctest --test-dir encoder/build --output-on-failure
```

The suite is dependency-free, and picks up two extra tests when the tools are
available: a decode round-trip through **libjpeg**, and the metadata compliance
audit through **exiftool**. Install both — the decode test is the only one that
would catch a wrong gain map:

```bash
apt-get install -y libjpeg-dev libimage-exiftool-perl lua5.4   # Linux
brew install jpeg-turbo exiftool lua                           # macOS
```

`./scripts/build.sh` runs the C++ suite and then the Lua harness against the
binary it just built; that is the one to run before believing anything works.

* [CLAUDE.md](CLAUDE.md) — invariants that are easy to break, and the gotchas
  that have already bitten.
* [docs/ACCEPTANCE.md](docs/ACCEPTANCE.md) — how each criterion in the build
  spec is verified, and which ones need real hardware.
* [docs/HANDOFF.md](docs/HANDOFF.md) — next steps, known risks, and what was
  deliberately left out.
