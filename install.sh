#!/usr/bin/env bash
# Builds DM Imaging and installs it (macOS and Linux).
# (install.ps1 is the Windows equivalent.)
#
#   ./install.sh                build, bundle the Qt runtime, install
#   ./install.sh --no-build     install what is already built
#   ./install.sh --uninstall    remove it again (settings and images are kept)
#   ./install.sh --prefix DIR   Linux: install under DIR (default /usr/local)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
os="$(uname -s)"
no_build=0
uninstall=0
prefix="/usr/local"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-build)  no_build=1 ;;
        --uninstall) uninstall=1 ;;
        --prefix)    prefix="$2"; shift ;;
        -h|--help)   sed -n '2,9p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

app_mac="/Applications/DM Imaging.app"

if [[ $uninstall -eq 1 ]]; then
    if [[ "$os" == "Darwin" ]]; then
        pkill -x "DM Imaging" 2>/dev/null || true
        rm -rf "$app_mac"
        echo "Removed $app_mac (settings in ~/Library and your images are untouched)."
    else
        sudo rm -f "$prefix/bin/DMImaging" \
                   "$prefix/share/applications/dmimaging.desktop" \
                   "$prefix/share/icons/hicolor/512x512/apps/dmimaging.png"
        sudo rm -rf "$prefix/share/dmimaging"
        command -v update-desktop-database >/dev/null 2>&1 && sudo update-desktop-database "$prefix/share/applications" || true
        echo "Removed DM Imaging from $prefix (the udev rule and your images are untouched)."
    fi
    exit 0
fi

if [[ $no_build -eq 0 ]]; then
    "$root/build.sh" --release --deploy
fi
bin="$root/build/release/bin"

if [[ "$os" == "Darwin" ]]; then
    # the bundle with the Qt runtime inside, produced by build.sh --deploy
    src="$root/build/release/deploy/DM Imaging.app"
    [[ -d "$src" ]] || src="$bin/DM Imaging.app"
    [[ -d "$src" ]] || { echo "DM Imaging.app not found - build first" >&2; exit 1; }
    pkill -x "DM Imaging" 2>/dev/null || true
    rm -rf "$app_mac"
    cp -R "$src" "$app_mac"
    # The bundle is not signed by a developer ID, so Gatekeeper would quarantine
    # it on first launch; clearing the attribute on our own build avoids that.
    xattr -dr com.apple.quarantine "$app_mac" 2>/dev/null || true
    echo "Installed $app_mac"
    echo "The camera needs no driver on macOS. Allow camera access when asked."
else
    [[ -x "$bin/DMImaging" ]] || { echo "DMImaging not found - build first" >&2; exit 1; }
    sudo cmake --install "$root/build/release" --prefix "$prefix"
    command -v update-desktop-database >/dev/null 2>&1 && sudo update-desktop-database "$prefix/share/applications" || true
    echo "Installed DM Imaging into $prefix"
    echo
    echo "One more step so the camera can be opened without root:"
    echo "    sudo sh $root/driver/install_udev_rule.sh"
    echo "then unplug the camera and plug it back in."
fi
