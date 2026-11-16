#pragma once

// The user's verdicts on the seven speed-ups judged on reference/equivalence.html.
// This file mirrors reference/VERDICTS.md (given 2026-11-16: "I see no difference. I
// approve them all"): one flag per id, true = accepted. The plugin always takes the
// accepted path; a rejected path stays compiled only where the bench needs it to
// measure against (hyprns-bench --force-sketch-path).

namespace Verdicts {

inline constexpr bool JUPITER_CLOUD_PER_FRAME       = true; // jupiter-cloud-per-frame
inline constexpr bool JUPITER_CLOUD_R16F            = true; // jupiter-cloud-r16f
inline constexpr bool MARBLE_BAKE                   = true; // marble-bake
inline constexpr bool MARBLE_DISPLACEMENT_PER_FRAME = true; // marble-displacement-per-frame
inline constexpr bool EMBERS_BAKE                   = true; // embers-bake
inline constexpr bool EMBERS_PROXIMITY_BLUR         = true; // embers-proximity-blur
inline constexpr bool EMBERS_HEAT_PER_FRAME         = true; // embers-heat-per-frame

} // namespace Verdicts
