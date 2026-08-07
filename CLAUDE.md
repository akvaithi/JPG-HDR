# Working in this repository

Notes for anyone — human or AI — changing this code. Read
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) first for *what* the pieces are;
this file is about the invariants that are easy to break and the workflow that
catches it when you do.

## Build and test

```bash
./scripts/build.sh                 # configure, build, run everything, install into the bundle
cmake --build encoder/build -j8    # just rebuild
ctest --test-dir encoder/build --output-on-failure
```

`scripts/build.sh` is the one to run before claiming anything works: it builds
the encoder, runs the C++ suite, and then runs the Lua harness against the
binary it just built.

Two tests are conditional. Install their tools — they cover things nothing else
does, and CI has them:

```bash
apt-get install -y libjpeg-dev libimage-exiftool-perl lua5.4    # Linux
brew install jpeg-turbo exiftool lua                            # macOS
```

* **`decode`** (needs libjpeg) — decodes our own bitstream and reconstructs the
  HDR image from it. This is the only test that would catch a wrong gain map.
* **`exiftool_compliance`** (needs exiftool) — the metadata audit from the
  build spec, run the way a reviewer would.

Benchmarks are reproducible:

```bash
./encoder/build/tests/make_fixture /tmp/big.tif 8192 5464 8.0 1   # 45MP, grainy
./encoder/build/iso21496_encoder --input /tmp/big.tif --output /tmp/big.jpg --json
```

## Invariants that are easy to break

These are load-bearing. Each one has a test; if you change the pipeline and a
test fails, suspect the change before the test.

1. **The gain map is measured against the base image as actually written** —
   after shaping, after per-channel clipping. Any new SDR processing must
   happen *before* the gain is computed, or the HDR rendition will drift. The
   `decode` test asserts the drift stays under 0.03 EV.

2. **Declared alternate headroom == measured max boost.** A decoder scales the
   gain it applies by `display_headroom / alternate_headroom`. Writing the
   user's ceiling here makes photos render dim on any display with partial
   headroom. This was a real bug; don't reintroduce it.

3. **The HDR clamp ceiling and the gain map's max boost are the same number.**
   That is what guarantees every highlight the base gives up is one the gain
   map can hand back. Change one, change the other.

4. **The gain map floor is not zero.** A lifted base is brighter than the HDR
   image through the midtones, so the gain map must be able to darken. The
   floor is swept analytically from the shaping function in `computeGainFloor`;
   if you add a shaping stage, it has to go through `SdrShaper` or the sweep
   will not see it.

5. **The gain map image is encoded before the primary.** The primary's XMP
   states the gain map's byte length, so it cannot be built first.

6. **MPF offsets are patched after both images exist**, and are measured from
   the first byte of the MP Endian field, not from the start of the file.

7. **The ISO 21496-1 payload is rational pairs, not float32.** The build spec
   says float32; the published standard and every shipping decoder use
   rationals. This deviation is deliberate and documented — see
   [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#iso-21496-1-payload).

## Gotchas that have already bitten

* **Optimised Huffman tables.** The symbol list must be swept over the full
  code-length range (up to 32), not just 1..16. The length-limiting adjustment
  rewrites the *histogram*; symbols keep their original `codesize`. Stopping at
  16 silently drops every rare symbol and the encode dies with "missing AC
  code" on any detailed image above roughly 1.5 MP. Regression test:
  `optimizedTablesKeepRareSymbols`.

* **`readRows` decodes whole strips.** Reading 4 rows from a 32-row-strip TIFF
  decodes the whole strip. Any pass over the image must read in multiples of
  `tiff.suggestedBandRows()`, or it will be 8x slower than it looks. This cost
  60 seconds on a 45 MP frame once.

* **`TiffReader::releaseFileData()` is called after the pixel pipeline.**
  Anything needing the raw file — Exif, ICC, XMP — must be extracted before
  `runPipeline`. `readRows` after release throws rather than returning garbage.

* **Extended Reinhard barely responds to its white point in the midtones.**
  `1 + L/ceiling^2` is within a rounding error of 1 for small L. Lifting by
  shrinking the ceiling moves mid grey by 0.001 EV. Use an exposure gain.

* **Contrast must be a uniform scale on RGB**, derived from luminance — never a
  per-channel power law. Per-channel spreads the channel ratios apart and
  raises saturation along with contrast, which then leaks into the HDR
  rendition on displays with partial headroom.

* **The test TIFF writer stores a single strip's offset inline.** When
  `stripCount == 1` the value lives in the IFD entry, not the value area;
  patching it at the value-area offset (0) overwrites the file header.

* **An export filter cannot change the output file extension.** Lightroom
  derives it from the export format, which the plug-in forces to TIFF. Hence
  the rename-after-export in `ExportFilterProvider.lua`, and hence the export
  *service* provider being the recommended path.

## Lightroom SDK notes

Nothing here can be tested without Lightroom, so be conservative.

* The HDR keys are `LR_export_useHDR` and `LR_export_maximizeCompatibility`.
  These are confirmed against Lightroom Classic 14 by a shipping third-party
  plug-in. Earlier guesses (`LR_export_isHDR`, `LR_export_hdrOutput`) were
  wrong and would have silently produced SDR renders.
* `setIfPresent` is used for keys whose existence varies across SDK versions;
  assigning an unknown key is usually harmless but assigning a *known* key the
  wrong type is not.
* `Info.lua` is the manifest Lightroom reads. `manifest.lua` exists because the
  build spec names it; it just re-exports `Info.lua`.

## Testing the Lua without Lightroom

`plugin/tests/lr_stubs.lua` stubs enough of the SDK (`import`, `LOC`,
`LrFileUtils`, `LrPathUtils`, `LrTasks`, …) to run the non-UI modules under
plain Lua. `plugin/tests/test_plugin.lua` uses it to exercise settings,
argument construction, shell quoting and report parsing against the real
binary. It works on a *copy* of the bundle in a temp directory, because it
pretends to be macOS and would otherwise leave a host binary in the macOS slot
for `scripts/package.sh` to pick up.

Syntax-check everything with `luac5.4 -p plugin/iso21496.lrdevplugin/*.lua`.

## Style

Match what is there. Comments explain *why* — a measured number, a standard's
requirement, a failure that motivated the code — not what the next line does.
Several comments cite specific measurements; if you change the behaviour they
describe, re-measure rather than deleting the number.

## Don't

* Don't add a third-party dependency to the encoder. Single static binary, no
  runtime requirements on the photographer's machine, is the whole point.
* Don't commit built binaries. `bin/macOS` and `bin/windows` are populated by
  the build scripts and CI.
* Don't change a default because it looks wrong — measure it. Most of the
  defaults here have a number behind them, and several came from correcting an
  earlier guess.
