# Library Alternatives Audit

## Purpose

Every piece of code in `esphome/components/{audio_processor, esp_afe, esp_aec,
i2s_audio_duplex, intercom_api}` that could plausibly be replaced by an
existing primitive (ESPHome core, ESP-IDF, FreeRTOS, esp-sr, esp-dsp, or an
established third-party library) was considered and a KEEP / MIGRATE / DEFER
verdict recorded below with rationale.

Scope: only primitives we might re-implement. Domain logic (esp-sr AFE
pipeline, AEC echo cancellation math) is not in scope — that is esp-sr's
responsibility and we treat it as a black box.

Reading order: skip to the summary table at the bottom for the headline,
read individual sections for the reasoning.

---

## 1. Ring buffer allocation policy

- Ours: `esphome/components/audio_processor/ring_buffer_caps.h:28-55` plus
  `ring_buffer_caps.cpp`. Three-way placement policy (INTERNAL /
  PREFER_PSRAM / PSRAM_ONLY), with a boot-time log line that names the
  buffer, size, policy, and verified placement (via `esp_ptr_internal`).
- Wraps `esphome::RingBuffer::create()` from `esphome/core/ring_buffer.h`,
  itself backed by FreeRTOS `xRingbufferCreateStatic` under the covers.

### Alternative: stock `esphome::RingBuffer::create()`

The upstream helper uses the default `RAMAllocator`, which tries PSRAM first
and falls back to internal RAM with no knob to force either side. For a
buffer that is hit every audio frame this is a latent glitch source: if the
allocator decides to put it in PSRAM during a high-fragmentation boot, the
buffer silently lives on the slower bus and contends with LVGL / flash cache.

**Verdict: KEEP.** The placement helper is ~80 lines and directly solves a
real upstream gap. Removing it means either accepting implicit placement
(unacceptable for audio hot paths) or duplicating the policy inline at
every call site.

**Trigger to re-evaluate:** if upstream `RingBuffer::create()` gains an
explicit `caps` argument, our wrapper becomes redundant and we can delete it.

---

## 2. Task spawn and lifecycle

- Ours after round-2 Phase 0 fix:
  - `esp_afe`: `xTaskCreatePinnedToCore` (dynamic) for `afe_feed` and
    `afe_fetch`, gated by `std::atomic<bool>` running flags. Tasks are
    recreated on every `recreate_instance_` call. See
    `esphome/components/esp_afe/esp_afe.cpp` `start_feed_task_` /
    `stop_feed_task_` / `start_fetch_task_` / `stop_fetch_task_`.
  - `i2s_audio_duplex`: `xTaskCreatePinnedToCore` (dynamic) for `i2s_duplex`,
    already idiomatic.
  - `intercom_api`: three dynamic tasks (server accept, tx, speaker rx).
- Cross-thread coordination uses atomics + a `SemaphoreHandle_t` mutex
  (`config_mutex_` in `esp_afe`). Drain protocol on `esp_afe.h:263-271`
  (`drain_request_` + `process_busy_`).

### Alternatives considered

- **FreeRTOS EventGroup**: named 24-bit event bits with blocking wait
  (`xEventGroupWaitBits`). Matches the pattern in upstream
  `micro_wake_word.cpp` (COMMAND_STOP, TASK_STARTING, etc.). Clearer
  semantics than loose atomics.
- **Task notifications** (`xTaskNotifyWait` / `ulTaskNotifyTake`): lightweight
  per-task signal, no shared object.
- **StreamBuffer / MessageBuffer**: queue patterns, not a fit for
  control-plane signalling.

### Evaluation

The esp_afe drain protocol is asymmetric: audio task polls (`process()`
checks `drain_request_.load()` once per frame, never blocks), config thread
waits with timeout. EventGroup would add a second FreeRTOS object without
removing either the atomic or the mutex. Task notifications imply waking a
specific task; drain_request_ is read-shared across process()/feed/fetch
and is not task-specific.

**Verdict: KEEP** atomics for the audio-hot-path control. The drain
protocol is hand-rolled but it is the correct shape for the asymmetric
writer/polling-reader pattern and it is zero-cost in the reader.

**Trigger to re-evaluate:** if we add another task that needs to be
notified synchronously (not polled), revisit and consider TaskNotify for
just that path.

