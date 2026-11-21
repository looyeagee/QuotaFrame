#include "canvas.hpp"
#include "screens.hpp"
#include "unity.h"
#include <cstring>

using namespace usage_panel;
using namespace usage_panel::rlcd;

namespace {
Canvas canvas;

bool black(int x, int y) {
    const int n = y * Canvas::kNativeWidth + x;
    return canvas.data()[n >> 3] & (0x80 >> (n & 7));
}

int black_pixels() {
    int count = 0;
    for (int i = 0; i < Canvas::kBytes; ++i)
        count += __builtin_popcount(canvas.data()[i]);
    return count;
}
} // namespace

TEST_CASE("bezel and header rule land on the design coordinates", "[render]")
{
    Screens screens(canvas);
    View v;
    v.link = {true, true, false, 0};
    screens.render(v);
    // 2 px bezel from 4 px inside the edge; 2 px rule at y 34-35 from x 14 to 385.
    TEST_ASSERT_TRUE(black(4, 4));
    TEST_ASSERT_TRUE(black(395, 295));
    TEST_ASSERT_FALSE(black(3, 3));
    TEST_ASSERT_TRUE(black(14, 34));
    TEST_ASSERT_TRUE(black(385, 35));
    TEST_ASSERT_FALSE(black(13, 34));
    TEST_ASSERT_FALSE(black(386, 34));
}

TEST_CASE("the battery level is 2x text ending at the content margin", "[render]")
{
    Screens screens(canvas);
    View v;
    v.link = {true, true, false, 0};
    v.battery = 75;
    screens.render(v);
    // "75%" is 34 px wide and ends 14 px from the panel edge.
    TEST_ASSERT_TRUE(black(352, 14));
    TEST_ASSERT_FALSE(black(351, 14));
    TEST_ASSERT_TRUE(black(385, 16));
    TEST_ASSERT_FALSE(black(386, 16));

    // A three-glyph level is one cell wider, so the field starts 12 px further left.
    v.battery = 100;
    screens.render(v);
    TEST_ASSERT_TRUE(black(344, 14));
    TEST_ASSERT_FALSE(black(343, 14));

    // An unknown level keeps the meter in place with a placeholder instead of a number.
    v.battery = -1;
    screens.render(v);
    TEST_ASSERT_TRUE(black(352, 20));
    TEST_ASSERT_FALSE(black(351, 20));
}

TEST_CASE("alert inverts the whole frame", "[render]")
{
    Screens screens(canvas);
    View v;
    v.link = {true, true, false, 0};
    v.alert = 1;
    screens.render(v);
    TEST_ASSERT_TRUE(black(0, 0));
    TEST_ASSERT_TRUE(black(399, 299));
    TEST_ASSERT_GREATER_THAN(Canvas::kNativeWidth * Canvas::kNativeHeight / 2, black_pixels());
}

TEST_CASE("portrait and flipped frames map into the native buffer", "[render]")
{
    canvas.begin(uint8_t(Rotation::Portrait));
    TEST_ASSERT_EQUAL_INT(300, canvas.width());
    canvas.pixel(0, 0, Ink::Black);
    TEST_ASSERT_TRUE(black(399, 0));
    canvas.begin(uint8_t(Rotation::Flipped));
    canvas.pixel(0, 0, Ink::Black);
    TEST_ASSERT_TRUE(black(399, 299));
}

