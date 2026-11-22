#pragma once
#include "canvas.hpp"
#include "usage_panel/panel_logic.hpp"
#include "usage_panel/view.hpp"
namespace usage_panel::zltech {
/**
 * Composes one complete frame for a view. Pure drawing; no hardware access.
 *
 * The panel is a 240 x 240 square, so there is no landscape/portrait layout split: every page
 * is laid out for one column of 220 px inside the bezel, and the rotation setting only turns
 * the finished image on the glass. Pages that were two-column on the wider panels stack their
 * blocks instead.
 */
class Screens {
  public:
    explicit Screens(Canvas &canvas) : c_(canvas) {}
    void render(const View &view);

  private:
    Canvas &c_;
    /** Page title (or the time), link chip and battery level above a rule. */
    void header(const View &v, const char *title);
    /**
     * Draws the battery level as a percentage right-aligned at `right`. @return the left edge of
     * the text, so a caller can stack another element beside it.
     */
    int battery(const View &v, int right, Ink ink = Ink::Black);
    /**
     * Inverted pixel-font label with rounded corners, drawn at `scale`. The text is placed at
     * (`x`, `top`) and the chip extends around it by `chip_pad`. @return text width.
     */
    int chip(int x, int top, const char *s, Ink ink = Ink::Black, int scale = 2);
    /** Glass a chip takes around its text at `scale`. */
    static int chip_pad(int scale);
    /** Height of a chip at `scale`, text box included. */
    static int chip_height(int scale);
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
    /** The page ring as dots centred under the content. */
    void page_dots(const View &v, int page);
    /**
     * The sessions page: the count leading the page, then one card per running session with a
     * state bar, the name, and the state word in a chip, as on the AMOLED target.
     */
    void sessions(const View &v);
    void home(const View &v);
    /** DeepSeek page: the account balance in segment digits. */
    void balance_page(const View &v);
    /** MiniMax page: the five-hour window above the weekly window. */
    void windows_page(const View &v);
    void alert(const View &v);
    void clock(const View &v);
    /**
     * Draws the song being played, the lyric line being sung, and the lines after it.
     *
     * The song and its artist share the two rows above the playhead, and the lyrics fill the
     * window below it.
     */
    void music(const View &v);
    /**
     * Draws the line being sung inside `width` starting at `x`.
     *
     * A line that fits is drawn as it is; a wider one scrolls through the room the row gives
     * it, clipped to that room so it cannot reach the rows above or below. The clip and the
     * offset both fall on text columns, which keeps every glyph whole.
     */
    void lyric_row(const View &v, int x, int top, int width, const char *line, Ink ink);
    /** Draws `line` fitted into `width` with an ellipsis when it has to be cut. */
    void context_row(int x, int top, int width, const char *line, Ink ink);
    void settings(const View &v);
    void pairing(const View &v);
    void ota(const View &v);
};
} // namespace usage_panel::zltech
