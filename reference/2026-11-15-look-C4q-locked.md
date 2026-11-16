# hyprflow look "C4q" (embers from above, coral): chosen 2026-11-15

Status: look chosen by Cristian in browser sketches. Not yet a design doc; nothing in the plugin has changed.
Working sketch (WebGL2, open in a browser): `hyprns/reference/c4q-sketch.html`, left card ("C4q · coral").

## What Cristian asked for

- Of the first four sketches, C (convection) "has a lot of potential".
- "I saw in it the same potential that a firepit has. It needs that same unpredictibility and texture."

## Decisions, in the order he made them

1. Not flames anchored to the bottom of the screen, and nothing that looks like sprites cut out against black.
2. Embers seen from above (C4), not fire rising through the screen (C3): no up or down, heat pushed around by slow uneven currents.
3. The hot patches slowly shift position; the faster of two speeds (C4b).
4. No static grain: it looked "like an old canvas painting that starts to deteriorate".
5. Texture is coals, in the "glowing cracks" form (C4g): dull dark crust, light from the cracks.
6. Cracks next to a hot patch light up too, fading with distance from the patch; short reach (C4i).
7. Cracks neither strong nor straight: wandering, thin, broken up, faint (C4l).
8. Cracks are not static: slow creep (C4m), about 20 s for a crack to change shape.
9. Quality pass accepted (C4o); the visible wiggle at sketch size he puts down to resolution.
10. Colours: fire-like, in Pantone pastels, the "coral" set. Espresso and brass was tried and rejected for this look.

## Colours (screen values; Pantone's hex values are simulations)

- Background (0.07, 0.065, 0.08), as in D2e.
- K1 dim glow: Marsala 18-1438, #955251 (other sources give #964F4C).
- K2 patch: Living Coral 16-1546, #FF6F61.
- K3 bright: Peach Pink 15-1530, #FA9A85.
- K4 hottest: Peach Fuzz 13-1023, #FFBE98.
- ramp(t): K1 to K2 over 0..0.25, K2 to K3 over 0.25..0.6, K3 to K4 over 0.6..1.

## The sketch's numbers (starting point for the plugin, not yet tuned on a monitor)

Lattice 400x225, fully periodic, 8 steps per frame at 60 frames per second.

Fluid: D2Q9 LBM, omega 1.9. No buoyancy.
- Three stirrers as in D2e (Lissajous paths, irrational frequency ratios, uneven envelopes): force 0.05 x env x gauss(radius 16 cells) x (stirrer velocity - u).
- Drag 1.2e-3.
- Wind: 2.2e-6 x (two sines with golden-ratio frequencies) x an uneven envelope; a gust of 9e-6 in a golden-angle direction for 1800 steps every 12000-25000 steps.

Heat T: MacCormack advection by the fluid, then per step
T = max((T + 0.02 x laplacian) x (1 - 4e-4) - 3e-4, 0).
- Ember bed: 0.6 x periodic value noise (10x6 cells) + 0.4 x (16x9 cells), the two layers sliding in different directions at 3e-4 and 2e-4 cells per step (this is the "shift" x10 setting).
- heat = smoothstep(0.66, 0.76, bed); T = max(T, heat x (0.35 + 0.85 x flicker noise)).
- Pop: every 3000-9000 steps a disc of radius 7-13 cells held at T 1.7 for 30 steps at a low-discrepancy position.

Display:
- T sampled bicubically (B-spline) from the lattice.
- Coals: Voronoi, 24 cells per screen height, true distance to the cell edge. Input warped by low-frequency noise (amplitude 5 cells, uneven sizes) and by fbm (amplitude 0.9 cell, wandering edges). Cell points orbit their home (radius 0.35 cell, angle = 6.5e-4 x step x (0.5 + id)); the fbm warp and the break-up mask drift at the same clock.
- Per-coal tint blended to neutral within 0.12 cell of an edge, so there is no hard step under the faint cracks.
- Crust: BG + K1 x 0.25 x glow x (0.4 + 0.6 x id), glow = smoothstep(0.41, 0.66, bed); then mix toward ramp(0.35 x Td) by 0.8 x smoothstep(0.05, 0.4, Td), Td = T x per-coal tint and pulse.
- Crack: width max(0.045 cell, 1.5 x fwidth), strength 0.35, broken up by mix(0.25, 1, smoothstep(0.25, 0.7, fbm)).
- Proximity P: Gaussian-weighted mean of T over 32 taps within 14 lattice cells; Pn = smoothstep(0, 0.35, P).
- Crack colour ramp(max(T, 0.12 + 0.5 x Pn)), opacity crack x max(smoothstep(0.02, 0.3, T), Pn).
- Soft bloom beside cracks: K2 x 0.25 x 0.35 x (1 - smoothstep(0, 0.2, edge)) x smoothstep(0.1, 0.6, T).
- Static dither of +-0.5/255 against banding.

## Open

- Cristian has not said "lock" in so many words; he chose "coral" after the last comparison.
- The crack wiggle at high resolution should be rechecked on the real monitor.
- The cracks move on their own clock, not carried by the fluid; carrying them was offered and not taken up.
- Speed relative to D2e (which runs at half speed) has not been discussed for this look.
- Looks A and B are still to be iterated; how the looks relate in the plugin is not decided.
