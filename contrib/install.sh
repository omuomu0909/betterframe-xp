#!/bin/sh
# Build and install betterframe-xp (user space only, no sudo). The layer stays
# DISABLED after install; use betterframectl enable + restart SteamVR yourself.
set -eu

src_dir=$(cd "$(dirname "$0")/.." && pwd)
data_dir="$HOME/.local/share/betterframe-xp"
conf_dir="$HOME/.config/betterframe-xp"
layer_dir="$HOME/.local/share/vulkan/implicit_layer.d"
bin_dir="$HOME/.local/bin"
unit_dir="$HOME/.config/systemd/user/steamvr.service.d"

cd "$src_dir/layer"
[ -d build ] || meson setup build --buildtype=release
ninja -C build

install -d "$data_dir/lib" "$conf_dir" "$layer_dir" "$bin_dir" "$unit_dir"
# Copy to a temp name then rename, so a running vrcompositor keeps its mapping.
# Skip it when the content is unchanged: a rename leaves the running vrcompositor holding a
# "(deleted)" file, and the panel then asks for a SteamVR restart that would change nothing.
if cmp -s build/src/libbetterframexp.so "$data_dir/lib/libbetterframexp.so"; then
  echo "layer: unchanged, not replaced"
else
  install -m755 build/src/libbetterframexp.so "$data_dir/lib/libbetterframexp.so.new"
  mv -f "$data_dir/lib/libbetterframexp.so.new" "$data_dir/lib/libbetterframexp.so"
fi
install -m755 "$src_dir/contrib/betterframectl" "$bin_dir/betterframectl"
# Start-failure guard: betterframectl enable wires it into the steamvr.service drop-in.
install -d "$HOME/.local/libexec"
install -m755 "$src_dir/contrib/betterframe-xp-guard" "$HOME/.local/libexec/betterframe-xp-guard"
# Updater the panel's Update tab calls, and the version it compares against the newest release.
install -m755 "$src_dir/contrib/betterframe-xp-update" "$HOME/.local/libexec/betterframe-xp-update.new"
mv -f "$HOME/.local/libexec/betterframe-xp-update.new" "$HOME/.local/libexec/betterframe-xp-update"
if [ -f "$src_dir/VERSION" ]; then install -m644 "$src_dir/VERSION" "$data_dir/VERSION"; else echo 0.0.0 > "$data_dir/VERSION"; fi
# Already enabled with an older drop-in (no guard)? Rewrite it so the guard is wired in.
# Takes effect at the next SteamVR start; this never restarts SteamVR.
if [ -f "$unit_dir/91-betterframe-xp.conf" ] && ! grep -q betterframe-xp-guard "$unit_dir/91-betterframe-xp.conf"; then
  "$bin_dir/betterframectl" enable >/dev/null
fi

manifest="$layer_dir/betterframe-xp.json"
cat > "$manifest" <<EOF
{
  "file_format_version": "1.0.0",
  "layer": {
    "name": "VK_LAYER_BETTERFRAME_XP",
    "type": "GLOBAL",
    "library_path": "$data_dir/lib/libbetterframexp.so",
    "api_version": "1.3.223",
    "implementation_version": "1",
    "description": "betterframe-xp sharpen/deband for Steam Frame vrcompositor",
    "functions": {
      "vkGetInstanceProcAddr": "vkBasalt_GetInstanceProcAddr",
      "vkGetDeviceProcAddr": "vkBasalt_GetDeviceProcAddr"
    },
    "enable_environment": {"ENABLE_BETTERFRAME_XP": "1"},
    "disable_environment": {"DISABLE_BETTERFRAME_XP": "1"}
  }
}
EOF

# The layer used to have a second manifest that loaded it into Steam Link's vrlink client for an
# observation probe. The probe is gone, so remove it.
rm -f "$layer_dir/betterframe-xp-vrlink.json"

[ -f "$conf_dir/params.conf" ] || install -m644 "$src_dir/contrib/params.conf" "$conf_dir/params.conf"
# An older params.conf is missing the keys added since (e.g. the colour grade). Append just those
# keys with the shipped defaults; values the user already set are never touched.
if [ -f "$conf_dir/params.conf" ]; then
  while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    key=$(printf '%s' "${line%%=*}" | tr -d ' \t')
    [ -n "$key" ] || continue
    grep -q "^[[:space:]]*$key[[:space:]]*=" "$conf_dir/params.conf" 2>/dev/null || {
      [ -z "$(tail -c 1 "$conf_dir/params.conf" 2>/dev/null)" ] || printf '\n' >> "$conf_dir/params.conf"
      printf '%s\n' "$line" >> "$conf_dir/params.conf"
    }
  done < "$src_dir/contrib/params.conf"
fi

# Dashboard panel: installed AND enabled, so it shows up in the SteamVR dashboard from the next start.
# `betterframectl panel-off` is the only way to turn it off; re-running this script enables it again.
if [ "${SKIP_PANEL:-0}" != 1 ]; then
  cmake -S "$src_dir/panel" -B "$src_dir/panel/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$src_dir/panel/build" -j"$(nproc)"
  install -m755 "$src_dir/panel/build/betterframe-xp-panel" "$bin_dir/betterframe-xp-panel.new"
  mv -f "$bin_dir/betterframe-xp-panel.new" "$bin_dir/betterframe-xp-panel"
  # Entry for the dashboard's "+" list: a .desktop file plus hicolor icons (Steam looks the icon up in
  # <size>/apps; 256 alone was not found, so 48/128/256 are all written). The panel draws its own icon.
  # Exec is an absolute path because Steam may start it without ~/.local/bin in PATH.
  data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
  for size in 48 128 256; do
    install -d "$data_home/icons/hicolor/${size}x${size}/apps"
    "$bin_dir/betterframe-xp-panel" --write-icon "$data_home/icons/hicolor/${size}x${size}/apps/betterframe-xp-panel.png" "$size" || true
  done
  install -d "$data_home/applications"
  sed "s|@BINARY@|$bin_dir/betterframe-xp-panel|" "$src_dir/contrib/betterframe-xp-panel.desktop" > "$data_home/applications/betterframe-xp-panel.desktop.tmp"
  chmod 644 "$data_home/applications/betterframe-xp-panel.desktop.tmp"
  mv -f "$data_home/applications/betterframe-xp-panel.desktop.tmp" "$data_home/applications/betterframe-xp-panel.desktop"
  if command -v update-desktop-database >/dev/null 2>&1; then update-desktop-database "$data_home/applications" || true; fi
  if command -v gtk-update-icon-cache >/dev/null 2>&1 && [ -f "$data_home/icons/hicolor/index.theme" ]; then
    gtk-update-icon-cache -q "$data_home/icons/hicolor" || true
  fi
  install -d "$HOME/.config/systemd/user"
  install -m644 "$src_dir/contrib/betterframe-xp-panel.service" "$HOME/.config/systemd/user/betterframe-xp-panel.service"
  systemctl --user daemon-reload
  systemctl --user enable betterframe-xp-panel.service >/dev/null
fi
printf 'Installed. Layer is %s.\n' "$("$bin_dir/betterframectl" status | head -1)"
