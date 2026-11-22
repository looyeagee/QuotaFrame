#pragma once
#include "usage_panel/canvas.hpp"
#include <cstdint>
namespace usage_panel::zltech {
/**
 * The inks this glass paints, in RGB565, in the order `Palette` declares them.
 *
 * The same five inks as the AMOLED target, so a page reads the same on both panels: near black
 * for text, white paper, amber for a session that is working, a cool grey for counts and
 * hairlines, green for one that is up and waiting. Read as colours these are #12181D, #FFFFFF,
 * #FF9E1B, #66727C and #00B37E.
 *
 * No byte swap is needed here: the controller is put in little-endian pixel order, which is the
 * order a `uint16_t` already has in memory.
 */
constexpr Palette kPalette{0x10C3, 0xFFFF, 0xFCE3, 0x638F, 0x058F};

/**
 * The drawing surface of the 240 x 240 colour panel.
 *
 * A frame is 112 KiB, more than the board can spare from internal RAM once NimBLE has its
 * buffers, so it lives in PSRAM and is reserved on the first `begin`: static constructors run
 * before the point where PSRAM is certainly usable.
 */
class Canvas : public usage_panel::CanvasSurface {
  public:
    static constexpr int kNativeWidth = 240, kNativeHeight = 240;
    static constexpr int kPixels = kNativeWidth * kNativeHeight;

    Canvas();
    /** Clears the frame to the orientation's paper colour, reserving it on first use. */
    void begin(uint8_t rotation, bool black_background = false);

  private:
    uint16_t *frame_ = nullptr;
    bool reserve();
};
} // namespace usage_panel::zltech
