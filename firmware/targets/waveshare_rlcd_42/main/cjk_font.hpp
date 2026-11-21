#pragma once

#include <cstddef>
#include <cstdint>

#include "canvas.hpp"

namespace usage_panel::rlcd {

/**
 * Text drawing that covers Chinese as well as ASCII.
 *
 * The panel's built-in font only has ASCII glyphs, so the music page draws
 * its titles and its lyric lines with a 12 x 12 bitmap font. Latin characters
 * keep using the built-in font and are aligned to the bottom of the line, so a
 * line that mixes scripts reads like one line. That font draws ASCII letters in
 * uppercase only, so lowercase letters arrive as their uppercase glyphs.
 */
class CjkText {
  public:
    /** Height of one line at `scale`, in pixels. */
    static constexpr int line_height(int scale) { return kGlyphSize * scale; }
    /** Width of `text` at `scale`, in pixels. */
    static int width(const char *text, int scale);
    /**
     * Copies `text` into `out` so that it fits `max_width` pixels at `scale`.
     *
     * A text that does not fit is cut at a character boundary and marked with an
     * ellipsis, so a long lyric line never runs into the panel edge.
     */
    static void fit(char *out, size_t out_size, const char *text, int max_width, int scale);
    /**
     * Draws `text` with its line top at `top`.
     * @return the advance width in pixels.
     */
    static int draw(Canvas &canvas, int x, int top, const char *text, int scale,
                    Ink ink = Ink::Black);

  private:
    static constexpr int kGlyphSize = 12;
    /** Index of `codepoint` in the font tables, or -1 when it is not covered. */
    static int lookup(uint32_t codepoint);
};

}  // namespace usage_panel::rlcd
