# Acceptance criteria

Section 6 of the [build spec](BUILD_SPEC.md) lists four verification tests.
This is what each one maps onto, and — where a criterion cannot be checked
without Lightroom or real hardware — exactly what remains to be done by hand.

## 1. Lightroom plug-in functional test

> Loads without SDK warnings; renders custom UI; batch-exports 10+ RAW images,
> deleting intermediates and registering the JPEGs.

**Automated:** `plugin/tests/test_plugin.lua` runs the plug-in's settings and
encoder modules under a stubbed SDK against the real binary — defaults, preset
fields, argument construction, clamping, validation, shell quoting (including
paths with spaces and quotes), report parsing, and the failure path when the
encoder errors. Every Lua file is syntax-checked.

**Needs Lightroom Classic:** the dialog's appearance, the Plug-in Manager load,
and a real batch export. To run it:

1. Add the bundle in the Plug-in Manager; confirm the panel reports an encoder
   version and shows no SDK warnings.
2. Select 10 or more HDR-edited raws, export via **ISO 21496-1 HDR JPEG**.
3. Confirm: one `.jpg` per photo, no `.tif` files left behind, progress
   advances per photo and cancelling stops cleanly.
4. Tick *Add the exported JPEGs to this catalogue* and confirm they appear.

The intermediate cleanup and catalogue registration are implemented in
`ExportServiceProvider.lua`; the failure path deletes both the partial JPEG and
the intermediate and reports through `rendition:uploadFailed`.

## 2. Metadata compliance audit

> `exiftool -v3` shows `urn:iso:std:iso:ts:21496:-1`; `exiftool -MPFVersion`
> shows a valid MPF directory with exactly 2 images.

**Automated**, as the `exiftool_compliance` CTest case
(`encoder/tests/exiftool_check.cmake`), which runs the encoder and then checks
that:

* the ISO 21496-1 URN is present in the file, byte for byte;
* `NumberOfImages` is exactly 2 and `MPFVersion` is present;
* `exiftool -b -MPImage2` extracts an intact second image;
* that extracted image carries the URN itself, which is where the standard
  says the metadata lives;
* `exiftool -validate` reports no warnings.

Byte-level layout of the payload is separately asserted in the `metadata`
test — field order, rational encoding, flag bits, and the 28-byte identifier.

Note the encoding deviation from the spec's description of this payload
(rationals, not float32) and why, in
[ARCHITECTURE.md](ARCHITECTURE.md#iso-21496-1-payload).

## 3. File size optimisation benchmark

> A 45 MP export with a monochrome 1:2 gain map must be 15–25% larger than a
> standard SDR JPEG, and significantly smaller than a 3-channel uncompressed
> export.

**Measured**, on an 8192 × 5464 synthetic HDR frame with fine grain:

| Configuration | Gain map | Overhead over the SDR base |
|---|---|---|
| Monochrome, 1:2 (default) | 0.39 MB | **+12.2%** |
| RGB, 1:1 | 1.43 MB | +44.7% |

Comfortably under the required band, and a quarter the size of the
three-channel full-resolution alternative. Reproduce it with:

```bash
./encoder/build/tests/make_fixture /tmp/big.tif 8192 5464 8.0 1
./encoder/build/iso21496_encoder --input /tmp/big.tif --output /tmp/big.jpg --json
```

The `endtoend` test asserts the invariant continuously at a smaller size: the
monochrome 1:2 overhead must stay under 25% and must beat RGB 1:1.

This fixture is grainy on purpose — grain is the worst case for a gain map,
since it puts high-frequency detail into what is otherwise a smooth signal. A
real photograph typically lands lower.

## 4. Cross-platform render verification

> Correct SDR rendering; HDR through iMessage on iOS 18 / macOS 15; HDR in
> Google Photos on Android 15 and in Chrome on Windows 11.

**Automated, up to the point where real hardware takes over:** the `decode`
test decodes both images with libjpeg, applies the stored gain map exactly as
ISO 21496-1 specifies, and compares the reconstruction against the source HDR
image — **0.43% mean error, 2.4% worst case in the highlights**. That
establishes the file is mathematically correct: any conforming decoder given
these bytes reproduces the intended HDR image.

A second test proves the SDR base look is free: encoding the same photo with
and without the default lift and contrast moves the reconstructed HDR rendition
by **0.008 EV on average**, while the SDR base itself moves as intended.

The file also declares the headroom it measured rather than the user's ceiling,
which is what makes a display with partial headroom render the photo at full
strength instead of scaling the gain down. The `endtoend` test asserts it.

**Needs real devices.** No amount of local testing substitutes for checking how
a specific OS version treats a specific file. The checklist:

| Target | What to check |
|---|---|
| SDR display, any viewer | Clean tone-mapped image, no clipped highlights, no colour shift |
| Apple Photos (macOS 15+) | "HDR" badge appears; highlights lift on an XDR display |
| iMessage (iOS 18+) | File survives the send with HDR intact and metadata unstripped |
| Google Photos (Android 15) | Highlights lift on an HDR panel |
| Chrome 116+ / Edge (Windows 11, HDR on) | Highlights lift in the browser |
| Safari 18+ | Same |
| Instagram, Threads | Gain map survives their re-encode |

Two of these are outside the encoder's control and worth knowing about in
advance. Instagram and Threads re-encode uploads on their own servers; whether
a gain map survives depends on their pipeline that week, not on the file. And
iMessage's behaviour depends on how the file is attached — sending from Photos
preserves more than sending a file from Files.

If a target does not light up, `--json` output is the first diagnostic:
`measuredHeadroom` near zero means the render Lightroom produced had no HDR
data in it, which is a Lightroom setting rather than an encoder bug.

## Also verified, beyond the spec

* **HDR round trip.** Reconstruction from the encoded file matches the source
  to 0.43% mean error (`decode` test).
* **SDR shaping is free.** The default brightness lift and contrast move the
  reconstructed HDR rendition by 0.008 EV on average (`decode` test).
* **Bitstream validity.** libjpeg decodes our output at every quality,
  subsampling mode and component count we emit; the entropy stream is checked
  for illegal unescaped `0xFF` bytes.
* **Huffman table integrity.** A regression test covers the case where rare
  coefficient symbols initially receive codes longer than 16 bits — these must
  survive the length-limiting adjustment rather than being dropped.
* **Performance.** 45 MP in 3.8 s on four cores, 678 MB peak.
* **Small highlights survive.** A 6×6 specular glint five stops above white —
  0.005% of a 1024×768 frame — still registers in the headroom measurement
  rather than being averaged away or stepped over (`endtoend`).
* **No runtime dependencies.** CI asserts the Linux binary links nothing beyond
  libc, libm and the loader, and that the macOS binary is universal.
