# hyprflow look "D2e": locked 2026-11-15

Status: look chosen by Cristian in browser sketches. Not yet a design doc; nothing in the plugin has changed.
Working sketch (WebGL2, open in a browser): `hyprns/reference/d2e-sketch.html`, left card.

## What Cristian asked for

- Brief: "The intriguing patterns that can also be found in nature that is never repeating. Predictible, yet never repeating. Tangible yet mysterious."
- The old vortex street was "boring and uninspired, just some repetitive swirls in a grid".
- Of four first sketches (marbled ink, banded storms, convection, living pattern), D "living pattern" was "the most intriguing, but feels unfinished"; he expected "more entropy, more movement and unexpected actions".

## Decisions, in the order he made them

1. Movement comes from the water, not from the chemistry: a calm reaction-diffusion pattern carried by a fluid (picked "D2 stirred" over restless chemistry and over both).
2. Edges crisp, not blurry.
3. The web "a bit more loose": a little looser (D2a), not the much looser worms-and-spots version.
4. Uneven stirring and rare sudden events are both in.
5. Colour and opacity follow the water's speed, fast = bright and solid (D2c), not the reverse.
6. Colours: espresso and brass. No blue.
7. Calm regions fade almost into the background (D2e), not the more visible variant.
8. Half the speed of the first sketches.

## The sketch's numbers (starting point for the plugin, not yet tuned on a monitor)

Lattice 400x225 for an 800x450 canvas (0.5 cells per pixel), 60 frames per second.

Pattern: Gray-Scott, Da 1.0, Db 0.5, 9-point Laplacian (0.2 edges, 0.05 corners), 6 steps per frame.
- Feed/kill blended between three points by slowly drifting value noise:
  (0.0367, 0.0649), (0.050, 0.0632), (0.031, 0.0595).
- Carried by the fluid: semi-Lagrangian lookup at 0.5 x the fluid velocity per step.
- A small seed (radius 3) every 400 steps at a golden-ratio position keeps it from dying out.

Fluid: D2Q9 LBM, omega 1.85, periodic, 1 step per frame.
- Three stirrers on Lissajous paths with irrational frequency ratios (golden ratio, sqrt 2, sqrt 5).
  Force 0.05 x env x gauss(radius 16 cells) x (stirrer velocity - u), drag 6e-4.
- Uneven: each stirrer's env = 1.6 x smoothstep(-0.2, 0.7, sin(e1 t) + 0.6 sin(e2 t)), again with irrational ratios.

Sudden events, one every 15000-30000 pattern steps (40-80 s in the sketch; rarer on the wallpaper), cycling:
- burst: disc radius 16 filled with the growing chemical;
- wipe: disc radius 48 cleared;
- gust: a fourth stirrer crossing at 0.13 cells/step for 1800 fluid steps.

Display:
- Edge: threshold 0.19 on the second chemical, antialiased with fwidth; relief shading from its gradient.
- v = smoothstep(0.002, 0.04, |u|).
- Colour = mix(espresso (0.33, 0.21, 0.14), brass (0.80, 0.64, 0.36), smoothstep(0.1, 0.9, v)).
- Opacity = mix(0.22, 1.0, smoothstep(0.0, 0.6, v)) over background (0.07, 0.065, 0.08).
- A faint brass glow where the pattern is changing.

## Open

- Looks C (convection), A and B are still to be iterated; Cristian wants to iterate each.
- How the looks relate in the plugin (one chosen, switchable, or rotating) is not decided.
- The plugin today has only the fluid solver; D2e needs a reaction-diffusion pass added.
