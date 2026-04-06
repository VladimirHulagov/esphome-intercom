# ESP AFE - Full Audio Front-End Pipeline

ESPHome component wrapping Espressif's **ESP-SR AFE** (Audio Front End) framework. Provides a complete audio processing pipeline: **AEC + Noise Suppression + VAD + AGC** in a single component, with runtime control switches and diagnostic sensors.

## Overview

`esp_afe` uses the closed-source `esp-sr` library's AFE pipeline, which chains multiple DSP stages:

```
[mic + ref input] -> |AEC| -> |NS| -> |VAD| -> |AGC| -> [clean output]
```

Unlike `esp_aec` (standalone echo cancellation only), `esp_afe` provides a full signal processing pipeline. Both components implement the `AudioProcessor` interface and are drop-in replacements for each other in `i2s_audio_duplex` and `intercom_api`.

### When to use esp_afe vs esp_aec

| Feature | esp_aec | esp_afe |
|---------|---------|---------|
| Echo Cancellation | Yes | Yes |
| Noise Suppression | No | Yes (WebRTC) |
| Voice Activity Detection | No | Yes (WebRTC) |
| Automatic Gain Control | No | Yes (WebRTC) |
| Runtime switches in HA | AEC only | AEC, NS, VAD, AGC |
| Diagnostic sensors | No | Input volume, output RMS, voice presence |
| CPU usage (SR LOW_COST) | ~22% Core 0 | ~23% Core 0 (8.4% feed + 15% fetch) |
| Internal RAM overhead | ~80 KB | ~150 KB (~70 KB extra for AFE framework) |
| Supported platforms | ESP32-S3, ESP32-P4 | ESP32-S3, ESP32-P4 |

**Choose `esp_aec`** when you need minimal RAM usage and only echo cancellation.
**Choose `esp_afe`** when you want noise suppression, VAD, AGC, and diagnostic sensors.

## Requirements

- **ESP32-S3** or **ESP32-P4** with PSRAM
- ESP-IDF framework
- `i2s_audio_duplex` or `intercom_api` as audio consumer

## Installation

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/n-IA-hane/esphome-intercom
      ref: audio-core-v2
    components: [audio_processor, i2s_audio_duplex, esp_afe]
```

> **Note**: `audio_processor` must be listed in `components:` because it provides the shared `AudioProcessor` interface header. It is also auto-loaded by `esp_afe`, but ESPHome's `external_components` loader requires it to be explicitly listed.

## Configuration

### Basic Setup

```yaml
esp_afe:
  id: afe_processor
  type: sr
  mode: low_cost

i2s_audio_duplex:
  id: i2s_duplex
  # ... pins ...
  processor_id: afe_processor
```

### Full Configuration

```yaml
esp_afe:
  id: afe_processor
  type: sr                    # sr (speech recognition) or vc (voice communication)
  mode: low_cost              # low_cost or high_perf
  mic_num: 1                  # Number of microphones (currently only 1 supported)
  aec_enabled: true           # Echo cancellation
  aec_filter_length: 4        # Echo tail in frames (4 = 64ms)
  ns_enabled: true            # Noise suppression (WebRTC)
  vad_enabled: false          # Voice activity detection
  vad_mode: 3                 # VAD aggressiveness (0-4, higher = more aggressive)
  vad_min_speech_ms: 128      # Min speech duration to trigger VAD
  vad_min_noise_ms: 1000      # Min noise duration before VAD clears
  vad_delay_ms: 128           # VAD state transition delay
  agc_enabled: true           # Automatic gain control (WebRTC)
  agc_compression_gain: 9     # AGC compression gain (0-30 dB)
  agc_target_level: 3         # AGC target level (0-31, lower = louder)
  memory_alloc_mode: more_psram  # Memory allocation strategy
  afe_linear_gain: 1.0        # Linear gain applied to output (0.1-10.0)
  task_core: 1                # FreeRTOS task core (0 or 1)
  task_priority: 5            # FreeRTOS task priority (1-24)
  ringbuf_size: 8             # Internal ring buffer size in frames
