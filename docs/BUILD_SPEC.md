# Product Specification & Build Document: Custom Lightroom Classic ISO 21496-1 Gain Map HDR Export Plugin

## 1. Executive Summary & Purpose

### 1.1 Objective
Build a complete software package comprising an Adobe Lightroom Classic (Lrc) export plugin (`.lrdevplugin`) and a bundled native command-line interface (CLI) binary tool (`iso21496_encoder`). The system will allow photographers to export High Dynamic Range (HDR) still photographs in the standardized **ISO 21496-1:2025 Gain Map JPEG** format directly from Lightroom Classic.

### 1.2 Target Experience
The exported files must mimic the file-size efficiency and highlight performance of an **iPhone HDR HEIC photograph**, but packaged inside a **universal JPEG container** containing ISO 21496-1 binary metadata. The resulting files must:
- Display as standard 8-bit SDR JPEGs on legacy screens, older operating systems, and non-HDR web apps.
- Illuminate peak HDR highlights (+1.0 to +4.0 EV / up to 1,280+ nits) on HDR-capable hardware.
- Transfer natively in full HDR over Apple iMessage (iOS 18+ / macOS 15+ Sequoia) without being converted or stripped.
- Render in full HDR across Android 15+ (Google Photos), Google Chrome (v116+), Microsoft Edge, Apple Safari (v18+), Instagram, and Threads.

---

## 2. System Architecture & Component Breakdown

The architecture comprises two distinct software layers that communicate via a post-processing render task bridge:

```
+-----------------------------------------------------------------------------------+
|                        Component 1: Lightroom Classic Plugin                      |
|                                                                                   |
|  - Plugin Manifest (`manifest.lua`)                                               |
|  - Export User Interface Dialog (`ExportDialogSections.lua`)                       |
|  - Post-Processing Task Manager (`ExportFilterProvider.lua`)                      |
+-----------------------------------------------------------------------------------+
                                         |
                       (Intermediate 16-bit Linear TIFF)
                                         v
+-----------------------------------------------------------------------------------+
|                     Component 2: Native CLI Encoder Binary                        |
|                                                                                   |
|  - Ingests intermediate linear HDR TIFF from Lightroom                            |
|  - Generates baseline SDR JPEG & 1-channel / 1:2 subsampled Gain Map JPEG          |
|  - Writes ISO 21496-1 APP2 metadata payload & MPF container structures            |
|  - Outputs finalized ISO 21496-1 JPEG                                             |
+-----------------------------------------------------------------------------------+
```

### 2.1 Component 1: Lightroom Classic Lua Plugin (`.lrdevplugin`)
- **Responsibility**: Provides UI integration within Lightroom's Export Dialog, captures user export parameters, configures Lightroom to render an uncompressed linear HDR intermediate file, invokes the CLI binary via system shell calls, handles progress UI, cleans up temporary files, and registers final JPEGs with the catalog export session.
- **Sub-module 1.1 (`manifest.lua`)**: Registers the plugin identifier (`com.custom.lightroom.export.iso21496`), title, minimum Lrc SDK version (v13.0+), export filter provider, and dialog section scripts.
- **Sub-module 1.2 (`ExportDialogSections.lua`)**: Renders custom UI controls inside Lightroom's Export Dialog panel.
- **Sub-module 1.3 (`ExportFilterProvider.lua`)**: Implements `postProcessRenderedPhotos` to manage the lifecycle of rendered renditions, construct CLI command execution strings (`LrTasks.execute`), report batch progress (`LrProgressScope`), and handle error states.

### 2.2 Component 2: Native CLI Encoder Binary (`iso21496_encoder`)
- **Responsibility**: Performs heavy pixel processing, tone mapping, logarithmic gain array derivation, quantization, ISO metadata binary encoding, and JPEG MPF container stitching.
- **Execution Interface**: Invoked by the Lua plugin via command-line arguments. Must be self-contained and bundled as pre-compiled platform binaries inside the plugin directory (`bin/macOS/iso21496_encoder` and `bin/windows/iso21496_encoder.exe`).

---

## 3. Functional Requirements & Parameter Specifications

### 3.1 User Interface & Parameter Specs (Export Dialog)
The plugin UI must expose the following configurable parameters to the user:

1. **Target HDR Headroom** (`iso_target_headroom`):
   - *Options*: `1.0` (+1.0 EV / 160 nits), `2.0` (+2.0 EV / 320 nits), `3.0` (+3.0 EV / 640 nits), `4.0` (+4.0 EV / 1280 nits - Default).
   - *Function*: Sets the maximum logarithmic boost stored in the ISO 21496-1 metadata payload (`AlternateHeadroom` and `MaxContentBoost`).
2. **Base Color Space** (`iso_color_space`):
   - *Options*: `DisplayP3` (Default / Recommended), `sRGB` (Legacy Compatibility), `Rec2020` (Wide Gamut).
   - *Function*: Defines the target color profile for the primary SDR baseline JPEG image.
