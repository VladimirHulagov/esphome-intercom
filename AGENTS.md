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
| Display (ST7701 RGB) | ✅ Working — ST7701Init via TCA9554 + st7701s RGB |
| LVGL UI | ✅ Working — Russian text, system pages, tileview, VA/IC groups |
| WiFi | ✅ Working |
| **Display colors** | 🔴 **Broken** — wrong colors, BLK not black |

### Display Solution

ST7701 init requires SPI commands sent through the TCA9554 IO expander:
- CS = EXIO0, MOSI = EXIO1, SCK = EXIO2, RST = EXIO7

**Solution:** Custom C++ component (`includes/st7701_init.h`) sends the full init
sequence via I2C bit-bang SPI through TCA9554 at HARDWARE priority (before display
setup). Then ESPHome's `st7701s` platform takes over the RGB DPI peripheral only,
using dummy SPI pins (GPIO43/44) so the real display isn't disturbed.

**Key files:**
- `includes/st7701_init.h` — ST7701Init component (I2CDevice + bit-bang SPI)
- Dummy SPI bus: GPIO43(CLK), GPIO44(MOSI/CS)
- `st7701s` with `spi_id: dummy_spi`, no `init_sequence`, no `reset_pin`

### Display Color Debug — All Attempts

**Data pins order** (DPI peripheral: D[4:0]=Blue, D[10:5]=Green, D[15:11]=Red):

| Test | data_pins order | Init key params | Result |
|------|----------------|-----------------|--------|
| 1 | Waveshare `[10,11,12,13,14,21,8,18,45,38,39,40,41,42,2,1]` | 0xB8=0x21, COLMOD=0x50 (pg 0x13), no INVON | Warm/pinkish |
| 2 | Waveshare same | Same + INVON (0x21) | Green-blue/cyan |
| 3 | BGR swap `[1,40,41,42,2,21,8,18,45,38,39,10,11,12,13,14]` | 0xB8=0x20, COLMOD=0x50, no INVON | Pinkish, B bits scrambled |
| 4 | BGR swap same | 0xB8=0x20, COLMOD=0x55, no INVON | Same as #3, COLMOD didn't help |
| 5 | Corrected `[40,41,42,2,1,21,8,18,45,38,39,10,11,12,13,14]` | 0xB8=0x20, COLMOD=0x55, INVON | Cornflower/lime/khaki |

**Detailed color results per test:**

**Test 1** — Waveshare pins, 0xB8=0x21, COLMOD=0x50 (pg 0x13), no INVON:
- R(0xF800) → Лиловый (lilac/purple)
- G(0x07E0) → Жёлтый (yellow)
- B(0x001F) → Светло-розовый (light pink)
- W(0xFFFF) → Бежевый (beige)
- BLK(0x0000) → Белый (white)
- GRY(0x7BEF) → Бледно-сиреневый (pale lilac)
- ORG(0xFD20) → Бледно-жёлтый (pale yellow)

**Test 2** — Waveshare pins + INVON:
- User: "всё стало зелёно-голубым" (green-blue/cyan overall)

**Test 3** — `[1,40,41,42,2,21,8,18,45,38,39,10,11,12,13,14]`, 0xB8=0x20, COLMOD=0x50, no INVON:
- R(0xF800) → Светло-фисташковый (light pistachio)
- G(0x07E0) → Фуксия (fuchsia)
- B(0x001F) → Светло-розовый (light pink)
- W(0xFFFF) → Розовый (pink)
- BLK(0x0000) → Бело-розовый (white-pink)
- GRY(0x7BEF) → Фиолетовый (purple)
- ORG(0xFD20) → Бежево-розовый (beige-pink)
- **Note:** GPIO1=B4(MSB) as D0(LSB) — blue bits scrambled

**Test 4** — Same as #3 but COLMOD=0x55:
- R(0xF800) → Бежово-розовый (beige-pink) [changed]
- Rest same as #3 — COLMOD change had minimal effect

**Test 5** — `[40,41,42,2,1,21,8,18,45,38,39,10,11,12,13,14]`, 0xB8=0x20, COLMOD=0x55, INVON:
- R(0xF800) → Васильковый (cornflower blue)
- G(0x07E0) → Салатовый (lime)
- B(0x001F) → Хаки (khaki)
- W(0xFFFF) → Светло-зелёный (light green)
- BLK(0x0000) → Хаки (khaki)
- GRY(0x7BEF) → Салатовый (lime)
- ORG(0xFD20) → Васильковый (cornflower blue)
- **Note:** R and ORG same color, G and GRY same, B and BLK same — display shows ~3 colors

### Official Waveshare BSP Reference (Critical Findings)

**Source:** `waveshareteam/Waveshare-ESP32-components` → `bsp/esp32_s3_touch_lcd_4b/`

**BSP pin mapping (CONFIRMED — matches our data_pins):**
```
DATA0=GPIO40 (B0), DATA1=GPIO41 (B1), DATA2=GPIO42 (B2), DATA3=GPIO2 (B3), DATA4=GPIO1 (B4)
DATA5=GPIO21 (G0), DATA6=GPIO8 (G1), DATA7=GPIO18 (G2), DATA8=GPIO45 (G3), DATA9=GPIO38 (G4), DATA10=GPIO39 (G5)
DATA11=GPIO10 (R0), DATA12=GPIO11 (R1), DATA13=GPIO12 (R2), DATA14=GPIO13 (R3), DATA15=GPIO14 (R4)
```

