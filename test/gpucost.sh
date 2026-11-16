#!/usr/bin/env bash
# gpucost.sh [--device NODE] [--size WxH] [--seconds S] [--looks a,b] [--load NAME CMD]: what each look
#   costs the GPU, measured by hyprns-bench (GPU timer queries) at 30 fps.
#   Prints a Markdown table: a row per look, a column per quality (1, 0.75, 0.5, 0.35),
#   each cell the mean GPU ms per frame and, in brackets, the largest frame. The table
#   is headed idle, or loaded with NAME: CMD runs on the same GPU for the whole
#   measurement (started before, stopped after) and NAME says what it is.
#   Defaults: $HYPRNS_DEVICE else /dev/dri/renderD128, 3440x1440, 20 s of each look's own
#   time, every look.
#   Builds the bench first (make bench).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
device="${HYPRNS_DEVICE:-/dev/dri/renderD128}"
size=3440x1440
seconds=20
looks=(pattern embers marble jupiter)
load_name=""
load_cmd=""
while [ $# -gt 0 ]; do
    case "$1" in
    --device) device="$2"; shift 2 ;;
    --size) size="$2"; shift 2 ;;
    --seconds) seconds="$2"; shift 2 ;;
    --looks) IFS=, read -r -a looks <<<"$2"; shift 2 ;;
    --load) load_name="$2"; load_cmd="$3"; shift 3 ;;
    *) sed -n 2,10p "$0"; exit 2 ;;
    esac
done

make -s -C "$root" bench >&2
bench="$root/build/hyprns-bench"
# each look's steps per second (time_scale 1)
declare -A sps=([pattern]=60 [embers]=480 [marble]=120 [jupiter]=360)
qs=(1 0.75 0.5 0.35)

[[ "$seconds" =~ ^[1-9][0-9]*$ ]] || { echo "gpucost.sh: --seconds takes a whole number of seconds" >&2; exit 2; }
load_pid=""
# the load runs in a session of its own: stopping it stops every process it started
stop_load() { [ -n "$load_pid" ] && kill -- "-$load_pid" 2>/dev/null && wait "$load_pid" 2>/dev/null; load_pid=""; }
trap stop_load EXIT
if [ -n "$load_cmd" ]; then
    setsid bash -c "$load_cmd" >/dev/null 2>&1 &
    load_pid=$!
    sleep 2
    kill -0 "$load_pid" 2>/dev/null || { echo "gpucost.sh: the load exited at once: $load_cmd" >&2; exit 2; }
fi

echo "### $device, $size, 30 fps, ${load_name:+loaded: }${load_name:-idle}"
echo
echo "GPU ms per frame, mean (largest frame), by quality:"
echo
echo "| look | q 1 | q 0.75 | q 0.5 | q 0.35 |"
echo "|---|---|---|---|---|"
for look in "${looks[@]}"; do
    row="| $look |"
    [ -n "${sps[$look]:-}" ] || { echo "gpucost.sh: no look $look" >&2; exit 2; }
    for q in "${qs[@]}"; do
        steps=$((sps[$look] * seconds))
        out="$("$bench" --look "$look" --size "$size" --steps "$steps" --quality "$q" --fps 30 --device "$device" --time 2>/dev/null)" || out='{}'
        row+="$(jq -r 'if .gpu_ms_per_frame == null then " n/a |" else " \(.gpu_ms_per_frame * 1000 | round / 1000) (\(.gpu_ms_max * 100 | round / 100)) |" end' <<<"$out")"
    done
    echo "$row"
done
