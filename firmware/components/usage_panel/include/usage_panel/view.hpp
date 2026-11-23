#pragma once
#include "usage_core/now_playing.hpp"
#include "usage_core/sessions.hpp"
#include "usage_core/usage_state.hpp"
#include <array>
#include <cstdint>
// The view and renderer depend only on usage_core so the screens can be rendered on a host.
namespace usage_panel {
/** Pages in navigation order; Settings sits outside the page ring. */
enum class Page : uint8_t { Home, Deepseek, Minimax, Sessions, Music, Clock, Settings };

enum class Rotation : uint8_t { Landscape, Portrait, Flipped };
enum class AlertMode : uint8_t { Off, Invert, Beep };

/** Persisted display settings; values index the option lists shown on the Settings page. */
struct Settings {
    uint8_t cycle = 0;      // Auto page cycle: OFF, 30 s, 1 min, 5 min.
    uint8_t alert = 2;      // AlertMode.
    uint8_t seconds = 1;    // 1 shows seconds on countdowns and the clock.
    uint8_t rotation = 0;   // Rotation.
    uint8_t brightness = 80; // Display brightness in percent; every target clamps it.
};
constexpr int kSettingRows = 5;
/** Rows of a panel without a dimmer: every row above, without the trailing brightness one. */
constexpr int kSettingRowsNoDimmer = kSettingRows - 1;

/** Brightness steps the Settings page cycles through; the Bridge may set any percent. */
constexpr uint8_t kBrightnessLevels[] = {20, 40, 60, 80, 100};
constexpr int kBrightnessLevelCount = 5;

struct LinkState {
    bool connected = false, encrypted = false, has_passkey = false;
    uint32_t passkey = 0;
};

/**
 * The geometry of a panel's sessions list.
 *
 * The list is drawn as a window of the roster, so the panel that draws it knows how tall
 * one row is and how many rows the glass holds. The scroll offset is counted in pixels of
 * that row height, which is what lets the list move with the finger.
 */
struct SessionsGeometry {
    int row_height = 0;   // Pixels of one row; 0 when the panel draws no scrollable list.
    int visible_rows = 0; // Rows the glass holds at once.
};

enum class OtaStage : uint8_t { Idle, Confirming, Receiving, Verifying, Rebooting };
struct OtaStatus {
    OtaStage stage = OtaStage::Idle;
    uint32_t offset = 0, size = 0;
    char version[24]{};
    const char *error = nullptr; // Title of a recent failure while idle; null when none.
};

/** Local wall-clock time read from the RTC. */
struct ClockTime {
    bool valid = false;
    uint16_t year = 0;
    uint8_t month = 0, day = 0, weekday = 0, hour = 0, minute = 0, second = 0;
};

struct View {
    UsageModel model;
    LinkState link;
    ClockTime clock;
    NowPlayingUpdate music;
    bool music_valid = false; // Whether a song was ever published.
    // Whether a sessions publication ever arrived. The page ring keeps the
    // sessions page out of reach until then, so a panel whose Bridge sends no
    // sessions never shows an empty page.
    bool sessions_valid = false;
    SessionsUpdate sessions;
    // The geometry of the sessions list this panel draws. A panel that publishes no
    // geometry has no scrollable list.
    SessionsGeometry sessions_geometry;
    // How far the sessions list has moved up, in pixels. The offset counts pixels, so
    // the list follows the finger instead of jumping a row at a time.
    int sessions_scroll_px = 0;
    uint64_t music_line_ms = 0; // When the line being sung began showing, for its scroll.
    // Milliseconds the source says the line had left when it appeared; 0 when it cannot say.
    // A scroll is shortened to fit inside it, never stretched to fill it. Only used when the
    // source states no timeline of its own.
    uint32_t music_line_budget_ms = 0;
    // Panel clock when the publication that carries the playhead arrived; the playhead is
    // advanced from there with the panel clock until the next publication arrives.
    uint64_t music_position_at_ms = 0;
    // Line the last composed frame drew as the one being sung. The panel advances the line
    // between publications, so a frame is composed as soon as this falls behind the song.
    uint8_t music_line_shown = 0;
    bool environment_valid = false;
    float temperature = 0, humidity = 0;
    int battery = -1; // Percent, or -1 when unknown.
    Page page = Page::Home;
    Settings settings;
    // Rows the Settings page of this panel offers. A panel without a dimmer
    // drops the trailing brightness row, and the focus walks the rows it has.
    uint8_t setting_rows = kSettingRows;
    int focus = 0;
    bool save_error = false;
    std::array<uint8_t, 2> warning{}; // Per provider: 0 normal, 1 >= 80 %, 2 >= 95 %.
    int alert = -1;                   // Provider index whose full-screen alert is shown.
    OtaStatus ota;
    uint64_t now_ms = 0;
    char device_name[24]{};

    /**
     * The distance the sessions list may scroll: the part of the roster the glass
     * cannot show at once. A panel that draws no scrollable list cannot scroll.
     */
    int sessions_scroll_limit() const {
        const int hidden = static_cast<int>(sessions.live_count) - sessions_geometry.visible_rows;
        return hidden > 0 ? hidden * sessions_geometry.row_height : 0;
    }
};
} // namespace usage_panel
