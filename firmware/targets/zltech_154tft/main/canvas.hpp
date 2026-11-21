#pragma once
#include <cstdint>
namespace usage_panel::zltech {
/** How a primitive or glyph sets its pixels. */
enum class Ink : uint8_t { Black, White };

/**
 * One-bit drawing surface for the 240 x 240 panel.
 *
 * The panel is square, so every orientation is 240 x 240 and the buffer is stored in
 * native order, row-major, MSB first, 1 = black. Rotation only turns the drawn image on
 * the panel; it does not change the logical coordinate system the screens are laid out in.
 *
 * Text uses a built-in 5 x 7 monospaced pixel font drawn at an integer `scale`, so every
 * stroke is `scale` pixels wide and falls on the pixel grid. Each character occupies a
 * 6 x 7 cell (one blank column); the line box is 7 * scale pixels high.
 */
class Canvas {
  public:
    static constexpr int kNativeWidth = 240, kNativeHeight = 240;
    static constexpr int kBytes = kNativeWidth * kNativeHeight / 8;

    /** Clears to black or white and selects the orientation later drawing lands in. */
    void begin(uint8_t rotation, bool black_background = false);
    int width() const { return kNativeWidth; }
    int height() const { return kNativeHeight; }
    const uint8_t *data() const { return bits_; }

    void pixel(int x, int y, Ink ink);
    void fill(int x, int y, int w, int h, Ink ink = Ink::Black);
    void frame(int x, int y, int w, int h, int thickness, Ink ink = Ink::Black);
    /** Horizontal dotted rule: `t` x `t` squares separated by `t` pixels. */
    void dotted(int x, int y, int w, int t = 2, Ink ink = Ink::Black);

    /**
     * Restricts later drawing to a rectangle, which is how a line of text is kept inside the
     * room a row gives it. `reset_clip` restores the whole panel; `begin` starts without one.
     */
    void clip(int x, int y, int w, int h);
    void reset_clip();

    /**
     * Draws pixel-font text with its cell top at `top`. ASCII letters are drawn in uppercase;
     * characters without a glyph are skipped. @return advance width in pixels.
     */
    int text(int x, int top, const char *s, int scale, Ink ink = Ink::Black);
    /** Draws text right-aligned so its last glyph column ends at `right` - 1. */
    int text_right(int right, int top, const char *s, int scale, Ink ink = Ink::Black);
    /** Width of the drawn text without the trailing blank column. */
    static int text_width(const char *s, int scale);

    /**
     * Draws seven-segment digits in a `w` x `h` box per digit with `t`-pixel segments.
     *
     * Accepted characters: `0`-`9`, `-` (middle segment), space (blank digit), `h` (blank half
     * digit), `H` (half digit showing 1), `:` and `.` (square dots `t` wide). As on a printed
     * segment LCD every digit occupies a fixed cell, so a `1` lights the right-hand segments of
     * its cell; half digits and dots are one segment wide. Digits are separated by `t / 2 + 2`
     * pixels. Unlit segments leave the background unchanged.
     * @return total width in pixels.
     */
    int segments(int x, int top, const char *s, int w, int h, int t, Ink ink = Ink::Black);
    static int segments_width(const char *s, int w, int t);

  private:
    uint8_t bits_[kBytes]{};
    uint8_t rotation_ = 0;
    int clip_x_ = 0, clip_y_ = 0, clip_w_ = kNativeWidth, clip_h_ = kNativeHeight;
    void segment(int left, int top, int w, int h, int t, int index, Ink ink);
};
} // namespace usage_panel::zltech
