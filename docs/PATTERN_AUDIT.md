# Pattern Audit

## Purpose

A targeted review of patterns in our custom components that "work, but feel
like workarounds". For each pattern we record what it is, why it looked
suspect, what it costs to keep, what it costs to change, and the verdict.

The focus is architectural trust: a pattern that works today but will
surprise the next contributor is a liability. A pattern that is unusual
but documented with its rationale is fine.

Seven patterns are indexed P1–P7. After round-2 Phase 0 two of them
surfaced real bugs that are already fixed in commits `05ac62f` and `94e553c`
(see **Debt eliminated** at the end).

---

## P1 — `esp_afe` drain protocol

**What.** `esp_afe.h:263-271` holds a pair of atomics, `drain_request_`
and `process_busy_`. Config-change path in `recreate_instance_`
(`esp_afe.cpp:365-379`) sets `drain_request_ = true`, then spins with
`vTaskDelay(1)` until `process_busy_` is observed clear or a 50 ms timeout
elapses. Audio hot path (`process()` at `esp_afe.cpp:747-758`) marks itself
busy on frame entry and bails to passthrough if it finds `drain_request_`
set.

**Why it looked suspect.** Busy-wait with a "proceed anyway" fallback. The
comment at `esp_afe.cpp:369-370` explicitly admits the failure mode:
"Timeout is a safety net: if i2s_audio_task is stuck elsewhere we proceed
anyway to avoid deadlocking the reconfiguration." Proceeding while a
consumer is mid-frame could expose a half-torn instance.

**What it costs to keep.** Two atomic bools, one mutex, 12 lines of
scattered inline comments. Zero overhead in the audio hot path (one
relaxed-order atomic load per frame).

**What it costs to change.** FreeRTOS `EventGroup` would replace the
busy-wait with a blocking wait, but the atomic in the reader side would
stay (we do not want `process()` to ever block). Net: another FreeRTOS
primitive, no latency win, no safety win.

**Verdict: KEEP.** The shape is correct for an asymmetric
writer/polling-reader. The "proceed anyway" fallback is pragmatic: in
practice `process()` is either running (and will bail at its next frame
boundary) or long-blocked in which case something else is already broken.

**Action for round 2.** Phase 2e: replace the scattered inline comments
with a single class-level block comment that documents the protocol as a
unit. No logic change.

---

## P2 — `i2s_audio_duplex::mic_ref_count_` (real bug, surgical fix landed)

**What.** `i2s_audio_duplex.cpp:606-611` increments/decrements
`mic_ref_count_` on `start_mic()` / `stop_mic()`; the audio task reads
`mic_running = mic_ref_count_.load() > 0` (line ~846) and gates the two
callback dispatch points (`raw_mic_callbacks_` for MWW on line 1151,
`mic_callbacks_` for intercom / VA on line 1226) on that flag.

**Why it was a real bug.** `stop()` at line 560 zeroes the counter.
`start()` never restores it. The internal restart path in `loop()`
(triggered by a processor `frame_spec_revision` change on SE toggle)
calls `stop(); start();`. After that sequence consumers registered once
at setup via `start_mic()` stay at count zero forever — MWW stops
detecting, intercom TX goes silent, VAD stalls. Only a reboot recovers.

**Fix in round-2 Phase 0** (`05ac62f`): snapshot the count in `loop()`
before `stop()` and restore it after `start()`. 8 lines, surgical, passes
the stress reproducer on WS3.

**Why the design is still weak.** Any future code path that calls
`stop()` outside of `loop()` without the same save/restore will reintroduce
the bug. The fix is defensive, not structural.

**Verdict: MIGRATE.** Phase 2a replaces the atomic counter with a
registration list:

```cpp
std::vector<void *> mic_consumers_;          // tokens registered at setup
bool has_mic_consumers() const { return !mic_consumers_.empty(); }
```

`start_mic()` / `stop_mic()` become idempotent wrappers around
`register_mic_consumer(token) / unregister_mic_consumer(token)`. The list
survives `stop()` by construction — no save/restore needed.

---

## P3 — Audio-task self-suicide on frame_spec change

