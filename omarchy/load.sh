#!/usr/bin/env bash
# Builds hyprns.so when needed and loads it into the running Hyprland. Run by the Omarchy
# shell plugin (omarchy/Service.qml) each time the shell starts it; also usable by
# hand: bash omarchy/load.sh
#
# - hyprns already loaded (from here or anywhere else): nothing to do, no notification.
# - Otherwise build when the build is missing or older than the source (after
#   `omarchy plugin update`), then `hyprctl plugin load`. A load that fails (most often:
#   built against another Hyprland's headers) is retried once after a clean rebuild.
# - A build shows one notification ("Installing hyprns"), replaced in place by the
#   result. A plain load at login is silent; a failure always says why.
#
# Everything is built in ~/.cache/hyprns, never in the plugin folder: the Omarchy
# shell watches its plugin folders and reloads every plugin (bar included) on each
# file written there.
# Never edits a config file and never runs `hyprctl reload`: Hyprland reloads its
# config by itself after a plugin loads (src/plugins/PluginSystem.cpp), which is what
# applies the hyprns settings.

set -u

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd) || exit 1
cache=${XDG_CACHE_HOME:-$HOME/.cache}/hyprns
state=${XDG_STATE_HOME:-$HOME/.local/state}/hyprns
so="$cache/hyprns.so"
log="$state/omarchy-load.log"
mk=(make -C "$root" "BUILD=$cache/build" "TARGET=$so")

mkdir -p "$cache" "$state"

# One run at a time: the shell can restart the service while a build is running.
exec 9>"$state/load.lock"
flock 9

: >"$log"

say() {
  logger -t hyprns -- "$*" 2>/dev/null
  printf '%s %s\n' "$(date '+%F %T')" "$*" >>"$log"
}

note_id=0
# notify HEADLINE BODY [urgency]: one card, updated in place after the first call.
notify() {
  local id
  if command -v omarchy-notification-send >/dev/null; then
    id=$(omarchy-notification-send --app-name hyprns -u "${3:-low}" -r "$note_id" -p "$1" "$2" 2>/dev/null) &&
      { [[ $id =~ ^[0-9]+$ ]] && note_id=$id; return; }
  fi
  notify-send -a hyprns -u "${3:-low}" "$1" "$2" >/dev/null 2>&1
}

loaded() {
  hyprctl plugin list 2>/dev/null | grep -q '^Plugin hyprns '
}

build() {
  # Below normal priority, so the desktop stays responsive while it compiles.
  nice -n 10 "${mk[@]}" -j"$(nproc)" all >>"$log" 2>&1
}

load() {
  out=$(hyprctl plugin load "$so" 2>&1)
  [[ $out == ok ]]
}

command -v hyprctl >/dev/null && [[ -n ${HYPRLAND_INSTANCE_SIGNATURE:-} ]] || {
  say "not running under Hyprland; nothing loaded"
  exit 0
}

if loaded; then
  say "hyprns is already loaded; nothing to do"
  exit 0
fi

built=0
if ! "${mk[@]}" -q all >/dev/null 2>&1; then
  if [[ -f $so ]]; then
    notify "Updating hyprns" "Rebuilding the live wallpaper plugin. The wallpaper switches over when it is done."
  else
    notify "Installing hyprns" "Building the live wallpaper plugin for this Hyprland (usually under a minute). The wallpaper switches over when it is done."
  fi
  if ! build; then
    say "build failed"
    notify "hyprns could not be built" "Needs the Hyprland headers, pixman, libdrm, GCC with C++23 and make. Log: $log" normal
    exit 1
  fi
  built=1
fi

if ! load; then
  say "load failed: $out; rebuilding from clean"
  notify "Updating hyprns" "Hyprland refused the old build; rebuilding it for this Hyprland."
  "${mk[@]}" clean >>"$log" 2>&1
  if ! build; then
    notify "hyprns could not be built" "Log: $log" normal
    exit 1
  fi
  built=1
  if ! load; then
    say "load failed again: $out"
    notify "hyprns was not loaded" "Hyprland said: $out. If Hyprland was just updated, log out and back in." normal
    exit 1
  fi
fi

say "loaded $so"
(( built )) && notify "hyprns is running" "Live wallpaper loaded. Settings: hyprctl hyprns status."
exit 0
