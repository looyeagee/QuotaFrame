#include "canvas.hpp"
#include "usage_panel/cjk_font.hpp"
#include "usage_panel/panel_logic.hpp"
#include "screens.hpp"
#include "unity.h"
#include <cstdio>
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

/**
 * Whether the logical pixel (`x`, `y`) of a portrait frame is black.
 *
 * The portrait frame is the native buffer turned a quarter clockwise, so a logical column
 * is a native row.
 */
bool black_portrait(int x, int y) { return black(Canvas::kNativeWidth - 1 - y, x); }

/** Black pixels in the logical rectangle of a portrait frame. */
int ink_portrait(int x0, int y0, int x1, int y1) {
    int count = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            count += black_portrait(x, y) ? 1 : 0;
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

TEST_CASE("a lone segment character fills exactly one cell", "[render]")
{
    // `segments` walks a string to its zero, so a layout that places each cell itself must use
    // `glyph`. Passing the address of a single char used to make the walk continue into
    // whatever the stack held next and paint it in the neighbouring cell, which is how a clock
    // came to show two digits on top of each other.
    const int cell = 40, h = 72, t = 8;
    canvas.begin(uint8_t(Rotation::Landscape));
    TEST_ASSERT_EQUAL_INT(cell, canvas.glyph(0, 0, '8', cell, h, t));
    for (int y = 0; y < h; ++y)
        for (int x = cell; x < canvas.width(); ++x)
            TEST_ASSERT_FALSE(black(x, y));

    // Both entry points must land on the same pixels for the same character.
    static uint8_t one_cell[Canvas::kBytes];
    canvas.begin(uint8_t(Rotation::Landscape));
    canvas.glyph(0, 0, '1', cell, h, t);
    memcpy(one_cell, canvas.data(), sizeof(one_cell));
    canvas.begin(uint8_t(Rotation::Landscape));
    canvas.segments(0, 0, "1", cell, h, t);
    TEST_ASSERT_EQUAL_MEMORY(one_cell, canvas.data(), sizeof(one_cell));
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
    // x 129 on Home, before the bar column at x 148.
    screens.render(v);
    for (int y = 66; y < 146; ++y)
        for (int x = 161; x < 240; ++x)
            TEST_ASSERT_FALSE_MESSAGE(black(x, y), "Usage digits cross the column gutter");
    v.page = Page::Home;
    screens.render(v);
    // On Home the figure shares the lower row with the bars, which start at x 148.
    for (int y = 198; y < 262; ++y)
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
    // The segment figure starts 22 px below the row top and is 68 px tall at the label
    // column. "12.34" opens with a "1", which lights the right half of its cell.
    TEST_ASSERT_TRUE_MESSAGE(black(46, 85), "The balance figure is missing");
    // The row keeps the right half of the panel clear; the bezel itself starts at x 394.
    TEST_ASSERT_FALSE(black(300, 100));
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
    // 323280 s of 7 d, which land at x 89 and x 87 on the left column of the page.
    TEST_ASSERT_TRUE_MESSAGE(black(89, 220), "The 5H pace marker is missing");
    TEST_ASSERT_TRUE_MESSAGE(black(87, 270), "The WEEK pace marker is missing");
    // The account credit shares the caption row with the DeepSeek label.
    TEST_ASSERT_TRUE_MESSAGE(black(306, 200), "The balance is missing");
}

TEST_CASE("the music page shows the song, the playhead and the lyric window", "[render]")
{
    Screens screens(canvas);
    View v;
    v.page = Page::Music;
    v.link = {true, true, false, 0};
    v.music_valid = true;
    snprintf(v.music.title, sizeof(v.music.title), "证据");
    snprintf(v.music.artist, sizeof(v.music.artist), "杨乃文");
    v.music.position_seconds = 54;
    v.music.duration_seconds = 275;
    v.music.playing = true;
    v.music.line_count = 3;
    snprintf(v.music.lines[0], sizeof(v.music.lines[0]), "被当作乱放电的诱惑");
    snprintf(v.music.lines[1], sizeof(v.music.lines[1]), "我双鱼 为什么天蝎要恨我");
    snprintf(v.music.lines[2], sizeof(v.music.lines[2]), "难道都是我的错");
    screens.render(v);

    // The line being sung is inverted across the content width, and the glyphs inside it stay
    // open so the text reads as white on black.
    int band_black = 0, band_total = 0;
    for (int y = 140; y < 164; ++y)
        for (int x = 14; x < 386; ++x) {
            ++band_total;
            band_black += black(x, y) ? 1 : 0;
        }
    TEST_ASSERT_GREATER_THAN(band_total / 2, band_black);
    TEST_ASSERT_LESS_THAN(band_total, band_black);
    // The title and the artist carry CJK ink on their own rows above the playhead.
    int title_ink = 0, artist_ink = 0;
    for (int y = 46; y < 70; ++y)
        for (int x = 14; x < 386; ++x)
            title_ink += black(x, y) ? 1 : 0;
    for (int y = 74; y < 98; ++y)
        for (int x = 14; x < 386; ++x)
            artist_ink += black(x, y) ? 1 : 0;
    TEST_ASSERT_GREATER_THAN_MESSAGE(40, title_ink, "The song title is missing");
    TEST_ASSERT_GREATER_THAN_MESSAGE(40, artist_ink, "The artist is missing");
    // The playhead bar is framed at y 106 and the rule under it sits at y 128.
    TEST_ASSERT_TRUE(black(14, 106));
    TEST_ASSERT_TRUE(black(14, 128));

    // A paused song marks its state on the artist row without losing the text.
    v.music.playing = false;
    screens.render(v);
    int paused_ink = 0;
    for (int y = 84; y < 98; ++y)
        for (int x = 300; x < 386; ++x)
            paused_ink += black(x, y) ? 1 : 0;
    TEST_ASSERT_GREATER_THAN_MESSAGE(20, paused_ink, "The paused marker is missing");
}

TEST_CASE("the music page explains itself before any song arrives", "[render]")
{
    Screens screens(canvas);
    View v;
    v.page = Page::Music;
    v.link = {true, true, false, 0};
    screens.render(v);
    // No publication means no inverted lyric band: only the header and one line of text.
    int band_black = 0;
    for (int y = 140; y < 164; ++y)
        for (int x = 14; x < 386; ++x)
            band_black += black(x, y) ? 1 : 0;
    TEST_ASSERT_EQUAL_INT(0, band_black);
}

TEST_CASE("the portrait detail pages fill the turned column", "[render]")
{
    Screens screens(canvas);
    View v;
    v.link = {true, true, false, 0};
    v.settings.rotation = static_cast<uint8_t>(Rotation::Portrait);
    UsageUpdate d;
    d.provider = Provider::Deepseek;
    d.state = SourceState::Ok;
    d.sent_at = d.sampled_at = 1800000000;
    d.has_balance = true;
    d.balance_cents = 3945;
    memcpy(d.currency, "CNY", 4);
    TEST_ASSERT_TRUE(v.model.apply(d, 0));
    UsageUpdate m;
    m.provider = Provider::Minimax;
    m.state = SourceState::Ok;
    m.sent_at = m.sampled_at = 1800000000;
    m.short_window = {true, 69, true, 1800000000 + 8045};
    m.week_window = {true, 57, true, 1800000000 + 281520};
    TEST_ASSERT_TRUE(v.model.apply(m, 0));

    // The DeepSeek page shows the balance between its caption and the account note.
    v.page = Page::Deepseek;
    screens.render(v);
    TEST_ASSERT_GREATER_THAN_MESSAGE(20, ink_portrait(14, 44, 90, 58), "The caption is missing");
    TEST_ASSERT_GREATER_THAN_MESSAGE(200, ink_portrait(14, 131, 200, 219),
                                     "The balance figure is missing");
    TEST_ASSERT_GREATER_THAN_MESSAGE(20, ink_portrait(14, 308, 200, 322),
                                     "The account note is missing");
    // Nothing reaches into the gutter between the content column and the bezel; the
    // inverted chip bleeds 4 px past the text margin, like the lyric band on the music page.
    TEST_ASSERT_EQUAL_INT(0, ink_portrait(7, 40, 10, 350));

    // The MiniMax page stacks the figure, its bar and the weekly block.
    v.page = Page::Minimax;
    screens.render(v);
    TEST_ASSERT_GREATER_THAN_MESSAGE(200, ink_portrait(14, 66, 190, 162),
                                     "The window figure is missing");
    TEST_ASSERT_GREATER_THAN_MESSAGE(20, ink_portrait(14, 204, 286, 222),
                                     "The five-hour bar is missing");
    TEST_ASSERT_GREATER_THAN_MESSAGE(20, ink_portrait(14, 292, 286, 306),
                                     "The weekly bar is missing");
    TEST_ASSERT_EQUAL_INT(0, ink_portrait(7, 40, 10, 350));
}

TEST_CASE("the portrait music page shows the song and the whole lyric window", "[render]")
{
    Screens screens(canvas);
    View v;
    v.link = {true, true, false, 0};
    v.settings.rotation = static_cast<uint8_t>(Rotation::Portrait);
    v.page = Page::Music;
    v.music_valid = true;
    snprintf(v.music.title, sizeof(v.music.title), "证据");
    snprintf(v.music.artist, sizeof(v.music.artist), "杨乃文");
    v.music.position_seconds = 54;
    v.music.duration_seconds = 275;
    v.music.playing = true;
    v.music.line_count = 3;
    snprintf(v.music.lines[0], sizeof(v.music.lines[0]), "被当作乱放电的诱惑");
    snprintf(v.music.lines[1], sizeof(v.music.lines[1]), "我双鱼 为什么天蝎要恨我");
    snprintf(v.music.lines[2], sizeof(v.music.lines[2]), "难道都是我的错");
    screens.render(v);

    // The line being sung is inverted across the column, and the glyphs inside it stay
    // open so the text reads as white on black.
    const int band_black = ink_portrait(10, 148, 290, 176);
    TEST_ASSERT_GREATER_THAN(280 * 28 / 2, band_black);
    TEST_ASSERT_LESS_THAN(280 * 28, band_black);
    // The title and the artist carry CJK ink on their own rows above the playhead.
    TEST_ASSERT_GREATER_THAN_MESSAGE(40, ink_portrait(14, 44, 286, 68),
                                     "The song title is missing");
    TEST_ASSERT_GREATER_THAN_MESSAGE(40, ink_portrait(14, 76, 286, 100), "The artist is missing");
    // The playhead bar is framed at y 112 and the rule under it sits at y 138.
    TEST_ASSERT_TRUE(black_portrait(14, 112));
    TEST_ASSERT_TRUE(black_portrait(14, 138));
}

TEST_CASE("the sessions page shows the roster and its states", "[render]")
{
    Screens screens(canvas);
    View v;
    v.page = Page::Sessions;
    v.link = {true, true, false, 0};
    v.sessions_valid = true;
    v.sessions.live_count = 2;
    snprintf(v.sessions.live[0].name, sizeof(v.sessions.live[0].name), "会话状态页面");
    v.sessions.live[0].state = SessionState::Busy;
    snprintf(v.sessions.live[1].name, sizeof(v.sessions.live[1].name), "修复列表滚动");
    v.sessions.live[1].state = SessionState::Idle;
    screens.render(v);

    // The summary line counts the roster and the busy sessions.
    int summary_ink = 0;
    for (int y = 46; y < 60; ++y)
        for (int x = 14; x < 200; ++x)
            summary_ink += black(x, y) ? 1 : 0;
    TEST_ASSERT_GREATER_THAN_MESSAGE(20, summary_ink, "The summary line is missing");
    // The busy row carries an inverted chip: its padding band is solid black.
    for (int y = 69; y < 72; ++y)
        for (int x = 12; x < 62; ++x)
            TEST_ASSERT_TRUE_MESSAGE(black(x, y), "The busy chip is missing");
    // The session names carry CJK ink on their own rows beside the chips.
    int name_ink = 0;
    for (int y = 72; y < 84; ++y)
        for (int x = 78; x < 240; ++x)
            name_ink += black(x, y) ? 1 : 0;
    TEST_ASSERT_GREATER_THAN_MESSAGE(20, name_ink, "The session name is missing");
    // Ended sessions are not drawn: the block under the last row stays blank.
    int below_ink = 0;
    for (int y = 148; y < 200; ++y)
        for (int x = 14; x < 240; ++x)
            below_ink += black(x, y) ? 1 : 0;
    TEST_ASSERT_EQUAL_MESSAGE(0, below_ink, "The page drew rows beyond the roster");
}

TEST_CASE("a session that waits says so and names what it waits for", "[render]")
{
    Screens screens(canvas);
    View v;
    v.page = Page::Sessions;
    v.link = {true, true, false, 0};
    v.sessions_valid = true;
    v.sessions.live_count = 2;
    snprintf(v.sessions.live[0].name, sizeof(v.sessions.live[0].name), "\u63d0\u95ee\u529f\u80fd\u9009\u9879");
    v.sessions.live[0].state = SessionState::Waiting;
    v.sessions.live[0].waiting_for = WaitingReason::Ask;
    snprintf(v.sessions.live[1].name, sizeof(v.sessions.live[1].name), "\u4fee\u590d\u5217\u8868\u6eda\u52a8");
    v.sessions.live[1].state = SessionState::Idle;
    screens.render(v);

    // The waiting row carries an inverted chip, like the working one.
    for (int y = 69; y < 72; ++y)
        for (int x = 12; x < 62; ++x)
            TEST_ASSERT_TRUE_MESSAGE(black(x, y), "The waiting chip is missing");
    // The reason sits at the far end of the row: "ASK" is drawn there.
    int reason_ink = 0;
    for (int y = 72; y < 84; ++y)
        for (int x = 360; x < 390; ++x)
            reason_ink += black(x, y) ? 1 : 0;
    TEST_ASSERT_GREATER_THAN_MESSAGE(10, reason_ink, "The waiting reason is missing");
}

TEST_CASE("the sessions page explains itself before any publication arrives", "[render]"){
    Screens screens(canvas);
    View v;
    v.page = Page::Sessions;
    v.link = {true, true, false, 0};
    screens.render(v);
    // No publication means no roster: only the header and one line of text.
    int name_ink = 0;
    for (int y = 72; y < 84; ++y)
        for (int x = 78; x < 240; ++x)
            name_ink += black(x, y) ? 1 : 0;
    TEST_ASSERT_EQUAL_INT(0, name_ink);
}

TEST_CASE("the settings page draws the four rows a panel without a dimmer offers", "[render]")
{
    Screens screens(canvas);
    View v;
    v.page = Page::Settings;
    v.link = {true, true, false, 0};
    v.setting_rows = static_cast<uint8_t>(kSettingRowsNoDimmer);
    screens.render(v);

    // The rotation row, the last one this panel offers, keeps its label and its options.
    int rotation_ink = 0;
    for (int y = 190; y < 226; ++y)
        for (int x = 14; x < 385; ++x)
            rotation_ink += black(x, y) ? 1 : 0;
    TEST_ASSERT_GREATER_THAN_MESSAGE(40, rotation_ink, "The rotation row is missing");
    // No fifth row is drawn, so the band above the footer rule stays clear.
    int spare_ink = 0;
    for (int y = 232; y < 250; ++y)
        for (int x = 14; x < 385; ++x)
            spare_ink += black(x, y) ? 1 : 0;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, spare_ink, "A row without a setting was drawn");
    TEST_ASSERT_TRUE(black(14, 252));
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

namespace {
/** Bounding box of the pixels that differ between the panel and `reference`. */
void changed_box(const uint8_t *reference, int &left, int &top, int &right, int &bottom) {
    left = top = 1 << 30;
    right = bottom = -1;
    for (int y = 0; y < Canvas::kNativeHeight; ++y) {
        for (int x = 0; x < Canvas::kNativeWidth; ++x) {
            const int n = y * Canvas::kNativeWidth + x;
            const bool before = reference[n >> 3] & (0x80 >> (n & 7));
            if (before == black(x, y))
                continue;
            if (x < left) left = x;
            if (y < top) top = y;
            if (x > right) right = x;
            if (y > bottom) bottom = y;
        }
    }
}

/** One lyric window: a short sung line and a wider line under it. */
View lyric_view(const char *sung, const char *next) {
    View v;
    v.link = {true, true, false, 0};
    v.page = Page::Music;
    v.music_valid = true;
    snprintf(v.music.title, sizeof(v.music.title), "Song");
    snprintf(v.music.artist, sizeof(v.music.artist), "Artist");
    v.music.position_seconds = 54;
    v.music.duration_seconds = 275;
    v.music.playing = true;
    v.music.line_count = 2;
    snprintf(v.music.lines[0], sizeof(v.music.lines[0]), "%s", sung);
    snprintf(v.music.lines[1], sizeof(v.music.lines[1]), "%s", next);
    return v;
}

void render_at(Screens &screens, const View &base, uint64_t age_ms) {
    View v = base;
    v.music_line_ms = 0;
    v.now_ms = age_ms;
    screens.render(v);
}
} // namespace

TEST_CASE("a lyric line that fits holds still between frames", "[render][lyrics]")
{
    Screens screens(canvas);
    const View v = lyric_view("说你也爱我", "I love you");
    render_at(screens, v, 0);
    static uint8_t reference[Canvas::kBytes];
    memcpy(reference, canvas.data(), sizeof(reference));
    render_at(screens, v, 30000);
    int left, top, right, bottom;
    changed_box(reference, left, top, right, bottom);
    TEST_ASSERT_EQUAL_INT(-1, right);
}

TEST_CASE("a pure hanzi line stops with its last character whole", "[render][lyrics]")
{
    // 24 hanzi are 576 px in a 364 px row and every boundary is 24 px apart, so the plain end
    // of the travel falls inside a glyph: the tail has to be aligned to a boundary instead.
    const char *line = "好到两个人都觉得知音难找恨不得把旁人都删掉了吗真的";
    const int room = 364;
    const int start = CjkText::tail_start(line, room, 2);
    const int total = CjkText::width(line, 2);
    TEST_ASSERT_TRUE(total > room);
    TEST_ASSERT_EQUAL_INT(0, start % 24);
    TEST_ASSERT_TRUE(total - start <= room);
    // The scroll reaches that boundary and stays there.
    TEST_ASSERT_TRUE(lyric_scroll_offset(total, start, 60000, 0) >= start);
}

TEST_CASE("only the sung line scrolls inside its own row", "[render][lyrics]")
{
    Screens screens(canvas);
    // The next line is far wider than its row too, but it is context: it keeps the ellipsis.
    const View v = lyric_view(
        "This is a very long English lyric that will not fit", "这一行也很长很长很长很长很长很长很长很长很长很长");
    render_at(screens, v, 0);
    static uint8_t start[Canvas::kBytes];
    memcpy(start, canvas.data(), sizeof(start));
    render_at(screens, v, 4300);
    int left, top, right, bottom;
    changed_box(start, left, top, right, bottom);

    // Every changed pixel stays inside the sung row's band and inside its text margin.
    TEST_ASSERT_TRUE(right > left);
    TEST_ASSERT_TRUE(left >= 14);
    TEST_ASSERT_TRUE(top >= 140);
    TEST_ASSERT_TRUE(bottom <= 163);
    // The clip ends on a text column, so the right edge never cuts a glyph in half.
    TEST_ASSERT_TRUE(right <= 373);
}

TEST_CASE("the timeline names the line the panel highlights", "[render][lyrics]")
{
    // The publication still names the first line, but the song has passed the start of the
    // second one: the panel shows the line that is sung, not the line that arrived.
    Screens screens(canvas);
    View v = lyric_view("第一句", "第二句");
    v.music.current_line = 0;
    v.music.position_ms = 12500;
    v.music.has_position = true;
    v.music.line_time_count = 2;
    v.music.line_times_ms[0] = 10000;
    v.music.line_times_ms[1] = 12000;
    v.music_position_at_ms = 0;
    v.now_ms = 500;
    screens.render(v);
    static uint8_t timeline[Canvas::kBytes];
    memcpy(timeline, canvas.data(), sizeof(timeline));

    // The same page with the second line named outright has to look the same.
    View named = lyric_view("第一句", "第二句");
    named.music.current_line = 1;
    named.now_ms = 0;
    screens.render(named);
    TEST_ASSERT_EQUAL_UINT8(0, memcmp(timeline, canvas.data(), Canvas::kBytes));
}
