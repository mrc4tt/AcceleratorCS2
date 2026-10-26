#!/usr/bin/env bash
# Cross-compile the Windows package on Linux, inside the image from docker/windows.Dockerfile.
# Uses clang targeting x86_64-pc-windows-msvc + lld-link against an MSVC CRT / Windows SDK sysroot
# made by xwin (https://github.com/Jake-Shadle/xwin).
#
# Usage: scripts/build-windows.sh
#
# Sysroot: $XWIN_DIR (default ~/.xwin). If it doesn't exist and xwin is installed, it's created with
#   xwin --accept-license splat --output "$XWIN_DIR"
# Uses $HL2SDKCS2 / $MMSOURCE_DEV if set, otherwise clones mrc4tt/hl2sdk (cs2) and metamod-source (master)
# into .deps/. The package ends up in dist/windows/.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
XWIN="${XWIN_DIR:-$HOME/.xwin}"

if ! docker info >/dev/null 2>&1; then
	echo "error: $(whoami) cannot talk to docker. Add it to the docker group (sudo usermod -aG docker $(whoami))" >&2
	echo "       and restart the session." >&2
	exit 1
fi

if [ ! -d "$XWIN/crt" ] || [ ! -d "$XWIN/sdk" ]; then
	if ! command -v xwin >/dev/null; then
		echo "error: no MSVC sysroot in $XWIN. Install xwin (cargo install xwin) or set XWIN_DIR" >&2
		exit 1
	fi
	xwin --accept-license splat --output "$XWIN"
fi

SDK="${HL2SDKCS2:-$ROOT/.deps/hl2sdk-cs2}"
MM="${MMSOURCE_DEV:-$ROOT/.deps/mmsource-2.0}"

if [ ! -d "$SDK" ]; then
	git clone --depth 1 -b cs2 https://github.com/mrc4tt/hl2sdk "$SDK"
fi
if [ ! -d "$MM" ]; then
	git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/alliedmodders/metamod-source "$MM"
fi

if ! grep -qE 'define METAMOD_PLAPI_VERSION[[:space:]]+(1[89]|[2-9][0-9])' "$MM"/core/ISmmPlugin*.h 2>/dev/null; then
	echo "error: $MM is too old (METAMOD_PLAPI_VERSION < 18), current Metamod will refuse to load the plugin" >&2
	exit 1
fi

echo "==> Building windows"
docker build -t acceleratorcs2-build:windows -f "$ROOT/docker/windows.Dockerfile" "$ROOT/docker"

docker run --rm -u "$(id -u):$(id -g)" \
	-v "$ROOT:/src" -v "$SDK:/deps/hl2sdk-cs2:ro" -v "$MM:/deps/mmsource-2.0:ro" -v "$XWIN:/xwin:ro" -w /src \
	-e HL2SDKCS2=/deps/hl2sdk-cs2 -e MMSOURCE_DEV=/deps/mmsource-2.0 \
	acceleratorcs2-build:windows \
	sh scripts/container-build.sh windows

rm -rf "$ROOT/dist/windows"
mkdir -p "$ROOT/dist"
cp -r "$ROOT/build/package/AcceleratorCS2" "$ROOT/dist/windows"
echo "==> windows package: dist/windows"
