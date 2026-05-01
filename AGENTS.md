# AGENTS.md — ESPHome Intercom

## Active Task: ESP32-S3-Touch-LCD-4B (Smart 86 Box) config

**Config file:** `waveshare-s3-touch-4b-va-intercom.yaml`
**Target mode:** VA (Voice Assistant) + Intercom + LVGL Display

### Board: Waveshare ESP32-S3-Touch-LCD-4B

- **SoC:** ESP32-S3-WROOM-1-N16R8 (16MB Flash, 8MB PSRAM octal)
- **Display:** ST7701 4" RGB 480x480 (init via SPI bit-bang through TCA9554, then RGB video mode)
- **Touch:** GT911 capacitive (I2C 0x14)
- **Audio:** ES8311 DAC (0x18) + ES7210 ADC (0x40) with AEC
- **IO Expander:** TCA9554PWR (0x20)
- **PMIC:** AXP2101
- **IMU:** QMI8658
- **RTC:** PCF85063
- **Backlight:** GPIO4 (LEDC PWM)
- **Speaker amp:** NS4150B (CTRL via TCA9554 EXIO3)
- **Speaker connector:** MX1.25 2P 8Ω 2W
- **Buttons:** BOOT (GPIO0), PWRKEY (TCA9554)
- **USB:** Two Type-C ports (USB TO UART + ESP32-S3 USB)

### ESP32 GPIO Pin Map

| GPIO | Function |
|------|----------|
| GPIO0 | BOOT button (INPUT_PULLUP) |
| GPIO4 | Display backlight (LEDC PWM) |
| GPIO5 | I2S MCLK |
| GPIO6 | I2S DIN (microphone) |
| GPIO7 | I2S LRCLK |
| GPIO9 | LCD PCLK |
| GPIO15 | I2S DOUT (speaker) |
| GPIO17 | LCD DE |
| GPIO18 | I2S BCLK |
| GPIO39 | I2C SDA (shared: ES8311, ES7210, GT911, TCA9554) |
| GPIO40 | I2C SCL |
| GPIO46 | LCD HSYNC |
| GPIO3 | LCD VSYNC |
| GPIO10-14 | LCD R0-R4 (RED data) |
| GPIO21,8,18,45,38,39 | LCD G0-G5 (GREEN data) |
| GPIO40,41,42,2,1 | LCD B0-B4 (BLUE data) |

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
| Audio (ES8311+ES7210) | ✅ Ready to flash |
| GT911 touch (INT+RST) | ✅ Configured |
| Backlight | ✅ GPIO4 LEDC |
| Speaker amp | ✅ TCA9554 EXIO3 |
| **Display (ST7701 RGB)** | 🔓 **Unblocked** — custom `ST7701Init` via TCA9554 bit-bang SPI + `st7701s` with dummy SPI pins |
| TCA9554 EXIO4 | Free (unused) |

### Display Solution

ST7701 init requires SPI commands sent through the TCA9554 IO expander:
- CS = EXIO0
- MOSI = EXIO1
- SCK = EXIO2
- RST = EXIO7

**Solution:** Custom C++ component (`includes/st7701_init.h`) sends the full init
sequence via I2C bit-bang SPI through TCA9554 at HARDWARE priority (before display
setup). Then ESPHome's `st7701s` platform takes over the RGB DPI peripheral only,
using dummy SPI pins (GPIO35-37) so the real display isn't disturbed.

**Key files:**
- `includes/st7701_init.h` — ST7701Init component (I2CDevice + bit-bang SPI)
- Dummy SPI bus: GPIO35(CLK), GPIO36(MOSI), GPIO37(CS)
- `st7701s` with `spi_id: dummy_spi`, no `init_sequence`, no `reset_pin`

### Known I2C Addresses

| Chip | Address |
|------|---------|
| GT911 touch | 0x14 |
| ES8311 DAC | 0x18 |
| TCA9554 expander | 0x20 |
| ES7210 ADC | 0x40 |

### Dependencies

- ESPHome external component: `intercom_api`, `i2s_audio_duplex`, `esp_aec` from `https://github.com/n-IA-hane/esphome-intercom`
- Package: `va_intercom.yaml` from same repo
- Micro wake word models: `hey_trowyayoh.json` + `alexa`
- Ringtone: `ringtone.flac`, timer finished: `timer_finished.flac`

### To Flash

```bash
esphome run waveshare-s3-touch-4b-va-intercom.yaml
```

Use bottom USB-C port (USB TO UART). `secrets.yaml` must contain `wifi_ssid` and `wifi_password`.
