# Platform binaries

The native encoder lives here, one build per platform:

```
bin/macOS/iso21496_encoder        universal arm64 + x86_64
bin/windows/iso21496_encoder.exe  x64
```

They are build outputs, not sources, so they are not committed. Produce them
with `scripts/build.sh` (macOS) and `scripts/build_windows.ps1` (Windows), or
take them from a CI build; `scripts/package.sh` assembles a bundle containing
both.

A bundle missing the binary for the platform it is installed on will load, and
the Plug-in Manager panel will say so, but exports will fail.
