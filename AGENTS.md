# AGENTS.md — ESPHome Intercom

## Active Task: ESP32-S3-Touch-LCD-4B (Smart 86 Box) config

**Config file:** `waveshare-s3-touch-4b-va-intercom.yaml`
**Target mode:** VA (Voice Assistant) + Intercom + LVGL Display

### Board: Waveshare ESP32-S3-Touch-LCD-4B

- **SoC:** ESP32-S3-WROOM-1-N16R8 (16MB Flash, 8MB PSRAM octal)
- **Display:** ST7701 4" RGB 480x480, 65K color (16-bit RGB565 DPI → 18-bit RGB666 panel)
- **Touch:** GT911 capacitive (I2C 0x14)
- **Audio:** ES8311 DAC (0x18) + ES7210 ADC (0x40) with AEC
- **IO Expander:** TCA9554PWR (0x20)
- **PMIC:** AXP2101
- **IMU:** QMI8658
- **RTC:** PCF85063 (I2C 0x51)
- **Backlight:** GPIO4 (LEDC PWM, active-low)
- **Speaker amp:** NS4150B (CTRL via TCA9554 EXIO3)
- **Speaker connector:** MX1.25 2P 8Ω 2W
- **Buttons:** BOOT (GPIO0), PWRKEY (TCA9554)
- **USB:** Two Type-C ports (USB TO UART + ESP32-S3 USB)

### ESP32 GPIO Pin Map

| GPIO | Function |
|------|----------|
| GPIO0 | BOOT button (INPUT_PULLUP) |
| GPIO4 | Display backlight (LEDC PWM, inverted) |
| GPIO5 | I2S MCLK |
| GPIO6 | I2S DIN (microphone) |
| GPIO7 | I2S LRCLK |
| GPIO9 | LCD PCLK |
| GPIO15 | I2S DOUT (speaker) |
| GPIO16 | I2S BCLK |
| GPIO17 | LCD DE |
| GPIO47 | I2C SDA |
| GPIO48 | I2C SCL |
| GPIO46 | LCD HSYNC |
| GPIO3 | LCD VSYNC |
| GPIO10-14 | LCD R0-R4 (RED data, 5 of 6 bits) |
| GPIO21,8,18,45,38,39 | LCD G0-G5 (GREEN data, 6 of 6 bits) |
| GPIO40,41,42,2,1 | LCD B0-B4 (BLUE data, 5 of 6 bits) |

**Note:** R5 and B5 are NOT connected to any GPIO (floating). Panel is 18-bit RGB666
but only 16 data pins are routed (5B+6G+5R).

### TCA9554 IO Expander Pin Map (I2C 0x20)

| Pin | Function |
|-----|----------|
| EXIO0 | ST7701 SPI CS |
| EXIO1 | ST7701 SPI MOSI (init commands) |
| EXIO2 | ST7701 SPI SCK |
| EXIO3 | NS4150B speaker amp CTRL |
| EXIO5 | GT911 TP_RST |
| EXIO6 | GT911 TP_INT |
| EXIO7 | ST7701 LCD RST |

### Status

| Component | Status |
|-----------|--------|
| Audio (ES8311+ES7210) | ✅ Working |
| GT911 touch | ✅ Working |
| Backlight (GPIO4 LEDC) | ✅ Working (inverted) |
| Speaker amp (TCA9554 EXIO3) | ✅ Working |
| Display (ST7701 RGB) | ✅ Working — correct colors with R↔B swap |
| LVGL UI | ✅ Working — Russian text, system pages, tileview, VA/IC groups |
| WiFi | ✅ Working |

### Display Solution

ST7701 init requires SPI commands sent through the TCA9554 IO expander:
- CS = EXIO0, MOSI = EXIO1, SCK = EXIO2, RST = EXIO7

**Solution:** Custom C++ component (`includes/st7701_init.h`) sends the full init
sequence via I2C bit-bang SPI through TCA9554 at HARDWARE priority (before display
setup). Then ESPHome's `st7701s` platform takes over the RGB DPI peripheral only,
using dummy SPI pins (GPIO43/44) so the real display isn't disturbed.

**Color fix — R↔B pixel swap:**
The Waveshare board routes DPI data pins as:
- Pixel bits [4:0] → GPIO10-14 → ST7701 Red[4:0]
- Pixel bits [10:5] → GPIO21-39 → ST7701 Green[5:0]
- Pixel bits [15:11] → GPIO40-1 → ST7701 Blue[4:0]

