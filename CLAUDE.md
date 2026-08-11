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
   the first byte of the MP Endian field, not from the start of the file. The
   segment itself goes **immediately after the Exif APP1**, before the ISO
   marker, the XMP and the ICC profile — where CIPA DC-007 says the MP Index
   belongs, and where both an iPhone camera JPEG and ImageIO's own writer put
   it. We wrote it last until this release. `test_endtoend` asserts it.

   That is correctness only. It was moved on the theory that its position was
   why iMessage flattened our files 27 → 26, and **a card built with it here was
   sent and lost anyway**. The cause was the `AMPF` marker in the JFIF APP0 —
   see the gotchas. Keep the placement; do not credit it with the fix.

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

## Export performance, measured

The encoder is not the slow part and never was. On an 8-core machine, a 45MP
16-bit intermediate (268 MB, what Lightroom actually hands over):

    read file        0.11 s
    pass 1 (measure) 0.36 s
    rest of pipeline 0.66 s
    gain map jpeg    0.06 s
    primary jpeg     0.40 s
                     ~1.6 s total

`--verbose` prints these. The pipeline scales 4.4x from one thread to eight, so
there is no idle hardware to reclaim; the remaining cost is Lightroom rendering
and writing 268 MB per photo, which we do not control.

What we do control is how fast those intermediates are consumed, because
Lightroom renders on several threads and gets ahead. Throughput, six photos:

    1 worker,  8 threads   0.64 photos/s
    2 workers, 4 threads   0.81
    3 workers, 3 threads   0.85
    4 workers, 2 threads   0.76
    3 workers, default     0.84

So the export encodes three at a time. Threads per worker are deliberately not
set: three-at-default matched three-at-three, and a fixed count would be wrong
on any other machine.

**Do not compress the intermediate.** It is the obvious idea and it is worse on
both counts. Measured on a 45MP 16-bit frame, grainy content against smooth:

    LZW, no predictor    119.3%   (larger than uncompressed)
    LZW + predictor 2     99.2%  /  1.7%
    ZIP + predictor 2     78.7%  /  0.3%

Detailed photographs sit near the grainy end, so LZW buys nothing — and it
costs: the same frame through our LZW path took 1.49 s in the pipeline against
1.01 s uncompressed, +47%. Slowing the consumer is exactly wrong when the
complaint is that intermediates pile up. ZIP would save around a fifth, but the
reader has no Deflate and adding one would cost inflate time on the same
critical path. The smooth-content column is synthetic gradients; ignore it.

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

* **A highlight shoulder driven by the local base layer was tried and
  rejected.** The reasoning was sound and the mechanism worked: take the knee
  from the guided filter's base layer so a large bright area rolls off while a
  specular on a dark ground keeps its punch. Measured across sixteen scenes it
  did exactly that, with a selectivity of 1.75 to 5.04, median 2.59, and at
  matched whole-frame brightness it beat any global knee — 2.4 : 1 against
  1.4 : 1 on the frame it was designed for.

  It was rejected on sight, universally, on all sixteen. The reason is what the
  base layer actually identifies: not "blown highlight" but "large and bright",
  which in a portrait or a group photograph is the *backdrop*. So it darkened
  backgrounds and left faces lit, which reads as a subject cut out from its
  scene. A measurement that says "large bright areas rolled off harder" and a
  photograph that says "the background went muddy" are the same fact.

  Do not rebuild it without a different signal. Anything keyed on local
  brightness will find the backdrop again.

* **Check the phone's power saving mode before believing an Android result.**
  Samsung suppresses HDR rendering in power save, which looks exactly like a
  file the device cannot read: recognised, listed as Ultra HDR, rendered flat.
  It cost a round of testing and nearly a round of encoder changes chasing a
  difference in gain map channel count that was not the problem. Use
  `scripts/make_testcard.py` — the card either reads "HDR ON" or it does not —
  and confirm on a second device before concluding anything about the file.