3. **Gain Map Resolution Subsampling** (`iso_gainmap_subsample`):
   - *Options*: `1` (Full resolution / 1:1 scale), `2` (Half resolution / 1:2 scale - Default), `4` (Quarter resolution / 1:4 scale).
   - *Function*: Downsamples the secondary gain map image array to minimize file size.
4. **Gain Map Channel Configuration** (`iso_gainmap_channels`):
   - *Options*: `Monochrome` (Single-channel achromatic luminance map - Default), `RGB` (3-channel full color gain map).
   - *Function*: Controls whether the gain map image encodes 1 channel or 3 color channels.
5. **Baseline JPEG Quality** (`iso_jpeg_quality`):
   - *Options*: Slider from `60` to `100` (Default: `90`).
   - *Function*: Sets the JPEG compression quality factor for the primary SDR baseline image.

---

## 4. Technical Limitations & SDK Constraints

1. **Lightroom Classic SDK Limitation**:
   - *Constraint*: Lightroom Classic's native Lua SDK (`LrExportSettings`) does **not** expose API options for programmatically controlling gain map channel counts, custom subsampling factors, or custom ISO 21496-1 APP2 binary metadata fields.
   - *Requirement*: The plugin MUST NOT rely on Lrc's native gain map writer. Instead, it MUST use a **Post-Processing Render Filter Bridge** pattern where Lrc exports an intermediate uncompressed 16-bit ProPhoto TIFF containing full linear HDR data, and the native CLI binary processes this TIFF into the final ISO 21496-1 JPEG.
2. **Platform & Binary Dependencies**:
   - *Constraint*: Lightroom plugins run on both macOS and Windows.
   - *Requirement*: The CLI tool must be compiled natively for both macOS (ARM64 Universal / x86_64) and Windows (x64) and included in the plugin package. It must operate without requiring external environment installations (e.g. Python or Homebrew) on the end-user's machine.
3. **Memory & Performance Constraints**:
   - *Constraint*: Photographers frequently batch-export dozens of high-resolution RAW images (45MP–60MP+).
   - *Requirement*: The CLI tool must process image buffers efficiently without memory leaks, utilizing multi-threaded SIMD / OpenMP CPU or GPU acceleration where available.

---

## 5. Standards Compliance & Output Specification

The final output file produced by the system MUST strictly adhere to the following specifications:

### 5.1 Container & Header Layout
- **Container**: Standard JPEG (`.jpg`) format structured as a CIPA DC-007 Multi-Picture Format (MPF) container.
- **Primary Baseline Image**: Standard 8-bit JPEG image in sRGB or Display P3 color space.
- **Secondary Image**: Appended 8-bit or 10-bit JPEG gain map image (subsampled to 1:2 scale and encoded as 1-channel achromatic monochrome by default).

### 5.2 ISO 21496-1 APP2 Binary Metadata Payload
The secondary gain map image's JPEG header must contain an `APP2` marker segment starting with the 28-byte URN string `urn:iso:std:iso:ts:21496:-1\0`. The binary payload following the URN string must encode:
- **Minimum Version**: `0` (uint16)
- **Writer Version**: `1` (uint16)
- **Flags**: Bitfield specifying single-channel vs. 3-channel gain map (uint8)
- **Base Headroom**: `0.0` (float32 log2 scale)
- **Alternate Headroom**: User-selected target value (e.g., `4.0` float32 log2 scale)
- **Gain Map Min Boost**: `0.0` (float32)
- **Gain Map Max Boost**: Target headroom limit (float32)
- **Gamma**: `2.2` (float32)
- **Offset SDR / HDR**: Small logarithmic offset constant (e.g., `0.01` float32)

---

## 6. Acceptance Criteria & Validation Benchmarks

The completed plugin package must satisfy the following verification tests before release:

1. **Lightroom Plugin Functional Test**:
   - Successfully loads into Lightroom Classic's Plugin Manager without SDK warnings.
   - Renders custom UI controls in the Export Dialog and correctly captures user settings.
   - Executes batch exports of 10+ RAW images concurrently, cleanly deleting intermediate TIFF files and registering final `.jpg` files in Lightroom.
2. **Metadata Compliance Audit**:
   - Running `exiftool -v3 <file.jpg>` confirms the presence of `urn:iso:std:iso:ts:21496:-1`.
   - Running `exiftool -MPFVersion <file.jpg>` confirms a valid MPF directory with exactly 2 images (Primary Base + Secondary Gain Map).
3. **File Size Optimization Benchmark**:
   - An exported 45MP ISO 21496-1 JPEG with monochrome 1:2 gain map subsampling must be within **15% to 25%** larger than a standard SDR JPEG (and significantly smaller than Lightroom's native 3-channel uncompressed export).
4. **Cross-Platform Render Verification**:
   - **SDR Displays**: Renders as a clean, properly tone-mapped SDR image with zero highlight clipping or color distortion.
   - **iMessage (iOS 18+ / macOS 15+ Sequoia)**: Transmits via iMessage without metadata stripping; displays the "HDR" badge in Apple Photos and illuminates specular highlights on XDR displays.
   - **Android 15 & Chrome (Windows 11 HDR)**: Opens in Google Photos and Chrome with full HDR highlight rendering.


