# Deployment guide

A short map of the YAML tree so you can pick the right starting point for
a new device without reading every file.

```
yamls/
├── intercom-only/     no wake word, no voice assistant
│   ├── dual-bus/      mic and speaker on separate I2S buses
│   └── single-bus/    one full-duplex I2S bus (codec or per-chip ADC+DAC)
└── full-experience/   intercom + wake word + voice assistant
    ├── dual-bus/
    └── single-bus/
        ├── aec/       mic pipeline = esp_aec (echo cancellation only)
        └── afe/       mic pipeline = esp_afe (AEC + NS + AGC + optional BSS)
```

## Decision tree

Follow the first branch that matches your hardware and intent.

1. **Do you want wake word + voice assistant on the device, or only
   intercom-style room-to-room calls?**
   - Just intercom → `yamls/intercom-only/`
   - Wake word / VA too → `yamls/full-experience/`

2. **How many I2S buses does your hardware expose?**
   - Two (mic bus + speaker bus, independent pins) → `dual-bus/`
   - One full-duplex bus (codec does both, or a shared bus with TDM) →
     `single-bus/`

3. **(full-experience / single-bus only) Do you have two microphones?**
   - Yes, and you want beamforming / noise suppression / AGC →
     `afe/` (runs `esp_afe`, esp-sr 2-mic BSS)
   - One mic, or you only need echo cancellation → `aec/` (runs
     `esp_aec`, echo cancellation only)

## Concrete examples

| Device | Chip | Mics | Config | YAML |
|---|---|---|---|---|
| Waveshare S3 Audio board | ESP32-S3 | 2 (ES7210 TDM) | full + afe | `full-experience/single-bus/afe/waveshare-s3-full-afe.yaml` |
| Waveshare P4 touch panel | ESP32-P4 | 2 (ES7210 TDM) | full + afe | `full-experience/single-bus/afe/waveshare-p4-full-afe.yaml` |
| Xiaozhi Ball V3 | ESP32-S3 | 1 (ES8311 ADC) | full + afe (SR low-cost) | `full-experience/single-bus/afe/xiaozhi-ball-v3-full-afe.yaml` |
| Xiaozhi intercom-only | ESP32-S3 | 1 | intercom + single | `intercom-only/single-bus/xiaozhi-intercom.yaml` |
| Generic S3 speaker + MEMS | ESP32-S3 | 1 | intercom + dual | `intercom-only/dual-bus/generic-s3-dual-intercom.yaml` |

## Hardware sizing

- **AFE + 2 mic BSS + two concurrent HTTPS streams** (music + TTS) is
  tight on internal RAM. On S3 boards enable
  `i2s_audio_duplex.audio_stack_in_psram: true` (see the
  `i2s_audio_duplex/README.md` "Advanced options" section) and
  `CONFIG_MBEDTLS_HARDWARE_AES: "n"`, otherwise `esp-aes` can fail to
  allocate DMA memory and break TLS reads mid-stream.
- **1-mic SR low-cost** (Xiaozhi) does not stress the budget and needs
  no extra tuning.
- **ESP32-P4** has more internal RAM than S3 and generally does not
  need the PSRAM-stack workaround.

## Local overrides

Every public YAML in the tree is loaded via ESPHome `packages:` as a
remote git include. To customize without forking, create a sibling file
named `<board>-local.yaml` (already in `.gitignore`) and point it at the
local components path with `type: local`. Any config block you declare
locally overrides the remote one via ESPHome's merge rules. Examples
already live next to the public YAMLs (e.g. `waveshare-s3-full-afe-local.yaml`).

## When to touch the sdkconfig

The YAMLs ship sdkconfig defaults sized for the common case. Board-specific
overrides that the defaults do not cover:

- `CONFIG_MBEDTLS_HARDWARE_AES: "n"` — see above, waveshare-s3 only.
- `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY: "y"` — required if you set
  `i2s_audio_duplex.audio_stack_in_psram: true`. Already on in our
  YAMLs.
- `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` + `CONFIG_MBEDTLS_DYNAMIC_BUFFER`
   — keep TLS allocations out of internal RAM. Already on in all
  full-experience YAMLs that do HTTPS streaming.

## Pointers

- Per-component docs: `esphome/components/<name>/README.md`.
- Architecture overview: `docs/ARCHITECTURE.md`.
- Component pattern audit: `docs/PATTERN_AUDIT.md`.
- Library alternatives (why we kept what we kept): `docs/LIBRARY_ALTERNATIVES.md`.
