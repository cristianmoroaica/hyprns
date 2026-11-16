# hyprflow look "A2n" (black marble, veins that come and go, faint brass, light sheen): chosen 2026-11-15

Status: chosen by Cristian in browser sketches; he has not said "lock". Not yet a design doc; nothing in the plugin has changed.
Working sketch (WebGL2, open in a browser): `hyprns/reference/a2n-sketch.html`, second card ("A2n").

## What Cristian asked for

- The first A (marbled ink) felt "very Windows XP screensaver".
- "actual marbling, the skin of a statue. I want to see the pores of David gently sweeping"

## Decisions, in the order he made them

1. Stone, not ink: a marble slab with veins, clouding and pores; the fluid only leans on it.
2. Black marble (A2), not white Carrara (A1).
3. "The movement seems right": displacement of at most about 3 lattice cells.
4. Add entropy: veins reappear in different areas at the same slow pace, "keep the gentle nature but ditch the monotony".
5. Pace: A2c, about 1 minute per vein (A2b, 3 minutes, not chosen).
6. Pores, several rounds, then dropped: "My biggest ick when it comes to pores is that they deform when moved"; static pores that come and go were tried, made smaller, sparser and fainter, and finally "let's ditch the pores completely". Do not bring pores back.
7. Brass: he liked an accident where the window behind showed through the sketch as warm spots: "some variety of subtle brassy spots tint".
8. Sheen (a pool of light from the upper left, first added to show pores): kept at one third strength (A2n).

## The sketch's numbers (starting point for the plugin, not tuned on a monitor)

Lattice 400x225, periodic, 2 steps per frame at 60 frames per second.

Fluid: D2Q9 LBM, omega 1.85, drag 6e-4, three stirrers as in D2e (force 0.05 x env x gauss(radius 16 cells) x (stirrer velocity - u)).

Displacement d (2 channels): MacCormack advection by the fluid, then d = (d + u) x 0.975 per step. The stone is sampled at m = p - d / 225 (p = screen position, height 1).

Stone:
- Grain direction: ma = rot(about 37 degrees) x m x (1, 2.6); warp wv = (fbm(1.3 m + 2), fbm(1.3 m + 9)).
- Clouding cl = fbm(1.1 ma + 1.4 wv); base colour mix((0.060, 0.058, 0.070), (0.115, 0.110, 0.128), smoothstep(0.25, 0.85, cl)).
- Four vein families k, scales (1.5, 3.6, 2.4, 6.0), weights (1.0, 0.6, 0.8, 0.4), periods 7200 steps x (1.0, 1.27, 1.618, 0.81).
  - phase = step / period + 0.37 k; f = fract(phase); seed = 17.31 x floor(phase) + 41.7 k.
  - life = smoothstep(0, 0.3, f) x smoothstep(1, 0.7, f).
  - r = fbm(scale x ma + 2.4 wv + seed); ridge = 1 - |2r - 1|; area mask = smoothstep(0.50, 0.72, noise(1.7 m + 0.7 seed + 20)).
  - vein += weight x life x mask x ridge^28; haze += weight x life x mask x ridge^5.
- vein = clamp(vein + 0.16 haze); colour mixed toward (0.80, 0.79, 0.76) by 0.6 x vein.
- Brass: bz = fbm(2.2 m + 70 + 9.1 n) blended between n = floor(step / 14400) and n + 1 (smoothstep of the fraction); colour mix((0.55, 0.38, 0.20), (0.80, 0.64, 0.36), noise(3 m + 90)) x 0.075 x smoothstep(0.50, 0.72, bz), added.
- No pores. Height field h = 0.7 fbm(5 m + 3) + 0.05 fbm(60 m) - 0.10 vein; its screen gradient x (height / 150) gives side lighting clamp(1 - dot(g, (-0.5, 0.5)), 0.6, 1.3), applied at 0.9.
- Sheen: light at (0.35 x aspect, 0.80, 0.55) above the slab, Blinn highlight with exponent 24 on the normal (-1.2 g, 1), colour (0.20, 0.20, 0.22) x 0.33, added.
- Static dither of +-0.5/255.

## Open

- Brass strength: I told him twice it is fainter than in his screenshot; he has not asked for more.
- Vein density: thinned once; he has not said whether it should be sparser.
- Look B still to iterate; how the looks relate in the plugin is not decided.
