#include "esp_log.h"
#include "usage_panel/app_runtime.hpp"

namespace {
constexpr char kTag[] = "example_target";
constexpr char kAdvertisingPrefix[] = "QF-EX-";
// The identity this panel publishes: the log tag, the BLE name prefix, the device name the
// Bridge shows, the target id it reports and the project an OTA image has to carry.
constexpr usage_panel::PanelIdentity kIdentity{kTag, kAdvertisingPrefix, "Example Target",
                                               "example_target", "example_target"};

/**
 * The board, the display and the screens of this target behind the shared runtime.
 *
 * The shared runtime owns the BLE link, the OTA session, the view and the navigation; the
 * target owns the hardware and the screens. Replace the bodies with the real board:
 * begin brings the peripherals up, load and save reach the settings store, poll reads the
 * clock and the sensors, sync anchors the clock, key reports the debounced keys, render
 * composes the frame and beep plays the alert.
 */
class Target final : public usage_panel::PanelTarget {
  public:
    bool begin() override { return true; }
    usage_panel::Settings load() override { return {}; }
    bool save(const usage_panel::Settings &) override { return true; }
    void poll(usage_panel::View &) override {}
    bool sync(const usage_panel::LocalCalendarTime &) override { return false; }
    usage_panel::Input key(uint64_t) override { return usage_panel::Input::None; }
    void render(const usage_panel::View &) override {}
    void beep() override {}
};
} // namespace

extern "C" void app_main(void) {
    Target target;
    usage_panel::PanelRuntime runtime(target, kIdentity);
    runtime.run();
}
