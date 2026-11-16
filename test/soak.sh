#!/usr/bin/env bash
# soak.sh MINUTES: a long run of hyprns in the nested instance (test/nested.sh start first).
#   Loads the plugin, shows each of the four looks for MINUTES each, then two rounds of
#   look switches 2 s apart, then 10 unload/load cycles. Prints per look its resets, final
#   q, GPU ms per frame and crash: no|yes, and a memory line for the nested Hyprland after
#   each stage: RSS and Pss_Anon from /proc/PID/smaps_rollup, and GPU memory where the
#   driver reports it (nvidia-smi). A crash is what Hyprland prints on one: "Hyprland has
#   crashed :( Consult the crash report at ..." on its stderr (nested.sh outfile; 0.56.2
#   writes nothing to hyprland.log then) or "Hyprland received signal" in either; or a
#   crash report for its PID; or the process gone. The summary is printed then too.
#   Every hyprctl call goes through nested.sh ctl, which refuses to run without a live
#   nested instance; this script also stops before anything if that check fails.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
so="$(cd "$here/.." && pwd)/hyprns.so"
N="$here/nested.sh"
[ $# -eq 1 ] && [[ "$1" =~ ^[0-9]+([.][0-9]+)?$ ]] || { sed -n 2,12p "$0"; exit 2; }
minutes="$1"
looks=(pattern embers marble jupiter)
period="$(awk -v m="$minutes" 'BEGIN { printf "%d", m * 60 }')"
[ "$period" -ge 1 ] || period=1

pid="$("$N" pid)" || { echo "soak.sh: no nested instance; refusing to run" >&2; exit 3; }
# the process's start time (/proc/PID/stat field 22): a PID reused after a crash has another
start_time() { awk '{ s = $0; sub(/.*\) /, "", s); split(s, a, " "); print a[20] }' "/proc/$pid/stat" 2>/dev/null; }
started="$(start_time)"
[ -n "$started" ] || { echo "soak.sh: cannot read /proc/$pid/stat; refusing to run" >&2; exit 3; }
logfile="$("$N" logfile)"
outfile="$("$N" outfile)"
[ -f "$so" ] || { echo "soak.sh: build hyprns.so first (make)" >&2; exit 2; }
# (0.56.2 names it after the PID with its trailing zeros cut: 1540 -> ...Report154.txt)
report="${XDG_CACHE_HOME:-$HOME/.cache}/hyprland/hyprlandCrashReport$(sed 's/0*$//' <<<"$pid").txt"
# a crash report counts only when written after this point (an earlier process with the
# same PID may have left one)
since="$(mktemp)"

# the lines Hyprland prints when it crashes (its signal handler), from the files that
# exist (a missing one is not a crash, and grep's exit 2 for it is not used)
crash_lines() {
    local f files=()
    for f in "$outfile" "$logfile"; do [ -f "$f" ] && files+=("$f"); done
    [ ${#files[@]} -gt 0 ] || return 0
    grep -h -m 3 -E 'Hyprland has crashed|Hyprland received signal' "${files[@]}" 2>/dev/null || true
}
new_report() { [ -e "$report" ] && [ "$report" -nt "$since" ]; }
# the nested Hyprland crashed: it printed so, it wrote a crash report since the start, or
# it is gone
crashed() {
    [ -n "$(crash_lines)" ] && return 0
    new_report && return 0
    [ "$(start_time)" != "$started" ]
}
alive() { ! crashed; }
ctl() { "$N" ctl "$@"; }

gpu_mib() {
    if command -v nvidia-smi >/dev/null; then
        nvidia-smi -q -d PIDS 2>/dev/null | awk -v p="$pid" '
            /Process ID/ { cur = ($NF == p) }
            cur && /Used GPU Memory/ { print $(NF-1); found = 1; exit }
            END { if (!found) print "n/a" }'
    else
        echo "n/a"
    fi
}

memline() {
    local label="$1"
    if [ "$(start_time)" != "$started" ]; then
        printf '%-26s nested Hyprland gone\n' "$label"
        return
    fi
    awk -v l="$label" -v g="$(gpu_mib)" '
        /^Rss:/ { rss = $2 } /^Pss_Anon:/ { pa = $2 }
        END { printf "%-26s RSS %7.1f MiB  Pss_Anon %7.1f MiB  GPU %s MiB\n", l, rss / 1024, pa / 1024, g }' "/proc/$pid/smaps_rollup" 2>/dev/null ||
        printf '%-26s nested Hyprland gone\n' "$label"
}

field() { jq -r --arg f "$1" '.monitors[0][$f] // "n/a"'; }

results=()
stage="start"
summary() {
    local rc=$?
    echo
    [ ${#results[@]} -gt 0 ] && printf '%s\n' "${results[@]}"
    if crashed; then
        echo "crash: yes (during: $stage)"
        crash_lines
        if new_report; then echo "crash report: $report"; fi
        rm -f "$since"
        exit 1
    fi
    rm -f "$since"
    if [ "$stage" = "done" ]; then
        echo "after the cycles: $after, crash: no"
    else
        echo "soak stopped during: $stage (exit $rc), crash: no"
    fi
    exit "$rc"
}
trap summary EXIT

echo "soak: $minutes min per look, $period s each, nested PID $pid"
ctl plugin load "$so" >/dev/null
sleep 3
memline "start"

for look in "${looks[@]}"; do
    stage="look $look"
    ctl hyprns set look "$look" >/dev/null
    for ((t = 0; t < period; t += 5)); do
        sleep 5
        alive || break
    done
    if alive; then
        st="$(ctl -j hyprns status)"
        results+=("$(printf '%-8s resets %s  q %s  GPU %s ms/frame  crash: no' "$look" "$(field resets <<<"$st")" "$(field q <<<"$st")" "$(field gpu_ms_per_frame <<<"$st")")")
    else
        results+=("$(printf '%-8s crash: yes' "$look")")
    fi
    memline "after $look"
    alive || exit 1
done

stage="switch rounds"
for _ in 1 2; do
    for look in "${looks[@]}"; do
        ctl hyprns set look "$look" >/dev/null
        sleep 2
    done
done
sleep 2
alive || exit 1
memline "after the switch rounds"

for i in $(seq 1 10); do
    stage="unload/load $i"
    ctl plugin unload "$so" >/dev/null
    sleep 1
    alive || exit 1
    ctl plugin load "$so" >/dev/null
    sleep 3
    alive || exit 1
    memline "after unload/load $i"
done

stage="final status"
st="$(ctl -j hyprns status)"
after="look $(field look <<<"$st"), resets $(field resets <<<"$st")"
ctl plugin unload "$so" >/dev/null
stage="done"
