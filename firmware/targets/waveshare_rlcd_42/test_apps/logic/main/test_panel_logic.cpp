#include "usage_panel/panel_logic.hpp"
#include "unity.h"

using namespace usage_panel;

namespace {
constexpr uint32_t kNow = 1790253127;

UsageWindow window(uint8_t used, uint32_t remaining) { return {true, used, true, kNow + remaining}; }

View linked_view() {
    View v;
    v.link = {true, true, false, 0};
    return v;
}

void publish(View &v, Provider p, uint8_t short_used) {
    UsageUpdate u;
    u.provider = p;
    u.state = SourceState::Ok;
    u.sampled_at = kNow;
    u.sent_at = kNow;
    u.short_window = window(short_used, 3600);
    u.week_window = window(10, 86400);
    v.model.apply(u, 0);
}
} // namespace

namespace {
// compute_pace is board-agnostic: these cases pin a five-hour window so they stay
// independent of the short-window length a target configures.
constexpr uint32_t kFiveHours = 5 * 3600;
} // namespace

TEST_CASE("pace compares usage with the elapsed window", "[pace]")
{
    // With 2:14:05 remaining in a five-hour window, 55 % of the time has elapsed.
    Pace p = compute_pace(window(42, 8045), kNow, kFiveHours);
    TEST_ASSERT_TRUE(p.valid);
    TEST_ASSERT_EQUAL_INT(-13, p.delta);
    p = compute_pace(window(84, 5045), kNow, kFiveHours);
    TEST_ASSERT_EQUAL_INT(12, p.delta);
    TEST_ASSERT_EQUAL_UINT32(12955, p.elapsed_s);
    p = compute_pace(window(61, 439320), kNow, kWeekWindowSeconds);
    TEST_ASSERT_EQUAL_INT(34, p.delta);
    p = compute_pace(window(18, 281520), kNow, kWeekWindowSeconds);
    TEST_ASSERT_EQUAL_INT(-35, p.delta);
}

TEST_CASE("pace is invalid without a usable reset", "[pace]")
{
    UsageWindow w = window(50, 100);
    w.has_reset = false;
    TEST_ASSERT_FALSE(compute_pace(w, kNow, kFiveHours).valid);
    TEST_ASSERT_FALSE(compute_pace(window(50, 0), kNow, kFiveHours).valid);
    TEST_ASSERT_FALSE(compute_pace(window(50, kFiveHours + 1), kNow, kFiveHours).valid);
    TEST_ASSERT_FALSE(compute_pace(window(50, 100), 0, kFiveHours).valid);
}

