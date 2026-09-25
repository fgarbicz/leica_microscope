#!/usr/bin/env bash
# Installs the udev rule that lets DM Imaging open the Leica DMC6200 without
# root (Linux only). Needs sudo; safe to run repeatedly.
set -euo pipefail
rule="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/99-leica-dmc6200.rules"
[[ -f "$rule" ]] || { echo "missing $rule" >&2; exit 1; }
if [[ "$(uname -s)" != "Linux" ]]; then
    echo "Not needed on $(uname -s): the camera is opened directly, with no driver to install."
    exit 0
fi
sudo install -m 0644 "$rule" /etc/udev/rules.d/99-leica-dmc6200.rules
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=usb
echo "Rule installed. Unplug the camera and plug it back in."
