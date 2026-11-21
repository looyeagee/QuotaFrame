#pragma once
#include "usage_core/now_playing.hpp"
#include "usage_core/usage_state.hpp"
#include <array>
#include <cstdint>
// The view and renderer depend only on usage_core so the screens can be rendered on a host.
namespace usage_panel::rlcd {
/** Pages in navigation order; Settings sits outside the KEY ring. */
enum class Page : uint8_t { Home, Deepseek, Minimax, Music, Clock, Settings };

enum class Rotation : uint8_t { Landscape, Portrait, Flipped };
enum class AlertMode : uint8_t { Off, Invert, Beep };

/** Persisted display settings; values index the option lists shown on the Settings page. */
struct Settings {
    uint8_t cycle = 0;    // Auto page cycle: OFF, 30 s, 1 min, 5 min.
    uint8_t alert = 2;    // AlertMode.
    uint8_t seconds = 1;  // 1 shows seconds on countdowns and the clock.
    uint8_t rotation = 0; // Rotation.
};
constexpr int kSettingRows = 4;

struct LinkState {
    bool connected = false, encrypted = false, has_passkey = false;
    uint32_t passkey = 0;
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
    int focus = 0;
    bool save_error = false;
    std::array<uint8_t, 2> warning{}; // Per provider: 0 normal, 1 >= 80 %, 2 >= 95 %.
    int alert = -1;                   // Provider index whose full-screen alert is shown.
    OtaStatus ota;
    uint64_t now_ms = 0;
    char device_name[24]{};
};
} // namespace usage_panel::rlcd
