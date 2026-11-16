#pragma once

#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>

// Draws the wallpaper over the whole monitor. Queued at RENDER_POST_WALLPAPER:
// after the background layer, before the bottom layer, so everything else
// (the dashboard, hyprglass's snapshot of the desktop) sees it as the wallpaper.
class CNsPassElement : public IPassElement {
  public:
    CNsPassElement()           = default;
    ~CNsPassElement() override = default;

    std::vector<UP<IPassElement>> draw() override;
    [[nodiscard]] bool            needsLiveBlur() override { return false; }
    [[nodiscard]] bool            needsPrecomputeBlur() override { return false; }

    [[nodiscard]] const char*      passName() override { return "CNsPassElement"; }
    [[nodiscard]] ePassElementType type() override { return EK_CUSTOM; }
};
