# Speed-up verdicts

Judged on `equivalence.html` (approved sketch left, optimised right, same seed, same step count).
Numbers: mean |left - right| of the 16x9 block means (RGB average, 0..255) as `mean/max` (max =
largest block), read back with `gl.readPixels` after N steps from the same seed, via
`await measure(N)` (or `measure(N, { mult: 6 })` for the x6 speed: 6x the steps per frame).
Measured 2026-11-15 on the RTX 4090 (ANGLE, OpenGL ES 3.2), page as of the fix round 1 commit.
Events: jupiter storm at 500, eye from 2500 (alive to 16500); embers pop at 2500 (alive 2500-2529),
gust 7000-8800. A "hidden null" variant (the left card's own code in a second context) gives 0
for every pair, so the contexts are deterministic and every non-zero number comes from the speed-up.

Stills (`stills/`): eq-jupiter.png at step 6000 (eye up), eq-marble.png at step 6000,
eq-embers.png at step 2520 (pop alive), eq-embers-7600.png at step 7600 (gust blowing),
eq-pattern.png at step 2880 (control, unchanged sketch).

Verdicts given by the user on 2026-11-16 after viewing the page: "I see no difference. I approve them all".

jupiter-cloud-per-frame: accepted
- "per frame only" (RGBA32F) vs approved: 1200 0.034/0.67, 1260 0.037/0.41, 2520 0.200/8.33, 6000 0.143/4.23, 7600 0.121/2.99, 6000 x6 0.582/10.14.
- The larger numbers from 2520 on come from the eye (largest at its onset, 2520). Still at 6000: same eye (at the left edge, wrapping) and same bands on all three cards.

jupiter-cloud-r16f: accepted
- R16F alone ("per frame only" vs "speed-up"): 1200 0.020/0.15, 1260 0.019/0.14, 2520 0.044/0.37, 6000 0.079/1.12, 7600 0.147/1.73, 6000 x6 0.008/0.08.
- Speed-up (per frame + R16F) vs approved: 1200 0.043/0.69, 1260 0.044/0.39, 2520 0.232/8.34, 6000 0.195/4.22, 7600 0.207/3.15, 6000 x6 0.583/10.12.
- This GPU truncates float to half on an R16F store (the page probes it). The page writes an exactly
  representable half by unbiased stochastic rounding (`sround16`) in the transport and source
  passes; the C++ port must do the same and not rely on the driver's rounding.

marble-bake: accepted
- Bake alone vs approved: 1200 0.029/0.08, 1260 (keyframe blend 0.5) 0.032/0.08, 2520 0.030/0.13, 6000 0.037/0.16, 7600 0.032/0.14, 6000 x6 0.037/0.16.
- Still at 6000: same veins and lighting.

marble-displacement-per-frame: accepted
- Exact sum d r^n + u r (1 - r^n)/(1 - r), displacement filtered by hand. Per frame alone vs approved: 1200 0.001/0.00, 1260 0.001/0.00, 2520 0.001/0.00, 6000 0.000/0.00, 7600 0.000/0.00, 6000 x6 0.001/0.01.
  (The first form (d + u n) r^n measured 0.004/0.03 at 1200 and 0.016/0.10 at 6000 x6.)
- Speed-up (bake + per frame) vs approved: 1200 0.029/0.08, 1260 0.032/0.08, 6000 0.037/0.16, 6000 x6 0.036/0.16.

embers-bake: accepted
- Bake alone vs approved: 1200 0.153/1.25, 1260 0.137/1.24, 2520 0.120/1.39, 6000 0.129/1.19, 7600 0.143/3.25, 6000 x6 0.129/1.19.
- Speed-up (all three) vs approved: 1200 0.201/1.91, 1260 0.197/1.81, 2520 0.264/6.19, 6000 0.248/3.98, 7600 0.257/3.67, 6000 x6 0.787/11.12.
- Stills at 2520 and 7600: same coals in the same places, same cracks and glow, the pop disc in both.

embers-proximity-blur: accepted
- Blur alone vs approved (heat and blurred heat filtered by hand): 1200 0.002/0.03, 1260 0.002/0.03, 2520 0.002/0.03, 6000 0.002/0.03, 7600 0.002/0.04, 6000 x6 0.002/0.03.

embers-heat-per-frame: accepted
- Per frame alone vs approved: 1200 0.099/2.07, 1260 0.099/1.57, 2520 (pop alive) 0.192/5.76, 6000 0.174/3.61, 7600 (gust) 0.187/2.95, 6000 x6 0.751/11.08.
- Before fix round 1: 1200 0.181/3.24, 2520 0.222/2.92, 6000 0.249/3.72, 7600 0.256/3.64, and at 6000 x6 68.5/199.7:
  the heat field grew without bound at x6 (diffusion term taken at the cell, not where the value came from).
- The largest block at 2520 is the pop, which is 21 steps old: on the left it is re-set and diffused
  every step, on the right set once per frame after a one-pass diffusion.
