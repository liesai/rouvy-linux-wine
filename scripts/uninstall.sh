#!/usr/bin/env bash
set -euo pipefail

readonly DATA_HOME="${XDG_DATA_HOME:-$HOME/.local/share}"
readonly BIN_HOME="${XDG_BIN_HOME:-$HOME/.local/bin}"
readonly WINE_PREFIX="${ROUVY_WINEPREFIX:-$DATA_HOME/wineprefixes/rouvy}"
readonly ROUVY_DIR="${ROUVY_DIR:-$WINE_PREFIX/drive_c/Program Files/VirtualTraining/Rouvy}"
readonly PLUGIN_DIR="$ROUVY_DIR/Rouvy_Data/Plugins/x86_64"

for name in AppUINativePlugin DnsZeroConfLib WclBlePluginCPP; do
  original="$PLUGIN_DIR/$name.original.dll"
  target="$PLUGIN_DIR/$name.dll"
  if [[ -f "$original" ]]; then
    cp -a "$original" "$target"
  fi
done

rm -f "$BIN_HOME/rouvy-wine" "$DATA_HOME/applications/rouvy-wine.desktop"
rm -f "$DATA_HOME/rouvy-linux-wine/rouvy-ble-host.py"
rmdir "$DATA_HOME/rouvy-linux-wine" 2>/dev/null || true

for desktop in \
  "$DATA_HOME/applications/wine/Programs/Rouvy/Rouvy.desktop" \
  "$DATA_HOME/applications/wine-protocol-com.rouvy.desktop"; do
  backup="$desktop.rouvy-linux-wine-original"
  if [[ -f "$backup" ]]; then
    cp -a "$backup" "$desktop"
    rm -f "$backup"
  fi
done
command -v update-desktop-database >/dev/null && \
  update-desktop-database "$DATA_HOME/applications" || true
printf 'DLL originales restaurées et lanceur local supprimé.\n'
