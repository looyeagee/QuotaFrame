#include "usage_ble/sessions_mailbox.hpp"

namespace usage_panel {

bool SessionsMailbox::begin() {
    if (lock_)
        return true;
    lock_ = xSemaphoreCreateMutex();
    return lock_ != nullptr;
}

bool SessionsMailbox::publish(const SessionsUpdate& update) {
    if (!lock_)
        return false;
    if (xSemaphoreTake(lock_, portMAX_DELAY) != pdTRUE)
        return false;
    pending_ = update;
    has_pending_ = true;
    xSemaphoreGive(lock_);
    return true;
}

bool SessionsMailbox::take(SessionsUpdate& out) {
    if (!lock_)
        return false;
    if (xSemaphoreTake(lock_, portMAX_DELAY) != pdTRUE)
        return false;
    const bool has_update = has_pending_;
    if (has_update) {
        out = pending_;
        has_pending_ = false;
    }
    xSemaphoreGive(lock_);
    return has_update;
}

}  // namespace usage_panel
