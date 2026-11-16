#!/usr/bin/env bash
# mass.sh [--device NODE] [--steps N] [--tol T]: mass conservation of every look's fluid.
#   Runs pattern, embers, marble and jupiter (CFluid, lattice 225x400) with hyprns-bench
#   on a 1440x2560 monitor for N steps (default 1000000) and checks that the fluid's mean
#   density is within T (default 0.0001) of 1, where it starts. Without exact mass
#   conservation the fp32 collision gains about 1.4e-8 per step (0.014 after 1M steps).
#   DEVICE: the render node (default $HYPRNS_DEVICE, else /dev/dri/renderD128).
#   Builds the bench first (make bench), or runs $HYPRNS_BENCH when set. Exit 1 when any
#   look fails.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
device="${HYPRNS_DEVICE:-/dev/dri/renderD128}"
steps=1000000
tol=0.0001
while [ $# -gt 0 ]; do
    case "$1" in
        --device) device="$2"; shift 2 ;;
        --steps) steps="$2"; shift 2 ;;
        --tol) tol="$2"; shift 2 ;;
        *) sed -n 2,9p "$0"; exit 2 ;;
    esac
done

bench="${HYPRNS_BENCH:-}"
if [ -z "$bench" ]; then
    make -s -C "$root" bench >&2
    bench="$root/build/hyprns-bench"
fi
fail=0
for look in pattern embers marble jupiter; do
    # the last status line: jupiter's span (rho_mean_min/max over its checks), the
    # others' mean density at that step
    line="$("$bench" --look "$look" --size 1440x2560 --steps "$steps" --status-every "$steps" --device "$device" 2>&1 >/dev/null |
        grep '"steps"' | tail -n 1)" || true
    if ! python3 - "$look" "$tol" "$line" <<'EOF'; then
import json, sys
look, tol, line = sys.argv[1], float(sys.argv[2]), sys.argv[3]
if not line:
    print(f"{look:8} no status line: FAIL")
    sys.exit(1)
s = json.loads(line)
lo = s.get("rho_mean_min", s.get("rho_mean"))
hi = s.get("rho_mean_max", s.get("rho_mean"))
worst = max(abs(lo - 1), abs(hi - 1))
ok = worst <= tol and s.get("resets", 0) == 0
print(f"{look:8} {s['steps']} steps: mean density {lo:.7f}..{hi:.7f}, |error| {worst:.2e} (tolerance {tol:g}), resets {s.get('resets', 0)}: {'ok' if ok else 'FAIL'}")
sys.exit(0 if ok else 1)
EOF
        fail=1
    fi
done
exit "$fail"
