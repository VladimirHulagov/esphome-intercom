# esp_aec

Standalone Espressif AEC (Acoustic Echo Cancellation) wrapper for
ESPHome.

## Purpose

Wraps `espressif/esp-sr`'s AEC primitive and exposes it through the
`AudioProcessor` interface. Intended for devices that only need echo
cancellation on the mic path (e.g. full-duplex intercom) and do not
need the wider AFE pipeline of `esp_afe` (noise suppression,
beamforming, VAD, AGC).

Both `esp_aec` and `esp_afe` implement `AudioProcessor`, so
`i2s_audio_duplex` and `intercom_api` accept either one interchangeably
via their `processor_id` config option.

## YAML example

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/n-IA-hane/esphome-intercom
      ref: audio-core-v2
    components: [audio_processor, esp_aec, i2s_audio_duplex, intercom_api]

esp_aec:
  id: aec_processor
  sample_rate: 16000
  filter_length: 4
  mode: sr_low_cost

i2s_audio_duplex:
  id: i2s_duplex
  processor_id: aec_processor
  # ...
```

## Public C++ API

| Method | Purpose |
|---|---|
| `setup() / dump_config() / get_setup_priority()` | Standard ESPHome component lifecycle. |
| `bool is_initialized() const` | True when the esp-sr AEC handle is ready. |
| `FrameSpec frame_spec() const` | Frame size and channel layout for `process()`. |
| `bool process(mic, ref, out, mic_channels)` | Run one AEC frame. |
| `FeatureControl feature_control(AudioFeature)` | `AEC` is `RESTART_REQUIRED`; others `NOT_SUPPORTED`. |
| `bool set_feature(AudioFeature, bool enabled)` | Toggle AEC (rebuilds the handle). |
| `ProcessorTelemetry telemetry() const` | Frame count + ring-buffer free percent. |
| `bool reconfigure(int type, int mode)` | Switch AEC mode. `type` is unused (reserved for forward compatibility) and `mode` maps to the underlying esp-sr modes: `1` = `sr_low_cost`, `2` = `sr_high_perf`, `3` = `voip_low_cost`, `4` = `voip_high_perf`. Prefer the YAML `mode:` string or the `AEC Mode` select entity in Home Assistant — both wrap this numeric API. |
| `bool reinit_by_name(const std::string &name)` | Same as `reconfigure()` but takes the mode name directly (`"sr_low_cost"` etc.). Recommended entry point for lambdas. |
| `std::string get_mode_name() const` | Current mode as a string. Read this after a `reconfigure()` / `reinit_by_name()` to confirm the switch was accepted (it may be rejected, for example, if `sr_high_perf` cannot allocate DMA). |

## Threading model

None. `esp_aec::process()` runs synchronously on the caller's audio
task. The caller (typically `i2s_audio_duplex`) owns the RT audio
thread.

## Dependencies

- ESP32 only; restricted to S3 and P4 variants (enforced in
  `_validate_esp32_variant`).
- Pulls `espressif/esp-sr ^2.3.0` via ESPHome's IDF component manager.

## Known constraints

- Sample rate fixed at 16 kHz (the rate esp-sr's AEC expects). The
  consumer must decimate if the I2S bus runs at a higher rate
  (i2s_audio_duplex.yaml does this via FirDecimator).
- Mode changes (`sr_low_cost` vs `sr_high_perf`) require a rebuild:
  ~70 ms audio gap. Do not change mode while a call is streaming.
- Filter length is compile-time-sized but runtime-mutable; longer
  filters give better tail coverage at the cost of CPU.
