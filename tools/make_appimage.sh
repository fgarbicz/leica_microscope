#!/usr/bin/env bash
# Builds the release, runs the tests and makes the Linux AppImage
# dist/DMImaging-<version>-<arch>.AppImage: one file with the Qt runtime and
# libusb inside, runnable on most current distributions without installing
# anything (tools/make_installer.ps1 and tools/make_dmg.sh are the Windows and
# macOS counterparts).
#
#   tools/make_appimage.sh            on Linux: build, test, package
#   tools/make_appimage.sh --docker   from macOS or any Docker host: the same in a
#                                     Debian 12 x86_64 container, so the AppImage
#                                     runs where glibc is 2.36 or newer (Debian 12,
#                                     Ubuntu 24.04 and later)
#
# Camera access still needs the udev rule on each PC; the AppImage carries it,
# and Tools > Install camera access rule installs it (driver/install_udev_rule.sh).
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ "${1:-}" == "--docker" ]]; then
    # the build tree is copied into the container, so nothing it produces lands
    # in the source tree except the finished AppImage in dist/
    mkdir -p "$root/dist"
    exec docker run --rm --platform linux/amd64 \
        -v "$root:/src:ro" -v "$root/dist:/dist" debian:bookworm bash -c '
            set -euo pipefail
            apt-get update -qq >/dev/null
            DEBIAN_FRONTEND=noninteractive apt-get install -y -qq qt6-base-dev qt6-svg-dev qt6-base-dev-tools \
                libusb-1.0-0-dev cmake ninja-build build-essential pkg-config file wget ca-certificates >/dev/null
            cp -r /src /w && rm -rf /w/build /w/dist
            cd /w && tools/make_appimage.sh
            cp dist/*.AppImage /dist/'
fi

[[ "$(uname -s)" == "Linux" ]] || { echo "make_appimage.sh runs on Linux (or use --docker)" >&2; exit 1; }
version="$(sed -n 's/^project(DMImaging VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")"
[[ -n "$version" ]] || { echo "version not found in CMakeLists.txt" >&2; exit 1; }
arch="$(uname -m)"
echo "DM Imaging $version ($arch)"

"$root/build.sh" --release --test
build="$root/build/release"

# linuxdeploy and its Qt plugin, fetched once into the build tree
tools="$build/appimage-tools"
mkdir -p "$tools"
for t in linuxdeploy linuxdeploy-plugin-qt; do
    if [[ ! -x "$tools/$t-$arch.AppImage" ]]; then
        wget -q -O "$tools/$t-$arch.AppImage" \
            "https://github.com/linuxdeploy/$t/releases/download/continuous/$t-$arch.AppImage"
        chmod +x "$tools/$t-$arch.AppImage"
        # An AppImage marks itself with "AI\2" in a reserved byte range of its ELF
        # header, and emulated x86_64 (Docker on Apple silicon, qemu) refuses such
        # a file with "Exec format error". Zeroing the mark changes nothing else;
        # the tool is unpacked, not mounted, anyway (APPIMAGE_EXTRACT_AND_RUN).
        printf '\0\0\0' | dd of="$tools/$t-$arch.AppImage" bs=1 seek=8 count=3 conv=notrunc 2>/dev/null
    fi
done
export PATH="$tools:$PATH"
# AppImages need FUSE to run, which containers usually lack; this unpacks them instead
export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE="$(command -v qmake6 || command -v qmake)"
# the SVG icon engine and image format: the interface icons are SVG
export EXTRA_QT_PLUGINS="svg"

appdir="$build/AppDir"
rm -rf "$appdir"
# where the application looks for its own files (PlatformUi's findResource, and
# the user guide lookup in MainWindow), and the licences of the program and of
# the Qt and libusb it carries
mkdir -p "$appdir/usr/share/dmimaging/driver" "$appdir/usr/share/doc/DMImaging"
cp "$root/driver/99-leica-dmc6200.rules" "$root/driver/install_udev_rule.sh" "$appdir/usr/share/dmimaging/driver/"
cp "$root/docs/USER_GUIDE.md" "$root/LICENSE" "$root/THIRD_PARTY_NOTICES.md" "$appdir/usr/share/doc/DMImaging/"
cp -R "$root/licenses" "$appdir/usr/share/doc/DMImaging/"

# libusb is on the AppImage exclude list (left to the system), but many desktops
# do not have it and the program does not start without it: named explicitly,
# it goes in. X11, OpenGL, fontconfig and freetype stay with the system, as every
# desktop has them and bundling them breaks graphics drivers.
libusb="$(ldconfig -p | awk '/libusb-1\.0\.so\.0 / {print $NF; exit}')"
[[ -f "$libusb" ]] || { echo "libusb-1.0.so.0 not found" >&2; exit 1; }

mkdir -p "$root/dist"
out="$root/dist/DMImaging-$version-$arch.AppImage"
rm -f "$out"
(cd "$build" && OUTPUT="$out" "linuxdeploy-$arch.AppImage" --appdir "$appdir" \
    --executable "$build/bin/DMImaging" \
    --library "$libusb" \
    --desktop-file "$root/resources/linux/dmimaging.desktop" \
    --icon-file "$root/resources/icons/app.png" --icon-filename dmimaging \
    --plugin qt --output appimage)
[[ -f "$out" ]] || { echo "the AppImage was not produced" >&2; exit 1; }
printf 'AppImage: %s (%s)\n' "$out" "$(du -h "$out" | cut -f1)"