```

### Configuration Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `id` | ID | Required | Component ID |
| `type` | string | `sr` | AFE type: `sr` (speech recognition, linear AEC) or `vc` (voice communication, nonlinear AEC) |
| `mode` | string | `low_cost` | AFE mode: `low_cost` or `high_perf` |
| `mic_num` | int | 1 | Number of microphones (currently capped to 1, dual-mic not yet implemented end-to-end) |
| `aec_enabled` | bool | true | Enable acoustic echo cancellation |
| `aec_filter_length` | int | 4 | AEC filter length in frames (1-8). 4 = 64ms tail, sufficient for most setups |
| `ns_enabled` | bool | true | Enable noise suppression (WebRTC engine) |
| `vad_enabled` | bool | false | Enable voice activity detection |
| `vad_mode` | int | 3 | VAD aggressiveness (0-4). Higher = rejects more noise but may miss quiet speech |
| `vad_min_speech_ms` | int | 128 | Minimum speech duration to trigger voice detection (32-60000 ms) |
| `vad_min_noise_ms` | int | 1000 | Minimum noise duration before VAD clears (64-60000 ms) |
| `vad_delay_ms` | int | 128 | VAD state transition delay (0-60000 ms) |
| `agc_enabled` | bool | true | Enable automatic gain control (WebRTC engine) |
| `agc_compression_gain` | int | 9 | AGC compression gain in dB (0-30) |
| `agc_target_level` | int | 3 | AGC target level (0-31, lower value = louder output) |
| `memory_alloc_mode` | string | `more_psram` | Memory allocation: `more_internal`, `internal_psram_balance`, `more_psram` |
| `afe_linear_gain` | float | 1.0 | Linear gain multiplier applied to output (0.1-10.0) |
| `task_core` | int | 1 | FreeRTOS task core affinity (0 or 1) |
| `task_priority` | int | 5 | FreeRTOS task priority (1-24) |
| `ringbuf_size` | int | 8 | Internal ring buffer size in frames (2-32). Larger = more latency tolerance, more memory |

### AFE Type and Mode

The combination of `type` and `mode` determines the AEC engine and DSP pipeline:

| type + mode | AEC Engine | CPU (Core 0) | MWW Compatible | Use Case |
|-------------|-----------|-------------|----------------|----------|
| `sr` + `low_cost` | `esp_aec3` (linear, SIMD) | **~23%** | **Yes** (10/10) | VA + MWW + Intercom |
| `sr` + `high_perf` | `esp_aec3` (FFT) | ~25% | Yes | Not recommended (DMA memory on S3) |
| `vc` + `low_cost` | `dios_ssp_aec` (Speex) | ~60% | No (2/10) | VoIP without wake word |
| `vc` + `high_perf` | `dios_ssp_aec` | ~64% | No | VoIP without wake word |

> **Important**: Use `sr` + `low_cost` for Voice Assistant + MWW setups. The `vc` modes add a residual echo suppressor (RES) that distorts spectral features, reducing MWW detection from 10/10 to 2/10.

NS and AGC always use the WebRTC engine regardless of type/mode. They work at boot but cannot be toggled individually at runtime via the AFE vtable (see [Feature Toggle Behavior](#feature-toggle-behavior)).

## Platform Entities

### Switch Platform

Runtime control of AFE features via Home Assistant switches:

```yaml
switch:
  - platform: esp_afe
    esp_afe_id: afe_processor
    aec:
      name: "Echo Cancellation"
      restore_mode: RESTORE_DEFAULT_ON
    ns:
      name: "Noise Suppression"
      restore_mode: RESTORE_DEFAULT_ON
    vad:
      name: "Voice Detection"
      restore_mode: RESTORE_DEFAULT_OFF
    agc:
      name: "Auto Gain Control"
      restore_mode: RESTORE_DEFAULT_ON
```

| Switch | Icon | Description |
|--------|------|-------------|
| `aec` | `mdi:ear-hearing` | Echo cancellation toggle (live, no audio gap) |
| `ns` | `mdi:volume-off` | Noise suppression toggle (requires AFE reinit, ~70ms gap) |
| `vad` | `mdi:account-voice` | Voice activity detection toggle (requires AFE reinit) |
| `agc` | `mdi:tune-vertical` | Auto gain control toggle (requires AFE reinit) |

### Binary Sensor Platform

```yaml
binary_sensor:
  - platform: esp_afe
    esp_afe_id: afe_processor
    vad:
      name: "Voice Presence"
      update_interval: 100ms
