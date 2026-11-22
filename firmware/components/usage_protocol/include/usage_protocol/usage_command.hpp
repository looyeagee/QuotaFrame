#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_desktop_buddy/esp_desktop_buddy.h"
#include "usage_core/usage_state.hpp"

namespace usage_panel {

/** Outcome of reading one command field. */
enum class FieldResult { Missing, Valid, Invalid };

/**
 * Reads one decimal uint32 field in its canonical text form.
 *
 * Leading zeros, signs, non-digits and values above UINT32_MAX are rejected,
 * so every command sees the same wire contract.
 */
FieldResult get_decimal_u32(
    const esp_desktop_buddy_command_view_t* request,
    const char* key, uint32_t& value);

/**
 * Copies `text` into `out` without splitting a UTF-8 character.
 *
 * A truncated character would break the decoder of the display, so the copy
 * stops at the last complete character that fits.
 */
void copy_utf8_truncated(char* out, size_t out_size, const char* text);

/** Result of decoding one usage version 1 command. */
struct UsageCommandParseResult {
    bool valid = false;
    uint32_t sequence = 0;
    UsageUpdate update{};
};

/**
 * Parses the privacy-allowlisted usage version 1 wire contract.
 *
 * Decimal integers must use their canonical text form, percentages must be
 * 0-100, reset timestamps may only accompany their corresponding windows,
 * `balance_cents` requires a `currency` of at most three characters, and state
 * must match the number of values present (windows and balance together).
 * sent_at may precede sampled_at by at most five minutes to tolerate skew.
 */
UsageCommandParseResult parse_usage_command(
    const esp_desktop_buddy_command_view_t* request);

}  // namespace usage_panel
