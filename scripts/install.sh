#!/usr/bin/env bash
set -euo pipefail

readonly ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
readonly BIN_HOME="${XDG_BIN_HOME:-$HOME/.local/bin}"
readonly WINE_PREFIX="${ROUVY_WINEPREFIX:-$DATA_HOME/wineprefixes/rouvy}"
readonly ROUVY_DIR="${ROUVY_DIR:-$WINE_PREFIX/drive_c/Program Files/VirtualTraining/Rouvy}"
readonly PLUGIN_DIR="$ROUVY_DIR/Rouvy_Data/Plugins/x86_64"
readonly APP_DIR="$DATA_HOME/rouvy-linux-wine"

for command in python3 install sed; do
  command -v "$command" >/dev/null || {
    printf 'Commande requise absente: %s\n' "$command" >&2
    exit 1
  }
done
python3 -c 'import dbus, gi' 2>/dev/null || {
  printf 'Modules Python requis absents: dbus et/ou gi.\n' >&2
  exit 1
}
if [[ ! -f "$ROUVY_DIR/Rouvy.exe" ]]; then
  printf 'Installation ROUVY introuvable: %s\n' "$ROUVY_DIR" >&2
  exit 1
fi

backup_and_install() {
  local source=$1 target=$2 backup="${2%.dll}.original.dll"
  if [[ ! -e "$backup" ]]; then
    cp -a "$target" "$backup"
  fi
  install -m 0644 "$source" "$target"
}

backup_and_install "$ROOT_DIR/build/AppUINativePlugin.dll" \
  "$PLUGIN_DIR/AppUINativePlugin.dll"
backup_and_install "$ROOT_DIR/build/DnsZeroConfLib.dll" \
  "$PLUGIN_DIR/DnsZeroConfLib.dll"
backup_and_install "$ROOT_DIR/build/WclBlePluginCPP.dll" \
  "$PLUGIN_DIR/WclBlePluginCPP.dll"

install -d -m 0755 "$APP_DIR" "$BIN_HOME" "$DATA_HOME/applications"
install -m 0755 "$ROOT_DIR/src/rouvy-ble-host.py" "$APP_DIR/rouvy-ble-host.py"
install -m 0755 "$ROOT_DIR/rouvy-wine" "$BIN_HOME/rouvy-wine"
sed "s|@LAUNCHER@|$BIN_HOME/rouvy-wine|g" "$ROOT_DIR/rouvy-wine.desktop.in" \
  >"$DATA_HOME/applications/rouvy-wine.desktop"
chmod 0644 "$DATA_HOME/applications/rouvy-wine.desktop"
command -v update-desktop-database >/dev/null && \
  update-desktop-database "$DATA_HOME/applications" || true

# Wine may publish a second menu entry that bypasses this launcher. Hide it so
# users do not accidentally lose both the DirectInput fix and the BLE helper.
vendor_desktop="$DATA_HOME/applications/wine/Programs/Rouvy/Rouvy.desktop"
if [[ -f "$vendor_desktop" ]]; then
  vendor_backup="$vendor_desktop.rouvy-linux-wine-original"
  [[ -e "$vendor_backup" ]] || cp -a "$vendor_desktop" "$vendor_backup"
  if grep -q '^NoDisplay=' "$vendor_desktop"; then
    sed -i 's/^NoDisplay=.*/NoDisplay=true/' "$vendor_desktop"
  else
    printf '\nNoDisplay=true\n' >>"$vendor_desktop"
  fi
fi

# Authentication/deep links must also start the helper and the selected Wine.
protocol_desktop="$DATA_HOME/applications/wine-protocol-com.rouvy.desktop"
if [[ -f "$protocol_desktop" ]]; then
  protocol_backup="$protocol_desktop.rouvy-linux-wine-original"
  [[ -e "$protocol_backup" ]] || cp -a "$protocol_desktop" "$protocol_backup"
  sed -i "s|^Exec=.*|Exec=$BIN_HOME/rouvy-wine %u|" "$protocol_desktop"
fi
command -v update-desktop-database >/dev/null && \
  update-desktop-database "$DATA_HOME/applications" || true

printf 'Installation terminée. Lancez: %s\n' "$BIN_HOME/rouvy-wine"