* **iMessage rebuilds the file from Apple's own gain map description, not the
  standard one.** It re-encodes, discards every ISO 21496-1 segment, and keeps
  what it recognises — so a standards-perfect file arrives as a single flat SDR
  image. `--apple-compatible` adds an `apdi`/`HDRToneMap` rdf:Description beside
  the hdrgm one, which fixes it: sent both ways, 2.3001 EV and 9.5% of the frame
  above SDR white on arrival against 0.00 EV and 0.0% without. Two things are
  load-bearing. The gain map must be *genuinely* single channel — a three
  channel map declaring `StoredFormat` L008 was sent and arrived flattened, so
  the flag forces mono rather than relabelling. And the APP10 `AROT` curve is
  not the signal: grafting it on changed nothing, and ImageIO writes a
  byte-identical blob regardless of the image. The cost is the per-channel
  highlight correction: 0.25 EV in saturated highlights, 0.61 EV of hue drift,
  on ~10% of the reference frame. Everything else is untouched.

* **`AMPF` in the JFIF APP0 is what tells Apple a JPEG has a second image.**
  Four bytes appended past the standard JFIF fields — Apple Multi-Picture
  Format. Every Apple producer writes it on a multi-picture JPEG and nobody
  else writes it at all:

      ours    4a46494600010100000100010000
      Apple   4a46494600010101012c012c0000 414d5046

  Without it, sending 27 → 26 delivered a flat SDR image every time: 3.0000 EV
  in, 0.0000 EV out, second image and MPF index and ICC profile all gone. The
  correlation is perfect across every file measured — iPhone camera JPEG,
  the same file after sending, three ImageIO-written files: all have it, all
  survived; every file this encoder wrote lacked it and every one was
  flattened.

  Only the primary gets it. The gain map image is not a multi-picture container
  and no Apple file marks it as one; `test_endtoend` asserts both halves.
  A non-standard APP0 length does not upset libultrahdr or exiftool — checked,
  not assumed, because Android reads this file too.

  It cost eight rounds of real sends to find, and the reason is worth keeping:
  every round compared segment *lengths* and segment *contents for the segments
  that looked interesting*. APP0 was 16 bytes against 20 and that reads like
  padding. Dump the bytes of every segment that differs at all, including the
  boring ones.

  Ruled out along the way, each by a real send — useful because they are the
  obvious suspects and they are all innocent:

  * the gain map's XMP — two cards differing only in `HDRGainMap:*` both lost;
  * Apple's MakerNote, `HDRHeadroom` and `HDRGain`, with Make/Model/Software
    reading as an iPhone;
  * Apple's own MPF type codes — gain map `0x000000`, no Representative flag;
  * the MPF segment's position, moved to immediately after Exif where CIPA
    DC-007 and both Apple producers put it. Kept for correctness, but the card
    built with it there was lost too.

  * the XMP APP1 on the primary (GContainer + hdrgm), which no survivor
    carries — added to a survivor, it survived anyway;
  * the missing ICC profile on the gain map image, which every survivor has and
    none of ours do — removed from a survivor, it survived anyway.

  Those last two were perfectly confounded across six files until a round built
  the other way round: take the file that *survives*, change one thing, and
  send a known-casualty control alongside. Both fell in one round.

  Then splice the file in half at the primary's EOI and swap the halves —
  `scratchpad/splice.py`. Apple's primary carrying **our** gain map image
  survived; **our** primary carrying Apple's gain map image was lost. So the
  gain map image is not involved at all and the fault is in the primary, and it
  is not metadata. Diffing the two primaries segment by segment, with the XMP
  already cleared, what is left is: `SOF0` 4:4:4 against ImageIO's 4:2:0,
  optimised Huffman tables against standard, no `DRI` against restart
  intervals, and small `JFIF`/`DQT`/Exif differences. The ICC and the ISO APP2
  are byte-identical.

  Chroma subsampling looked like the favourite — Apple's stack has a known
  4:4:4 failure in this area, ImageIO's own writer segfaults inside
  VideoToolbox on a `444f` gain map — and it was wrong. `--chroma-subsample`
  and `--no-optimize` were both sent and both lost. The answer was `AMPF`,
  above.

  Two methodological lessons, five sends between them. Variants that change
  what is *in* the segments while leaving the layout alone cannot distinguish
  layout hypotheses, and a marker dump comparing survivor to casualty is one
  command. And **every card in a batch needs different pixels**: three variants
  built by byte surgery on one encode — the right way to isolate a variable —
  were deduplicated by Photos into a single asset, came back byte-identical
  with the same `IMG_` number, and voided the round. Distinct assets get
  distinct `IMG_` numbers, which is how to tell. Send a known-casualty control
  alongside, too, or a round where everything survives cannot be told from a
  round that did not happen.

