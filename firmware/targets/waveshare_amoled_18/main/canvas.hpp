#pragma once
#include "usage_panel/canvas.hpp"
#include <cstdint>
namespace usage_panel::amoled {
/**
 * The byte order the SH8601 latches pixels in.
 *
 * The controller reads each pixel high byte first off the QSPI lines, while the canvas holds
 * little-endian `uint16_t`. Every routine of the surface stores a palette value verbatim, so
 * declaring the palette already swapped makes a finished frame the exact byte stream the glass
 * expects: no per-frame shuffle, and one place where the order is stated rather than implied.
 */
constexpr uint16_t wire_order(uint16_t rgb565) {
    return static_cast<uint16_t>((rgb565 << 8) | (rgb565 >> 8));
}
/**
 * The inks this glass paints, in RGB565, in the order `Palette` declares them.
 *
 * Near black instead of pure black for the ink: on an AMOLED pure black is an unlit pixel,
 * and text at that value reads as a hole in the layout rather than as a stroke. Amber marks
 * a session that is working, a cool grey carries the counts and hairlines, and green marks
 * one that is up and waiting. Red marks a session that waits for its reader, which is the
 * one state that spends the reader's time rather than its own. Read as colours these are
 * #12181D, #FFFFFF, #FF9E1B, #66727C, #00B37E and #E5484D.
 */
constexpr Palette kPalette{wire_order(0x10C3), wire_order(0xFFFF), wire_order(0xFCE3),
                           wire_order(0x638F), wire_order(0x058F), wire_order(0xE249)};

/**
 * The drawing surface of the 368 x 448 colour panel.
 *
 * The glass takes RGB565, so this surface keeps a whole frame in that form and the panel
 * sends it as it stands: no expansion pass, and every ink is a colour on the glass. A frame
 * is 322 KiB, more than internal RAM holds, so the buffer is reserved on the first `begin`
 * rather than at construction: static constructors run before the point where PSRAM is
 * certainly usable.
 */
class Canvas : public usage_panel::CanvasSurface {
  public:
    static constexpr int kNativeWidth = 368, kNativeHeight = 448;
    static constexpr int kPixels = kNativeWidth * kNativeHeight;

    Canvas();
    /** Clears the frame to the orientation's paper colour, reserving it on first use. */
    void begin(uint8_t rotation, bool black_background = false);

  private:
    uint16_t *frame_ = nullptr;
    bool reserve();
};
} // namespace usage_panel::amoled
