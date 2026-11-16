#include "check.hpp"
#include "core/Settings.hpp"

TEST(config) {
    SSettings s;
    CHECK(applySetting(s, "look", "marble").empty() && s.lookFor("DP-4") == "marble");
    CHECK(applySetting(s, "monitor_looks", "DP-4:jupiter, DP-5:embers").empty());
    CHECK(s.lookFor("DP-4") == "jupiter" && s.lookFor("DP-5") == "embers" && s.lookFor("HDMI-A-1") == "marble");
    CHECK(!applySetting(s, "look", "lava").empty() && s.lookFor("HDMI-A-1") == "marble");
    CHECK(!applySetting(s, "monitor_looks", "DP-4=jupiter").empty() && s.lookFor("DP-4") == "jupiter");
    CHECK(!applySetting(s, "monitor_looks", "DP-4:lava").empty());
    CHECK(!applySetting(s, "quality", "0.6").empty());

    // a connector named twice is refused, keeps the old value and lists the looks
    const std::string dup = applySetting(s, "monitor_looks", "DP-4:marble,DP-4:jupiter");
    CHECK(!dup.empty());
    CHECK(s.lookFor("DP-4") == "jupiter" && s.lookFor("DP-5") == "embers");
    CHECK(dup.ends_with("; the looks are: pattern, embers, marble, jupiter"));

    // a key no look reads any more gets the refusal of any unknown key
    SSettings fresh;
    const std::string gone = applySetting(fresh, "cells_per_px", "0.5"), unknown = applySetting(fresh, "no_such_key", "0.5");
    CHECK(gone.starts_with("unknown setting 'cells_per_px' (one of: "));
    CHECK(gone.substr(gone.find(" (one of: ")) == unknown.substr(unknown.find(" (one of: ")));
    CHECK(describeSettings(fresh) == describeSettings(SSettings{}));
}
