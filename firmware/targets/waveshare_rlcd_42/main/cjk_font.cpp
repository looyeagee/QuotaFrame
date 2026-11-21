#include "cjk_font.hpp"

#include <cstring>

#include "cjk_font_data.hpp"

namespace usage_panel::rlcd {
namespace {

constexpr int kGlyphPixels = 12;
// Twelve rows of twelve pixels, two bytes per row.
constexpr int kBytesPerGlyph = 2 * kGlyphPixels;
// ASCII keeps the five columns and one blank of the panel's own font, so a Latin word or a
// number occupies the cells it always did; every ASCII glyph of the table fits that cell.
constexpr int kAsciiCell = 6;
/** Columns a covered character is drawn in. */
int cell_columns(uint32_t codepoint) {
    return codepoint < 0x80 ? kAsciiCell : kGlyphPixels;
}

/** Reads the next UTF-8 character; an invalid sequence becomes U+FFFD. */
uint32_t next_codepoint(const unsigned char *&p) {
    const unsigned char first = *p++;
    if (first < 0x80)
        return first;
    int extra = 0;
    uint32_t codepoint = first;
    if ((first & 0xE0) == 0xC0) {
        extra = 1;
        codepoint = first & 0x1Fu;
    } else if ((first & 0xF0) == 0xE0) {
        extra = 2;
        codepoint = first & 0x0Fu;
    } else if ((first & 0xF8) == 0xF0) {
        extra = 3;
        codepoint = first & 0x07u;
    } else {
        return 0xFFFD;
    }
    for (int index = 0; index < extra; ++index) {
        if ((*p & 0xC0) != 0x80)
            return 0xFFFD;
        codepoint = (codepoint << 6) | (*p++ & 0x3Fu);
    }
    return codepoint;
}

/** Advance of one character, or zero when nothing is drawn in its place. */
int advance_of(uint32_t codepoint, int scale) {
    return cell_columns(codepoint) * scale;
}

}  // namespace

int CjkText::lookup(uint32_t codepoint) {
    int low = 0;
    int high = cjk_data::kCjkGlyphCount - 1;
    while (low <= high) {
        const int middle = (low + high) / 2;
        const uint16_t candidate = cjk_data::kCjkCodepoints[middle];
        if (candidate == codepoint)
            return middle;
        if (candidate < codepoint)
            low = middle + 1;
        else
            high = middle - 1;
    }
    return -1;
}

int CjkText::width(const char *text, int scale) {
    int total = 0;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p;) {
        const uint32_t codepoint = next_codepoint(p);
        // A control character carries no glyph and takes no room.
        if (codepoint >= 0x20)
            total += advance_of(codepoint, scale);
    }
    return total;
}

void CjkText::fit(char *out, size_t out_size, const char *text, int max_width, int scale) {
    if (!out || out_size == 0)
        return;
    out[0] = '\0';
    if (!text || !*text)
        return;
    // U+2026 HORIZONTAL ELLIPSIS marks a line that had to be cut.
    constexpr char kEllipsis[] = "\xE2\x80\xA6";
    if (width(text, scale) <= max_width) {
        size_t length = 0;
        while (text[length] && length + 1 < out_size)
            ++length;
        std::memcpy(out, text, length);
        out[length] = '\0';
        return;
    }
    const int ellipsis = width(kEllipsis, scale);
    int used = 0;
    size_t cursor = 0, kept = 0;
    while (text[cursor]) {
        const unsigned char *p =
            reinterpret_cast<const unsigned char *>(text) + cursor;
        const uint32_t codepoint = next_codepoint(p);
        const size_t next =
            static_cast<size_t>(reinterpret_cast<const char *>(p) - text);
        const int advance = codepoint >= 0x20 ? advance_of(codepoint, scale) : 0;
        if (used + advance + ellipsis > max_width)
            break;
        used += advance;
        cursor = next;
        kept = next;
    }
    if (kept + sizeof(kEllipsis) > out_size)
        kept = out_size > sizeof(kEllipsis) ? out_size - sizeof(kEllipsis) : 0;
    std::memcpy(out, text, kept);
    std::memcpy(out + kept, kEllipsis, sizeof(kEllipsis));
}

int CjkText::draw(Canvas &canvas, int x, int top, const char *text, int scale, Ink ink) {
    int pen = x;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p;) {
        const uint32_t codepoint = next_codepoint(p);
        // A control character carries no glyph and takes no room.
        if (codepoint < 0x20)
            continue;
        const int columns = cell_columns(codepoint);
        const int cell = columns * scale;
        const int index = lookup(codepoint);
        if (index < 0) {
            // An empty box marks a character the font does not cover. An ASCII character the
            // table lacks keeps its cell blank, so the rest of the line stays in place.
            if (codepoint >= 0x80)
                canvas.frame(pen + scale, top + scale, cell - 2 * scale, cell - 2 * scale,
                             scale, ink);
            pen += cell;
            continue;
        }
        const uint8_t *glyph = cjk_data::kCjkGlyphData + index * kBytesPerGlyph;
        for (int row = 0; row < kGlyphPixels; ++row) {
            const int bits = (glyph[row * 2] << 8) | glyph[row * 2 + 1];
            for (int column = 0; column < columns; ++column) {
                if (bits & (1 << (kGlyphPixels - 1 - column)))
                    canvas.fill(pen + column * scale, top + row * scale, scale, scale, ink);
            }
        }
        pen += cell;
    }
    return pen - x;
}

}  // namespace usage_panel::rlcd
