# Handoff

Where the project stands, what has not been proven, and what to do next.

## State

Everything in the build spec is implemented and, where it can be checked
without Adobe software or an HDR panel, verified. Seven CTest cases and a
60-check Lua harness pass; CI builds a macOS universal binary, a Windows x64
binary and the plug-in bundle.

Measured on a 45 MP (8192 × 5464) synthetic HDR frame with fine grain, four
cores:

| | |
|---|---|
| Encode time | 3.8 s |
| Peak memory | 678 MB |
| Gain map overhead (mono 1:2) | +12.2% |
| HDR reconstruction error | 0.43% mean, 2.4% worst in highlights |
| HDR shift from SDR shaping | 0.008 EV |

**Nothing has run inside Lightroom Classic, and no output has been opened on an
HDR display.** Those are the two things that matter most and neither could be
done in this environment. Everything below is ordered around that.

## Before anything else: the first real export

1. Build both binaries (`scripts/build.sh` on macOS,
   `scripts/build_windows.ps1` on Windows) or take them from a CI run, then
   `scripts/package.sh` to assemble the bundle.
2. On macOS, clear quarantine: `xattr -dr com.apple.quarantine <bundle>`.
3. Add the bundle in **File ▸ Plug-in Manager**. The panel reports the encoder
   version — if it does not, stop and fix that first.
4. Export one HDR-edited raw via **ISO 21496-1 HDR JPEG**.
5. Run the encoder on the intermediate by hand with `--json --verbose` (tick
   *Keep the intermediate TIFF* to get one) and read `measuredHeadroom`.

That last step is the single most informative diagnostic. If
`measuredHeadroom` is near zero, the render Lightroom produced had no HDR data
in it — a Lightroom settings problem, not an encoder one, and everything
downstream will look wrong for that reason alone.

Then walk the checklists in [ACCEPTANCE.md](ACCEPTANCE.md) §1 and §4.

## Known risks

Ordered by how likely they are to bite.

**1. The Lightroom HDR export keys.** `LR_export_useHDR` and
`LR_export_maximizeCompatibility` come from a shipping third-party plug-in
tested against Lightroom Classic 14, not from documentation. They are the most
likely thing to be wrong or renamed. The export dialog now warns rather than
silently producing an SDR file, so a failure here should be visible instead of
mysterious — but verify on the first export.

**2. Gain map gamma 2.2 vs 1.0.** The build spec asks for 2.2 and that is the
default. Apple, Google and libultrahdr all write 1.0. Both are legal and the
value is signalled in the metadata, so a conforming decoder handles either —
but 1.0 is the well-travelled path, and a decoder bug in gamma handling would
show up on our files and not on a Pixel's. If device testing shows anything odd
about highlight rolloff, try `--gamma 1.0` before suspecting anything else.

**3. The SDR base defaults.** 0.43 EV lift and 1.14 contrast are borrowed from
a reference implementation that tuned them on one photo, one photographer's
taste, and a different tone curve. They put mid grey at +0.21 EV relative to
the source here. This is the setting most likely to want adjusting once you see
real photos; it is fully exposed, and *Neutral* in the dialog turns it off.

**4. The export filter's file naming.** The filter variant renames `.tif` to
`.jpg` after the export finishes, and skips the rename when *Add to This
Catalog* is on. The export *service* provider has none of this awkwardness. If
the filter turns out to be more trouble than it is worth in practice, consider
dropping it — the spec asks for the `postProcessRenderedPhotos` bridge, which
the service provider also satisfies in substance.

**5. Instagram and Threads.** Both re-encode uploads server-side. Whether a
gain map survives depends on their pipeline that week, not on the file. Do not
treat a failure there as an encoder bug without checking a Pixel-produced file
behaves the same way.

## Next steps, in the order I would do them

### 1. Cross-validate against libultrahdr — half a day