```

| Sensor | Device Class | Description |
|--------|-------------|-------------|
| `vad` | `sound` | Voice activity state. ON when speech detected, OFF when noise/silence. Requires `vad_enabled: true` in config |

### Sensor Platform

```yaml
sensor:
  - platform: esp_afe
    esp_afe_id: afe_processor
    input_volume:
      name: "Input Volume"
      update_interval: 250ms
    output_rms:
      name: "Output RMS"
      update_interval: 250ms
```

| Sensor | Unit | Description |
|--------|------|-------------|
| `input_volume` | dBFS | RMS level of mic input before processing. Useful for mic gain calibration |
| `output_rms` | dBFS | RMS level of processed output. Compare with input to see NS/AGC effect |

## Actions

### esp_afe.set_mode

Change the AFE type and mode at runtime. The entire AFE pipeline is recreated (~70ms gap).

```yaml
select:
  - platform: template
    name: "AFE Mode"
    options:
      - sr_low_cost
      - sr_high_perf
      - vc_low_cost
      - vc_high_perf
    set_action:
      - esp_afe.set_mode:
          id: afe_processor
          mode: !lambda 'return x;'
```

Valid mode strings: `sr_low_cost`, `sr_high_perf`, `vc_low_cost`, `vc_high_perf`.

## Feature Toggle Behavior

AEC and NS/AGC/VAD toggle differently due to ESP-SR internals:

| Feature | Toggle Method | Audio Gap | Notes |
|---------|-------------|-----------|-------|
| AEC | Live vtable call | None | Immediate on/off via `afe_enable_aec()` / `afe_disable_aec()` |
| NS | AFE reinit | ~70ms | WebRTC NS doesn't support runtime vtable toggle. Full AFE destroy + recreate |
| AGC | AFE reinit | ~70ms | Same as NS. WebRTC AGC has no vtable entry |
| VAD | AFE reinit | ~70ms | Same as NS |

**Why reinit?** The WebRTC NS, AGC, and VAD engines are controlled internally via `webrtc_process(data, enable_ns, enable_agc)`. The enable/disable vtable entries only exist for neural model backends (NSNet, VADNet), not for WebRTC. Toggling these features requires destroying and rebuilding the entire AFE instance with the changed config flags.

The reinit is safe: the old AFE is destroyed first (ESP-SR's FFT resources are a global singleton, only one instance can exist), then the new one is built. If the build fails, audio falls through to passthrough mode.

## Architecture

```
               AudioProcessor interface
                       |
              +--------+--------+
              |                 |
           EspAec            EspAfe
         (AEC only)    (AEC+NS+VAD+AGC)
              |                 |
              +--------+--------+
                       |
            +----------+-----------+
            |                      |
    i2s_audio_duplex         intercom_api
    (processor_id)            (aec_id)
```

Both `EspAec` and `EspAfe` implement `AudioProcessor`. The consumer components (`i2s_audio_duplex` and `intercom_api`) call `process(mic, ref, out)` without knowing which implementation is behind it.

### Internal Pipeline

```
feed() ----> AFE internal tasks ----> fetch()
  |           (FreeRTOS, Core 1)         |
  |                                      |
  mic + ref interleaved             clean mono output
  (512 samples * 2 channels)        (512 samples)
