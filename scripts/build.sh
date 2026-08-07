#!/usr/bin/env bash
# Builds the native encoder and drops it into the plug-in bundle.
#
#   scripts/build.sh [--debug] [--no-tests]
#
# On macOS this produces a universal arm64 + x86_64 binary, which is what the
# plug-in has to ship: Lightroom Classic runs natively on both.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_type=Release
run_tests=1

for arg in "$@"; do
	case "$arg" in
		--debug) build_type=Debug ;;
		--no-tests) run_tests=0 ;;
		*) echo "unknown option: $arg" >&2; exit 2 ;;
	esac
done

case "$(uname -s)" in
	Darwin) platform=macOS; binary_name=iso21496_encoder ;;
	Linux)  platform=linux; binary_name=iso21496_encoder ;;
	*)      echo "use scripts/build_windows.ps1 on Windows" >&2; exit 2 ;;
esac

cmake_args=(-S "$repo_root/encoder" -B "$repo_root/encoder/build"
            -DCMAKE_BUILD_TYPE="$build_type")
if [ "$platform" = macOS ]; then
	cmake_args+=(-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
	             -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0)
fi

cmake "${cmake_args[@]}"
cmake --build "$repo_root/encoder/build" --config "$build_type" --parallel

if [ "$run_tests" = 1 ]; then
	ctest --test-dir "$repo_root/encoder/build" --output-on-failure
fi

src="$repo_root/encoder/build/$binary_name"
dest_dir="$repo_root/plugin/iso21496.lrdevplugin/bin/$platform"
mkdir -p "$dest_dir"
cp "$src" "$dest_dir/$binary_name"
chmod +x "$dest_dir/$binary_name"

if [ "$platform" = macOS ]; then
	echo "architectures: $(lipo -archs "$dest_dir/$binary_name")"
fi

# Exercise the plug-in's Lua against the binary we just built, when a Lua
# interpreter is available. Lightroom bundles its own; this is only for CI.
lua_bin="$(command -v lua5.4 || command -v lua || true)"
if [ "$run_tests" = 1 ] && [ -n "$lua_bin" ]; then
	fixture="$(mktemp -d)/fixture.tif"
	"$repo_root/encoder/build/tests/make_fixture" "$fixture" 320 240 6.0
	"$lua_bin" "$repo_root/plugin/tests/test_plugin.lua" \
		"$dest_dir/$binary_name" "$(dirname "$fixture")" "$fixture"
fi

echo "built $dest_dir/$binary_name"