**What.** When the processor's `frame_spec_revision()` changes (SE toggle
flips mic channels 2 ↔ 1), the audio task at
`i2s_audio_duplex.cpp:864-871` sets `needs_restart_ = true` and breaks
out of its loop. `loop()` at lines 150-163 then calls `stop(); start();`
to reallocate buffers with the new channel count.

**Why it looked suspect.** A realtime task that kills itself to let the
main thread reallocate its buffers is unusual. Every restart loses
per-task state (speaker underrun counter, telemetry accumulators, FIR
decimator delay lines) and pays ~70 ms of audio gap.

**What it costs to keep.** Two atomic flags (`needs_restart_`,
`duplex_running_`) and the reallocation cost (~70 ms at 16 kHz on WS3,
measured).

**What it costs to change.** The mic-channel-dependent buffers
(`processor_mic_buffer`, `rx_decimator_`) can be reinitialised in place
from inside the task on detection of a revision bump. Pre-allocating the
worst-case buffers (2 mic) at setup costs ~1 kB of PSRAM in the 1-mic
case, acceptable.

**Verdict: MIGRATE** (round-2 Phase 2b). Draft sketch in the round-2 plan
file. Not urgent: SE toggles are rare in production and the current path
works with P2 fixed. Tech debt, not a bug.

---

## P4 — Four AEC reference paths

**What.** `process_aec_and_callbacks_` at
`i2s_audio_duplex.cpp:1146-1231` selects one of four AEC reference
sources via nested conditionals: TDM hardware slot, stereo I2S loopback,
software ring buffer, previous-frame fallback. Selection is
config-driven at boot and fixed thereafter.

**Why it looked suspect.** Nested-conditional dispatch four levels deep
is hard to read and hard to unit-test. Each branch has its own decimation
logic.

**What it costs to keep.** Readability. No runtime cost — the `if`
ladder compiles to a predictable jump.

**What it costs to change.** Introduce a small polymorphic hierarchy
(`AecReferenceSource` base, four concrete subclasses), instantiate one at
setup based on YAML flags, replace the ladder with a single virtual
call. vtable dispatch adds ~20 ns per frame, trivially under budget.

**Verdict: MIGRATE** (round-2 Phase 2c). The change is internal-only, no
YAML impact. See LIBRARY_ALTERNATIVES.md §3 for why the four sources
themselves are irreducible.

---

## P5 — "All features disabled" teardown fast path

**What.** When every AFE feature is off (`aec_enabled_ == ns_enabled_ ==
vad_enabled_ == agc_enabled_ == false` and also `se_enabled_` off for
2-mic), `recreate_instance_` at `esp_afe.cpp:396-406` tears down the
esp-sr instance entirely and sets `afe_stopped_ = true`. `process()` at
`esp_afe.cpp:729-734` then uses a passthrough fast path that copies raw
mic data to the output using `last_spec_*` cached sizes.

**Why it looked suspect.** A second code path in `process()` that does
something different when the instance does not exist. The `last_spec_*`
members exist only to support this one fallback.

**What it costs to keep.** A few cached member variables, about 6 lines
of fallback code in `process()` and `frame_spec()`, and a boot-order
caveat (first setup call must go through the normal build path so the
cache gets populated before the first toggle-to-all-off).

**What it costs to change.** Removing it means keeping the esp-sr
instance alive even when every feature is off. esp-sr still runs its
internal ringbuffer/worker for a pipeline of `[input] -> [output]` with
no filters — harmless but ~100 kB of PSRAM permanently in use.

**Verdict: KEEP.** The toggle-to-all-off scenario is real (a user who
wants raw mic without AEC/NS for debugging) and paying 100 kB of PSRAM
for a mode nobody uses most of the time is wasteful. The fallback is
correct and the boot-order caveat is enforced by the code at lines
396-397.

**Action for round 2.** Document the fast path in `ARCHITECTURE.md`
(Phase 1) with a class-level note in `esp_afe.h` pointing to it.

---

## P6 — FIR decimator lazy init inside the audio task

**What.** `i2s_audio_duplex.cpp:696-701` initialises the RX decimator
inside `audio_task_()` rather than in `setup()`, because the channel
count depends on `processor_->frame_spec()` which is first read from the
task.

**Why it looked suspect.** Initialising non-trivial state from a
real-time task priority reads like a latent foot-gun.