```

The AFE framework runs its own internal FreeRTOS tasks for feed/fetch. `process()` bridges synchronously: it feeds the interleaved mic+ref buffer, then fetches the processed output. A mutex protects concurrent access during reinit.

## Complete Example

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/n-IA-hane/esphome-intercom
      ref: audio-core-v2
    components: [audio_processor, intercom_api, i2s_audio_duplex, esp_afe]

esp_afe:
  id: afe_processor
  type: sr
  mode: low_cost
  aec_filter_length: 4
  ns_enabled: true
  agc_enabled: true
  vad_enabled: true

i2s_audio_duplex:
  id: i2s_duplex
  # ... I2S pins ...
  processor_id: afe_processor
  buffers_in_psram: true

# AFE switches
switch:
  - platform: esp_afe
    esp_afe_id: afe_processor
    aec:
      name: "Echo Cancellation"
    ns:
      name: "Noise Suppression"
    vad:
      name: "Voice Detection"
    agc:
      name: "Auto Gain Control"

# AFE diagnostic sensors
sensor:
  - platform: esp_afe
    esp_afe_id: afe_processor
    input_volume:
      name: "Input Volume"
    output_rms:
      name: "Output RMS"

# VAD binary sensor
binary_sensor:
  - platform: esp_afe
    esp_afe_id: afe_processor
    vad:
      name: "Voice Presence"

# Runtime mode switching
select:
  - platform: template
    name: "AFE Mode"
    options:
      - sr_low_cost
      - sr_high_perf
    initial_option: sr_low_cost
    set_action:
      - esp_afe.set_mode:
          id: afe_processor
          mode: !lambda 'return x;'
```

## Memory Usage

| Component | Internal RAM | PSRAM | Notes |
|-----------|-------------|-------|-------|
| AFE framework | ~70 KB | Variable | Cannot be moved to PSRAM (no esp-sr API) |
| AEC filter | ~20 KB | ~60 KB | Depends on filter_length |
| NS (WebRTC) | ~5 KB | ~10 KB | |
| AGC (WebRTC) | ~2 KB | ~3 KB | |
| Feed buffer | - | ~2 KB | 512 * 2 channels * 2 bytes |
| Ring buffers | - | Variable | ringbuf_size * frame_size |

With `memory_alloc_mode: more_psram`, total internal RAM usage is approximately **~100 KB** (vs ~80 KB for standalone `esp_aec`). The extra ~20 KB is the AFE framework overhead.

> **Tip**: Monitor free internal RAM with `sensor.free_internal`. If it drops below ~30 KB, WiFi/TLS stability may suffer. Consider reducing `ringbuf_size` or using `esp_aec` instead.

## Known Limitations

1. **Dual-mic beamforming**: `mic_num` is capped to 1. The AFE supports multi-mic (format "MMR") but `i2s_audio_duplex` doesn't yet pass both TDM channels to the processor. Declared as `NOT_SUPPORTED` in `feature_control(SE)`.

2. **NS/AGC/VAD runtime toggle**: Requires full AFE reinit (~70ms audio gap). This is a limitation of the WebRTC engine used by ESP-SR. The vtable enable/disable functions only work with neural model backends (NSNet, VADNet), which require a model partition.

3. **data_volume**: The AFE's built-in `data_volume` field is always 0.0 dB because it requires WakeNet to be active. Input/output RMS is computed locally instead.

4. **ESP-SR is closed source**: No API to move AFE internal allocations to PSRAM. The ~70 KB internal RAM overhead is unavoidable.

5. **Single instance**: ESP-SR's FFT resources are a global singleton. Only one AFE (or AEC) instance can exist at a time.

## Troubleshooting

### AFE setup fails (NULL config)

Check logs for `afe_config_init returned NULL`. This means esp-sr couldn't initialize with the requested type/mode. Verify your `type` and `mode` combination is valid.

### High internal RAM usage

With AFE active, free internal RAM may drop to ~35 KB. This is expected. If WiFi becomes unstable:
- Set `memory_alloc_mode: more_psram`
- Reduce `ringbuf_size` (minimum 4, but 8 is recommended)
- Consider using `esp_aec` instead if you don't need NS/AGC/VAD

### Switch toggle has no effect

NS, AGC, and VAD toggles require AFE reinit. Check logs for `Reinit requested` and `Reinit complete`. If reinit fails, the old AFE is destroyed and audio falls to passthrough.

### Voice presence always OFF

Ensure `vad_enabled: true` in the `esp_afe` config. VAD is disabled by default.

### Audio gap when toggling NS/AGC/VAD

This is expected (~70ms). Only AEC can be toggled without audio interruption.

## License

MIT License
