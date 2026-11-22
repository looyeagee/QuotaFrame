#include "beeper.hpp"
#include "board.hpp"
#include "canvas.hpp"
#include "screens.hpp"
#include "st7789.hpp"
#include "usage_panel/panel_target.hpp"

using namespace usage_panel;
using namespace usage_panel::zltech;

namespace {
constexpr char kTag[] = "zltech154";
constexpr char kAdvertisingPrefix[] = "QF-ZLT-154-";
// The identity this panel publishes: the log tag, the BLE name prefix, the device name the
// Bridge shows, the target id it reports, the project an OTA image has to carry, and whether
// the target draws the sessions page. The backlight is on a dimmer, so the brightness setting
// stays: `dimmable` takes its default.
constexpr PanelIdentity kIdentity{kTag, kAdvertisingPrefix, "ZLTech 1.54 TFT", "zltech_154tft",
                                  "zltech_154tft", true};
// This board has no codec or amplifier wired up in this target; the alert stays silent.
constexpr char kNoAudio[] = "no audio path; alerts stay silent";
} // namespace

extern "C" void app_main() {
    run_panel<Canvas, Board, St7789, Beeper, Screens>(kIdentity, kNoAudio);
}
