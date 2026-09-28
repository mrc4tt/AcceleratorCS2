#!/usr/bin/env bash
# Build the Linux packages inside the build images from docker/ (same as CI).
#
# Usage: scripts/build-linux.sh [steamrt3|steamrt4|all]   (default: all)
#
# Uses $HL2SDKCS2 / $MMSOURCE_DEV if set, otherwise clones hl2sdk (cs2) and metamod-source (master)
# into .deps/. Packages end up in dist/<runtime>/.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TARGET="${1:-all}"

case "$TARGET" in
	steamrt3|steamrt4) RUNTIMES=("$TARGET") ;;
	all) RUNTIMES=(steamrt3 steamrt4) ;;
	*) echo "usage: $0 [steamrt3|steamrt4|all]" >&2; exit 1 ;;
esac

SDK="${HL2SDKCS2:-$ROOT/.deps/hl2sdk-cs2}"
MM="${MMSOURCE_DEV:-$ROOT/.deps/mmsource-2.0}"

if [ ! -d "$SDK" ]; then
	git clone --depth 1 -b cs2 https://github.com/alliedmodders/hl2sdk "$SDK"
fi
if [ ! -d "$MM" ]; then
	git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/alliedmodders/metamod-source "$MM"
fi

if ! grep -qE 'define METAMOD_PLAPI_VERSION[[:space:]]+(1[89]|[2-9][0-9])' "$MM"/core/ISmmPlugin*.h 2>/dev/null; then
	echo "error: $MM is too old (METAMOD_PLAPI_VERSION < 18), current Metamod will refuse to load the plugin" >&2
	exit 1
fi

for rt in "${RUNTIMES[@]}"; do
	echo "==> Building $rt"
	# Cheap when nothing changed: docker reuses the cached layers.
	docker build -t "acceleratorcs2-build:$rt" -f "$ROOT/docker/$rt.Dockerfile" "$ROOT/docker"

	# Objects from the other runtime's toolchain must not be reused.
	rm -rf "$ROOT/build" "$ROOT/bin"

	docker run --rm -u "$(id -u):$(id -g)" \
		-v "$ROOT:/src" -v "$SDK:/deps/hl2sdk-cs2:ro" -v "$MM:/deps/mmsource-2.0:ro" -w /src \
		-e HL2SDKCS2=/deps/hl2sdk-cs2 -e MMSOURCE_DEV=/deps/mmsource-2.0 \
		"acceleratorcs2-build:$rt" \
		sh -c 'premake5 gmake && make -C build config=release_x64 -j"$(nproc)" && premake5 package'

	rm -rf "$ROOT/dist/$rt"
	mkdir -p "$ROOT/dist"
	cp -r "$ROOT/build/package/AcceleratorCS2" "$ROOT/dist/$rt"
	echo "==> $rt package: dist/$rt"
done
