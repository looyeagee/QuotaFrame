# ESP32-S3-RLCD-4.2

Target: `waveshare_rlcd_42`; application: `ws_rlcd_42`.
ESP-IDF: v6.1. Flash: 16 MB; PSRAM: 8 MB (octal).

```text
eim run "idf.py -C firmware/targets/waveshare_rlcd_42 build" v6.1
eim run "idf.py -C firmware/targets/waveshare_rlcd_42 -p <PORT> flash" v6.1
```

Evidence level: **partial hardware**. The firmware, `test_apps/logic` and the protocol test
app compile with ESP-IDF v6.1; `ws_rlcd_42.bin` is 0xcfb30 bytes with 80 % of the app partition
free. On one board, the landscape Home and detail pages rendered live usage from a bonded
Bridge, which reconnected with an encrypted link after flashing. The music page has so far been
verified only in the host preview and by the unit tests that compose its frame: a device has not
drawn a published song, and the Chinese glyphs have not been read on the panel. The portrait
pages of the five-page ring are composed from the same view model and covered by the host
preview and the logic tests, but a panel has not shown them yet: portrait and flipped layouts,
the key handling, sensors, audio, first-time pairing and OTA have not been exercised on
hardware.

The board's three buttons are KEY (GPIO18), PWR and BOOT (GPIO0). PWR switches the power
circuit and reaches no GPIO, so the firmware reads two keys: KEY on the left, BOOT on the right.
A press of KEY steps back, a press of BOOT steps forward, the chord of both keys stands in for
the middle button, and a key held for 800 ms opens the settings page. The logic test image runs
the whole board logic suite on the chip: 45 cases, all passing, including the three that cover
the portrait pages.

The device advertises as `QF-WS-S3-R42-` followed by the final two Bluetooth MAC bytes and
reports `waveshare_rlcd_42` in the authenticated status. Usage publication, RTC
synchronization, screen toggle/page commands and OTA reuse the shared BLE components.

## Display

The panel renders two services: DeepSeek publishes an account balance, MiniMax
publishes its five-hour and weekly quota windows.

The 4.2-inch reflective LCD (ST7305) shows 400 x 300 black-and-white pixels without a backlight.
It has no refresh waveform and no ghosting, so every change is a full-frame SPI write of
15,000 bytes (about 12 ms at 10 MHz). Frames are composed every 250 ms and sent only when they
differ from the frame on the panel, so countdowns and the clock tick every second. The panel
scans at about 16 Hz along the landscape x axis; each write waits for the TE rising edge (GPIO6)
so it stays ahead of the scan instead of leaving a vertical seam.

At 120 PPI one pixel is about 0.21 mm and stays visible, so the screens follow the look of a
printed segment LCD instead of smoothing vector type:

- Chinese text (song titles, artists, lyric lines) uses a 12 x 12 bitmap font drawn at 2x, so
  a mixed line keeps Latin characters in the built-in font on the same baseline. The font is
  generated from the Fusion Pixel 12px font (OFL-1.1); see [glyphs and preview](#glyphs-and-preview).
- Figures (usage, clock, pairing code, OTA progress) are seven-segment digits.
  As on a printed segment LCD, every digit has a fixed cell and a `1` lights the right half of
  its cell. Percentages use a fixed field of a hundreds half digit and two right-aligned cells,
  with the percent sign a third of a digit after it.
- All other text uses a built-in 5 x 7 pixel font at 2x (labels, 14 px cap height, 2 px strokes)
  or 3x (countdowns on the detail and alert pages). Integer scaling keeps every stroke on the
  pixel grid.
- Bars are a 2 px outline holding up to twenty solid 5 % cells.
- No dither patterns are used: on this panel ordered dither shows as coarse dark streaks, not
  grey, and makes unlit segments or empty bar cells compete with the data.
- Primary rules are solid, secondary separators dotted; a 2 px bezel frame sits 4 px inside the
  panel edge. Chips (link status, focused setting, pace) are inverted with rounded corners.

The display labels the short window `5H` for MiniMax and used-percentage meaning as the other targets:

| Page | Content |
| --- | --- |
| Home | DEEPSEEK row with the account balance in 68 px segment digits and its currency; MINIMAX row with the 5H bar (used percentage, reset countdown, pace marker) and the WEEK bar |
| Deepseek | Balance in 96 px segment digits with the currency and the sample age |
| Minimax | One service: the 5H window in 80 px figures with its RESET countdown, then the 5H and WEEK sections each with a usage bar, elapsed-time marker and elapsed time. OVER +N% marks usage at least 5 percentage points above the elapsed fraction of the window |
| Music | The song and its artist in 24 px CJK text, the playhead with elapsed and total time, then the line being sung inverted above the next four lines; PAUSED marks a stopped player |
| Sessions | The running Claude Code sessions: a BUSY chip or a plain IDLE beside each name under a summary line counting them |
| Clock | Time with seconds; the date, the SHTC3 temperature and the humidity share the top row. Below the dotted rule the MiniMax 5H and WEEK bars keep the left column, and the right column carries the DEEPSEEK caption with its balance on the same row, then the song being played above the line being sung and the line after it |
| Settings | Auto cycle, alert mode, seconds, rotation |
| Alert | Full-screen inverted page once the MiniMax 5H window reaches 95 %; NEAR LIMIT below 100 %, EXHAUSTED at 100 % |

The board keeps no local history: it has no SD-card trend log and no trend page.

Text is at least 2x (14 px, 2.9 mm cap height) and strokes at least 2 px in both orientations.
Layout follows one grid: content starts 14 px from the panel edge, the first line sits 10 px
below the header rule, rows and footers are separated by 10 px, and inverted chips grow around
their text so it stays on the same left edge as plain labels. Home uses 64 px figures level with
the top of the 5H bar and the bottom of the WEEK bar; detail uses 80 px figures. On Home the countdown sits to the right of its window
label without a RESET prefix; the detail page labels it RESET. The header shows a chip only for
OFFLINE, PAIRING and UPDATING; LINKED is the normal state and is not shown. Battery level is
2x text at the right end of the top row, such as `75%`, and `--%` while the level is unknown.

Offline pages retain the last values at full contrast with their sample age and the OFFLINE
header chip, but show RESET -- rather than a live countdown. Missing windows also show --;
RESET WAIT is reserved for a known reset time that has passed. On Home, a service with neither
window collapses to NO DATA (offline) or WAITING FOR DATA (linked). Pace and time markers
assume fixed windows of five hours and seven days because usage.v1 carries only reset times,
which matches the MiniMax intervals. The DeepSeek row has no window and no pace marker. They are omitted offline, for missing windows, or when
the remaining time does not fit the assumed window.

The Clock page has no header rule: the date, the temperature and the humidity share its top
row, the temperature and the humidity in 2x text so the reading stays compact. The readings
shrink their gaps when the OFFLINE chip takes the top-right corner, and the humidity is
dropped when even the tightest spacing does not fit. In portrait the chip moves to the bottom
of the page, because the narrow width leaves the top row to the date and the readings.

Rotation is manual (the board has no accelerometer): LAND, PORT (landscape turned 90 degrees
clockwise) or FLIP. Portrait keeps the same pages as landscape: every page has a portrait
layout, so the page ring is the same in both orientations. The portrait column is 272 px wide,
so the detail pages stack their blocks instead of putting them side by side, the balance
figure sits between its caption and the account note, and the music page draws seven lyric
rows under the playhead, which is the whole window the Bridge sends.

The music page only fills while a Bridge publishes `nowplaying.v1`: the Bridge reads the song
from the NetEase Cloud Music client on the computer and sends a window of lyric lines with the
one being sung at its head, so the panel shows lyrics without holding a whole song. The page
draws five lyric rows under the playhead in landscape and seven in portrait, and the Bridge
sends six lines after the sung one:
four fill the landscape rows below the sung line and two are slack, because the panel moves on
to the next line on the song's own timeline rather than waiting for the next publication; the
portrait window holds all seven. A line may
carry up to 192 bytes, so a Chinese lyric of sixty-four
characters arrives whole; the row shows about fifteen of them in landscape and eleven in
portrait, and scrolls the rest. A device that never received a song shows WAITING FOR DATA
(linked) or NO MUSIC (offline); a song with no lyrics keeps the title, the artist and the
playhead and shows NO LYRICS below the rule.

The Clock page shows the same song in a summary block: the balance shares the DeepSeek caption
row so the space below it carries the title and artist on one row, then the line being sung
inverted above the line after it. Two lyric rows at the music scale are all the block holds, so
a lyric line has nine glyphs in landscape and eleven in portrait; PAUSED marks a stopped player
in the label scale, which keeps six of those glyphs for the song. The block falls back to
NO MUSIC, WAITING FOR DATA or NO LYRICS on the same rows.

Only the line being sung scrolls; the lines below it stay put and keep the trailing ellipsis of
a line that does not fit, because they are context rather than the words being sung. A sung line
that does not fit holds still for up to 0.9 s, then moves right to left until its whole tail fits,
and stays there until the next line replaces it. The pace comes from the text: 0.9 s plus 0.1 s per
text column of the line, counted over the whole line rather than over the distance the row has
to scroll, so a narrow row scrolls faster instead of longer and the same words take the same
time on the music page and on the clock page.

The Bridge sends the playhead and the start of every line of the window in milliseconds
(`position_ms` and `line_times`), because the lyric file states its timestamps that precisely.
The panel places the song on that timeline: it advances the playhead with its own clock between
two publications, moves on to the next line when the song reaches it, composes a frame at that
moment instead of waiting for the quarter-second frame boundary, and scrolls the sung line at
the age the song states. A source that sends only seconds keeps the panel on the publication
clock, and the line then changes at most one publication late.

The Bridge also sends `line_end`, the position at which the line stops, and the panel uses it
only as an upper bound: it shortens the scroll to finish inside the line when the line is short,
and ignores it otherwise. A scroll always ends 0.4 s before the line does, because a frame composed
every 250 ms would otherwise never show the last one. That is what an instrumental interlude needs — a line followed by thirty
seconds of nothing has minutes to spare, so the content pace wins, the scroll finishes and
rests, and nothing crawls across the gap. The scroll stops on a character boundary with the last
character whole and flush against the end of the row, so neither edge of a moving line ever
shows half a glyph. A paused player holds the line at its first word instead of scrolling away.
The parameters and the reasoning behind each of them are in
[歌词滚动算法](../../../docs/design/lyric-scroll.md).

The window the Bridge sends is rebuilt around the sung line every time it changes, so the page
always has one line of context above the highlight and four below it.

The Sessions page shows the Claude Code sessions running on the computer. The Bridge reads
`~/.claude/sessions` on the host and sends the display name of every running session with its
state; a change reaches the panel as soon as Claude writes it. A busy session carries an inverted
BUSY chip, an idle one a plain IDLE; the summary line counts how many sessions run and how many
are busy. The page holds four running sessions, the same bound the Bridge applies. Sessions that
have ended are not drawn. Only the display names and the busy flag travel: session ids, working
directories, git branches and the conversations themselves stay on the computer.

The page is only in the ring once a publication has arrived, so a panel whose Bridge sends no
sessions never shows an empty page. A panel that received a publication but has no live session
shows WAITING FOR DATA (linked) or NO SESSIONS (offline).

## Controls

- KEY (GPIO18), left of the case: previous page.
- BOOT (GPIO0), right of the case: next page, HOME → DEEPSEEK → MINIMAX → MUSIC → CLOCK (portrait: HOME ↔ CLOCK). The Sessions page sits between MINIMAX and MUSIC once it has data.
- Both keys together: the home page, and the next settings row while the settings page is open. The
  PWR button between them switches the power circuit and reaches no GPIO, so the chord of the two
  keys takes over the action of the middle button.
- Hold either key for 800 ms: open Settings; hold again to leave.
- Settings: the left and right keys change the value of the focused row and save it to NVS, the
  chord of both keys moves the focus to the next row. The reflective panel has no backlight, so
  the page holds four rows and stops at rotation; the panel therefore declares no
  `brightness.set.v1` capability.
- Alert: any key dismisses it; it returns after usage drops below 93 % and reaches 95 % again.
- Pairing: any key requests a new code.
- OTA confirmation: any key confirms, holding a key denies. Navigation is blocked during OTA.

Auto cycle advances the ring every 30 s, 1 min or 5 min; any key press restarts the interval.
The Bridge screen-toggle command switches to and from Clock; page commands step the ring.

## Alerts

`ALERT 95%` selects OFF, PAGE (full-screen page) or +BEEP (page plus three 2 kHz tones
through the ES8311 codec and speaker amplifier). The same three values are the `alert_set`
modes of the `alert.set.v1` capability, so the Bridge control page sets the same persisted
field the Settings page writes; `status.alert` reports it back. The tone plays once when an
alert is raised.
Reconnecting restores an undismissed alert without repeating its tone. A provider's alert
re-arms after its warning level falls below 2 (usage below 93% with hysteresis).
The codec is configured for DAC-only slave operation at 16 kHz; its register values are derived
from Espressif `esp_codec_dev` 1.3.5 (Apache-2.0); fixed source links, notices and local adaptations
are recorded in [UPSTREAM.md](UPSTREAM.md).

## Peripherals

| Function | Connection |
| --- | --- |
| ST7305 (SPI3) | MOSI 12, SCLK 11, DC 5, CS 40, RESET 41, TE 6 |
| I2C | SDA 13, SCL 14: PCF85063 0x51, SHTC3 0x70, ES8311 0x18 |
| Keys | KEY 18, BOOT 0, active low; PWR switches the power circuit and reaches no GPIO |
| Battery | GPIO4 / ADC1 channel 3 through a 1:3 divider |
| Speaker | I2S MCLK 16, BCLK 9, WS 45, DOUT 8; amplifier enable 46 |

The ST7305 initialization sequence and pixel layout come from the Waveshare
ESP32-S3-RLCD-4.2 factory program; see [driver provenance](UPSTREAM.md).
The battery percentage is a linear voltage proxy between
3.0 V and 4.12 V; the board has no fuel gauge, so the OTA battery gate is
not registered (the voltage cannot distinguish battery from USB power).

The firmware shows no charging state. The ETA6098 `STAT` pin drives the CHG LED only and `VBUS`
does not reach the MCU, so no GPIO or PMIC register exposes "charging"; the green CHG LED is the
only accurate indication and it goes dark when the charge terminates. Software inference was
rejected: a voltage trend lags by the sampling window and cannot tell a charger from USB power,
so the display would sometimes contradict the LED.

## Hardware verification checklist

- Panel orientation and pixel order (text reads correctly in LAND, PORT and FLIP).
- KEY / BOOT debounce, the chord of both keys and the 800 ms hold.
- SHTC3 readings against a reference; Waveshare's example subtracts 4 °C for board
  self-heating, which this firmware does not apply until measured.
- Battery percentage on battery and on USB power.
- The CHG LED against a real charger: it should light during the charge and go dark at full.
- ES8311 tone level and amplifier noise.
- Pairing, reconnect, usage rendering with a real Bridge, and OTA with physical confirmation.

## Glyphs and preview

The pixel font and the seven-segment digits are defined in the shared package, in
`firmware/components/usage_panel/src/canvas.cpp`. Chinese text needs a font of its own, so
`firmware/components/usage_panel/src/cjk_font.cpp` draws a generated 12 x 12 bitmap font:
`tools/rlcd42/gen_cjk_font.py` rasterises the Fusion Pixel 12px font (OFL-1.1) into
`firmware/components/usage_panel/src/cjk_font_data.cpp`, which holds 7024 glyphs (printable ASCII, GB2312 level 1 and 2, and
the full-width punctuation) as twelve rows of twelve pixels each, about 180 KB of flash. ASCII
is drawn from that table in the six pixel cell of the built-in font, so a Latin word or a number
keeps the width it always had while sitting on the baseline of the hanzi beside it; the built-in
5 x 7 font still draws the panel's labels and the playhead figures. The license text ships
next to the target as [FONT_LICENSE.txt](FONT_LICENSE.txt).

`tools/rlcd42/render_preview.cpp` renders every screen on a host into PBM images using the
design's sample data; the build command is in the file header.
