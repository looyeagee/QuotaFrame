#pragma once

#include "esp_log.h"
#include "usage_panel/app_runtime.hpp"

namespace usage_panel {

/**
 * The wiring every panel target shares: the board, the display, the alert path and the screens.
 *
 * The target names its own types; the bodies below are the same on every board, so a change to
 * the wiring lands in this package instead of in every target.
 *
 * @tparam CanvasT The drawing surface of the panel.
 * @tparam BoardT The board: its peripherals, its keys and its settings store.
 * @tparam PanelT The display controller.
 * @tparam BeeperT The alert path.
 * @tparam ScreensT The screen layout.
 */
template <typename CanvasT, typename BoardT, typename PanelT, typename BeeperT, typename ScreensT>
class PanelTargetImpl : public PanelTarget {
  public:
    /**
     * @param identity The identity the panel publishes; its tag names the target in the logs.
     * @param no_audio_warning What the log says when the alert path cannot be brought up.
     */
    PanelTargetImpl(const PanelIdentity &identity, const char *no_audio_warning)
        : identity_(identity), no_audio_warning_(no_audio_warning) {}

    bool begin() override {
        if (!board_.begin())
            return false;
        if (!panel_.begin())
            return false;
        // The alert path is optional: a panel without a speaker still shows the alert.
        if (!beeper_.begin(board_))
            ESP_LOGW(identity_.tag, "%s", no_audio_warning_);
        return true;
    }
    Settings load() override { return board_.load(); }
    bool save(const Settings &settings) override { return board_.save(settings); }
    void poll(View &view) override { board_.poll(view); }
    bool sync(const LocalCalendarTime &time) override { return board_.sync(time); }
    PanelInput read_input(uint64_t now_ms) override { return board_.read_input(now_ms); }
    SessionsGeometry sessions_geometry() const override { return screens_.sessions_geometry(); }
    void render(const View &view) override {
        screens_.render(view);
        panel_.show(canvas_);
    }
    void beep() override { beeper_.alert(); }
    void apply_brightness(uint8_t percent) override { panel_.set_brightness(percent); }

  private:
    PanelIdentity identity_;
    const char *no_audio_warning_;
    BoardT board_;
    BeeperT beeper_;
    PanelT panel_;
    CanvasT canvas_;
    ScreensT screens_{canvas_};
};

/**
 * Brings the panel up and runs it.
 *
 * The target and the runtime live in static storage: the drawing surface and the view model
 * are larger than the stack of the task that calls app_main, so they cannot be locals there.
 */
template <typename CanvasT, typename BoardT, typename PanelT, typename BeeperT, typename ScreensT>
void run_panel(const PanelIdentity &identity, const char *no_audio_warning) {
    static PanelTargetImpl<CanvasT, BoardT, PanelT, BeeperT, ScreensT> target(identity,
                                                                              no_audio_warning);
    static PanelRuntime runtime(target, identity);
    runtime.run();
}

} // namespace usage_panel
