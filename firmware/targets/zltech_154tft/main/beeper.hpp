#pragma once
namespace usage_panel::zltech {
/**
 * Alert tone placeholder for this board.
 *
 * The speaker is an ES8311 codec behind an I2S DAC and an I2C control bus, and this target
 * does not bring either up, so the beep setting is persisted and shown but stays silent.
 * Keeping the type lets the application loop and the Settings page stay identical to the
 * other maintained targets.
 */
class Beeper {
  public:
    bool begin();
    void alert();
    bool ready() const { return false; }
};
} // namespace usage_panel::zltech
