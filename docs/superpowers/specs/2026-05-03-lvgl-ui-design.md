# LVGL UI Design — Waveshare ESP32-S3-Touch-LCD-4B (480x480)

## Overview

Text-only, full-screen-state LVGL UI for the intercom/VA device. Hybrid architecture: system overlay pages are separate LVGL pages; main content (idle, VA, intercom, weather) lives on one `main_page` with mutually exclusive groups. Weather is accessible via horizontal swipe (tileview) from idle.

Language: Russian.

## Board

- Display: 480x480 ST7701 RGB
- Touch: GT911 capacitive
- RTC: PCF85063 (I2C 0x51) for clock persistence across reboots
- Fonts: Figtree, Roboto, Roboto Mono (Google Fonts), MaterialDesignIcons

## Fonts

| ID | Family | Weight | Size | Glyphs | Purpose |
|----|--------|--------|------|--------|---------|
| `font_text` | Figtree | 300 | 20 | GF_Latin_Core + Cyrillic | Body text, hints, STT/TTS text |
| `font_title` | Figtree | 600 | 30 | GF_Latin_Core + Cyrillic | Headings, peer name |
| `font_state` | Roboto | 500 | 36 | GF_Latin_Core + Cyrillic | VA state labels |
| `font_info` | Roboto | 400 | 24 | GF_Latin_Core + Cyrillic | Weather, secondary info |
| `font_btn` | Figtree | 600 | 22 | GF_Latin_Core + Cyrillic | Button labels |
| `font_clock` | Roboto Mono | 400 | 48 | GF_Latin_Core + digits `: ` | Large clock on idle |
| `font_icon` | MDI | — | 48 | Weather glyphs | Weather icons |
| `font_icon_sm` | MDI | — | 24 | WiFi + weather glyphs | Small icons (wifi, forecast) |

## Color Palette

| Element | Hex | Description |
|---------|-----|-------------|
| Background | `0x0D1117` | Dark blue (GitHub dark) |
| Text primary | `0xE6EDF3` | Near-white |
| Text secondary | `0x8B949E` | Gray |
| Text hint | `0x484F58` | Dark gray |
| VA listening | `0x2ECC71` | Green |
| VA thinking | `0xF39C12` | Orange |
| VA replying | `0x58A6FF` | Blue |
| VA error | `0xE74C3C` | Red |
| Button action | `0x2ECC71` | Green (call, answer) |
| Button danger | `0xE74C3C` | Red (hangup, decline) |
| Button nav | `0x21262D` | Dark gray (prev, next) |
| IC ringing bg | `0x3D0000` | Dark red |
| IC outgoing bg | `0x1A1000` | Dark amber |
| IC in call bg | `0x002200` | Dark green |

## LVGL Page Architecture

### System Overlay Pages (separate `lv_disp_load_scr` targets)

1. **`initializing_page`** — Black bg, centered "Загрузка..." in gray (`0x8B949E`), `font_state`. Shown at boot.

2. **`no_wifi_page`** — Black bg, "Нет WiFi" in red (`0xE74C3C`), `font_state`. "Ожидание подключения..." in gray, `font_text`.

3. **`no_ha_page`** — Black bg, "Нет Home Assistant" in orange (`0xF39C12`), `font_state`. "Ожидание API..." in gray, `font_text`.

4. **`timer_finished_page`** — Black bg, "Таймер!" in orange (`0xF39C12`), `font_state`. Button "Стоп" (200x60, red bg, white text, `font_btn`), `on_click`: `switch.turn_off: timer_ringing`.

### Main Page (`main_page`)

Single page containing all operational UI. Layout:

```
┌──────────────────────────────────────────┐
│ top_bar (height 40px, transparent bg)    │  ← always visible
│  HH:MM      peer_name    ☀+22°   📶    │
├──────────────────────────────────────────┤
│ content area (440x440)                    │
│                                          │
│  tileview (idle_tile ↔ weather_tile)     │  ← shown in idle state
│  OR                                      │
│  VA/IC group (one visible at a time)     │  ← shown during activity
│                                          │
└──────────────────────────────────────────┘
```

**Visibility logic:** Exactly one of {tileview, va_listening_group, va_thinking_group, va_replying_group, va_error_group, va_muted_group, ic_ringing_in_group, ic_outgoing_group, ic_in_call_group} is visible at any time.

### Tileview

- `top_tileview`: 460x400, centered below top_bar
- Two tiles, horizontal scroll:
  - **`idle_tile`** (col 0): Peer name label (`font_title`, white, width 280, centered), three buttons at bottom:
    - "< Пред" (nav bg, `on_click`: `button.press: prev_contact_button`)
    - "ВЫЗОВ" (green bg, `on_click`: `button.press: call_button`)
    - "След >" (nav bg, `on_click`: `button.press: next_contact_button`)
    - Hint label below: "Нажмите для голосового" (`font_text`, hint color)
    - Touch area (transparent, full tile): `on_click` → `voice_assistant.start`/`voice_assistant.stop` toggle
  - **`weather_tile`** (col 1): Weather icon (`font_icon`, gold), temperature (`font_state`, white), location (`font_text`, gray), condition (`font_text`, secondary), humidity+wind (`font_text`, gray), 5-day forecast row (day name + icon + max/min temp)

### VA Groups (hidden by default)

Each group: full content area size (460x400), transparent bg, centered content.

**`va_listening_group`**: "СЛУШАЮ..." label (`font_state`, green). Hint "Нажмите чтобы остановить" (gray). Touch → `voice_assistant.stop`.

