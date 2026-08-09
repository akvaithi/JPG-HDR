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
  HDR image from it. This is the only test that would catch a wrong gain map, so
  run it before believing anything about the gain map.

  It does not run in two places, both for the same reason: a universal build
  cannot link Homebrew's single-architecture libjpeg, so `./scripts/build.sh`
  skips it on macOS, and the macOS CI runner has Mono installed, whose stale
  `jpeglib.h` wins the header search regardless of `JPEG_ROOT`. **Run a native
  build locally to exercise it**, which is where it caught the inverted gamma
  convention:

  ```bash
  cmake -S encoder -B encoder/build-native -DCMAKE_BUILD_TYPE=Release
  cmake --build encoder/build-native --parallel
  ctest --test-dir encoder/build-native --output-on-failure
  ```

  CI covers it on Linux.
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

2. **Declared alternate headroom == the measured *luminance* headroom.** A
   decoder scales the gain it applies by `display_headroom /
   alternate_headroom`. Writing the user's ceiling here makes photos render dim
   on any display with partial headroom. This was a real bug; don't reintroduce
   it. On the reference frame this matches Lightroom's `crs:HDRMaxValue` to four
   decimals.

3. **The gain map's per-channel range is measured, and is *not* the declared
   headroom.** They answer different questions: the headroom is how far above
   SDR white the picture goes, the range is what each channel needs to get back
   there from the base. A saturated highlight puts far more into one channel
   than into the luminance it contributes, so the range legitimately exceeds the
   headroom — Lightroom writes maxima up to 0.39 EV above its own. Tying the two
   together, and clamping every channel at the luminance ceiling, is what
   desaturated warm highlights toward neutral: on the reference render, red
   peaked 0.29 EV above the luminance ceiling and 32,523 pixels were being
   truncated. Each channel is now held at its own measured ceiling, and the
   range comes from the gains themselves rather than from a bound — nothing the
   range fails to cover would survive quantisation.

4. **The gain map floor is not zero** *when the base is lifted enough to need
   it*. A lifted base is brighter than the HDR image through the midtones, so
   the gain map must be able to darken. The floor is swept analytically from the
   shaping function in `computeGainFloor`; if you add a shaping stage, it has to
   go through `SdrShaper` or the sweep will not see it. A floor of exactly 0 is
   not a bug on its own — with contrast above 1 and a small lift the base is
   never brighter than the source, and the sweep correctly finds nothing to
   darken.

5. **The SDR shaping is solved, not fixed.** `solveShaping` reads a lift and a
   contrast off the luminance histogram gathered by the headroom pass, because
   the Reinhard's `1/(1+L)` slope means the correction a frame needs is a
   property of the frame. Two things about it are load-bearing: the contrast is
   solved *before* the lift and the lift *through* it (the contrast pivots about
   mid grey, which moves the anchor the lift is solving for), and
   `kContrastRestore` is 0.4 rather than 1.0 for a documented reason — full
   restoration is `--tone-map clip` by another name. Passing `--sdr-lift` or
   `--sdr-contrast` switches the solver off; the `EncoderOptions` struct does
   *not* infer that, so a library caller must set `sdrShape` itself. The
   `decode` test covers both paths.

6. **The local operator is the default and is not a curve.** `compressBase`
   compresses a guided-filter base layer while the detail layer is added back
   untouched, so two pixels of equal luminance can land on different outputs —
   which is the entire point, and which is why `computeGainFloor` is *not* used
   there. Its floor is exactly 0 because the shoulder never rises above the
   identity. That property is why the shoulder is deliberately not normalised to
   reach white; normalising it would push the slope above 1 just past the knee.
   The detail layer takes the highlights back to white anyway.

7. **The gain map image is encoded before the primary.** The primary's XMP
   states the gain map's byte length, so it cannot be built first.

8. **MPF offsets are patched after both images exist**, and are measured from
   the first byte of the MP Endian field, not from the start of the file.

9. **The gain map is stored as `pow(norm, gamma)`, and decoders read it back
   as `pow(stored, 1/gamma)`** — Ultra HDR and ISO 21496-1 agree. This was
   inverted, and the `decode` test encoded *and* decoded through the same
   inverted convention, so it round-tripped perfectly while every real decoder
   read the file 1.26 EV hot through the midtones. Measured against Lightroom's
   own export of the same edit before the fix: +1.30 EV at the median, 55% of
   the frame above SDR white where Lightroom put 11%. Any test here must decode
   the way the standard says, not the way this encoder writes. The default gamma
   is 1/2.2 — below 1, which is what spends codes on the low gains.

10. **The ISO 21496-1 payload is rational pairs, not float32.** The build spec
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

* **Lightroom's HDR intermediate is Rec. 2020 PQ, 16-bit — not what the export
  settings ask for.** `updateExportSettings` sets ProPhotoRGB; with HDR output
  on, Lightroom overrides it. The encoder detects this correctly from the ICC
  profile and its measured headroom then matches `crs:HDRMaxValue` in the same
  file's XMP to four decimals. Don't "fix" the input handling to match what the
  export settings requested.

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

* **`LrTasks.execute` yields, and dialog sections are built on the main
  thread.** Running the encoder while assembling a view fails with "We can only
  wait from within a task", or "Yielding is not allowed within a C or metamethod
  call" from inside a binding, and Lightroom refuses to draw the panel. Anything
  that shells out has to go through `LrTasks.startAsyncTask` and arrive by
  binding — see `IsoEncoder.version`, which returns nil rather than raising when
  it cannot yield. Button actions are not tasks either. `lr_stubs.lua` models
  the rule, so the Lua tests catch it.

* **A per-channel `hdrgm` value is an `rdf:Seq`, never a comma-separated
  attribute.** `hdrgm:GainMapMax="2.46645, 2.29249, 2.28328"` has all three
  numbers in it and is not a form any decoder parses. It shipped for several
  releases and measured perfectly the whole time, because Apple reads the ISO
  rationals and libultrahdr prefers them — only the XMP-only readers (Android,
  Google Photos, Chrome) ever saw it. To test that path, strip the ISO APP2
  segments and run `ultrahdr_app -m 1 -j file.jpg -P` on what is left; with the
  payload present it never reaches the XMP. `scripts/validate.py` now
  distinguishes the two syntaxes rather than accepting either.

* **A JPEG gain map cannot survive re-encoding, and that is structural.** The
  map is a second image after the primary's EOI, so anything that decodes and
  re-encodes the primary keeps the SDR base and nothing else — iMessage among
  them. It is not stripping metadata; the HDR is simply not in the part it
  re-encodes. HEIC carries the map as an item inside the container and comes
  through intact (2.3001 EV before and after, same as an iPhone's own file);
  `scripts/to_heic.swift` repackages one. Note that ImageIO will not hand back
  the gain map's pixels from a JPEG source — the auxiliary dictionary has the
  description and metadata but no `kCGImageAuxiliaryDataInfoData` — so the map
  has to be decoded from the second image and supplied as a planar buffer. An
  earlier version of that script missed this and concluded, wrongly, that HEIC
  could not carry a gain map at all.

* **Never combine "match the render" with `--no-auto-max-boost`.** With no cap
  to fall back on, the encoder would declare its 10-stop maximum as the photo's
  requirement, and invariant 2 does the rest. `IsoSettings.buildArguments`
  drops the flag in that case; the dialog also disables the checkbox.

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