**BSP display config:**
- `BSP_LCD_BITS_PER_PIXEL = 16` (DPI sends 16-bit RGB565)
- `BSP_LCD_BIT_PER_PIXEL = 18` (panel configured for 18-bit RGB666)
- `BSP_LCD_BIGENDIAN = 1`
- `BSP_LCD_COLOR_FORMAT = ESP_LCD_COLOR_FORMAT_RGB565`
- `rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB` (in panel dev config)
- `BSP_LCD_DATA_WIDTH = 16` (16 data pins)

**BSP init sequence (CRITICAL DIFFERENCES from ours):**
1. **SLPOUT (0x11) sent FIRST** — before any config registers
2. **COLMOD = 0x66** (RGB666/18-bit), NOT 0x50 or 0x55
3. **Page 0x00** for MADCTL/COLMOD/INVON (not page 0x13)
4. **INVON (0x21)** included
5. **0xB8 = 0x21** (not 0x20)
6. **0xC2 (pg 0x10) = 0x21,0x08** (not 0x31,0x05)
7. **0xCD (pg 0x10) = 0x08** (not 0x00)
8. **0xB1 (pg 0x11) = 0x30** (not 0x32)
9. **0xB2 (pg 0x11) = 0x87** (not 0x07)
10. **0xC2 (pg 0x11) has 20ms delay**
11. **No page 0x13 access** — no 0xE5 command in page 0x13

**BSP full init sequence:**
```
0x11 (SLPOUT)                    ← FIRST, with 120ms delay
0xFF → 0x77,0x01,0x00,0x00,0x10  (page 0x10)
0xC0 → 0x3B,0x00
0xC1 → 0x0D,0x02
0xC2 → 0x21,0x08                 ← different: was 0x31,0x05
0xCD → 0x08                      ← different: was 0x00
0xB0 → 0x00,0x11,0x18,0x0E,0x11,0x06,0x07,0x08,0x07,0x22,0x04,0x12,0x0F,0xAA,0x31,0x18
0xB1 → 0x00,0x11,0x19,0x0E,0x12,0x07,0x08,0x08,0x08,0x22,0x04,0x11,0x11,0xA9,0x32,0x18
0xFF → 0x77,0x01,0x00,0x00,0x11  (page 0x11)
0xB0 → 0x60
0xB1 → 0x30                      ← different: was 0x32
0xB2 → 0x87                      ← different: was 0x07
0xB3 → 0x80
0xB5 → 0x49
0xB7 → 0x85
0xB8 → 0x21                      ← BSP uses 0x21 (not 0x20)
0xC1 → 0x78
0xC2 → 0x78                      ← with 20ms delay
0xE0 → 0x00,0x1B,0x02
0xE1 → 0x08,0xA0,0x00,0x00,0x07,0xA0,0x00,0x00,0x00,0x44,0x44
0xE2 → 0x11,0x11,0x44,0x44,0xED,0xA0,0x00,0x00,0xEC,0xA0,0x00,0x00
0xE3 → 0x00,0x00,0x11,0x11
0xE4 → 0x44,0x44
0xE5 → 0x0A,0xE9,0xD8,0xA0,0x0C,0xEB,0xD8,0xA0,0x0E,0xED,0xD8,0xA0,0x10,0xEF,0xD8,0xA0
0xE6 → 0x00,0x00,0x11,0x11
0xE7 → 0x44,0x44
0xE8 → 0x09,0xE8,0xD8,0xA0,0x0B,0xEA,0xD8,0xA0,0x0D,0xEC,0xD8,0xA0,0x0F,0xEE,0xD8,0xA0
0xEB → 0x02,0x00,0xE4,0xE4,0x88,0x00,0x40
0xEC → 0x3C,0x00
0xED → 0xAB,0x89,0x76,0x54,0x02,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x20,0x45,0x67,0x98,0xBA
0xFF → 0x77,0x01,0x00,0x00,0x00  (page 0x00)  ← NOT page 0x13!
0x36 → 0x00                      (MADCTL)
0x3A → 0x66                      (COLMOD RGB666)  ← NOT 0x50 or 0x55
0x21                             (INVON)  ← with 120ms delay
0x29                             (DISPON)
```

**BSP uses `rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB`** — this tells the ESP-IDF
st7701 panel driver to swap R↔B in the frame buffer. In ESPHome, we use raw DPI
(default BGR element order), so NO swap is needed. The data_pins order
`[40,41,42,2,1,21,8,18,45,38,39,10,11,12,13,14]` is CORRECT for ESPHome.

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

- **Fix init sequence to match official Waveshare BSP** — SLPOUT first, COLMOD=0x66, page 0x00, correct register values
- Verify weather data loads from HA (requires HA connection)
- Test VA (voice assistant) flow end-to-end
- Test intercom call flow end-to-end
- Clean up debug code (heap diagnostics, I2S logging, interval monitor, color_test_page)
- Consider PCF85063 RTC integration for clock persistence
