# Audit findings — audio-core-v2

Living triage document. Each finding has a severity, target area, concrete
evidence, recommended action and the phase of the refactor plan that
addresses it. Sources: three Explore agents (C++ component mapping, YAML
mapping, ESPHome upstream patterns) plus Codex independent audit
(`/tmp/codex_audit_out.txt`). See `~/.claude/plans/ticklish-skipping-puddle.md`
for the phase definitions.

## Summary

| Area | Critical | High | Medium | Low |
|---|---:|---:|---:|---:|
| YAML sanitation | 0 | 4 | 6 | 3 |
| SDK options | 0 | 2 | 6 | 3 |
| C++ dead code / unused | 0 | 1 | 5 | 2 |
| C++ logging / dump_config | 0 | 3 | 3 | 1 |
| C++ upstream conformance | 0 | 3 | 3 | 2 |
| C++ lifecycle / safety | 1 | 1 | 0 | 0 |
| Packages / duplication | 0 | 5 | 2 | 0 |
| Test matrix coverage | 0 | 2 | 1 | 0 |
| Documentation | 0 | 0 | 4 | 0 |

Total: 1 critical, 21 high, 30 medium, 11 low.

Severity scale:
- **Critical**: memory leak, race, API break or user-visible broken feature. Fix before further refactor.
- **High**: hurts maintainability or correctness under load, should be addressed in the planned phase.
- **Medium**: inconsistency, redundancy, clarity — worth fixing but not blocking.
- **Low**: cosmetic, stylistic, optional.

---

## 1. YAML sanitation