**What it costs to keep.** One extra branch per task start. The
initialisation itself is idempotent (guarded by a channel-count compare)
and happens exactly once under normal flow.

**What it costs to change.** Move the init to `setup()` and defer the
channel count: the task would still need to pick up dynamic changes
(P3's territory). They are the same refactor.

**Verdict: KEEP**, rename the comment at line 106 from "deferred to
audio_task_" to "lazy init on first frame" in round-2 Phase 2f so the
intent is clear.

---

## P7 — Static TCB reuse in `esp_afe` (real bug, fixed)

**What it was.** `esp_afe` spawned `afe_feed` and `afe_fetch` with
`xTaskCreateStaticPinnedToCore`, reusing `feed_task_tcb_`,
`feed_task_stack_`, `fetch_task_tcb_`, `fetch_task_stack_` members across
stop/start cycles on every reconfigure.

**Why it was a real bug.** FreeRTOS static task deletion leaves the TCB
on `xTasksWaitingTermination` until IDLE on the target core processes
it. Under rapid toggle stress, IDLE on core 1 stayed starved long enough
that the old task's `xStateListItem` was still linked into a ready list
when the next start re-initialised the same TCB, breaking the list.
`prvAddNewTaskToReadyList` then crashed with `StoreProhibited` at
`EXCVADDR 0x4`.

**Reproduce** (before fix): on WS3 AFE, SE off → AEC off → NS on within a
few seconds → panic in FreeRTOS kernel.

**Fix in round-2 Phase 0** (`94e553c`): switch to dynamic
`xTaskCreatePinnedToCore`. FreeRTOS owns TCB+stack lifetime; each start
gets fresh memory, old memory is reclaimed by IDLE on its own schedule,
no list corruption. Cost: ~16 kB internal heap churn per reconfigure.

**Why the design is still improvable.** Dynamic task creation
successfully eliminates the list-corruption class of bug but introduces
16 kB of transient heap traffic per user toggle. A toggle-heavy session
over months of uptime could fragment internal RAM.

**Verdict: MIGRATE** (round-2 Phase 2b, shared with P3). Keep tasks alive
for the lifetime of the component, gate their work loop on a paused
flag, destroy and rebuild the esp-sr instance under mutex without
touching the tasks. Zero heap churn, no list reuse, tasks keep their
telemetry.

---

## Summary

| ID | Pattern | Verdict | Status |
|---:|---------|---------|--------|
| P1 | Drain protocol atomics | KEEP | Round-2 Phase 2e: class-level docs |
| P2 | `mic_ref_count_` refcount | MIGRATE | Phase 0 surgical fix landed; Phase 2a structural migration queued |
| P3 | Audio task self-suicide on spec change | MIGRATE | Round-2 Phase 2b |
| P4 | Four AEC reference paths | MIGRATE (dispatch only) | Round-2 Phase 2c |
| P5 | All-features-disabled teardown | KEEP | Document in ARCHITECTURE.md |
| P6 | FIR decimator lazy init in task | KEEP | Rename comment in Phase 2f |
| P7 | Static TCB reuse | MIGRATE | Phase 0 dynamic-task fix landed; Phase 2b structural migration queued |

Two real bugs fixed in Phase 0 (P2, P7). Three architectural debts
queued for Phase 2 (P2-structural, P3+P7-structural, P4-dispatch). Two
patterns kept with documentation (P1, P5). One renamed (P6).

---

## Debt eliminated in round 2

Filled as Phase 2 lands.

- **P2 surgical** — commit `05ac62f` ("i2s_audio_duplex: preserve
  mic_ref_count_ across frame_spec restart"). MMR→MR reproducer clean on
  WS3.
- **P7 surgical** — commit `94e553c` ("esp_afe: switch feed/fetch tasks
  to dynamic creation"). Static TCB reuse crash cleared under the
  SE/AEC/NS toggle-burst stress test on WS3.
- **P2 structural** — pending Phase 2a.
- **P3 + P7 structural** — pending Phase 2b.
- **P4 dispatch refactor** — pending Phase 2c.
- **P6 comment rename** — pending Phase 2f.
- **P5 documentation** — pending Phase 1 ARCHITECTURE.md.
- **P1 class-level comment** — pending Phase 2e.