* **A JPEG gain map survives re-encoding when the re-encoder chooses to carry
  it.** This entry used to say the opposite — that the map is a second image
  after the primary's EOI, so re-encoding the primary structurally cannot keep
  it. That is wrong, and an iPhone 17 camera JPEG through iMessage disproves
  it: primary 3,683,912 → 3,667,000 bytes, so plainly re-encoded, and it
  arrived with its gain map at 2.1892 EV, its MPF index, its ICC profile and
  its APP10 `AROT` curve. What it lost was the ISO 21496-1 payload.

  The same send flattens our files completely — every APP2 gone, ICC included,
  new Huffman and quantisation tables, restart intervals we never wrote, an
  added thumbnail. So there are two paths, and the question is what puts a file
  on the good one. Not the gain map's XMP: files differing only there both died.

  It is not a capability limit either. ImageIO *will* hand back the gain map's
  pixels from a JPEG source, ours as readily as Apple's — 480,000 bytes via
  `kCGImageAuxiliaryDataInfoData` — which this entry also used to deny.
  (`scripts/to_heic.swift` decodes the second image by hand anyway; that is
  belt and braces now, not a necessity.) A transcoder has everything it needs
  to re-attach our map and does not.

  HEIC carries the map as an item inside the container and comes through intact
  (2.3001 EV before and after, same as an iPhone's own file);
  `scripts/to_heic.swift` repackages one. An earlier version of that script
  concluded, wrongly, that HEIC could not carry a gain map at all.

  Two measurements are worth keeping because they are cheap to redo and both
  have already misled: whether a container survives, and whether ImageIO can
  read what is in it. They are different questions and answering one does not
  answer the other.

* **HEIC output is parked, deliberately, and the reason is the point of the
  project.** `iso21496_heic` builds on macOS and `scripts/to_heic.swift`
  repackages a file by hand; neither is wired into the export dialog and
  `scripts/package.sh` ships the binary only with `--with-heic`. Leave it that
  way.

  HEIC existed here for exactly one reason: it was the only container that
  survived an iMessage send. `AMPF` removed that reason — the JPEG survives now,
  measured, in both directions. What HEIC would add in its place is an
  Apple-only output from a plug-in whose whole purpose is one file that works
  everywhere. Android, Google Photos and Chrome read the gain map JPEG; they do
  not read this. Shipping both would mean asking the photographer which of their
  viewers matter, which is the question this project exists to make unnecessary.

  It also costs quality: every Apple-container route is single channel, 0.23 EV
  in the highlights and 0.57 EV of hue drift against the three-channel JPEG,
  because ImageIO's writer segfaults inside VideoToolbox on a `444f` auxiliary.

  Keep the code — it is measured, it works, and it is the fallback if Apple ever
  breaks the JPEG path again. Do not promote it to a shipping option without a
  reason as concrete as the one that has just gone away.

* **A three-channel gain map only fits in a JPEG, and only because we write
  the JPEG ourselves.** Handed a `444f` map, ImageIO's writer segfaults inside
  VideoToolbox (`vt_Copy_444v_Crop`) for HEIC *and* AVIF — it is the auxiliary
  image path, not the container. Every Apple-container route is therefore
  single channel, costing 0.23 EV in the highlights and 0.57 EV of hue drift
  against the three-channel JPEG. Getting three channels into HEIF would mean
  writing the container by hand and bringing our own HEVC or AV1 encoder, which
  the no-dependency rule rules out.

* **Apple has two gain map formats and they are not interchangeable.** ISO
  21496-1 (`kCGImageAuxiliaryDataTypeISOGainMap`) is the interoperable one; the
  older Apple format (`kCGImageAuxiliaryDataTypeHDRGainMap`,
  `urn:com:apple:photo:2020:aux:hdrgainmap`) is what an iPhone's own photos
  carry *alongside* it — an iPhone 17 camera JPEG has both, plus an APP10
  `AROT` curve, on the primary *and* on the gain map image. Reports of iMessage
  working hinge on the Apple form specifically, so a file carrying only the ISO
  one is not evidence that Apple's stack will handle it everywhere.

* **Apple has two conventions for describing the same gain map, and both are
  Apple's.** An iPhone 17 camera JPEG writes the three `apdi` fields and then
  the 2020 schema's own two:

      HDRGainMap:HDRGainMapVersion    131072      (0x20000; exiftool: "0.2.0.0")
      HDRGainMap:HDRGainMapHeadroom   4.560482    linear, not log2

  `HDRGainMapHeadroom` is the multiplier itself: the camera writes 4.560482 for
  a map ImageIO reports at a 4.5605 content headroom. `HDRToneMap:Alternate‑
  Headroom` beside it is in stops, so the two fields carry the same fact in
  different units and writing one into the other understates it by a stop or
  more.

  ImageIO's own JPEG writer does the opposite: `HDRToneMap` exactly as this
  encoder writes it, with no `HDRGainMap` fields at all. Check with
  `scripts/to_apple_jpeg.swift`, which rewrites one of our files through
  ImageIO. So neither schema is "the" Apple form and our file was never
  inconsistent for using `HDRToneMap` — an earlier note here said it was, on the
  strength of the camera file alone, before checking what Apple's own writer
  emits.

  We now write both. It is free — 0.0000 EV median *and* max drift through
  ImageIO's HDR decode against the same file without them — but it is **not**
  an iMessage fix, which is what it was added for. Two cards differing only in
  these fields were sent 27 → 26 together and both arrived at 0.00 EV.

  Do not conclude from a local ImageIO probe that a file is iMessage-safe.
  ImageIO reports "Apple HDR gain map: FOUND" for files with only the
  `HDRToneMap` description, and still does with every ISO APP2 segment stripped
  out — it reconstructs the view from the Adobe `hdrgm` XMP. The transcoder is
  stricter than the reader, so the probe cannot tell the two apart and only a
  real send can.

* **What ImageIO's JPEG writer lays out, when you want a reference file.**
  `JFIF, Exif, MPF, ISO APP2, ICC` on the primary — MPF *before* the ISO marker
  and the profile — restart intervals, no XMP on the primary at all (so no
  GContainer), and the gain map image typed `0x000000` (Undefined) in MPF
  rather than `0x050000`. The camera types it `0x000000` too. We write
  `0x050000`, which is what MPF asks for and what a reader trusting the index
  needs; Apple evidently never reads it.

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

`LrTasks.startAsyncTask` in the stub **queues** the function rather than running
it. It used to run it on the spot, which is a poor model and hid a real bug: the
export loop starts several encodes and names each output from what exists on
disk, and under a synchronous stub each task had already written its file before
the next was named, so the race could not happen and the test passed with the
bug in it. Queued tasks run when something waits — `sleep` or `yield` — or on
`stubs.drainTasks()`. Anything asserting on what a task produced has to drain
first; `testDialogsBuildOutsideATask` does.

Verify a new plug-in test by reintroducing the bug and watching it fail. Two
tests in this file have passed against broken code.

Syntax-check everything with `luac -p plugin/iso21496.lrdevplugin/*.lua`
(Homebrew installs Lua 5.5 as plain `luac` now; `luac5.4` may not exist).

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
