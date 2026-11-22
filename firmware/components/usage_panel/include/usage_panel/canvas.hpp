#pragma once
#include "usage_panel/view.hpp"
#include <cstdint>
namespace usage_panel {
/**
 * What a primitive or glyph paints with.
 *
 * A one-bit surface has paper and ink and nothing between, so every ink but `White` is
 * black on it. A colour surface resolves each of them against its palette, which is how a
 * page marks a state without carrying colour types through the layout code.
 */
enum class Ink : uint8_t { Black, White, Accent, Muted, Good };

/** The colours a colour surface paints the inks with, in RGB565. */
struct Palette {
    uint16_t ink = 0x0000;    // Primary text and strokes.
    uint16_t paper = 0xFFFF;  // Background.
    uint16_t accent = 0xFD20; // A state that is doing something.
    uint16_t muted = 0x8410;  // Secondary text: counts, captions, hairlines.
    uint16_t good = 0x0640;   // A state that is healthy and waiting.
};

/**
 * A drawing surface for a panel.
 *
 * Coordinates are logical: the native width and height in landscape and flipped landscape,
 * and the two swapped in portrait.
 *
 * A surface holds either a one-bit buffer, row-major, MSB first, 1 = ink, or an RGB565 one
 * of the same geometry, which is what a colour glass sends without expanding anything. The
 * drawing routines are the same for both: they differ only in what a pixel is worth.
 *
 * Text uses a built-in 5 x 7 monospaced pixel font drawn at an integer `scale`, so every stroke
 * is `scale` pixels wide and falls on the pixel grid. Each character occupies a 6 x 7 cell
 * (one blank column); the line box is 7 * scale pixels high.
 */
class CanvasSurface {
  public:
    CanvasSurface() = default;

    /** Clears to black or white and selects the logical orientation for later drawing. */
    void begin(uint8_t rotation, bool black_background = false);
    int width() const { return width_; }
    int height() const { return height_; }
    const uint8_t *data() const { return bits_; }
    /** The RGB565 buffer, or null on a one-bit surface. */
    const uint16_t *color_data() const { return pixels_; }
    bool color() const { return pixels_ != nullptr; }
    void set_palette(const Palette &palette) { palette_ = palette; }

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
     * Draws one seven-segment character in a `w` x `h` box with `t`-pixel segments.
     *
     * This is the only way to draw a single character: `segments` walks a string, so handing it
     * the address of a lone `char` makes it read past that byte until it happens to meet a zero
     * and paint whatever it finds on the way. A layout that positions each cell itself calls
     * this instead, and can move by `char_width(c, w, t)`.
     * @return the cell width of `c` in pixels.
     */
    int glyph(int x, int top, char c, int w, int h, int t, Ink ink = Ink::Black);
    /**
     * Draws seven-segment digits in a `w` x `h` box per digit with `t`-pixel segments.
     *
     * Accepted characters: `0`-`9`, `-` (middle segment), space (blank digit), `h` (blank half
     * digit), `H` (half digit showing 1), `:` and `.` (square dots `t` wide). As on a printed
     * segment LCD every digit occupies a fixed cell, so a `1` lights the right-hand segments of
     * its cell; half digits and dots are one segment wide. Digits are separated by `t / 2 + 2`
     * pixels. Unlit segments leave the background unchanged.
     *
     * `s` must be zero-terminated; use `glyph` for a single character.
     * @return total width in pixels.
     */
    int segments(int x, int top, const char *s, int w, int h, int t, Ink ink = Ink::Black);
    /** Cell width of one seven-segment character, including a trailing blank column. */
    static int char_width(char c, int w, int t);
    /** Blank pixels `segments` leaves between two characters. */
    static int digit_gap(int t);
    static int segments_width(const char *s, int w, int t);

  protected:
    ~CanvasSurface() = default;

    /** Points the surface at a buffer of `native_width` x `native_height` pixels. */
    void attach(uint8_t *buffer, int native_width, int native_height);
    /**
     * Points a colour surface at its RGB565 buffer, which the caller owns: a full frame does
     * not fit in internal RAM, so the target decides where it lives.
     */
    void attach_color(uint16_t *pixels, int native_width, int native_height);

  private:
    uint8_t *bits_ = nullptr;
    uint16_t *pixels_ = nullptr;
    Palette palette_{};
    int native_width_ = 0, native_height_ = 0;
    uint8_t rotation_ = 0;
    int width_ = 0, height_ = 0;
    int clip_x_ = 0, clip_y_ = 0, clip_w_ = 0, clip_h_ = 0;
    uint16_t color_for(Ink ink) const;
    void segment(int left, int top, int w, int h, int t, int index, Ink ink);
};

/**
 * A canvas that owns the buffer of a panel of `Width` x `Height` pixels.
 *
 * The geometry of a panel is a property of its glass, so a target names it once and every
 * drawing routine works on the same surface.
 */
template <int Width, int Height>
class SizedCanvas : public CanvasSurface {
  public:
    static constexpr int kNativeWidth = Width;
    static constexpr int kNativeHeight = Height;
    static constexpr int kBytes = kNativeWidth * kNativeHeight / 8;

    SizedCanvas() { attach(bits_, kNativeWidth, kNativeHeight); }

  private:
    uint8_t bits_[kBytes]{};
};
} // namespace usage_panel
