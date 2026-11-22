#include "beeper.hpp"
#include "board.hpp"
#include "canvas.hpp"
#include "screens.hpp"
#include "sh8601.hpp"
#include "usage_panel/panel_target.hpp"

using namespace usage_panel;
using namespace usage_panel::amoled;

namespace {
constexpr char kTag[] = "wsamoled18";
constexpr char kAdvertisingPrefix[] = "QF-WS-AMO18-";
// The identity this panel publishes: the log tag, the BLE name prefix, the device name the
// Bridge shows, the target id it reports, the project an OTA image has to carry, and whether
// this glass renders the sessions page.
constexpr PanelIdentity kIdentity{kTag, kAdvertisingPrefix, "Waveshare AMOLED 1.8",
                                  "waveshare_amoled_18", "ws_amoled_18", true};
// The ES8311 codec is present but not brought up in this target; the alert stays silent.
constexpr char kNoAudio[] = "no audio path; alerts stay silent";
} // namespace

extern "C" void app_main() {
    run_panel<Canvas, Board, Sh8601, Beeper, Screens>(kIdentity, kNoAudio);
}
