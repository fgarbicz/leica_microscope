#!/usr/bin/env bash
# Configures and builds DM Imaging on macOS and Linux.
# (build.ps1 is the Windows equivalent and takes the same options.)
#
#   ./build.sh                  Release build of everything
#   ./build.sh --debug
#   ./build.sh --no-app         core library, tools and tests only
#   ./build.sh --test           also run the tests
#   ./build.sh --deploy         bundle the Qt runtime (macdeployqt / linuxdeploy)
#   ./build.sh --clean
#
# Qt is found automatically from qmake6/qmake on PATH, Homebrew or QTDIR.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
config=Release
build_app=ON
run_tests=0
deploy=0
clean=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug)   config=Debug ;;
        --release) config=Release ;;
        --no-app)  build_app=OFF ;;
        --test)    run_tests=1 ;;
        --deploy)  deploy=1 ;;
        --clean)   clean=1 ;;
        -h|--help) sed -n '2,15p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

os="$(uname -s)"
build="$root/build/$(echo "$config" | tr '[:upper:]' '[:lower:]')"
[[ $clean -eq 1 ]] && rm -rf "$build"

# ---- Qt
qt_prefix="${QTDIR:-}"
if [[ -z "$qt_prefix" ]]; then
    if command -v qmake6 >/dev/null 2>&1; then
        qt_prefix="$(qmake6 -query QT_INSTALL_PREFIX)"
    elif command -v qmake >/dev/null 2>&1; then
        qt_prefix="$(qmake -query QT_INSTALL_PREFIX)"
    elif [[ "$os" == "Darwin" ]] && command -v brew >/dev/null 2>&1; then
        qt_prefix="$(brew --prefix qt 2>/dev/null || true)"
    fi
fi
if [[ -z "$qt_prefix" || ! -d "$qt_prefix" ]]; then
    cat >&2 <<'MSG'
Qt 6 was not found. Install it and try again:
  macOS          brew install qt
  Debian/Ubuntu  sudo apt install qt6-base-dev qt6-svg-dev
  Fedora         sudo dnf install qt6-qtbase-devel qt6-qtsvg-devel
Or set QTDIR to the Qt prefix.
MSG
    exit 1
fi

# ---- dependency check with an actionable message
if ! pkg-config --exists libusb-1.0 2>/dev/null; then
    cat >&2 <<'MSG'
libusb-1.0 was not found (needed by the native Leica camera driver):
  macOS          brew install libusb
  Debian/Ubuntu  sudo apt install libusb-1.0-0-dev
  Fedora         sudo dnf install libusb1-devel
MSG
    exit 1
fi

generator=()
command -v ninja >/dev/null 2>&1 && generator=(-G Ninja)

cmake -S "$root" -B "$build" "${generator[@]}" \
      -DCMAKE_BUILD_TYPE="$config" \
      -DCMAKE_PREFIX_PATH="$qt_prefix" \
      -DBUILD_APP="$build_app"
cmake --build "$build" --parallel

bin="$build/bin"

if [[ $deploy -eq 1 && "$build_app" == "ON" ]]; then
    if [[ "$os" == "Darwin" ]]; then
        # macdeployqt rewrites the bundle in place: it copies Qt inside and
        # repoints the executable and the plugins at those copies. A later
        # plain rebuild would relink the executable against the system Qt while
        # the bundled cocoa plugin still loads the bundled Qt, and the app dies
        # with "You might be loading two sets of Qt binaries". So deployment
        # works on a copy and the build tree's own bundle stays a plain
        # development build.
        deployed="$build/deploy/DM Imaging.app"
        rm -rf "$build/deploy"
        mkdir -p "$build/deploy"
        cp -R "$bin/DM Imaging.app" "$deployed"
        # codesign refuses to sign a bundle carrying extended attributes
        # ("resource fork, Finder information, or similar detritus not allowed")
        xattr -cr "$deployed"
        "$qt_prefix/bin/macdeployqt" "$deployed" -always-overwrite -no-strip
        xattr -cr "$deployed"
        # Ad-hoc signature: enough to run on the machine that built it.
        # Distributing it to other Macs needs a Developer ID certificate, see
        # docs/DEPLOYMENT.md.
        codesign --force --deep --sign - "$deployed"
        echo "Bundled app: $deployed"
    else
        # linuxdeploy is optional; without it the app uses the system Qt
        if command -v linuxdeploy >/dev/null 2>&1; then
            linuxdeploy --appdir "$build/AppDir" --executable "$bin/DMImaging" \
                        --desktop-file "$root/resources/linux/dmimaging.desktop" \
                        --icon-file "$root/resources/icons/app.png" --plugin qt --output appimage
        else
            echo "linuxdeploy not found: skipping the AppImage. 'sudo make install' uses the system Qt."
        fi
    fi
fi

if [[ $run_tests -eq 1 ]]; then
    # the interface tests need no screen, so they also run over ssh and in containers
    QT_QPA_PLATFORM=offscreen ctest --test-dir "$build" --output-on-failure
fi

echo "Build output: $bin"