Add a CI step that decodes our output with `google/libultrahdr`'s
`ultrahdr_app` and compares the reconstructed HDR against ours. Right now the
decode test uses our own understanding of the standard on both sides of the
round trip, so a misreading of the spec would be invisible. An independent
decoder closes that gap, and it is the highest-value test still missing.

### 2. Sign and notarise the macOS binary — half a day

Every downloaded copy currently needs a manual `xattr` step, which is a real
adoption barrier and looks alarming to photographers. The reference project has
`adhoc`, `notarize` and `staple` Makefile targets worth copying. Needs an Apple
Developer account.

### 3. Memory-map the input TIFF — half a day

Peak memory is 678 MB on a 45 MP export, dominated by holding the whole
intermediate in RAM. `mmap` on POSIX and `CreateFileMapping` on Windows would
cut that to roughly 200 MB and speed up the cold run, at the cost of a small
platform-specific layer in `common.cpp`. Matters for batch exports on 16 GB
machines.

### 4. Carry Lightroom's XMP through — one to two days

Both this plug-in and the reference implementation drop Lightroom's XMP packet
(IPTC, `dc:creator`, `dc:rights`, develop settings), because JPEG permits one
XMP packet and the gain map metadata needs it. Exif carries Artist and
Copyright so the legally important fields survive, but the rest does not.

The fix neither implementation has: parse the source packet and *merge* the
`hdrgm:` and `Container:` properties into it rather than replacing it. It is
XML surgery on an untrusted input, so it needs care and its own tests, but it
would make this the only gain map exporter that preserves a photographer's
full metadata.

### 5. Scene-adaptive SDR shaping — exploratory

The lift and contrast are constants applied to every photo. A low-key night
shot and a bright beach scene do not want the same treatment. Deriving the lift
from the image's own tone distribution — median luminance, or the gap between
the tone-mapped and original midtone — would be more defensible than one
number. Worth prototyping only after real photos show the fixed values failing.

### 6. Smaller things

* **Progressive JPEG** for the base image: typically 5–10% smaller at identical
  quality, universally supported. A scan-script loop over the existing entropy
  coder.
* **Rec. 2020 output currently uses the sRGB transfer curve.** Legal, and the
  ICC profile says so truthfully, but a naive decoder assuming BT.1886 would be
  wrong. Consider emitting a matching curve, or document it louder.
* **10-bit gain maps** are mentioned in the spec but not implemented; baseline
  JPEG is 8-bit and no target decoder reads more. Revisit only if that changes.
* **`--peak-detect` block size** is fixed at roughly a 2048-pixel long edge.
  If small-highlight clipping shows up on real photos, this is the knob.
* The empty `encoder/cmake/` directory can go.

## Things deliberately not done

* **No third-party dependency in the encoder.** The TIFF reader, JPEG encoder
  and ICC generation are all written here so the plug-in ships one static
  binary per platform with no runtime requirements. That is a spec requirement
  (§4.2), not a preference. Adding libjpeg or libtiff would undo it.
* **No HEIC path.** The repository you compared against has one; HEIC gain maps
  do not render on Android at all, which is why that project ships a separate
  Ultra HDR JPEG plug-in. A JPEG container is the cross-platform answer and
  this project is only that.
* **Float32 ISO payload.** The spec describes one; the standard and every
  shipping decoder use rationals. Deliberate deviation, documented in
  [ARCHITECTURE.md](ARCHITECTURE.md#iso-21496-1-payload).

## Where to look

| | |
|---|---|
| [CLAUDE.md](../CLAUDE.md) | Invariants, gotchas, workflow — read before changing code |
| [ARCHITECTURE.md](ARCHITECTURE.md) | How the pipeline works and why |
| [ACCEPTANCE.md](ACCEPTANCE.md) | Each spec criterion, how it is verified, what needs hardware |
| [INSTALL.md](INSTALL.md) | Installation, settings, troubleshooting |
| [BUILD_SPEC.md](BUILD_SPEC.md) | The original specification |
