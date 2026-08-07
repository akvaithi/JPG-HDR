# Installing and using the plug-in

## Requirements

* Adobe Lightroom Classic 13.0 or later (the plug-in declares
  `LrSdkMinimumVersion = 13.0`).
* macOS 11 or later, or Windows 10/11 x64.
* Nothing else. The encoder is a single static binary — no Python, no
  Homebrew, no Visual C++ redistributable.

## Getting a build

Either download `iso21496-lightroom-plugin.zip` from the project's CI
artifacts, or build it yourself:

```bash
# macOS: produces a universal arm64 + x86_64 binary
./scripts/build.sh

# Windows, from a Developer PowerShell
pwsh scripts/build_windows.ps1

# Combine both platform binaries into one bundle
./scripts/package.sh \
    --mac-binary   plugin/iso21496.lrdevplugin/bin/macOS/iso21496_encoder \
    --windows-binary plugin/iso21496.lrdevplugin/bin/windows/iso21496_encoder.exe
```

A Lightroom plug-in is cross-platform: one bundle carries both binaries, and
`package.sh` refuses to build an incomplete one unless you pass
`--allow-missing`.

## Installing

1. Unzip the bundle somewhere permanent — Lightroom loads it from wherever it
   sits, so not the Downloads folder.
2. **File ▸ Plug-in Manager ▸ Add**, and select `iso21496.lrdevplugin`.
3. The plug-in's panel reports the encoder version. If it says the encoder is
   missing or would not run, fix that before exporting — see Troubleshooting.

### macOS Gatekeeper

An unsigned binary downloaded from the internet carries a quarantine flag, and
Lightroom's attempt to run it fails silently. Clear it once:

```bash
xattr -dr com.apple.quarantine /path/to/iso21496.lrdevplugin
```

If you distribute the plug-in, sign and notarise the binary instead; then this
step disappears for your users.

## Exporting

Two ways in, both driving the same encoder.

### The export destination (recommended)

**File ▸ Export**, then choose **ISO 21496-1 HDR JPEG** in the *Export To* menu
at the top. Two extra sections appear at the bottom of the dialog: the main
settings, and *ISO 21496-1 Advanced*.

Because the plug-in owns the whole export, the files that land in your
destination folder are named `.jpg`, and the intermediate TIFF is deleted for
you. File Settings is fixed to a 16-bit ProPhoto TIFF; that is the intermediate,
not the output.

### The export filter

**File ▸ Export**, then in the *Post-Process Actions* list on the left add
**Encode as ISO 21496-1 HDR JPEG**. Use this when the encoder has to slot into
an existing preset alongside other filters.

One caveat: an export filter cannot change the file extension Lightroom derived
from the export format, and the plug-in forces that format to TIFF to get HDR
data out of Lightroom. The filter therefore renames each finished file from
`.tif` to `.jpg` once the export completes. If you tick *Add to This Catalog*,
the rename is skipped — otherwise the catalogue would point at a path that no
longer exists — and your files keep `.tif` names while containing ISO 21496-1
JPEGs. Use the export destination instead if that matters, and let its own
*Add the exported JPEGs to this catalogue* checkbox do the import.

## Settings

### Target HDR headroom

How far above SDR white the gain map is allowed to push, in stops:

| Setting | Peak (at a 100-nit SDR reference) |
|---|---|
| +1.0 EV | ~160 nits |
| +2.0 EV | ~320 nits |
| +3.0 EV | ~640 nits |
| +4.0 EV | ~1280 nits (default) |

This is a ceiling, not a target. With *Fit the gain map range to the image* on
(the default) the encoder measures how much headroom the photo actually uses,
stores that as the gain map's maximum boost, and declares it as the file's
required headroom.

That last part matters more than it sounds. A decoder applies the gain scaled
by how much headroom the display has relative to what the file asks for. A
photo that needs 1.6 stops but claims to need 4 would render at half strength
on a display with 2 stops of headroom — plenty for that photo — so the file
asks for what it measured.

