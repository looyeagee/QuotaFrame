// Host stand-in for the target's canvas. The real one reserves its frame in PSRAM through
// esp_heap_caps, which does not exist off the chip; a static buffer draws the same picture.
#include "canvas.hpp"
namespace usage_panel::amoled {
namespace {
uint16_t g_frame[Canvas::kPixels];
} // namespace

Canvas::Canvas() { set_palette(kPalette); }

void Canvas::begin(uint8_t rotation, bool black_background) {
    attach_color(g_frame, kNativeWidth, kNativeHeight);
    CanvasSurface::begin(rotation, black_background);
}
} // namespace usage_panel::amoled