| # | Sev | Finding | Evidence | Action | Phase |
|--:|:--:|---|---|---|:--:|
| 1.1 | — | ~~local YAML variants tracked in git~~ — verified: `*-local.yaml` already gitignored and untracked. No action needed. | — | RESOLVED (pre-existing) | — |
| 1.2 | — | ~~Fake `secrets.yaml` committed~~ — verified: `secrets.yaml` already gitignored globally and per-dir; zero matches in `git ls-files`. No action needed. | — | RESOLVED (pre-existing) | — |
| 1.3 | High | Dev-diary comments across YAMLs ("like old announcement behavior", session dates) | `xiaozhi-full-aec.yaml:397`, scattered | Strip session/date references, keep only timeless rationale | B |
| 1.4 | High | `test` variable name in LVGL text wrapping lambda — unclear intent, likely leftover | `xiaozhi-full-aec.yaml:1749-1757, 1805-1813` | Rename to semantic name or inline | B |
| 1.5 | Medium | `generic-s3-full.yaml` not renamed to `-aec` suffix unlike its peers | `yamls/full-experience/single-bus/aec/generic-s3-full.yaml` | Decide: rename for consistency or document why it stays | B |
| 1.6 | Medium | WIP ringtone unresolved in dual-bus intercom | `generic-s3-dual-intercom.yaml:7` ("not yet implemented") | Fix or mark as unsupported in README | B |
| 1.7 | Medium | Comments mix IT and EN | Multiple files | Translate all comments to English | B/E |
| 1.8 | Medium | Commented-out display interval ("DISABLED to prevent SPI race") | `xiaozhi-intercom.yaml:337-340` | Either delete (if never coming back) or document workaround with issue link | B |
| 1.9 | Medium | GitHub issue references in comments with no context | `waveshare-p4-full-aec.yaml` (esp-hosted#597) | Keep refs only where the workaround is still active; remove stale | B |
| 1.10 | Medium | Animation-timing inline notes ("Hold at peak (~1s)") | `waveshare-p4-full-aec.yaml:~1553` | Move to docstring of the script or delete if obvious | B |
| 1.11 | Low | `secrets.yaml` content differs across dirs | `yamls/**/secrets.yaml` | After gitignore, ensure one canonical local secrets file | B |
| 1.12 | Low | Inconsistent naming between `aec/` and `afe/` file suffixes | Tree root | Align naming convention in README | B |
| 1.13 | Low | `.esphome/` build artifacts present under `afe/` | `yamls/full-experience/single-bus/afe/.esphome/` | Ensure `.esphome/` is gitignored globally | B |

## 2. SDK options (Codex section 3)

| # | Sev | Finding | Evidence | Action | Phase |
|--:|:--:|---|---|---|:--:|
| 2.1 | High | `CONFIG_ESP_WIFI_PHY_IRAM_OPT` is stale (current IDF uses `CONFIG_ESP_PHY_IRAM_OPT`) | `xiaozhi-ball-v3-full-afe.yaml:173`, `esp_phy/Kconfig:187-193` | Remove or rename to correct symbol | B |
| 2.2 | High | `CONFIG_LWIP_MAX_SOCKETS=10` is redundant (ESPHome floor is already 10) | `waveshare-s3-full-aec.yaml:129`, `esp32/__init__.py:1513-1514` | Drop override; keep 16 only where headroom needed | B |
| 2.3 | Medium | `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` redundant when `network.enable_high_performance: true` already set | `xiaozhi...yaml:177`, `waveshare-p4...yaml:153-154` vs `wifi/__init__.py:586-595` | Drop where redundant; keep only where high-perf networking is off | B |
| 2.4 | Medium | `CONFIG_ESP_DEBUG_OCDAWARE` is a debug option in production YAMLs | Multiple | Move to `packages/debug/jtag.yaml`, off by default | B/C |
| 2.5 | Medium | `CONFIG_MBEDTLS_*` overrides (external mem, dynamic buffer, hardware AES) not justified by audited component paths | xiaozhi-*yamls | Keep only if benchmarks show RAM relief; otherwise remove | C |
| 2.6 | Medium | `CONFIG_ESP_TASK_WDT_TIMEOUT_S=30` (P4) override — no runtime justification found | `waveshare-p4-full-*.yaml` | Move to shared P4 system package; keep only if SDIO recovery requires it | C |
| 2.7 | Medium | `CONFIG_ESP_HOSTED_SDIO_RX/TX_Q_SIZE=50` is connectivity-specific | `waveshare-p4-full-*.yaml` | Move to `packages/board/waveshare_p4_c6.yaml` | C |
| 2.8 | Medium | `CONFIG_SPIRAM_FETCH_INSTRUCTIONS` / `CONFIG_SPIRAM_RODATA` duplicated across all YAMLs | Every S3/P4 YAML | Extract to `packages/profile/afe_memory.yaml`; on P4 consider `CONFIG_SPIRAM_XIP_FROM_PSRAM` (upstream-preferred) | C |
| 2.9 | Medium | `CONFIG_ESP32S3_DATA_CACHE_64KB` + `_LINE_64B` required by esp-sr but duplicated in every S3 YAML | Every S3 YAML | Extract to `packages/platform/esp32s3_base.yaml` with ref `esp_process_sdkconfig.c:26-31` | C |
| 2.10 | Medium | `CONFIG_CACHE_L2_CACHE_256KB` required by esp-sr on P4 but duplicated | P4 YAMLs | Extract to `packages/platform/esp32p4_base.yaml` with ref `esp_process_sdkconfig.c:60-65` | C |
| 2.11 | Low | ESP wifi/phy IRAM opts manual overrides not justified by component code | Multiple | Group in `packages/profile/wifi_performance.yaml`, opt-in | C |
| 2.12 | Low | `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536` default is 0, override justified by internal allocs | `waveshare-p4-full-afe.yaml` | Move to `packages/profile/afe_memory.yaml`; document which internal allocs need it | C |
| 2.13 | Low | P4 `cpu_frequency: 360MHz` not captured in a shared package — lived inline until snapshot commit | `waveshare-p4-full-afe.yaml:11` | Move into `packages/platform/esp32p4_base.yaml` with comment on esphome#13425 | C |

## 3. C++ dead code / unused

| # | Sev | Finding | Evidence | Action | Phase |
|--:|:--:|---|---|---|:--:|
| 3.1 | High | Unused internal-trigger getters in `intercom_api` never wired from YAML | `intercom_api.h:203-206` vs `intercom_api/__init__.py:176-215` | Remove unless exposing matching YAML hooks is planned | E |
| 3.2 | Medium | Unused getters `get_use_stereo_aec_reference()` / `get_reference_channel_right()` | `i2s_audio_duplex.h:404,408` | Remove | E |
| 3.3 | Medium | Python unused imports | `i2s_audio_duplex/__init__.py:13` (`fv`), `intercom_api/__init__.py:13` (`switch`), `intercom_api/number.py:5` and `intercom_api/switch.py:4` (`CONF_ID`) | Delete imports | E |
| 3.4 | Medium | Dev-diary comments in C++ referencing specific JTAG sessions | `esp_afe.cpp:289-297` ("JTAG audit 2026-04-18"), `esp_afe.cpp:1140-1143` | Rewrite as timeless technical rationale | E |
| 3.5 | Medium | Buffer-removal archaeology comments | `i2s_audio_duplex.h:509,513` and `.cpp:746,809,813,954` | Delete or rephrase as "why we do not have buffer X" if still informative | E |
| 3.6 | Medium | Hard-coded board-name context in C++ | `intercom_api.cpp:405-406` ("Waveshare/P4/Xiaozhi") | Generalize or move to README | E |
| 3.7 | Medium | Workaround history comments | `intercom_api.cpp:1210-1213, 1344` | Rewrite as constraint explanation without commit-history framing | E |
| 3.8 | Low | Disabled AFE diagnostic sensors block commented | `xiaozhi-ball-v3-full-afe.yaml:1441-1444` | Delete if not re-enabling; otherwise put behind a documented substitution flag | B |
| 3.9 | Low | No `#if 0` / TODO / FIXME found in component trees (Codex verified) | n/a | No action | — |

## 4. C++ logging / dump_config

| # | Sev | Finding | Evidence | Action | Phase |
|--:|:--:|---|---|---|:--:|
| 4.1 | High | `ESP_LOGI` used for state transitions where upstream uses `D`/`V` (biggest convention drift per Codex) | `esp_afe.cpp:241-242,315-317,459-460,583-584,1019-1020`; `i2s_audio_duplex.cpp:153,453,482,503,550,672,687,796-799,824,870,957`; `intercom_api.cpp:376-380,464,479,498,507-509,552,618,629,869-874,1495,1503,1600,1676` | Demote transitions/tuning to `D`; keep boot summaries and user-facing events at `I` | E/F |
| 4.2 | High | No explicit `dump_config()` output in components | All 4 custom components | Add ESP_LOGCONFIG per parameter (benchmark: `voice_assistant`, `micro_wake_word`) | E/F |
| 4.3 | High | Per-frame telemetry at `I`/`D` not rate-limited | `i2s_audio_duplex.cpp:796-799,957` | Rate-limit with `millis()` threshold | F |
| 4.4 | Medium | Inconsistent TAG snake_case across files | Spot-check all four components | Standardize `static const char *const TAG = "<component>";` | E |
| 4.5 | Medium | `ESP_LOGCONFIG` not used for startup summary (upstream does) | All 4 | Use `ESP_LOGCONFIG(TAG, ...)` in `dump_config()` | E |
| 4.6 | Medium | Comment `// Debug telemetry stripped at compile time when logger.level >= INFO` absent | All 4 | Add a one-liner at the top of each component to anchor the logging policy | F |
| 4.7 | Low | Log tag strings in mixed styles ("esp_afe", "audio.duplex") | Grep all TAGs | Pick `<component>` consistently (snake_case matching folder) | E |

## 5. C++ upstream conformance

| # | Sev | Finding | Evidence | Action | Phase |
|--:|:--:|---|---|---|:--:|
| 5.1 | High | `audio_processor` puts `AudioProcessor`, `FrameSpec`, enums in top-level `esphome` namespace, upstream uses component-local | `audio_processor/audio_processor.h:5-103` vs `voice_assistant.h:29-30`, `micro_wake_word.h:24-25` | Move into `esphome::audio_processor`; update 3 consumers in one atomic commit | E |
| 5.2 | High | `intercom_api` opens raw sockets at runtime but does not call `socket.consume_sockets` during validation | `intercom_api.cpp:1567-1600,1630-1676` vs `api/__init__.py:230-237` | Add quota reservation in `intercom_api/__init__.py` FINAL_VALIDATE_SCHEMA; once in place, drop the `CONFIG_LWIP_MAX_SOCKETS` override | E |
| 5.3 | High | `esp_afe.cpp:1140-1145` allocates `fetch_output_ring_` with bare `RingBuffer::create()` bypassing the audit-introduced helpers | vs `intercom_api.cpp:132-142`, `i2s_audio_duplex.cpp:117-118,792-793`, `audio_processor/ring_buffer_caps.h:12-55` | Route through `ring_buffer_caps` helpers | E |
| 5.4 | Medium | Schema style: parent-ID duplicated in nested child schemas where upstream is root-driven | `esp_afe/switch.py:33-53`, `intercom_api/number.py:29-59`, `intercom_api/switch.py:27-56` | Refactor to root-only parent-ID (like `esp_afe/sensor.py:40-50`, `i2s_audio/speaker/__init__.py:115-164`) | E |
| 5.5 | Medium | Task stack sizes not exposed as `static constexpr` in i2s_audio_duplex / intercom_api (esp_afe already does) | `esp_afe.h` has `kFeedTaskStackWordsSingleMic` etc. | Port pattern to the other two components | E |
| 5.6 | Medium | Component cross-references missing in header docstrings | All 4 | Add one-line "Implements / collaborates with" blurb at class level | E |
| 5.7 | Low | Folder and class naming mostly consistent with upstream, except namespace issue (5.1) | n/a | No action beyond 5.1 | — |
| 5.8 | Low | FreeRTOS raw calls used, but so does upstream — no action required | `i2s_audio_microphone.cpp:35-45,355-371`, `micro_wake_word.cpp:242-280` | No action | — |

## 6. C++ lifecycle / safety

| # | Sev | Finding | Evidence | Action | Phase |
|--:|:--:|---|---|---|:--:|
| 6.1 | Critical | `duplex_microphone` may leak a counting semaphore if event-group creation fails and the component calls `mark_failed()` | `i2s_audio_duplex/microphone/duplex_microphone.cpp:17-30` | Add cleanup path that deletes the semaphore before returning | E |
| 6.2 | High | Drain protocol (`drain_request_` / `process_busy_`) documentation scattered across inline comments | `esp_afe.h` and `.cpp` | Add single class-level comment (~10 lines) explaining the lock-free handshake | E |

## 7. Packages / duplication

| # | Sev | Finding | Evidence | Action | Phase |
|--:|:--:|---|---|---|:--:|
| 7.1 | High | LED 12-state `!extend update_led` lambda duplicated in 6 full-experience YAMLs | All full-experience YAMLs | Extract to `packages/ui/led_pattern_12.yaml` | C |
| 7.2 | High | ES7210 TDM init lambda duplicated across Waveshare AEC and AFE YAMLs | `waveshare-{p4,s3}-full-aec.yaml`, `afe/waveshare-*` | Extract to `packages/codec/es7210_tdm.yaml` | C |
| 7.3 | High | ES8311 register 0x44 digital-AEC-reference lambda duplicated across Xiaozhi variants | `xiaozhi-intercom.yaml:38-59`, `xiaozhi-full-aec.yaml:121-137`, `xiaozhi-ball-v3-full-aec-local.yaml:115-131` | Extract to `packages/codec/es8311_stereo_ref.yaml` | C |
| 7.4 | High | LVGL intercom page defined inline in both `xiaozhi-intercom.yaml` and `xiaozhi-full-aec.yaml`; intercom-only still uses ili9xxx C++ lambdas with no swipe gestures while the full experience has CST816 swipe logic | Both files | Extract to `packages/ui/lvgl_intercom_page_round_240.yaml` (incl. CST816 swipe: next/prev contact, page navigation) | D |
| 7.5 | High | Circular-display text wrapping duplicated | `xiaozhi-full-aec.yaml:1747-1820`, `xiaozhi-ball-v3-full-aec-local.yaml` | Extract to `packages/ui/circular_display_text.yaml` (optional) | D |
| 7.6 | Medium | Media-player callback handlers nearly identical across full YAMLs | All full-experience YAMLs | Extract to shared script package if rewrite is clean; otherwise leave | C/D |
| 7.7 | Medium | `display/LVGL page routing` lambda duplicated in P4 and Xiaozhi | `waveshare-p4-full-aec.yaml`, `xiaozhi-full-aec.yaml` | Evaluate extraction (display-dependent) | D |

## 8. Test matrix coverage

| # | Sev | Finding | Evidence | Action | Phase |
|--:|:--:|---|---|---|:--:|
| 8.1 | High | No standalone duplex-only example covering S4 combination (i2s_audio_duplex with no consumers) | Repo root | Add `yamls/examples/duplex-only-s3.yaml` | D |
| 8.2 | High | Intercom-only YAMLs all bundle `esp_aec` — S2 (intercom + AEC plugin) is effectively the same as S1 | Matrix audit | Explicit decision: document S1==S2 in the plan; no new YAML needed | D |
| 8.3 | Medium | No way to run a display device without LVGL (architectural rule confirmed with user: LVGL mandatory on display devices) | Codex + user directive | Codify in README: "display devices always ship with LVGL" | D/G |

## 9. Documentation

| # | Sev | Finding | Evidence | Action | Phase |
|--:|:--:|---|---|---|:--:|
| 9.1 | Medium | `audio_processor` lacks a README | `esphome/components/audio_processor/` | Add README describing the interface contract and ring-buffer policy | G |
| 9.2 | Medium | Existing component READMEs are written in dev-diary / post-mortem tone | `esp_afe/README.md`, `i2s_audio_duplex/README.md`, `intercom_api/README.md` | Rewrite to consumer orientation with YAML example + API | G |
| 9.3 | Medium | No top-level `yamls/README.md` map of what each YAML provides | `yamls/README.md` (38 lines) | Expand into a one-table index of device YAMLs with the component matrix | G |
| 9.4 | Medium | No documentation of LVGL intercom page extraction policy | Plan Fase D | Document in the intercom-only README and in each LVGL package header | G |

---

## Cross-cutting decisions captured

- **Branch**: work happens on `audio-core-v2-audit`, branched from `audio-core-v2@1a98499`.
- **Default comments language**: English only. IT comments are translated during E.5.
- **Runtime / task architecture / ring buffer / AFE feed-fetch / FIR decimator / TCP wire format**: untouched by default. Changes require explicit user approval.
- **YAML API**: unchanged by default. Breaking changes require explicit user approval.
- **Commit cadence**: one commit per resolved finding or small group of related findings; no per-phase review gate. If something breaks end-to-end, bisect the history.
- **Codex as antagonist**: for any non-trivial refactor proposal, send Codex a focused prompt with the intended diff and 1-3 questions, treat the reply as sparring input, not an oracle.

## Open questions deferred to execution

- **2.5 (MBEDTLS overrides)**: keep or drop requires a benchmark. Defer decision to Fase C with a measurement run.
- **2.8 (SPIRAM FETCH/RODATA on P4)**: Codex suggests `CONFIG_SPIRAM_XIP_FROM_PSRAM` is upstream-preferred on P4. Validate before swapping.
- **1.5 (generic-s3-full.yaml not renamed)**: confirm with user whether to rename for consistency.
- **1.6 (generic-s3-dual-intercom WIP ringtone)**: either fix or drop from public shipping list.
- **5.2 (socket quota)**: once implemented, reassess whether `CONFIG_LWIP_MAX_SOCKETS=16` override is still needed.

---

## Round-2 status (2026-04-19)

Round 1 closed 14 commits (phases A–E largely applied). Round 2 is a
targeted follow-up on `audio-core-v2-audit` that addresses the patterns
the round-1 scope explicitly left alone. Plan:
`~/.claude/plans/ticklish-skipping-puddle.md`. Deliverables already on disk:
[`LIBRARY_ALTERNATIVES.md`](LIBRARY_ALTERNATIVES.md),
[`PATTERN_AUDIT.md`](PATTERN_AUDIT.md), [`ARCHITECTURE.md`](ARCHITECTURE.md).

### Closed in round 2

| # | Status | Notes |
|---|---|---|
| **P2 surgical** (mic_ref_count lost across restart) | CLOSED | Commit `05ac62f`. Save/restore around internal restart. Validated on WS3. Structural follow-up P2a in Phase 2. |
| **P7 surgical** (static TCB reuse corruption) | CLOSED | Commit `94e553c`. Feed/fetch tasks use dynamic `xTaskCreatePinnedToCore`. Validated on WS3 under rapid toggle stress. Structural follow-up P7/P3 in Phase 2b. |
| **4.1 VAD state log demote** | PARTIAL | One `ESP_LOGI` in `esp_afe.cpp` VAD transition demoted to `LOGD` as part of `94e553c`. Remaining log sites queued in Phase 3. |

### Open — queued for round-2 Phase 2 (structural refactor)

| # | Target | Phase |
|---|---|:--:|
| P2 structural (refcount → consumer registry) | `i2s_audio_duplex` | 2a |
| P3/P7 structural (task-alive + paused flag) | `esp_afe` | 2b |
| P4 (AEC ref four-path → polymorphic) | `i2s_audio_duplex` | 2c |
| P5 (all-features-disabled) | documentation, `esp_afe` | 2d (done in `ARCHITECTURE.md` §7) |
| P1 (drain protocol class-level doc) | `esp_afe.h` | 2e |
| P6 (FIR lazy-init rename) | `i2s_audio_duplex` | 2f |
| 4.1/4.2/4.3 (log systematic pass + dump_config) | all components | 3 |
| 9.* (professional READMEs + DEPLOYMENT_GUIDE) | docs | 4 |

### Open — deferred past round 2

- **Protobuf wire format** (LIBRARY_ALTERNATIVES §5): no breaking-change budget.
- **S2 / S4 YAML examples** (8.1, 8.2): user scope decision — skipped.
- **FIR migration to `dsps_fird_s16_aes3`**: design pending
  (`project_fir_migration_design.md`), not applied.