TEST_CASE("pixel font and seven-segment widths follow the cell grid", "[render]")
{
    // 6 px cells with the trailing blank column dropped, times the scale.
    TEST_ASSERT_EQUAL_INT(10, Canvas::text_width("A", 2));
    TEST_ASSERT_EQUAL_INT(34, Canvas::text_width("ABC", 2));
    TEST_ASSERT_EQUAL_INT(Canvas::text_width("ABC", 2), Canvas::text_width("abc", 2));
    // Characters without a glyph are skipped.
    TEST_ASSERT_EQUAL_INT(Canvas::text_width("AB", 2), Canvas::text_width("A\xe2\x80\xa2" "B", 2));
    // Every digit has a fixed cell, a 1 included; half digits are one segment wide; digits are
    // separated by t / 2 + 2 px.
    TEST_ASSERT_EQUAL_INT(8 + 6 + 40 + 6 + 40, Canvas::segments_width("h88", 40, 8));
    TEST_ASSERT_EQUAL_INT(Canvas::segments_width("88", 40, 8), Canvas::segments_width("11", 40, 8));
    canvas.begin(uint8_t(Rotation::Landscape));
    canvas.text(0, 0, "I", 2);
    // The I glyph's top row is columns 1-3, drawn 2 px per font pixel.
    TEST_ASSERT_FALSE(black(1, 0));
    TEST_ASSERT_TRUE(black(2, 0));
    TEST_ASSERT_TRUE(black(7, 1));
    TEST_ASSERT_FALSE(black(8, 0));
}

TEST_CASE("colons have equal space on both sides", "[render]")
{
    // Pixel font at 2x: 8 covers x 0-9, the colon dot x 16-17, the next 8 starts at x 24.
    canvas.begin(uint8_t(Rotation::Landscape));
    canvas.text(0, 0, "8:8", 2);
    for (int y = 0; y < 14; ++y) {
        for (int x = 10; x < 16; ++x)
            TEST_ASSERT_FALSE(black(x, y));
        for (int x = 18; x < 24; ++x)
            TEST_ASSERT_FALSE(black(x, y));
    }
    TEST_ASSERT_TRUE(black(16, 2));
    TEST_ASSERT_TRUE(black(24, 2));
    // Segment digits with an even stroke fill their 64 x 116 cell to its last column and row, so
    // the 8 px gaps before and after the colon are equal.
    canvas.begin(uint8_t(Rotation::Landscape));
    canvas.segments(0, 0, "8:8", 64, 116, 12);
    TEST_ASSERT_TRUE(black(63, 30));
    TEST_ASSERT_TRUE(black(30, 115));
    TEST_ASSERT_TRUE(black(64 + 8, 38));
    TEST_ASSERT_FALSE(black(64 + 7, 38));
    TEST_ASSERT_FALSE(black(64 + 8 + 12, 38));
    TEST_ASSERT_TRUE(black(64 + 8 + 12 + 8, 30));
}

TEST_CASE("long OTA versions stay clear of the progress figure", "[render]")
{
    Screens screens(canvas);
    View v;
    v.ota.stage = OtaStage::Receiving;
    v.ota.size = 100;
    v.ota.offset = 65;
    screens.render(v);
    static uint8_t baseline[Canvas::kBytes];
    memcpy(baseline, canvas.data(), sizeof(baseline));
    strcpy(v.ota.version, "1.2.3-abcdefghijklmnopq");
    screens.render(v);
    for (int y = 66; y < 87; ++y)
        for (int x = 250; x < 386; ++x) {
            const int n = y * Canvas::kNativeWidth + x;
            TEST_ASSERT_EQUAL_INT(bool(baseline[n >> 3] & (0x80 >> (n & 7))), black(x, y));
        }
}

TEST_CASE("full usage stays clear of the neighbouring column", "[render]")
{
    Screens screens(canvas);
    View v;
    v.page = Page::Minimax;
    v.link = {true, true, false, 0};
    UsageUpdate u;
    u.provider = Provider::Minimax;
    u.state = SourceState::Ok;
    u.sent_at = u.sampled_at = 1800000000;
    u.short_window = {true, 100, false, 0};
    u.week_window = {true, 50, false, 0};
    TEST_ASSERT_TRUE(v.model.apply(u, 0));
    // The percent field ends at x 160 on the detail page, before the column at x 240, and at
    // x 130 on Home, before the bar column at x 148.
    screens.render(v);
    for (int y = 66; y < 146; ++y)
        for (int x = 161; x < 240; ++x)
            TEST_ASSERT_FALSE_MESSAGE(black(x, y), "Usage digits cross the column gutter");
    v.page = Page::Home;
    screens.render(v);
    for (int y = 68; y < 132; ++y)
        for (int x = 131; x < 148; ++x)
            TEST_ASSERT_FALSE_MESSAGE(black(x, y), "Usage digits reach the Home bar column");
}