---

## 3. AEC reference buffering

- Ours: `esphome/components/i2s_audio_duplex/i2s_audio_duplex.cpp`
  `process_aec_and_callbacks_` (around line 1146) dispatches across four
  reference sources:
  - TDM hardware reference (slot loopback, e.g. ES7210 TDM slot 2)
  - Stereo AEC reference (I2S stereo RX with DAC loopback on one channel)
  - Ring-buffer reference (post-volume PCM stored by `play()`)
  - Previous-frame reference (decimated copy of the last TX frame)
- The four paths are gated by YAML flags (`use_tdm_ref`,
  `use_stereo_aec_ref`, `aec_use_ring_buffer`) and selected once at setup.

### Alternative: reuse upstream `voice_assistant` / `speaker` AEC glue

Upstream does not have AEC glue. `voice_assistant` consumes pre-AEC audio
via `microphone::MicrophoneSource` callbacks. `speaker::Speaker` produces
audio but does not expose a reference hook. `esp-sr`'s AFE expects the
reference interleaved into the feed buffer but does not buffer it for you.
There is no primitive we can drop in.

### Evaluation

The four paths map to four hardware realities that exist in our device
matrix (ES7210 TDM, ES8311 stereo loopback, single-bus mic with software
ref, and minimalist fallback). Each is irreducible hardware-wise. What is
awkward today is not the presence of four paths but the nested-conditional
dispatch: the selection is implicit in a chain of `if` branches rather
than an explicit strategy object.

**Verdict: KEEP** the four sources; this is domain knowledge, not
re-invented wheel. Refactor the dispatch is tracked as Phase 2c of the
round-2 plan (introduce an `AecReferenceSource` interface with four
concrete implementations selected at setup).

**Trigger to re-evaluate:** if ESPHome upstream ever introduces an AEC
reference abstraction in `speaker` or a new `audio` core component,
migrate to it.

---

## 4. FIR decimator

- Ours: `esphome/components/i2s_audio_duplex/i2s_audio_duplex.h:65-270`.
  `FirDecimator` and `MultiChannelFirDecimator` wrap
  `dsps_fird_init_s16` / `dsps_fird_s16` from esp-dsp 1.7.0. On ESP32-S3
  the kernel resolves to `dsps_fird_s16_aes3` (AESHE SIMD). 32-tap Kaiser
  window, cutoff 7500 Hz, Q15 fixed point, symmetric linear phase.

### Alternative: use `dsps_fird_s16` directly

esp-dsp exposes the SIMD kernel but not a packaged decimator: coefficient
table, delay-line management, scratch buffer allocation, multi-channel
deinterleave, and memory-placement policy are all caller responsibilities.

### Evaluation

Our wrapper is ~200 lines of glue around `dsps_fird_s16`. Removing it means
pushing those concerns into every call site (mic path, RX decimator,
play-ref decimator). The 32-tap Kaiser coefficients are empirically tuned
for 48 kHz -> 16 kHz with 7500 Hz cutoff; that tuning is a project
decision, not an upstream default.

**Verdict: KEEP.** The wrapper is thin, well-factored, and isolates a
hardware-accelerated kernel from the rest of the code.

**Trigger to re-evaluate:** if we change sample rates or need non-Kaiser
filters, revisit the coefficient generation path.

---

## 5. Intercom TCP wire format

- Ours: `esphome/components/intercom_api/intercom_protocol.h:47-52`. A
  4-byte binary header (`type: u8, flags: u8, length: u16 LE`) followed by
  the raw payload. Eight message types (`AUDIO`, `START`, `STOP`, `PING`,
  `PONG`, `ERROR`, `RING`, `ANSWER`), three flag bits, five error codes.
  No schema versioning, no compression.

### Alternatives considered

- **Protobuf (proto3)**: pattern used by upstream `esphome/api`. Gives
  schema versioning, typed fields, autogenerated Python/JS stubs, optional
  varint compression. Cost: ~5 kB firmware overhead (nanopb runtime) +
  serialise/deserialise latency.
- **MessagePack / CBOR**: smaller than Protobuf but also requires a
  runtime library; neither is pre-integrated in ESP-IDF.
