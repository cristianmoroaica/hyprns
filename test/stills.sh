#!/usr/bin/env bash
# stills.sh [--reference] LOOK STEPS [DEVICE]: the bench's still of a look next to its sketch's.
#   Renders LOOK for STEPS of its steps with hyprns-bench at the size of
#   reference/stills/LOOK-sketch.png, writes build/stills/LOOK-plugin.png (with
#   --reference: reference/stills/LOOK-plugin.png, the tracked image the README shows)
#   and prints compare_stills.py of the two; exits with the bench's code when it fails,
#   else the comparison's. DEVICE: the render node (default $HYPRNS_DEVICE, else
#   /dev/dri/renderD128). Builds the bench first (make bench).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
outdir="$root/build/stills"
if [ "${1:-}" = "--reference" ]; then
    outdir="$root/reference/stills"
    shift
fi
[ $# -ge 2 ] || { sed -n 2,8p "$0"; exit 2; }
look="$1"
steps="$2"
device="${3:-${HYPRNS_DEVICE:-/dev/dri/renderD128}}"
sketch="$root/reference/stills/$look-sketch.png"
plugin="$outdir/$look-plugin.png"
[ -f "$sketch" ] || { echo "stills.sh: no sketch still $sketch" >&2; exit 2; }

make -s -C "$root" bench >&2
mkdir -p "$outdir"
size="$(python3 -c 'import sys; from PIL import Image; w, h = Image.open(sys.argv[1]).size; print(f"{w}x{h}")' "$sketch")"
echo "stills.sh: $look, $steps steps, $size, $device -> $plugin" >&2
"$root/build/hyprns-bench" --look "$look" --size "$size" --steps "$steps" --device "$device" --png "$plugin"
python3 "$here/compare_stills.py" "$sketch" "$plugin"
