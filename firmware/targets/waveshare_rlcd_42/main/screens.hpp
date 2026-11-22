#pragma once
#include "canvas.hpp"
#include "usage_panel/panel_logic.hpp"
#include "usage_panel/view.hpp"
namespace usage_panel::rlcd {
/** Composes one complete frame for a view. Pure drawing; no hardware access. */
class Screens {
  public:
    explicit Screens(Canvas &canvas) : c_(canvas) {}
    void render(const View &view);

  private:
    Canvas &c_;
    bool portrait_ = false;
    /** Page title (or the time), link status chip, page position and battery level above a rule. */
    void header(const View &v, const char *title, int page = -1);
    /**
     * Draws the battery level as a percentage right-aligned at `right`. @return the left edge of
     * the text, so a caller can stack another element beside it.
     */
    int battery(const View &v, int right, Ink ink = Ink::Black);
    /**
     * Inverted pixel-font label with rounded corners. The text is placed at (`x`, `top`) and
     * the chip extends 4 px around it. @return text width.
     */
    int chip(int x, int top, const char *s, Ink ink = Ink::Black);
    /**
     * 2 px outlined bar filled with up to twenty 5 % cells inset by 2 px; a negative `percent`
     * leaves it empty. `marker` is the elapsed fraction of the window, or negative to omit it.
     */
    void bar(int x, int y, int w, int h, int percent, float marker = -1.f);
    /**
     * Seven-segment percentage `h` pixels high in a fixed field: a hundreds half digit and two
     * right-aligned digit cells, then a percent sign of the same stroke weight at a fixed
     * position. A negative `percent` shows dashes. @return field width.
     */
    int big_percent(int x, int top, int percent, int h, Ink ink = Ink::Black);
    static int big_percent_width(int h);
    void home(const View &v);
    /** DeepSeek row: the account balance in segment digits. */
    void balance_row(const View &v, int top);
    /** MiniMax row: the five-hour and weekly bars. */
    void windows_row(const View &v, int top);
    void home_portrait(const View &v);
    void focus(const View &v, int provider);
    /**
     * Portrait detail pages: the DeepSeek balance and the MiniMax windows, stacked down
     * the narrow column of the turned panel.
     */
    void focus_portrait(const View &v, int provider);
    void alert(const View &v);
    /**
     * Draws the room readings on the date row as one group after `x`.
     *
     * The temperature and the humidity keep one gap each, after the date and between the
     * two values. The gap shrinks to `min_gap` when the status chip leaves little room, and
     * the humidity is dropped when even the tightest spacing does not fit before `limit`.
     */
    void room_readings(const View &v, int x, int limit, int preferred_gap, int min_gap);
    void clock(const View &v);
    /**
     * Draws the song being played, the lyric line being sung, and up to `rows` lines from it.
     *
     * The song and its artist share one row at `top` so the lyric rows below fit a narrow
     * column. The line being sung is inverted, like the lyric window on the music page.
     */
    void now_playing_summary(const View &v, int x, int top, int width, int rows);
    /**
     * Draws the line being sung inside `width` starting at `x`.
     *
     * A line that fits is drawn as it is; a wider one scrolls through the room the row gives
     * it, clipped to that room so it cannot reach the rows above or below. The clip and the
     * offset both fall on text columns, which keeps every glyph whole.
     */
    void lyric_row(const View &v, int x, int top, int width, const char *line, Ink ink);
    /** Draws a lyric line above or below the sung one: fitted to `width`, with an ellipsis. */
    void context_row(int x, int top, int width, const char *line, Ink ink);
    /** Music page: the song, its artist, the playhead and the lyric window. */
    void music(const View &v);
    /** Portrait music page: the same blocks over the taller lyric window of the turned panel. */
    void music_portrait(const View &v);
    void settings(const View &v);
    void pairing(const View &v);
    void ota(const View &v);
};
} // namespace usage_panel::rlcd
