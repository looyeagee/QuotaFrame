#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "usage_core/now_playing.hpp"

namespace usage_panel {

/**
 * Latest-value mailbox for the now-playing publication.
 *
 * A song is a snapshot and not a stream of events: a newer publication
 * replaces the previous one, and the application loop reads it when it next
 * renders. The lock keeps the writer (the Bluetooth task) and the reader (the
 * display loop) from seeing a half-written publication.
 */
class NowPlayingMailbox {
public:
    /** Allocates the lock. Returns false when allocation fails. */
    bool begin();
    /** Stores one publication; false when the mailbox is not ready. */
    bool publish(const NowPlayingUpdate& update);
    /** Takes the pending publication; false when nothing new arrived. */
    bool take(NowPlayingUpdate& out);

private:
    SemaphoreHandle_t lock_ = nullptr;
    NowPlayingUpdate pending_{};
    bool has_pending_ = false;
};

}  // namespace usage_panel
