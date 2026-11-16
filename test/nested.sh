#!/usr/bin/env bash
# Nested Hyprland for testing hyprns without touching the live session.
#   nested.sh start        start it as a floating 720x1280 window on workspace 10, silently
#   nested.sh stop         kill it (only the recorded nested Hyprland, proved as below; a
#                          stale record is removed, nothing is signalled, exit 3)
#   nested.sh ctl ARGS...  hyprctl into the nested instance (never the host: see nested_sig)
#   nested.sh grab FILE    screenshot of the nested output (never the host screens)
#   nested.sh log          the nested instance's log tail
#   nested.sh pid          the nested Hyprland's PID
#   nested.sh logfile      the recorded instance's log path (it may have exited)
#   nested.sh outfile      the nested Hyprland's stdout and stderr (test/.nested.out)
# Environment for start:
#   HYPRNS_TEST_CONFIG=FILE   the nested config (default nested.lua; a .conf is hyprlang)
#   HYPRNS_TEST_TRANSFORM=1   rotate the nested output to portrait (nested.lua only)
#   HYPRNS_TEST_EXTRA=FILE    a Lua file added to nested.lua's config
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
# instance signature, wayland socket, PID, the process's start time (/proc/PID/stat
# field 22, clock ticks since boot), the config path it was started with
state="$here/.nested"
# the nested Hyprland's stdout and stderr: its crash handler writes there, not to its log
out="$here/.nested.out"

host_instances() { hyprctl instances -j | jq -r '.[].instance'; }

# A process's start time: /proc/PID/stat field 22 (counted after the ")" that ends the
# command name, which may itself hold spaces or parentheses). Empty if there is none.
start_time() {
    local stat
    stat="$(cat "/proc/$1/stat" 2>/dev/null)" || return 0
    stat="${stat##*) }"
    # field 3 is the first after the name: field 22 is the 20th from here
    awk '{ print $20 }' <<<"$stat"
}

# Whether PID is the Hyprland started with CONFIG at START_TIME: a PID reused by another
# process has another start time, and a Hyprland started otherwise has another command line.
proves() {
    local pid="$1" start="$2" config="$3"
    [[ "$pid" =~ ^[1-9][0-9]*$ ]] && [ -n "$start" ] && [ -n "$config" ] || return 1
    [ "$(start_time "$pid")" = "$start" ] || return 1
    [ "$(cat "/proc/$pid/comm" 2>/dev/null)" = "Hyprland" ] || return 1
    # the command line holds "-c CONFIG" (NUL-separated arguments)
    tr '\0' '\n' <"/proc/$pid/cmdline" 2>/dev/null | grep -qxF -- "$config" || return 1
}

# The recorded nested instance, checked: the state file is there; the signature is not
# empty and not this session's own; the PID is proved to be the Hyprland that start
# launched (start time, name and config path, see proves); and hyprctl instances lists
# that signature with that PID. Prints the signature; returns 1 (with the reason on
# stderr) otherwise. Nothing is signalled and no hyprctl -i runs unless this passed.
nested_check() {
    [ -f "$state" ] || { echo "nested.sh: no nested instance (no $state); run nested.sh start" >&2; return 1; }
    local sig pid start config
    sig="$(sed -n 1p "$state")"
    pid="$(sed -n 3p "$state")"
    start="$(sed -n 4p "$state")"
    config="$(sed -n 5p "$state")"
    [ -n "$sig" ] && [ -n "$pid" ] || { echo "nested.sh: $state has no signature or PID" >&2; return 1; }
    [ "$sig" != "${HYPRLAND_INSTANCE_SIGNATURE:-}" ] || { echo "nested.sh: the recorded signature is this session's own; refusing" >&2; return 1; }
    proves "$pid" "$start" "$config" ||
        { echo "nested.sh: PID $pid is not the nested Hyprland that was started (start time, name or config differ)" >&2; return 1; }
    hyprctl instances -j | jq -e --arg i "$sig" --arg p "$pid" '[.[] | select(.instance == $i and (.pid | tostring) == $p)] | length == 1' >/dev/null ||
        { echo "nested.sh: instance $sig with PID $pid is not listed by hyprctl instances" >&2; return 1; }
    printf '%s\n' "$sig"
}

# The same, but a failed check stops the script (exit 3): a call never falls through to
# the host, and nothing is signalled. The record is removed only when its process is
# gone or is not the one started (proves fails); a failure while that process still runs
# (a transient hyprctl error) keeps it, so a running nested Hyprland is never orphaned.
nested_sig() {
    nested_check && return 0
    if [ -f "$state" ]; then
        if proves "$(sed -n 3p "$state")" "$(sed -n 4p "$state")" "$(sed -n 5p "$state")"; then
            echo "nested.sh: the check failed but the nested Hyprland still runs; record kept, nothing signalled" >&2
        else
            rm -f "$state"
            echo "nested.sh: stale record removed; nothing signalled" >&2
        fi
    fi
    exit 3
}