In RGB565, bits [4:0]=Blue, [15:11]=Red — so pixel Blue→display Red, pixel Red→display Blue.
Fixed by swapping R↔B in `st7701s.cpp` `draw_pixels_at`:
```cpp
p16[i] = ((p & 0x1F) << 11) | (p & 0x07E0) | ((p >> 11) & 0x1F);
```

**ESPHome LVGL colors are 24-bit `0xRRGGBB`**, NOT raw RGB565. Use `0xFF0000` for red,
`0x00FF00` for green, `0x0000FF` for blue. Values like `0xF800` are interpreted as
`0x00F800` = green, not RGB565 red.

**Key files:**
- `includes/st7701_init.h` — ST7701Init component (I2CDevice + bit-bang SPI)
- Dummy SPI bus: GPIO43(CLK), GPIO44(MOSI/CS)
- `st7701s` with `spi_id: dummy_spi`, no `init_sequence`, no `reset_pin`
- Docker: `/esphome/esphome/components/st7701s/st7701s.cpp` — R↔B swap patch

### LVGL UI Architecture

**Hybrid approach** — system overlay pages are separate LVGL pages; main content lives
on one `main_page` with mutually exclusive groups.

**System overlay pages** (separate `lv_disp_load_scr` targets):
- `initializing_page` — "Загрузка..." (shown at boot)
- `no_wifi_page` — "Нет WiFi" red on black
- `no_ha_page` — "Нет Home Assistant" orange on black
- `timer_finished_page` — "Таймер!" with Stop button

**main_page** contains:
- `top_bar` — clock (`font_clock`), peer name, wifi icon
- `main_tileview` — swipe between idle_tile (call buttons) and weather_tile (5-day forecast)
- 5 VA groups: listening, thinking, replying, error, muted (hidden, one visible at a time)
- 3 IC groups: ringing_in, outgoing, in_call (hidden, one visible at a time)

**State machine:** `draw_display` script checks WiFi → API → phase → intercom state,
switches pages/groups via `lv_disp_load_scr` / `LV_OBJ_FLAG_HIDDEN`.

**Fonts:** All Roboto (Google Fonts) with `GF_Latin_Core` + `GF_Cyrillic_Core` glyphsets.
`font_text` (20), `font_title` (30), `font_state` (36), `font_info` (24), `font_btn` (22),
`font_clock` (Roboto Mono 48), `font_icon`/`font_icon_sm` (MDI weather/wifi glyphs).

**IMPORTANT:** Russian text must be real UTF-8 in YAML, NOT `\xD0\x9D` escaped bytes.
ESPHome/LVGL renders escaped bytes as individual Latin-1 characters (mojibake).
Use `GF_Cyrillic_Core` glyphset (not just `GF_Latin_Core`) for Cyrillic support.
Figtree font does NOT support Cyrillic on Google Fonts — use Roboto or Noto Sans instead.

### Known I2C Addresses

| Chip | Address |
|------|---------|
| GT911 touch | 0x14 |
| ES8311 DAC | 0x18 |
| TCA9554 expander | 0x20 |
| AXP2101 PMIC | 0x34 |
| ES7210 ADC | 0x40 |
| PCF85063 RTC | 0x51 |
| QMI8658 IMU | 0x6B |

### Dependencies

- ESPHome external component: `intercom_api`, `i2s_audio_duplex`, `esp_aec` from `https://github.com/n-IA-hane/esphome-intercom`
- Package: `va_intercom.yaml` from same repo
- Micro wake word models: `hey_trowyayoh.json` + `alexa`
- Ringtone: `ringtone.flac`, timer finished: `timer_finished.flac`

### Constraints & Preferences

