#!/usr/bin/env bash
# Build all packages locally and publish them as a GitHub release.
#
# Usage: scripts/release.sh [--no-build] [--draft]
#
# The tag is v<version>, with the version taken from AcceleratorCS2::GetVersion() in extension.cpp,
# so bumping GetVersion() (and pushing that commit) is what makes a new release. Refuses to run if
# that release already exists, if the working tree has uncommitted changes, or if HEAD isn't pushed.
#   --no-build  reuse dist/steamrt3, dist/steamrt4 and dist/windows from an earlier build
#   --draft     create the release as a draft
# Needs: docker (via build-linux.sh / build-windows.sh), gh logged in with push rights, tar, zip.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD=1
DRAFT=()

for arg in "$@"; do
	case "$arg" in
		--no-build) BUILD=0 ;;
		--draft) DRAFT=(--draft) ;;
		*) echo "usage: $0 [--no-build] [--draft]" >&2; exit 1 ;;
	esac
done

cd "$ROOT"

command -v gh >/dev/null && gh auth status >/dev/null 2>&1 || { echo "error: install gh and run gh auth login" >&2; exit 1; }
command -v zip >/dev/null || { echo "error: zip is not installed" >&2; exit 1; }

VERSION="$(grep -A2 'AcceleratorCS2::GetVersion()' extension.cpp | sed -n 's/.*return "\(.*\)".*/\1/p')"
if [ -z "$VERSION" ]; then
	echo "error: could not read the version from AcceleratorCS2::GetVersion() in extension.cpp" >&2
	exit 1
fi
TAG="v$VERSION"
NAME="AcceleratorCS2-$TAG"

if gh release view "$TAG" >/dev/null 2>&1; then
	echo "error: release $TAG already exists, bump GetVersion() in extension.cpp first" >&2
	exit 1
fi

# The build applies patches to vendor/breakpad, so changes inside submodules are ignored.
if ! git diff --quiet --ignore-submodules=dirty HEAD; then
	echo "error: uncommitted changes, commit them first so the release matches a commit" >&2
	exit 1
fi

git fetch -q origin
COMMIT="$(git rev-parse HEAD)"
if ! git branch -r --contains "$COMMIT" | grep -q .; then
	echo "error: $COMMIT is not pushed, push it first" >&2
	exit 1
fi

if [ "$BUILD" = 1 ]; then
	"$ROOT/scripts/build-linux.sh" all
	"$ROOT/scripts/build-windows.sh"
fi

OUT="$ROOT/dist/release"
rm -rf "$OUT"
mkdir -p "$OUT"
for rt in steamrt3 steamrt4 windows; do
	[ -d "dist/$rt" ] || { echo "error: dist/$rt is missing, run without --no-build" >&2; exit 1; }
done
(cd dist/steamrt3 && tar -czf "$OUT/$NAME-steamrt3.tar.gz" *)
(cd dist/steamrt4 && tar -czf "$OUT/$NAME-steamrt4.tar.gz" *)
(cd dist/windows && zip -qr "$OUT/$NAME-windows.zip" *)
ls -la "$OUT"

echo "==> Publishing $TAG at $COMMIT"
gh release create "$TAG" "${DRAFT[@]}" \
	--target "$COMMIT" \
	--title "$TAG" \
	--notes "### Metamod \`2.0-1461\` or later is required!" \
	--generate-notes \
	"$OUT"/*.tar.gz "$OUT"/*.zip
