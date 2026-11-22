#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "usage_core/sessions.hpp"

namespace usage_panel {

/**
 * Latest-value mailbox for the sessions publication.
 *
 * Session state is a snapshot and not a stream of events: a newer publication
 * replaces the previous one, and the application loop reads it when it next
 * renders. The lock keeps the writer (the Bluetooth task) and the reader (the
 * display loop) from seeing a half-written publication.
 */
class SessionsMailbox {
public:
    /** Allocates the lock. Returns false when allocation fails. */
    bool begin();
    /** Stores one publication; false when the mailbox is not ready. */
    bool publish(const SessionsUpdate& update);
    /** Takes the pending publication; false when nothing new arrived. */
    bool take(SessionsUpdate& out);

private:
    SemaphoreHandle_t lock_ = nullptr;
    SessionsUpdate pending_{};
    bool has_pending_ = false;
};

}  // namespace usage_panel