TEST_CASE("countdowns follow the design formats", "[format]")
{
    char b[32];
    format_short_countdown(window(0, 8045), kNow, true, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("2:14:05", b);
    format_short_countdown(window(0, 8045), kNow, false, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("2:14", b);
    format_week_countdown(window(0, 439320), kNow, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("5D 02:02", b);
    format_short_countdown(window(0, 0), kNow, true, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("WAIT", b);
    UsageWindow none{};
    format_week_countdown(none, kNow, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("--", b);
    format_elapsed(12955, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("3H35M", b);
    format_elapsed(2 * 86400 + 4 * 3600 + 59 * 60, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("2D 04H", b);
    format_age(59, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("NOW", b);
    format_age(120, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("2M AGO", b);
    format_age(3 * 3600, b, sizeof(b));
    TEST_ASSERT_EQUAL_STRING("3H AGO", b);
}

TEST_CASE("the landscape ring and the portrait ring cycle in both directions", "[nav]")
{
    TEST_ASSERT_EQUAL_UINT8(uint8_t(Page::Deepseek), uint8_t(Navigator::next(Page::Home, false)));
    TEST_ASSERT_EQUAL_UINT8(uint8_t(Page::Minimax), uint8_t(Navigator::next(Page::Deepseek, false)));
    TEST_ASSERT_EQUAL_UINT8(uint8_t(Page::Music), uint8_t(Navigator::next(Page::Minimax, false)));
    TEST_ASSERT_EQUAL_UINT8(uint8_t(Page::Clock), uint8_t(Navigator::next(Page::Music, false)));
    TEST_ASSERT_EQUAL_UINT8(uint8_t(Page::Home), uint8_t(Navigator::next(Page::Clock, false)));
    TEST_ASSERT_EQUAL_UINT8(uint8_t(Page::Clock), uint8_t(Navigator::next(Page::Home, true)));
    TEST_ASSERT_EQUAL_UINT8(uint8_t(Page::Home), uint8_t(Navigator::next(Page::Clock, true)));
    TEST_ASSERT_EQUAL_UINT8(uint8_t(Page::Music), uint8_t(Navigator::previous(Page::Clock, false)));
    TEST_ASSERT_EQUAL_UINT8(uint8_t(Page::Clock), uint8_t(Navigator::previous(Page::Home, false)));
    TEST_ASSERT_EQUAL_UINT8(uint8_t(Page::Home), uint8_t(Navigator::previous(Page::Deepseek, false)));
    TEST_ASSERT_EQUAL_UINT8(uint8_t(Page::Clock), uint8_t(Navigator::previous(Page::Home, true)));
    for (Page page : {Page::Deepseek, Page::Minimax, Page::Music, Page::Settings})
        TEST_ASSERT_EQUAL_UINT8(uint8_t(Page::Home), uint8_t(Navigator::previous(page, true)));
    TEST_ASSERT_EQUAL_UINT8(uint8_t(Page::Home), uint8_t(Navigator::previous(Page::Settings, false)));
    // The header ring draws one marker per page, the music page third of five.
    TEST_ASSERT_EQUAL_INT(5, ring_pages());
    TEST_ASSERT_EQUAL_INT(3, ring_index(Page::Music));
    TEST_ASSERT_EQUAL_INT(-1, ring_index(Page::Settings));
}

TEST_CASE("the left key steps back, the right key forward and the chord goes home", "[nav]")
{
    Navigator nav;
    View v = linked_view();
    nav.input(Input::Back, v, 0);
    TEST_ASSERT_EQUAL_UINT8(uint8_t(Page::Clock), uint8_t(v.page));
    nav.input(Input::Forward, v, 0);
    nav.input(Input::Forward, v, 0);
    TEST_ASSERT_EQUAL_UINT8(uint8_t(Page::Deepseek), uint8_t(v.page));
    // The chord of both keys stands in for the middle button and returns to the home page.
    nav.input(Input::Home, v, 0);
    TEST_ASSERT_EQUAL_UINT8(uint8_t(Page::Home), uint8_t(v.page));
}

TEST_CASE("settings open on hold, cycle values and return to the previous page", "[nav]")
{
    Navigator nav;
    View v = linked_view();
    v.page = Page::Minimax;
    nav.input(Input::Hold, v, 0);
    TEST_ASSERT_EQUAL_UINT8(uint8_t(Page::Settings), uint8_t(v.page));
    TEST_ASSERT_EQUAL_INT(0, v.focus);
    publish(v, Provider::Deepseek, 96);
    // Only the MiniMax slot carries a quota warning, so the alert belongs to slot 1.
    v.warning = {0, 2};
    TEST_ASSERT_TRUE(nav.update_alert(v));
    TEST_ASSERT_EQUAL_INT(1, v.alert);
    // The right key steps the value of the focused row.
    TEST_ASSERT_TRUE(nav.input(Input::Forward, v, 0).save);
    TEST_ASSERT_EQUAL_UINT8(1, v.settings.cycle);
    // The chord moves the focus to the next row.
    nav.input(Input::Home, v, 0);
    nav.input(Input::Home, v, 0);
    TEST_ASSERT_EQUAL_INT(2, v.focus);
    TEST_ASSERT_TRUE(nav.input(Input::Forward, v, 0).save);
    TEST_ASSERT_EQUAL_UINT8(0, v.settings.seconds);
    nav.input(Input::Home, v, 0);
    TEST_ASSERT_EQUAL_INT(3, v.focus);
    TEST_ASSERT_TRUE(nav.input(Input::Forward, v, 0).save);
    TEST_ASSERT_EQUAL_UINT8(uint8_t(Rotation::Portrait), v.settings.rotation);
    nav.input(Input::Hold, v, 0);
    // Claude has no portrait page, so leaving Settings in portrait lands on Home.
    TEST_ASSERT_EQUAL_UINT8(uint8_t(Page::Home), uint8_t(v.page));
    // The alert still stands on the page behind the settings and a key dismisses it.
    TEST_ASSERT_EQUAL_INT(1, v.alert);
    nav.input(Input::Forward, v, 0);
    TEST_ASSERT_EQUAL_INT(-1, v.alert);
}

TEST_CASE("the left key steps a setting back through its options", "[nav]")
{
    Navigator nav;
    View v = linked_view();
    nav.input(Input::Hold, v, 0);
    nav.input(Input::Forward, v, 0);
    TEST_ASSERT_EQUAL_UINT8(1, v.settings.cycle);
    TEST_ASSERT_TRUE(nav.input(Input::Back, v, 0).save);
    TEST_ASSERT_EQUAL_UINT8(0, v.settings.cycle);
    // The first option wraps around to the last one.
    TEST_ASSERT_TRUE(nav.input(Input::Back, v, 0).save);
    TEST_ASSERT_EQUAL_UINT8(3, v.settings.cycle);
}

TEST_CASE("settings retry a failed save without changing the value", "[nav]")
{
    Navigator nav;
    View v = linked_view();
    nav.input(Input::Hold, v, 0);
    nav.input(Input::Forward, v, 0);
    TEST_ASSERT_EQUAL_UINT8(1, v.settings.cycle);
    v.save_error = true;
    TEST_ASSERT_TRUE(nav.input(Input::Forward, v, 0).save);
    TEST_ASSERT_EQUAL_UINT8(1, v.settings.cycle);
}

TEST_CASE("alert raises once, dismisses and re-arms below the threshold", "[alert]")
{
    Navigator nav;
    View v = linked_view();
    publish(v, Provider::Minimax, 96);
    // Only the MiniMax slot carries a quota warning.
    v.warning = {0, 2};
    TEST_ASSERT_TRUE(nav.update_alert(v));
    TEST_ASSERT_EQUAL_INT(1, v.alert);
    TEST_ASSERT_FALSE(nav.update_alert(v));
    v.link.encrypted = false;
    TEST_ASSERT_FALSE(nav.update_alert(v));
    TEST_ASSERT_EQUAL_INT(-1, v.alert);
    v.link.encrypted = true;
    TEST_ASSERT_FALSE(nav.update_alert(v));
    TEST_ASSERT_EQUAL_INT(1, v.alert);
    nav.input(Input::Forward, v, 0);
    TEST_ASSERT_FALSE(nav.update_alert(v));
    TEST_ASSERT_EQUAL_INT(-1, v.alert);
    // Dropping below level 2 re-arms the next raise.
    v.warning = {0, 1};
    nav.update_alert(v);
    v.warning = {0, 2};
    TEST_ASSERT_TRUE(nav.update_alert(v));
    TEST_ASSERT_EQUAL_INT(1, v.alert);
}

TEST_CASE("a hold keeps the alert on screen", "[alert]")
{
    Navigator nav;
    View v = linked_view();
    publish(v, Provider::Minimax, 96);
    v.warning = {0, 2};
    nav.update_alert(v);
    TEST_ASSERT_EQUAL_INT(1, v.alert);
    // The hold is the settings gesture, so it leaves the alert standing.
    nav.input(Input::Hold, v, 0);
    TEST_ASSERT_EQUAL_INT(1, v.alert);
    TEST_ASSERT_EQUAL_UINT8(uint8_t(Page::Home), uint8_t(v.page));
}

TEST_CASE("alert stays off when disabled or unlinked", "[alert]")
{
    Navigator nav;
    View v = linked_view();
    publish(v, Provider::Minimax, 97);
    v.warning = {0, 2};
    v.settings.alert = uint8_t(AlertMode::Off);
    TEST_ASSERT_FALSE(nav.update_alert(v));
    v.settings.alert = uint8_t(AlertMode::Invert);
    v.link.encrypted = false;
    TEST_ASSERT_FALSE(nav.update_alert(v));
    TEST_ASSERT_EQUAL_INT(-1, v.alert);
}

TEST_CASE("auto cycle advances after the interval and pauses in settings", "[cycle]")
{
    Navigator nav;
    View v = linked_view();
    v.settings.cycle = 1;
    nav.auto_cycle(v, 0);
    TEST_ASSERT_FALSE(nav.auto_cycle(v, 29999));
    TEST_ASSERT_TRUE(nav.auto_cycle(v, 30000));
    TEST_ASSERT_EQUAL_UINT8(uint8_t(Page::Deepseek), uint8_t(v.page));
    v.page = Page::Settings;
    TEST_ASSERT_FALSE(nav.auto_cycle(v, 90000));
}

TEST_CASE("a lyric line that fits never moves", "[lyrics]")
{
    // No overflow, no motion, however long the line stays on screen.
    for (uint64_t age : {0, 1400, 5000, 60000, 3600000})
        TEST_ASSERT_EQUAL_INT(0, lyric_scroll_offset(360, 0, age, 8000));
}

TEST_CASE("the scroll pace comes from the length of the text", "[lyrics]")
{
    // A 45 column line: 900 ms plus 100 ms a column is 5.4 s of motion after the 900 ms hold.
    const int total = 540;
    const int travel = 336; // the clock page's column
    TEST_ASSERT_EQUAL_INT(0, lyric_scroll_offset(total, travel, 0, 0));
    TEST_ASSERT_EQUAL_INT(0, lyric_scroll_offset(total, travel, 899, 0));
    // At half the motion the line is halfway through its columns.
    TEST_ASSERT_EQUAL_INT(168, lyric_scroll_offset(total, travel, 900 + 2700, 0));
    // It stops at the end and stays there: the same line is on screen for a whole interlude.
    for (uint64_t age : {900 + 5400, 20000, 30000, 600000})
        TEST_ASSERT_EQUAL_INT(336, lyric_scroll_offset(total, travel, age, 0));
    // The same words on the wider music row: same time, shorter distance, so slower motion.
    TEST_ASSERT_EQUAL_INT(180, lyric_scroll_offset(total, 180, 900 + 5400, 0));
}

TEST_CASE("the time the line has left may shorten the scroll, never stretch it", "[lyrics]")
{
    const int total = 540;
    const int travel = 336;
    // Eight seconds of line: the content pace wins, and the scroll ends well before it.
    TEST_ASSERT_EQUAL_INT(336, lyric_scroll_offset(total, travel, 6400, 8000));
    TEST_ASSERT_EQUAL_INT(336, lyric_scroll_offset(total, travel, 8000, 8000));
    // Four seconds: the budget wins, and the tail is on screen before the line changes. The
    // last frame the panel composes would otherwise fall short of it.
    TEST_ASSERT_TRUE(lyric_scroll_offset(total, travel, 3600, 4000) == 336);
    TEST_ASSERT_TRUE(lyric_scroll_offset(total, travel, 3000, 4000) < 336);
    // Two seconds: the hold gives way to the motion rather than the other way round.
    TEST_ASSERT_TRUE(lyric_scroll_offset(total, travel, 1600, 2000) == 336);
    // A line with almost nothing left still shows its end instead of never reaching it.
    TEST_ASSERT_TRUE(lyric_scroll_offset(total, travel, 100, 500) == 336);
}

TEST_CASE("the scroll ends a frame before the line does", "[lyrics]")
{
    // Whatever time the line has, the tail is reached at least 400 ms before the switch, which
    // is one composed frame: ending exactly at the switch means the last frame never shows it.
    for (uint32_t budget : {600u, 1100u, 2100u, 3100u, 8000u}) {
        const int at_end = lyric_scroll_offset(540, 336, budget - 400, budget);
        TEST_ASSERT_EQUAL_INT(336, at_end);
    }
}

TEST_CASE("the scroll steps whole text columns", "[lyrics]")
{
    for (uint64_t age = 0; age < 12000; age += 250) {
        TEST_ASSERT_EQUAL_INT(0, lyric_scroll_offset(540, 336, age, 0) % kLyricScrollColumn);
        TEST_ASSERT_EQUAL_INT(0, lyric_scroll_offset(540, 336, age, 4000) % kLyricScrollColumn);
    }
}

namespace {
/** One publication carrying a single lyric line. */
NowPlayingUpdate publication(const char *line) {
    NowPlayingUpdate update;
    snprintf(update.title, sizeof(update.title), "Song");
    update.playing = true;
    update.line_count = 1;
    snprintf(update.lines[0], sizeof(update.lines[0]), "%s", line);
    return update;
}

/** One publication that states the start of every line of its window. */
NowPlayingUpdate timed_publication(uint32_t position_ms, const uint32_t *starts, uint8_t count) {
    NowPlayingUpdate update;
    snprintf(update.title, sizeof(update.title), "Song");
    update.playing = true;
    update.line_count = count;
    update.position_ms = position_ms;
    update.has_position = true;
    update.line_time_count = count;
    for (uint8_t index = 0; index < count; ++index) {
        update.line_times_ms[index] = starts[index];
        snprintf(update.lines[index], sizeof(update.lines[index]), "line %u",
                 static_cast<unsigned>(index));
    }
    return update;
}
} // namespace

TEST_CASE("the scroll restarts only when the sung line changes", "[lyrics]")
{
    View v;
    apply_now_playing(v, publication("first line"), 1000);
    TEST_ASSERT_EQUAL_UINT64(1000, v.music_line_ms);
    TEST_ASSERT_TRUE(v.music_valid);

    // The Bridge republishes the same window every tick; the line keeps its place.
    apply_now_playing(v, publication("first line"), 5000);
    TEST_ASSERT_EQUAL_UINT64(1000, v.music_line_ms);

    apply_now_playing(v, publication("second line"), 9000);
    TEST_ASSERT_EQUAL_UINT64(9000, v.music_line_ms);

    // A window without lyrics is not a line either, so it restarts the next one.
    NowPlayingUpdate empty;
    empty.line_count = 0;
    apply_now_playing(v, empty, 12000);
    TEST_ASSERT_EQUAL_UINT64(12000, v.music_line_ms);
    TEST_ASSERT_FALSE(v.music.line_count);
}

TEST_CASE("the scroll follows the timeline of the song", "[lyrics]")
{
    // The song states that the line began 1.5 s ago; the panel shows it there, not where
    // the publication happened to arrive.
    const uint32_t starts[] = {10000, 12000, 15000};
    View v;
    apply_now_playing(v, timed_publication(11500, starts, 3), 1000);
    TEST_ASSERT_EQUAL_UINT64(1500, music_line_age_ms(v, 1000));
    // The panel clock carries the playhead on, so the age grows with the song.
    TEST_ASSERT_EQUAL_UINT64(1900, music_line_age_ms(v, 1400));
    // A paused song stands still.
    v.music.playing = false;
    TEST_ASSERT_EQUAL_UINT64(1500, music_line_age_ms(v, 1400));
}

TEST_CASE("the panel moves on to the next line by itself", "[lyrics]")
{
    const uint32_t starts[] = {10000, 12000, 15000};
    View v;
    apply_now_playing(v, timed_publication(11500, starts, 3), 1000);
    TEST_ASSERT_EQUAL_UINT8(0, music_current_line(v, 1000));
    // Half a second later the song has reached the next line, without a new publication.
    TEST_ASSERT_EQUAL_UINT8(1, music_current_line(v, 1500));
    // The panel never invents a place the source did not state: once the publications
    // stop, the playhead holds where the last three seconds left it.
    TEST_ASSERT_EQUAL_UINT8(1, music_current_line(v, 100000));
}

TEST_CASE("a new line is composed as soon as the song reaches it", "[lyrics]")
{
    const uint32_t starts[] = {10000, 12000, 15000};
    View v;
    apply_now_playing(v, timed_publication(11500, starts, 3), 1000);
    // The frame that just went out drew the first line.
    v.music_line_shown = music_current_line(v, 1000);
    TEST_ASSERT_FALSE(music_line_moved(v, 1300));
    // Half a second later the song is on the next line, which has not been drawn yet: the
    // frame that shows it must not wait for the quarter-second boundary.
    TEST_ASSERT_TRUE(music_line_moved(v, 1500));
    v.music_line_shown = music_current_line(v, 1500);
    TEST_ASSERT_FALSE(music_line_moved(v, 1500));
    // A song that was never published has nothing to move on to.
    View idle;
    TEST_ASSERT_FALSE(music_line_moved(idle, 1000));
}

TEST_CASE("a quiet link stops the playhead instead of walking on", "[lyrics]")
{
    // The Bridge publishes once a second. If it stops, the panel must hold the line it
    // last heard: carrying on with its own clock would walk the song's lyrics past the
    // place the listener is at, and the next frame would look many seconds ahead.
    const uint32_t starts[] = {10000, 12000, 15000};
    View v;
    apply_now_playing(v, timed_publication(11500, starts, 3), 1000);
    // Inside the window the playhead follows the panel clock.
    TEST_ASSERT_EQUAL_UINT64(14000, music_position_ms(v, 3500));
    // Past it the playhead stands where the last three seconds left it.
    TEST_ASSERT_EQUAL_UINT64(14500, music_position_ms(v, 11000));
    TEST_ASSERT_EQUAL_UINT64(14500, music_position_ms(v, 60000));
    TEST_ASSERT_EQUAL_UINT8(1, music_current_line(v, 60000));
}

TEST_CASE("a seek back moves the sung line back", "[lyrics]")
{
    // 愿与愁: sung at 113 s, then the listener seeks back to 103 s.
    const uint32_t before_seek[] = {106860, 112519, 119200, 125760, 130129, 134699};
    const uint32_t after_seek[] = {92530, 99950, 106860, 112519, 119200, 125760};
    View v;
    apply_now_playing(v, timed_publication(113000, before_seek, 6), 1000);
    TEST_ASSERT_EQUAL_UINT8(1, music_current_line(v, 1000));
    // Ten seconds of listening, with the Bridge publishing once a second.
    for (uint64_t now = 2000; now <= 11000; now += 1000)
        apply_now_playing(
            v, timed_publication(static_cast<uint32_t>(113000 + (now - 1000)), before_seek, 6),
            now);
    TEST_ASSERT_EQUAL_UINT8(2, music_current_line(v, 12000));
    // The publication that follows the seek names the place the song went back to.
    apply_now_playing(v, timed_publication(103000, after_seek, 6), 12000);
    TEST_ASSERT_EQUAL_UINT8(1, music_current_line(v, 12000));
    TEST_ASSERT_EQUAL_UINT64(3050, music_line_age_ms(v, 12000));
}

TEST_CASE("the length of the line comes from the timeline", "[lyrics]")
{
    const uint32_t starts[] = {10000, 12000, 15000};
    View v;
    apply_now_playing(v, timed_publication(11500, starts, 3), 1000);
    // The line lasts until the next one begins.
    TEST_ASSERT_EQUAL_UINT32(2000, music_line_budget_ms(v, 1000));
    TEST_ASSERT_EQUAL_UINT32(3000, music_line_budget_ms(v, 1500));
    // The last line of the window has no successor, so its length is unknown.
    View last;
    apply_now_playing(last, timed_publication(15000, starts, 3), 1000);
    TEST_ASSERT_EQUAL_UINT32(0, music_line_budget_ms(last, 100000));
}

TEST_CASE("a source without a timeline keeps the panel's own clock", "[lyrics]")
{
    View v;
    apply_now_playing(v, publication("first line"), 1000);
    TEST_ASSERT_EQUAL_UINT64(0, music_line_age_ms(v, 1000));
    TEST_ASSERT_EQUAL_UINT64(2000, music_line_age_ms(v, 3000));
    TEST_ASSERT_EQUAL_UINT8(0, music_current_line(v, 3000));
    TEST_ASSERT_EQUAL_UINT32(0, music_line_budget_ms(v, 3000));
}