- **JSON lines**: human-readable but ~3x bandwidth and requires a JSON
  parser; inappropriate for 16 kHz audio frames at 32 ms cadence.

### Evaluation

The current wire format is minimal for a reason: the payload is already
16-bit PCM audio, no field is optional, and the peer is ESP-to-ESP or
ESP-to-HA companion app (known consumers). Adding Protobuf would double
the on-device code path length for each frame without solving a real
problem today.

A Protobuf migration becomes compelling when / if:
- A third party wants to implement a client from a public spec (schema
  versioning helps)
- We want HA to talk to the intercom over its native API channel (would
  be over HA's own Protobuf transport anyway)

**Verdict: DEFER.** Explicitly out-of-scope for round-2. Document the
wire format in `docs/DEPLOYMENT_GUIDE.md` (Phase 4b of round-2 plan) so
that any third party has a spec to work from.

**Trigger to re-evaluate:** first serious external-client request, or HA
native integration.

---

## 6. Drain protocol (config change during audio)

See Section 2. Keeping the atomic-based drain protocol. The `drain_request_`
and `process_busy_` pair on `esp_afe.h:269-270` is the correct shape for
the asymmetric writer/polling-reader pattern.

**Verdict: KEEP.** Tracked for round-2 Phase 2e (class-level block comment
to replace the scattered inline notes, no logic change).

---

## 7. Reference counting for mic / speaker consumers

- Ours: `esphome/components/i2s_audio_duplex/i2s_audio_duplex.cpp`
  `start_mic` (~line 606) / `stop_mic` (~line 613) manipulating
  `mic_ref_count_` atomic; `ctx.mic_running = mic_ref_count_.load() > 0`
  gates the two callback dispatch points (`raw_mic_callbacks_` and
  `mic_callbacks_`) in `process_aec_and_callbacks_`.
- Round-2 Phase 0 commit `05ac62f` preserves the count across the internal
  restart triggered by `frame_spec_revision` changes.

### Alternative: direct callback registration (upstream pattern)

Upstream `microphone::MicrophoneSource` exposes `add_data_callback()` and
consumers (MWW, voice_assistant) register once at setup. There is no
explicit start / stop concept — the mic is always enabled, callbacks
always fire, consumer decides whether to act on the data.

### Evaluation

The upstream pattern works for a passive producer (the microphone source is
just a data pipe with no lifecycle of its own). `i2s_audio_duplex` is an
active producer: it owns the I2S task, allocates buffers, and can be
paused. A refcount that is zero means "no consumer is interested; don't
spend cycles invoking callbacks".

The Phase 0 fix (save / restore across restart) is structurally fragile:
another future stop path could forget to preserve the count and the bug
reappears. Round-2 Phase 2a replaces the atomic counter with an explicit
`std::vector` of consumer tokens registered at setup — a list whose
content is stable across `stop()` / `start()` by construction.

**Verdict: KEEP refcount as a concept** (we still need to know whether any
consumer is listening), **MIGRATE implementation** to a registration list
in Phase 2a. This eliminates the "count lost on stop()" failure mode and
matches the shape of the upstream pattern while preserving the ability to
skip work when no consumer cares.

---

## Summary

| Area | Verdict | When to revisit |
|------|---------|-----------------|
| 1. Ring buffer allocation policy | KEEP | If upstream `RingBuffer::create()` gains a caps argument |
| 2. Task lifecycle / atomics for drain | KEEP | If we add a task needing synchronous notification |
| 3. AEC reference sources | KEEP (refactor dispatch in 2c) | If upstream `speaker` exposes an AEC ref hook |
| 4. FIR decimator wrapper | KEEP | If sample rates or filter shape change |
| 5. Intercom TCP wire format | DEFER | First external-client request, or HA native integration |
| 6. Drain protocol | KEEP (documentation in 2e) | If the scheme is ported to a new component |
| 7. Mic / speaker refcount | KEEP concept, MIGRATE impl (2a) | After Phase 2a validation |

No area was found where we have reimplemented a primitive that a solid
upstream alternative already provides. The audit produced three concrete
refactors queued for round-2 Phase 2 (dispatch for AEC ref sources, consumer
registry for mic gating, class-level drain protocol documentation) rather
than any migrations.
