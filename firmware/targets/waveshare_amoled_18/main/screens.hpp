#pragma once
#include "canvas.hpp"
#include "usage_panel/panel_logic.hpp"
#include "usage_panel/view.hpp"
namespace usage_panel::amoled {
/**
 * Composes one complete frame for a view. Pure drawing; no hardware access.
 *
 * The glass is natively 368 x 448 portrait, so the pages are laid out for one 336 px column
 * inside the bezel. Upper blocks sit at fixed tops and lower blocks (the footer, the page
 * dots and the MiniMax block on the home page) anchor to the panel height, so the three
 * rotation settings all render inside the glass: rotation only turns the finished image on
 * the glass, the logical layout stays one column.
 */
class Screens {
  public:
    explicit Screens(Canvas &canvas) : c_(canvas) {}
    void render(const View &view);

  private:
    Canvas &c_;
    /** Page title (or the time), link chip and battery level above a rule. */
    void header(const View &v, const char *title);
    /** Draws the battery level as a percentage right-aligned at `right`. */
    int battery(const View &v, int right, Ink ink = Ink::Black);
    /** Inverted pixel-font label with rounded corners. @return text width. */
    int chip(int x, int top, const char *s, Ink ink = Ink::Black);
    /**
     * 2 px outlined bar filled with the used percentage; a negative `percent` leaves it
     * empty. `marker` is the elapsed fraction of the window, or negative to omit it.
     */
    void bar(int x, int y, int w, int h, int percent, float marker = -1.f);
    /** Seven-segment percentage figure in a fixed field. @return field width. */
    int big_percent(int x, int top, int percent, int h, Ink ink = Ink::Black);
    static int big_percent_width(int h);
    /** The page ring as squares centred above the bottom edge. */
    void page_dots(const View &v, int page);
    void home(const View &v);
    /** DeepSeek page: the account balance in segment digits. */
    void balance_page(const View &v);
    /** MiniMax page: the five-hour window above the weekly window. */
    void windows_page(const View &v);
    void alert(const View &v);
    void clock(const View &v);
    /** Draws the song being played and the lyric window below its playhead. */
    void music(const View &v);
    /**
     * Sessions page: the live count in the state colour, then one card per running
     * session with its state bar, its name and its state word. Sessions that have
     * ended are not drawn; the page is about what is running now.
     */
    void sessions(const View &v);
    /** Draws the line being sung inside `width` starting at `x`. */
    void lyric_row(const View &v, int x, int top, int width, const char *line, Ink ink);
    /** Draws `line` fitted into `width` with an ellipsis when it has to be cut. */
    void context_row(int x, int top, int width, const char *line, Ink ink);
    void settings(const View &v);
    void pairing(const View &v);
    void ota(const View &v);
};
} // namespace usage_panel::amoled
