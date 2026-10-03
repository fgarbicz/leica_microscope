#!/usr/bin/env bash
# Builds the release, runs the tests and makes the macOS disk image
# dist/DMImaging-<version>-macOS-<arch>.dmg: the app and a shortcut to
# Applications, to drag it onto (tools/make_installer.ps1 is the Windows
# counterpart, tools/make_appimage.sh the Linux one).
#
#   tools/make_dmg.sh               build, test, package
#   tools/make_dmg.sh --skip-build  package the existing build/release/deploy bundle
#
# The app carries the Homebrew Qt and libusb it was built with, so it runs on the
# macOS version those were built for, or newer (docs/DEPLOYMENT.md). It is signed
# ad hoc: on another Mac, open it the first time with right-click > Open, or sign
# and notarise it with a Developer ID (also in docs/DEPLOYMENT.md).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ "$(uname -s)" == "Darwin" ]] || { echo "make_dmg.sh runs on macOS" >&2; exit 1; }

skip_build=0
[[ "${1:-}" == "--skip-build" ]] && skip_build=1

version="$(sed -n 's/^project(DMImaging VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")"
[[ -n "$version" ]] || { echo "version not found in CMakeLists.txt" >&2; exit 1; }
arch="$(uname -m)"
echo "DM Imaging $version ($arch)"

if [[ $skip_build -eq 0 ]]; then
    "$root/build.sh" --release --test --deploy
fi
app="$root/build/release/deploy/DM Imaging.app"
[[ -d "$app" ]] || { echo "$app not found - build first (build.sh --deploy)" >&2; exit 1; }

stage="$root/build/package/dmg"
rm -rf "$stage"
mkdir -p "$stage"
cp -R "$app" "$stage/"
ln -s /Applications "$stage/Applications"

mkdir -p "$root/dist"
dmg="$root/dist/DMImaging-$version-macOS-$arch.dmg"
rm -f "$dmg"
hdiutil create -volname "DM Imaging $version" -srcfolder "$stage" -fs HFS+ -format UDZO -ov "$dmg" >/dev/null
hdiutil verify "$dmg" >/dev/null
printf 'Disk image: %s (%s)\n' "$dmg" "$(du -h "$dmg" | cut -f1)"
