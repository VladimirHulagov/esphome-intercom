# audio_processor

Shared C++ interface for audio-processor components. Not a user-facing
component - it only exposes headers that `esp_aec`, `esp_afe`,
`i2s_audio_duplex` and `intercom_api` consume.

## Purpose

Defines the `AudioProcessor` virtual base class and the value types
used to describe a processing frame (`FrameSpec`), feature toggles
(`AudioFeature`, `FeatureControl`) and telemetry
(`ProcessorTelemetry`). Consumers reference the interface, never the
concrete implementation, so swapping `esp_aec` for `esp_afe` in a
device YAML does not require code changes in `i2s_audio_duplex` or
`intercom_api`.

Also ships `ring_buffer_caps.h`, a small helper that creates ring
buffers with an explicit memory-placement policy (internal RAM vs
PSRAM). Audio hot-path buffers should always be allocated through the
helper so placement is auditable at boot.

## YAML example

`audio_processor` has no config of its own. Include it in any device
YAML alongside the concrete processor:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/n-IA-hane/esphome-intercom
      ref: audio-core-v2
    components: [audio_processor, esp_aec, i2s_audio_duplex, intercom_api]
```

## C++ API summary

All types live in `esphome::audio_processor`.

| Type | Purpose |
|---|---|
| `class AudioProcessor` | Abstract base. One virtual `process(mic, ref, out, mic_channels)` call per frame, plus runtime queries (`frame_spec`, `telemetry`, `feature_control`). |
| `struct FrameSpec` | Sample rate, channel counts and per-channel sample counts expected on the input and produced on the output. |
| `enum class AudioFeature` | `AEC`, `NS`, `VAD`, `AGC`, `SE` - the set of toggles a processor may expose. |
| `enum class FeatureControl` | How a given feature can be controlled at runtime: `NOT_SUPPORTED`, `BOOT_ONLY`, `RESTART_REQUIRED`, `LIVE_TOGGLE`. |
| `struct ProcessorTelemetry` | Thread-safe snapshot of per-frame counters and diagnostic gauges. |

Ring-buffer helpers (in `ring_buffer_caps.h`):

| Helper | When to use |
|---|---|
| `create_internal(len, name)` | Audio hot-path ring buffers that must never incur PSRAM bus contention. |
| `create_prefer_psram(len, name)` | Large non-realtime buffers; tries PSRAM first, falls back to internal. |
| `create_psram(len, name)` | PSRAM only; fails if PSRAM is absent. Rare. |

## Threading model

None directly - the component only defines interfaces. Each
implementation owns its own task topology:

- `esp_aec`: no background tasks; `process()` runs synchronously on the
  caller's audio task.
- `esp_afe`: two tasks (`feed_task`, `fetch_task`) coordinating with
  `process()` via a lock-free drain handshake.

## Dependencies

ESP32 only. `ring_buffer_caps.cpp` links against
`heap_caps_aligned_alloc` from esp-idf and `RingBuffer` from ESPHome
core.

## Known constraints

- Frame sample counts must match between producer (processor) and
  consumer (i2s_audio_duplex / intercom_api). Consumers read
  `frame_spec_revision()` at init and poll in their audio loop; on
  change they restart to re-read `frame_spec()`.
- Placement policy is advisory: ESPHome's default `RingBuffer::create`
  falls back between pools silently. Prefer the
  `ring_buffer_caps.h` helpers whenever placement matters.
