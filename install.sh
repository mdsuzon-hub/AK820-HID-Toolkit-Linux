#!/usr/bin/env bash
# AK820 Lamp Controller - installer for Debian/Ubuntu
# Usage: ./install.sh            install (build + system setup)
#        ./install.sh --uninstall remove everything
set -euo pipefail

APP="ak820-controller"
BIN="/usr/local/bin/${APP}"
RULES_SRC="70-ak820.rules"
RULES_DST="/etc/udev/rules.d/70-ak820.rules"
MODLOAD="/etc/modules-load.d/uinput.conf"
AUTOSTART="${XDG_CONFIG_HOME:-$HOME/.config}/autostart/ak820-controller.desktop"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

log() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }

if [[ $EUID -eq 0 ]]; then
    echo "Run as a normal user (sudo is requested when needed)." >&2
    exit 1
fi

if [[ "${1:-}" == "--uninstall" ]]; then
    log "Stopping app"
    pkill -x "${APP}" 2>/dev/null || true
    log "Removing files"
    sudo rm -f "${BIN}" "${RULES_DST}" "${MODLOAD}"
    rm -f "${AUTOSTART}"
    sudo udevadm control --reload
    sudo udevadm trigger
    log "Uninstalled."
    exit 0
fi

command -v apt-get >/dev/null || { echo "apt-get not found (Debian/Ubuntu only)." >&2; exit 1; }

log "Installing dependencies"
sudo apt-get update -qq
sudo apt-get install -y build-essential cmake pkg-config \
    qt6-base-dev libhidapi-dev libx11-dev

# GNOME needs an extension to show tray icons
if [[ "${XDG_CURRENT_DESKTOP:-}" == *GNOME* ]]; then
    log "GNOME detected: installing AppIndicator extension"
    sudo apt-get install -y gnome-shell-extension-appindicator || true
    gnome-extensions enable appindicatorsupport@rgcjonas.gmail.com 2>/dev/null || true
fi

log "Building"
cmake -S "${DIR}" -B "${DIR}/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "${DIR}/build" -j"$(nproc)"

log "Installing binary to ${BIN}"
sudo install -Dm755 "${DIR}/build/${APP}" "${BIN}"

log "Enabling uinput (volume-key filtering on Wayland)"
sudo modprobe uinput
echo uinput | sudo tee "${MODLOAD}" >/dev/null

log "Installing udev rules (no root needed at runtime)"
sudo install -Dm644 "${DIR}/${RULES_SRC}" "${RULES_DST}"
sudo udevadm control --reload
sudo udevadm trigger

log "Done."
cat <<EOF

  Start:      ${APP}
  Autostart:  enabled automatically on first run (toggle in the tray menu)
  Next:       unplug/replug the keyboard (or log out and in) if the app
              reports missing device permissions.
  Uninstall:  ./install.sh --uninstall

EOF