### Base colour space

The colour space of the SDR image everything else sees. **Display P3** is the
recommended default: wide enough for modern displays, and what Apple and
Android write. **sRGB** for maximum compatibility with old software, **Rec.
2020** when the rest of your pipeline is.

### Gain map resolution and channels

A monochrome gain map at 1:2 is the default because it is nearly free: on a
45 MP frame it adds about 12% to the file size, where a full-resolution
three-channel map adds around 45%. Choose **RGB** only if your highlights change
hue as they brighten — coloured neon, stained glass, sunsets through smoke.

### Baseline JPEG quality

Standard JPEG quality for the SDR image, 60 to 100. The gain map has its own
quality setting in the Advanced section (default 50); it is a smooth control
signal, so it tolerates far more compression than the picture does.

### Advanced

* **Tone mapping** — how HDR highlights are rolled into the SDR base.
  *Reinhard* (default) keeps shadows and midtones untouched and compresses only
  the top end. *Filmic* rolls off earlier for a flatter look. *Hard clip*
  simply clips above white, which maximises SDR contrast and puts all the HDR
  information in the gain map.
* **SDR base look** — *Brightness lift* (0.43 EV) and *Contrast* (1.14) shape
  the SDR base, i.e. what everything without an HDR display shows. A plain tone
  curve lands about a quarter stop dark and flat compared to a hand-graded SDR
  edit; these correct for that. Neither changes the HDR rendition — the gain
  map is measured against whatever base they produce. Press *Neutral* for an
  unshaped base. These defaults are borrowed from a reference implementation
  and were tuned on one photo and one photographer's taste, so treat them as a
  starting point rather than a truth.
* **Highlight measurement** — *Softened* (the default) averages the image down
  before measuring its peak, so a few hot pixels or a speck of sensor noise
  cannot define the headroom for the whole frame. *Exact peak* uses the true
  per-pixel maximum; use it if you have a genuinely tiny highlight that matters
  and you would rather spend gain map precision on it.
* **Gain map quality** — 50 by default. A gain map is a smooth luminance ratio,
  so JPEG artefacts in it are not visually significant the way they are in the
  picture; 50 is what current camera pipelines write and costs roughly a third
  the bytes of 85.
* **Gain map gamma** — the encoding curve for the stored gain values, 2.2 by
  default per the build spec. Decoders read this from the metadata.
* **Rendered encoding / primaries** — normally *Detect from the rendered file*,
  which reads the ICC profile Lightroom embedded. Override these if you have
  configured Lightroom to render something else, for example Rec. 2100 PQ.
* **Keep the intermediate TIFF** — leaves the render on disk next to the JPEG.
  Useful when reporting a problem.

## Troubleshooting

**"The ISO 21496-1 encoder is missing from the plug-in"** — the bundle was
assembled without a binary for your platform. Rebuild with `scripts/build.sh`
or `scripts/build_windows.ps1`, or re-download a complete bundle.

**The Plug-in Manager says the encoder would not run** — on macOS this is
almost always quarantine (see above). On Windows, check that antivirus software
has not quarantined `bin/windows/iso21496_encoder.exe`.

**Exports fail with a message about compression** — something changed the
export's TIFF compression away from None. The plug-in sets this itself, so this
usually means another post-process filter is interfering; move the ISO 21496-1
filter to the end of the chain.

**The HDR effect is missing on an HDR display** — check that the photo actually
has highlights above SDR white. Run the encoder with `--json`: if
`measuredHeadroom` is near zero, the render had no HDR data in it, which points
at Lightroom's HDR output setting rather than at the plug-in.

**Diagnostics** — turn on *Write a diagnostic log to the Documents folder* in
the Plug-in Manager. The log records every encoder command line and its result,
which is the fastest way to see what actually ran.
