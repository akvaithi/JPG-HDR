#!/usr/bin/env bash
# Assembles the distributable plug-in bundle in dist/.
#
#   scripts/package.sh [--mac-binary PATH] [--windows-binary PATH] [--with-heic]
#
# Both platform binaries have to be built before packaging: Lightroom Classic
# plug-ins are cross-platform bundles, and a photographer on Windows must get
# the same .lrdevplugin as one on macOS.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bundle_src="$repo_root/plugin/iso21496.lrdevplugin"
dist="$repo_root/dist"
mac_binary=""
win_binary=""
allow_missing=0
# The HEIC repackager is opt in, and parked rather than pending. It existed
# because HEIC was the only container that survived an iMessage send; the AMPF
# marker fixed the JPEG, so what it would add now is an Apple-only output from a
# plug-in built to produce one file that works everywhere. See CLAUDE.md.
with_heic=0

while [ $# -gt 0 ]; do
	case "$1" in
		--mac-binary) mac_binary="$2"; shift 2 ;;
		--windows-binary) win_binary="$2"; shift 2 ;;
		--allow-missing) allow_missing=1; shift ;;
		--with-heic) with_heic=1; shift ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done

rm -rf "$dist"
mkdir -p "$dist"
cp -R "$bundle_src" "$dist/"
bundle="$dist/iso21496.lrdevplugin"

[ -n "$mac_binary" ] && install -m 0755 "$mac_binary" "$bundle/bin/macOS/iso21496_encoder"
# The HEIC repackager, opt in with --with-heic. It sits beside the encoder in
# the same build tree. Nothing in the plug-in calls it yet, so a bundle without
# it behaves identically; this exists so that shipping it is a decision rather
# than a side effect of having built it.
if [ "$with_heic" = 1 ] && [ -n "$mac_binary" ] &&
   [ -x "$(dirname "$mac_binary")/iso21496_heic" ]; then
	install -m 0755 "$(dirname "$mac_binary")/iso21496_heic" \
		"$bundle/bin/macOS/iso21496_heic"
else
	rm -f "$bundle/bin/macOS/iso21496_heic"
fi
[ -n "$win_binary" ] && install -m 0755 "$win_binary" "$bundle/bin/windows/iso21496_encoder.exe"

# A Linux binary is useful for CI but must not ship to photographers.
rm -rf "$bundle/bin/linux"
find "$bundle" -name '.DS_Store' -delete

missing=0
for required in bin/macOS/iso21496_encoder bin/windows/iso21496_encoder.exe; do
	if [ ! -f "$bundle/$required" ]; then
		echo "warning: $required is missing from the bundle" >&2
		missing=1
	fi
done
if [ "$missing" = 1 ] && [ "$allow_missing" = 0 ]; then
	echo "error: refusing to package an incomplete bundle (pass --allow-missing to override)" >&2
	exit 1
fi

cp "$repo_root/README.md" "$bundle/README.md"
mkdir -p "$bundle/docs"
cp "$repo_root/docs/INSTALL.md" "$bundle/docs/INSTALL.md"

( cd "$dist" && zip -qr iso21496-lightroom-plugin.zip iso21496.lrdevplugin )
echo "packaged $dist/iso21496-lightroom-plugin.zip"