- Only commit successful changes, not debug attempts
- ESPHome Docker container runs in `/mnt/services/esphome/esphome/` (config mapped to `/config`)
- esphome-intercom repo is at `/mnt/services/esphome/esphome-intercom/`
- Flash via USB at `/dev/ttyACM0` (native USB Serial/JTAG, top USB-C port)
- esptool: `sudo PYTHONPATH=/home/lacitis/.local/lib/python3.12/site-packages /usr/local/bin/python3.12 -m esptool`
- Git remote: `VladimirHulagov/esphome-intercom` (fork of `n-IA-hane/esphome-intercom`)
- WiFi: SSID=`WLAN_IOT`, password=`zuki28_1w` in `secrets.yaml`
- Must push to GitHub before compile — device config loads packages via git URL (`ref: main`)
- Must use `ghcr.io/esphome/esphome` image (2026.4.3) to compile
- Must run `sudo rm -rf /mnt/services/esphome/esphome/.esphome/packages/852a4dc7` to clear stale package cache before compile
- Must run `docker kill esphome` before flash — container grabs `/dev/ttyACM0`
- Do NOT delete `.esphome/external_components/86fcf02e/` — upstream has API changes
- LVGL `buffer_size: 10%` (not 25%) to free internal DMA memory for I2S
- **CRITICAL:** `includes/st7701_init.h` must be manually synced: `sudo cp esphome-intercom/includes/st7701_init.h esphome/includes/st7701_init.h`
- OTA flash: `docker exec esphome esphome run esp32-s3-smart-86-box-hall.yaml --no-logs`
- Device IP: `10.1.30.57`
- Must `docker exec esphome esphome clean ...` + `sudo rm -rf .../packages` before each build to avoid PlatformIO cache errors

### Key Decisions

- GPIO43/44 for dummy SPI (not GPIO35/36 PSRAM octal pins)
- Push-pull I2C recovery at boot priority 1100 (GPIO47/48 have no external pullups)
- Direct PCA9554 I2C writes via shadow register `pca9554_output_state`
- I2S BCLK=GPIO16 (GPIO18 is LCD G2 data pin)
- Stereo AEC reference (`use_stereo_aec_reference: true`)
- I2S port 1 (`i2s_num: 1`)
- Backlight: LEDC PWM with `inverted: true` on GPIO4 (active-low)
- Audio task stack in PSRAM (`audio_stack_in_psram: true`)
- Font: Roboto (not Figtree) — Figtree lacks Cyrillic on Google Fonts
- Glyphsets: `GF_Latin_Core` + `GF_Cyrillic_Core` inlined per font (not via substitution)
- data_pins: `[40,41,42,2,1,21,8,18,45,38,39,10,11,12,13,14]` — B0-B4,G0-G5,R0-R4 (confirmed by BSP)
- ESPHome uses default DPI BGR element order (no `rgb_ele_order` — that's BSP-only)
- Panel is 18-bit RGB666 but only 16 DPI pins (R5/B5 floating)
- **R↔B swap in `st7701s.cpp`** — compensates for Waveshare routing (pixel Blue→display Red)
- **ESPHome LVGL colors are 24-bit `0xRRGGBB`**, not raw RGB565
- **INVON enabled** in init — required for correct gamma, does NOT invert RGB data

### To Flash

```bash
# Push changes
git push origin main
# Clear package cache + sync init file
sudo rm -rf /mnt/services/esphome/esphome/.esphome/packages/852a4dc7
sudo cp /mnt/services/esphome/esphome-intercom/includes/st7701_init.h /mnt/services/esphome/esphome/includes/st7701_init.h
# OTA (preferred when device is online)
docker start esphome; sleep 3
docker exec esphome esphome clean esp32-s3-smart-86-box-hall.yaml
sudo rm -rf /mnt/services/esphome/esphome/.esphome/packages/852a4dc7
docker exec esphome esphome run esp32-s3-smart-86-box-hall.yaml --no-logs
# USB (alternative, when OTA unavailable)
docker kill esphome
sudo rm -rf /mnt/services/esphome/esphome/.esphome/build/esp32-s3-smart-86-box-hall
docker run --rm -v /mnt/services/esphome/esphome:/config -v /mnt/services/esphome/esphome-intercom:/config/intercom ghcr.io/esphome/esphome:2026.4.3 compile esp32-s3-smart-86-box-hall.yaml
sudo PYTHONPATH=/home/lacitis/.local/lib/python3.12/site-packages /usr/local/bin/python3.12 -m esptool --chip esp32s3 --port /dev/ttyACM0 --baud 460800 --before usb-reset --after hard-reset write-flash 0x0 /mnt/services/esphome/esphome/.esphome/build/esp32-s3-smart-86-box-hall/.pioenvs/esp32-s3-smart-86-box-hall/firmware.factory.bin
```

### Next Steps

- Verify weather data loads from HA (requires HA connection)
- Test VA (voice assistant) flow end-to-end
- Test intercom call flow end-to-end
- Consider PCF85063 RTC integration for clock persistence