case "${1:-}" in
start)
    if [ -f "$state" ]; then
        nested_check >/dev/null 2>&1 && { echo "already running"; exit 0; }
        echo "nested.sh: removing a stale record ($state)" >&2
        rm -f "$state"
    fi
    config="${HYPRNS_TEST_CONFIG:-$here/nested.lua}"
    [ -f "$config" ] || { echo "nested.sh: no config $config" >&2; exit 2; }
    config="$(cd "$(dirname "$config")" && pwd)/$(basename "$config")"
    extra="${HYPRNS_TEST_EXTRA:-}"
    # the paths go into a Lua string and a shell command line: quoted, and no quotes in them
    case "$config$extra$out" in *[\'\"\\\$\`]*) echo "nested.sh: a config path with a quote, \$, \\ or \` is not supported" >&2; exit 2 ;; esac
    before="$(host_instances | sort)"
    # render_unfocused: the host keeps sending frame callbacks although workspace 10 is
    # not shown, so the nested output keeps rendering (a headless output inside the
    # nested instance does not work on every driver: NVIDIA's GBM refuses its buffers).
    hyprctl dispatch "hl.dsp.exec_cmd('env HYPRNS_TEST_TRANSFORM=${HYPRNS_TEST_TRANSFORM:-0} \"HYPRNS_TEST_EXTRA=$extra\" Hyprland -c \"$config\" >\"$out\" 2>&1', { workspace = '10 silent', float = true, size = '720 1280', render_unfocused = true })" >/dev/null
    new=""
    for _ in $(seq 1 50); do
        sleep 0.2
        new="$(comm -13 <(echo "$before") <(host_instances | sort) | head -1)"
        [ -n "$new" ] && break
    done
    [ -n "$new" ] || { echo "nested Hyprland did not start" >&2; exit 1; }
    [ "$new" != "${HYPRLAND_INSTANCE_SIGNATURE:-}" ] || { echo "nested.sh: the new instance is this session's own; refusing" >&2; exit 1; }
    rec="$(hyprctl instances -j | jq -r --arg i "$new" '.[] | select(.instance==$i) | .instance, .wl_socket, (.pid|tostring)')"
    pid="$(sed -n 3p <<<"$rec")"
    start="$(start_time "$pid")"
    proves "$pid" "$start" "$config" || { echo "nested.sh: the new instance's PID $pid is not a Hyprland started with $config; not recorded" >&2; exit 1; }
    printf '%s\n%s\n%s\n' "$rec" "$start" "$config" > "$state"
    sleep 1
    echo "nested: $(sed -n 1p "$state") on $(sed -n 2p "$state")"
    ;;
stop)
    # only the recorded instance, proved (nested_check): a stale record (the nested
    # Hyprland exited or crashed, the PID may be reused, its lock file may remain)
    # signals nothing, is removed, and stop exits 3
    [ -f "$state" ] || exit 0
    nested_sig >/dev/null
    pid="$(sed -n 3p "$state")"
    kill "$pid" 2>/dev/null || true
    for _ in $(seq 1 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
    rm -f "$state"
    ;;
ctl)
    shift
    sig="$(nested_sig)"
    hyprctl -i "$sig" "$@"
    ;;
grab)
    nested_sig >/dev/null
    WAYLAND_DISPLAY="$(sed -n 2p "$state")" grim "$2"
    ;;
log)
    sig="$(nested_sig)"
    tail -n "${2:-40}" "$XDG_RUNTIME_DIR/hypr/$sig/hyprland.log"
    ;;
pid)
    nested_sig >/dev/null
    sed -n 3p "$state"
    ;;
logfile)
    # no liveness check: for reading the log of an instance that may have exited
    [ -f "$state" ] || { echo "nested.sh: no $state" >&2; exit 3; }
    sig="$(sed -n 1p "$state")"
    [ -n "$sig" ] && [ "$sig" != "${HYPRLAND_INSTANCE_SIGNATURE:-}" ] || { echo "nested.sh: no usable signature in $state" >&2; exit 3; }
    printf '%s\n' "$XDG_RUNTIME_DIR/hypr/$sig/hyprland.log"
    ;;
outfile)
    printf '%s\n' "$out"
    ;;
*)
    sed -n 2,16p "$0"; exit 2 ;;
esac
