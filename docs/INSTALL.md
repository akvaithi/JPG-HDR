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

### HDR headroom

How far above SDR white the gain map may push. **Match the render** is the
default and the right answer almost always: the encoder measures how much
headroom the photo actually uses, stores that as the gain map's maximum boost,
and declares it as the file's required headroom. That figure is the one
Lightroom shows for the photo, because both are reading the same render.

The alternative is a cap, in stops:

| Cap | Peak (at a 100-nit SDR reference) |
|---|---|
| +1.0 EV | ~160 nits |
| +2.0 EV | ~320 nits |
| +3.0 EV | ~640 nits |
| +4.0 EV | ~1280 nits |

Reach for one only to hold a set down deliberately. A decoder applies the gain
scaled by how much headroom the display has relative to what the file asks for,
so a photo that needs 1.6 stops but claims 4 renders at half strength on a
display with 2 stops of headroom — plenty for that photo. Declaring more than
you need costs brightness everywhere.

### Depth

How much local contrast the base image keeps. The encoder splits the picture
into a smooth base and the detail riding on it, compresses only the base, and
hands the detail back at this strength. 1.0 reproduces what the render had;
**1.25** is the default and puts more tonal separation into the base than a
plain SDR export carries.

This matters beyond the SDR fallback. Any display without the file's full
headroom shows a blend anchored on the base image, so *Depth* is visible on most
HDR screens too — measured against Lightroom at half a stop of display headroom,
it is worth 12.6% more highlight separation.

### Baseline JPEG quality

Standard JPEG quality for the SDR image, 60 to 100, and the file size control.
The gain map is always written at full precision — full resolution, three
channels, 4:4:4 — because every way of shrinking it costs more colour accuracy
than the bytes it saves. Measured on a 24 MP frame: quality 70 gives 8.0 MB,
80 gives 9.2 MB, 90 gives 11.8 MB, and the colour error moves from 0.063 EV to
0.051 EV across that whole range. Above 90 costs bytes and measures the same.

### Advanced

* **Base colour space** — the colour space of the SDR image everything else
  sees. **Display P3** is the recommended default: wide enough for modern
  displays, and what Apple and Android write. **sRGB** for maximum compatibility
  with old software, **Rec. 2020** when the rest of your pipeline is.
* **Rendered encoding / primaries** — normally *Detect from the rendered file*,
  which reads the ICC profile Lightroom embedded. Override these if you have
  configured Lightroom to render something else, for example Rec. 2100 PQ.
* **PQ/HLG diffuse white** — the nits that map to SDR white when the render is
  PQ or HLG, which is what Lightroom writes for an HDR export. 203 is the
  standard reference and should not need changing.
* **Copy Exif, GPS and copyright from the render**, **Add the exported JPEGs to
  this catalogue**, and **Keep the intermediate TIFF** — housekeeping. The last
  leaves the render on disk next to the JPEG, which is what to send if you are
  reporting a problem.

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
