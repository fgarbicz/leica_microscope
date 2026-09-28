#!/usr/bin/env bash
# Uploads the installers in dist/ to a GitHub release under fixed names, which
# are what the README's Download links point at
# (releases/latest/download/<name>): those links then always give the newest
# version without editing the README.
#
#   tools/publish_release.sh v1.1.3
#
# Uploads whichever of these exist for the version in CMakeLists.txt (made with
# tools/make_installer.ps1, tools/make_dmg.sh and tools/make_appimage.sh):
#   dist/DMImaging-Setup-<version>.exe           -> DMImaging-Setup-Windows-x64.exe
#   dist/DMImaging-<version>-macOS-arm64.dmg     -> DMImaging-macOS-arm64.dmg
#   dist/DMImaging-<version>-x86_64.AppImage     -> DMImaging-Linux-x86_64.AppImage
# An asset of the same name already on the release is replaced. Needs the GitHub
# CLI (gh), signed in.
set -euo pipefail

tag="${1:?usage: tools/publish_release.sh <tag>, e.g. v1.1.3}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="$(sed -n 's/^project(DMImaging VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")"
[[ "$tag" == "v$version" ]] || { echo "tag $tag does not match version $version in CMakeLists.txt" >&2; exit 1; }
gh release view "$tag" >/dev/null || { echo "no release $tag on GitHub (create it first)" >&2; exit 1; }

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
uploads=()
add() { # <file in dist> <fixed name>
    if [[ -f "$root/dist/$1" ]]; then
        cp "$root/dist/$1" "$stage/$2"
        uploads+=("$stage/$2")
        echo "  $1 -> $2"
    else
        echo "  (no dist/$1)"
    fi
}
echo "DM Imaging $version -> $tag"
add "DMImaging-Setup-$version.exe" "DMImaging-Setup-Windows-x64.exe"
add "DMImaging-$version-macOS-arm64.dmg" "DMImaging-macOS-arm64.dmg"
add "DMImaging-$version-x86_64.AppImage" "DMImaging-Linux-x86_64.AppImage"
[[ ${#uploads[@]} -gt 0 ]] || { echo "nothing to upload - build the installers first" >&2; exit 1; }
gh release upload "$tag" "${uploads[@]}" --clobber
echo "Uploaded ${#uploads[@]} file(s)."