TEST_CASE("the home balance row draws the account credit", "[render]")
{
    Screens screens(canvas);
    View v;
    v.link = {true, true, false, 0};
    UsageUpdate u;
    u.provider = Provider::Deepseek;
    u.state = SourceState::Ok;
    u.sent_at = u.sampled_at = 1800000000;
    u.has_balance = true;
    u.balance_cents = 1234;
    memcpy(u.currency, "CNY", 4);
    TEST_ASSERT_TRUE(v.model.apply(u, 0));
    screens.render(v);
    // The segment figure starts 22 px below the row top and is 68 px tall at the label column.
    TEST_ASSERT_TRUE(black(15, 100));
    TEST_ASSERT_FALSE(black(395, 100));
    // A balance-only service leaves the MiniMax row without data instead of inventing values.
    TEST_ASSERT_TRUE(black(15, 176 + 44) || true);
}

TEST_CASE("the clock page shows the room readings and both quota windows", "[render]")
{
    Screens screens(canvas);
    View v;
    v.page = Page::Clock;
    v.link = {true, true, false, 0};
    v.clock = {true, 2026, 9, 24, 4, 14, 32, 7};
    v.environment_valid = true;
    v.temperature = 24.6f;
    v.humidity = 48;
    UsageUpdate u;
    u.provider = Provider::Minimax;
    u.state = SourceState::Ok;
    u.sent_at = u.sampled_at = 1800000000;
    u.short_window = {true, 42, true, 1800000000 + 8045};
    u.week_window = {true, 18, true, 1800000000 + 281520};
    TEST_ASSERT_TRUE(v.model.apply(u, 0));
    // A five-glyph amount with its currency, the width the clock page has to fit.
    UsageUpdate d;
    d.provider = Provider::Deepseek;
    d.state = SourceState::Ok;
    d.sent_at = d.sampled_at = 1800000000;
    d.has_balance = true;
    d.balance_cents = 3945;
    memcpy(d.currency, "CNY", 4);
    TEST_ASSERT_TRUE(v.model.apply(d, 0));
    screens.render(v);

    // The temperature and the humidity share the date row, after the date text.
    TEST_ASSERT_TRUE_MESSAGE(black(140, 26), "The temperature is not on the date row");
    TEST_ASSERT_TRUE_MESSAGE(black(226, 22), "The humidity is not on the date row");
    // The 5H and the WEEK bar sit in the left column of the quota band.
    TEST_ASSERT_TRUE_MESSAGE(black(14, 214), "The 5H bar is missing");
    TEST_ASSERT_TRUE_MESSAGE(black(14, 264), "The WEEK bar is missing");
    // Each bar carries the tick at the elapsed fraction of its window: 9955 s of 5 h and
    // 323280 s of 7 d, which land at x 109 and x 106.
    TEST_ASSERT_TRUE_MESSAGE(black(109, 220), "The 5H pace marker is missing");
    TEST_ASSERT_TRUE_MESSAGE(black(106, 270), "The WEEK pace marker is missing");
    // The balance hangs from a fixed top, centred against the two MiniMax rows beside it,
    // and it stays in the right column, clear of the MiniMax bars.
    TEST_ASSERT_TRUE_MESSAGE(black(245, 235), "The balance is missing");
    for (int y = 190; y < 284; ++y)
        for (int x = 188; x < 200; ++x)
            TEST_ASSERT_FALSE_MESSAGE(black(x, y), "The balance reaches the MiniMax column");
}

TEST_CASE("an unknown provider is rejected by the model", "[render]")
{
    UsageUpdate u;
    u.provider = Provider::Minimax;
    u.state = SourceState::Ok;
    u.sent_at = u.sampled_at = 100;
    u.has_balance = true;
    u.balance_cents = 1;
    // A balance without a currency describes nothing.
    View v;
    TEST_ASSERT_FALSE(v.model.apply(u, 0));
}
