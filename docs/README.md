# Documentation

Welcome. These pages cover everything beyond the project pitch on the [top-level README](../README.md).

## Pick your path

- **Choosing a YAML preset** — [DEPLOYMENT_GUIDE.md](DEPLOYMENT_GUIDE.md) — a decision tree that maps hardware and features (intercom only, VA + MWW, touch display, 2-mic beamforming) to the right ready-to-flash config under [`yamls/`](../yamls/).

- **Configuration reference** — [reference.md](reference.md) — every `intercom_api`, `esp_aec` and `esp_afe` option, every action and condition, the Home Assistant services, and worked automation examples.

- **How the audio stack works** — [ARCHITECTURE.md](ARCHITECTURE.md) — component decomposition, threading and core-affinity model, data flow per frame, the `audio_processor` contract, the drain protocol for glitch-free config changes.

- **Troubleshooting** — [troubleshooting.md](troubleshooting.md) — common symptoms (no devices found, no audio, echo, latency, ringing without connect, full-mode discovery) with concrete checks.

## Per-component docs

Each ESPHome component ships its own README with the full option list, YAML snippets and component-specific notes:

- [`intercom_api`](../esphome/components/intercom_api/README.md) — TCP intercom server, call state machine, Home Assistant bridge
- [`i2s_audio_duplex`](../esphome/components/i2s_audio_duplex/README.md) — full-duplex I²S driver with FIR decimation, AEC reference capture and dual mic outputs
- [`esp_aec`](../esphome/components/esp_aec/README.md) — standalone ESP-SR echo cancellation
- [`esp_afe`](../esphome/components/esp_afe/README.md) — full Espressif AFE pipeline (AEC + NS + VAD + AGC, optional dual-mic beamforming)
- [`audio_processor`](../esphome/components/audio_processor/README.md) — abstract processor interface that `esp_aec` / `esp_afe` implement
- [`mdns_discovery`](../esphome/components/mdns_discovery/README.md) — mDNS peer discovery for full-mode intercom
- [`old_intercom_udp`](../esphome/components/old_intercom_udp/README.md) — deprecated UDP-based predecessor, kept for backward compatibility
