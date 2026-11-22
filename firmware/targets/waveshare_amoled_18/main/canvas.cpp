#include "canvas.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
namespace usage_panel::amoled {
namespace {
constexpr char kTag[] = "canvas";
} // namespace

Canvas::Canvas() { set_palette(kPalette); }

bool Canvas::reserve() {
    if (frame_ != nullptr)
        return true;
    frame_ = static_cast<uint16_t *>(
        heap_caps_malloc(kPixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (frame_ == nullptr) {
        ESP_LOGE(kTag, "no PSRAM for a %d px colour frame", kPixels);
        return false;
    }
    attach_color(frame_, kNativeWidth, kNativeHeight);
    return true;
}

void Canvas::begin(uint8_t rotation, bool black_background) {
    // Without a buffer the surface keeps a zero-sized clip, so a page draws nothing instead
    // of writing through a null pointer.
    if (!reserve())
        return;
    CanvasSurface::begin(rotation, black_background);
}
} // namespace usage_panel::amoled
