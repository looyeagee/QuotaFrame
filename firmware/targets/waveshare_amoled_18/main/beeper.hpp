#pragma once
#include "board.hpp"
/**
 * Alert tone placeholder for this board.
 *
 * The speaker is an ES8311 codec behind an I2S interface, and this target does not bring
 * either up, so the beep setting is persisted and shown but stays silent. Keeping the type
 * lets the application loop and the Settings page stay identical to the other targets.
 */
namespace usage_panel::amoled {
class Beeper {
  public:
    /** There is no audio path on this board, so there is nothing to bring up. */
    bool begin(const Board &board);
    void alert();
    bool ready() const { return false; }
};
} // namespace usage_panel::amoled
