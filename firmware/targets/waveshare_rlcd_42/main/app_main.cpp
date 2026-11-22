#include "beeper.hpp"
#include "board.hpp"
#include "canvas.hpp"
#include "screens.hpp"
#include "st7305.hpp"
#include "usage_panel/panel_target.hpp"

using namespace usage_panel;
using namespace usage_panel::rlcd;

namespace {
constexpr char kTag[] = "rlcd42";
constexpr char kAdvertisingPrefix[] = "QF-WS-S3-R42-";
// The identity this panel publishes: the log tag, the BLE name prefix, the device name the
// Bridge shows, the target id it reports and the project an OTA image has to carry.
constexpr PanelIdentity kIdentity{kTag, kAdvertisingPrefix, "Waveshare RLCD 4.2", "waveshare_rlcd_42",
                                  "ws_rlcd_42"};
// The codec of this board hangs off the board's I2C bus; without it the alert stays silent.
constexpr char kNoAudio[] = "ES8311 unavailable; alerts stay silent";
} // namespace

extern "C" void app_main() {
    run_panel<Canvas, Board, St7305, Beeper, Screens>(kIdentity, kNoAudio);
}
