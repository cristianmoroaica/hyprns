# hyprflow look "B7" (Jupiter, dark, through a camera): chosen 2026-11-15

Status: look chosen by Cristian in browser sketches ("this is it"). Not yet a design doc; nothing in the plugin has changed.
Working sketch (WebGL2, open in a browser): `hyprns/reference/b7-sketch.html`.

## What Cristian asked for

- Of the first four sketches, B (banded storms) felt "very Windows XP screensaver".
- "I want it to be the living surface of Jupiter, violent swirls followed by flowy rivers. Milky espresso, brass, cream and steel."

## Decisions, in the order he made them

1. Quality first: the cloud layer on the coarse fluid grid was "too low" with "some sort of scanlining"; it is now computed at full output resolution.
2. "increase viscosity a bit" (omega 1.92 to 1.88).
3. Cream replaced by almost black.
4. Pantone colours were tried; the result was "too bright and pink".
5. "we lack texture": a cloud texture carried by the fluid was tried and was "awful". Do not bring it back.
6. "some swirls are almost square and we never see a perfect one": from time to time one clean oval storm, "like the eye of jupiter".
7. "Change the colors, you can even change the color count, make it dark mode", plus "a space cinematic filter with some ISO noise". Chosen as B7.

## Colours (my own dark values, not Pantone-matched)

Ramp over the cloud value c in six steps of 0.2:
- 0.0 night steel (0.070, 0.095, 0.135)
- 0.2 near-black (0.030, 0.030, 0.040)
- 0.4 espresso (0.135, 0.085, 0.065)
- 0.6 umber (0.270, 0.170, 0.105)
- 0.8 dim brass (0.520, 0.365, 0.160)
- 1.0 near-black (0.045, 0.040, 0.045)

## The sketch's numbers (starting point for the plugin, not yet tuned on a monitor)

Fluid lattice 400x225, periodic in x, walls top and bottom, 6 steps per frame at 60 frames per second. D2Q9 LBM, omega 1.88.

Forces:
- Jets: jet(y) = 0.04 x sin(7 pi y / H); force 1.5e-3 x (jet - u.x, -0.3 u.y).
- Planet rotation (beta plane), applied to the deviation dv = u - (jet, 0): f = 4.5e-4 x (y - H/2); force f x (dv.y, -dv.x) - 0.5 f^2 dv. The last term cancels the energy gain of the explicit step; without it the flow ran away once the eye stirred it.
- Storm outbreak every 6500-11500 steps: 3 or 4 vortices on one of the six band edges (y = k H / 7), started 260 steps apart, radius 11-23 cells, alternating spin, each lasting 1500-2400 steps with 250 steps fade-in and 500 fade-out. Force 6.5e-4 x strength x gauss(r) x (-d.y, d.x) / r.
- Eye: first at step 2500, life 14000 steps (1500 in, 2500 out), then a pause of 9000-17000 steps; on an odd band edge (k = 1, 5, 3 in turn), where its spin matches the shear between the jets. Radius 20 cells, stretched 1.5 in x, drifting west at 0.004 cells per step. Force 4e-3 x env x exp(-q2 / 5) x (vt - u), vt = 0.07 x exp(0.5 - 0.5 q2) x (-dd.y, dd.x / 1.5) / r, with dd = (d.x / 1.5, d.y), q2 = |dd|^2 / r^2.

Cloud layer (one scalar c) at 4x the lattice, 1600x900: MacCormack advection by the bilinearly sampled fluid velocity, clamped to the four neighbours, then per step:
- c = mix(c, band(y), 2.5e-4), band(y) = 0.5 + 0.36 sin(7 pi y + pi/2) + 0.14 sin(17 pi y + 2).
- Storms: c = mix(c, colour, 0.004 x |strength| x exp(-d^2 / (0.5 r^2))), colour one of 0, 0.5, 0.72.
- Eye: target 0.74 + 0.26 exp(-8 q2) - 0.5 exp(-12 (q - 1.3)^2); c = mix(c, target, 0.008 x env x exp(-0.45 q2)).

Display:
- colour = ramp(c) x (1 + clamp(1.5 x gradient . (-0.6, 0.6), +-0.08)), gradient over 2 cloud pixels.
- Lens fringing: red and blue sampled at q +- (q - 0.5) x 0.0035 x rr, rr = squared distance from centre in screen heights.
- Contrast: col = 0.35 x smoothstep-curve(col) + 0.75 col.
- Shadows: + (0, 0.006, 0.012) x (1 - smoothstep(0, 0.25, luminance)).
- Vignette: x mix(1, 0.30, smoothstep(0.15, 1.05, rr)).
- ISO noise: cells of 1.5 pixels, new pattern every 2.5 frames (24 per second); amplitude 0.030 + 0.050 sqrt(luminance), 0.8 luminance noise (sum of two hashes) + 0.35 per-channel colour noise.

## Open

- I saw only stills; the motion, and whether the noise shimmer is pleasant on a full monitor behind the glass cards, is Cristian's judgement alone.
- I noted the picture is nearly one colour (amber and black, steel barely visible) and the eye's rings are very regular; he accepted it as is.
- Cost: the cloud layer runs at full output resolution, three passes per step, six steps per frame. Far heavier than D2e, C4q and A2n; needs a budget decision for the plugin.
- How the four looks relate in the plugin is not decided.