**`va_thinking_group`**: STT text label (WRAP, `font_text`, white, width 420). "ДУМАЮ..." label (`font_state`, orange). Hint. Touch → `voice_assistant.stop`.

**`va_replying_group`**: TTS text label (WRAP, `font_text`, white, width 420). "ОТВЕЧАЮ" label (`font_state`, blue). Hint. Touch → `voice_assistant.stop`.

**`va_error_group`**: "ОШИБКА" label (`font_state`, red). Hint "Нажмите чтобы повторить". Touch → `voice_assistant.start`.

**`va_muted_group`**: "МИКРОФОН ВЫКЛ" label (`font_state`, red). "Нажмите чтобы включить" hint. Button "ВКЛЮЧИТЬ" (green, `on_click`: `switch.turn_off: mute`).

### Intercom Groups (hidden by default)

**`ic_ringing_in_group`**: bg `0x3D0000`. "ВХОДЯЩИЙ ЗВОНОК" (`font_title`, red). Peer label (`font_state`, white). Two buttons: "ОТВЕТИТЬ" (green, `on_click`: `intercom_api.answer_call: intercom`) and "ОТКЛОНИТЬ" (red, `on_click`: `button.press: decline_button`).

**`ic_outgoing_group`**: bg `0x1A1000`. "ЗВОНЮ..." (`font_title`, orange). Peer label (`font_state`, white). "Ожидание ответа..." (`font_text`, gray). Button "ОТМЕНА" (red, `on_click`: `button.press: call_button`).

**`ic_in_call_group`**: bg `0x002200`. "РАЗГОВОР" (`font_title`, green). "с:" + peer label (`font_state`, white). Button "ЗАВЕРШИТЬ" (red, `on_click`: `button.press: call_button`).

## State Machine

### `draw_display` script (new, `mode: restart`)

Called from `ui_state_changed`. Decision tree:

1. WiFi disconnected → `lv_disp_load_scr(no_wifi_page)`, return
2. API disconnected → `lv_disp_load_scr(no_ha_page)`, return
3. `voice_assistant_phase == 10` (not_ready) → `lv_disp_load_scr(no_ha_page)`, return
4. `voice_assistant_phase == 20` (timer) → `lv_disp_load_scr(timer_finished_page)`, return
5. Otherwise → ensure `main_page` is active, then:
   - Hide all VA groups + IC groups + tileview
   - Map state to visible element:

| Condition | Show |
|-----------|------|
| `current_mode == 0` && VA idle/muted | tileview (idle) or va_muted_group |
| `voice_assistant_phase == 2` (listening) | va_listening_group |
| `voice_assistant_phase == 3` (thinking) | va_thinking_group |
| `voice_assistant_phase == 4` (replying) | va_replying_group |
| `voice_assistant_phase == 11` (error) | va_error_group |
| Intercom ringing (incoming) | ic_ringing_in_group |
| Intercom outgoing | ic_outgoing_group |
| Intercom in call (streaming) | ic_in_call_group |

### `ui_state_changed` (`!extend`)

Existing hook already maps `g_applied_led` → `voice_assistant_phase` and `current_mode`. Add call to `draw_display` at the end.

### `ui_ic_call` (`!extend`)

Sets `peer_name`, `previous_mode`, `current_mode = 1`. Existing code. No changes needed — `update_status` → `ui_state_changed` → `draw_display` handles the rest.

### `ui_tts_started` (`!extend`)

Sets `voice_assistant_phase` to replying. Updates `replying_text_label` with the TTS text. Existing code. Add: update LVGL label widget content.

### `ui_stt_ended` (`!extend`)

Currently empty (`then: []`). Add: update `thinking_text_label` with STT text.

## Data Sources

### Clock

- `pcf85063` RTC component on I2C 0x51 for time persistence
- `time:` component with `on_time` trigger (every minute) updates clock label in top_bar
- NTP sync when WiFi connected

### Weather

- `homeassistant.service` call to `weather.get_forecasts` on an interval (every 10 minutes)
- Lambda parses response, updates weather labels (icon, temp, condition, forecast)
- Weather entity configured via substitution (default: `weather.home`)

### WiFi Status

- `sensor:` with `platform: wifi_signal`, updates wifi icon in top_bar

### Peer Name

- Set by `ui_ic_call` hook and `intercom_api` callbacks
- Updated in top_bar label and IC group labels via `draw_display`

## Intercom Tileview Behavior

When an intercom event occurs:
1. Snap tileview to idle_tile (col 0) if on weather_tile
2. Hide tileview entirely
3. Show appropriate IC group

When intercom ends and returning to idle:
1. Hide IC group
2. Show tileview at idle_tile

## What is NOT Included (YAGNI)

- AI avatar images / animations (text-only UI)
- IMU sensor display
- Display brightness controls on-device (managed via HA)
- Top-layer timer overlay (timer gets its own page)
- Circular text layout (`compute_layout_metrics` and related globals)

## Files Modified

- `waveshare-s3-touch-4b-va-intercom.yaml` — all changes in this file:
  - Fonts: add `font_btn`, `font_clock`, `font_icon`, `font_icon_sm`
  - LVGL: replace test `main_page` with full page structure
  - Add system overlay pages
  - Add `draw_display` script
  - Extend `ui_stt_ended` to update thinking label
  - Add `time:` component for clock
  - Add `pcf85063` RTC component
  - Add weather sensor/service polling
  - Add `sensor: wifi_signal` for wifi icon
  - Remove `compute_layout_metrics` and circular layout globals (unused)
