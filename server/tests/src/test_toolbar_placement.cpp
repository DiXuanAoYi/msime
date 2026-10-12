#include "tests/includes/test_framework.h"
#include "window/toolbar_placement_policy.h"
#include <cmath>

TEST_CASE(toolbar_visible_bar_reaches_all_work_area_edges_at_each_dpi)
{
    // Deterministic geometry includes negative coordinates and a taskbar-excluding work area.
    const RECT work{-2560, -200, 0, 1400};
    for (const double scale : {1.0, 1.25, 1.5, 2.0})
    {
        const auto px = [scale](double dip) { return static_cast<LONG>(std::round(dip * scale)); };
        const RECT insets{px(16), px(14), px(32), px(34)};
        const LONG width = px(520), height = px(40) + insets.top + insets.bottom;
        for (const LONG x : {work.left - 100L, work.right + 100L})
            for (const LONG y : {work.top - 100L, work.bottom + 100L})
            {
                const RECT host{x, y, x + width, y + height};
                const auto fitted = toolbar_placement::FitVisibleContent(host, work, insets);
                const auto content = toolbar_placement::VisibleContent(fitted, insets);
                REQUIRE(content.left >= work.left && content.top >= work.top);
                REQUIRE(content.right <= work.right && content.bottom <= work.bottom);
                REQUIRE_EQ(fitted.right - fitted.left, width);
                REQUIRE_EQ(fitted.bottom - fitted.top, height);
                REQUIRE(content.left == work.left || content.right == work.right);
                REQUIRE(content.top == work.top || content.bottom == work.bottom);
            }
    }
}
TEST_CASE(toolbar_shadow_toggle_preserves_visible_origin_and_center_selects_screen)
{
    const RECT oldInsets{16, 14, 32, 34}, disabled{1, 1, 1, 1};
    const auto moved = toolbar_placement::KeepVisibleOrigin({-40, 100}, oldInsets, disabled);
    REQUIRE_EQ(moved.x + disabled.left, -40L + oldInsets.left);
    REQUIRE_EQ(moved.y + disabled.top, 100L + oldInsets.top);
    const auto center = toolbar_placement::Center({-60, 80, 180, 130});
    REQUIRE(center.x > 0); // Visible center belongs to the right monitor despite a negative host origin.
    const RECT bad{1000, 1000, 1000, 1000}, host{10, 20, 110, 60};
    const auto visible = toolbar_placement::VisibleContent(host, bad);
    REQUIRE(EqualRect(&visible, &host));
}
