#include "usage_ble/now_playing_mailbox.hpp"

namespace usage_panel {

bool NowPlayingMailbox::begin() {
    if (lock_)
        return true;
    lock_ = xSemaphoreCreateMutex();
    return lock_ != nullptr;
}

bool NowPlayingMailbox::publish(const NowPlayingUpdate& update) {
    if (!lock_)
        return false;
    if (xSemaphoreTake(lock_, portMAX_DELAY) != pdTRUE)
        return false;
    pending_ = update;
    has_pending_ = true;
    xSemaphoreGive(lock_);
    return true;
}

bool NowPlayingMailbox::take(NowPlayingUpdate& out) {
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
