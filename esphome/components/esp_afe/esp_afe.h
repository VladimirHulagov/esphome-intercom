#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/components/switch/switch.h"
#include "aec_processor.h"

#ifdef USE_ESP32

#include <esp_afe_sr_iface.h>
#include <esp_afe_sr_models.h>
#include <esp_afe_config.h>

namespace esphome {
namespace esp_afe {

class EspAfe : public Component, public AecProcessor {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::PROCESSOR; }

  // AecProcessor interface
  bool is_initialized() const override { return this->afe_data_ != nullptr; }
  int get_frame_size() const override { return this->fetch_chunksize_; }
  int get_mic_num() const override { return this->mic_num_; }
  void process(const int16_t *mic_in, const int16_t *ref_in,
               int16_t *out, int frame_size) override;

  // Config setters (called from Python codegen)
  void set_afe_type(int type) { this->afe_type_ = type; }
  void set_afe_mode(int mode) { this->afe_mode_ = mode; }
  void set_mic_num(int num) { this->mic_num_ = num; }
  void set_aec_enabled(bool en) { this->aec_enabled_ = en; }
  void set_aec_filter_length(int len) { this->aec_filter_length_ = len; }
  void set_ns_enabled(bool en) { this->ns_enabled_ = en; }
  void set_vad_enabled(bool en) { this->vad_enabled_ = en; }
  void set_agc_enabled(bool en) { this->agc_enabled_ = en; }
  void set_agc_compression_gain(int gain) { this->agc_compression_gain_ = gain; }
  void set_agc_target_level(int level) { this->agc_target_level_ = level; }
  void set_task_core(int core) { this->task_core_ = core; }
  void set_task_priority(int prio) { this->task_priority_ = prio; }
  void set_ringbuf_size(int size) { this->ringbuf_size_ = size; }

  // Runtime toggles (for switches and automations)
  void enable_aec();
  void disable_aec();
  void enable_ns();
  void disable_ns();
  void enable_vad();
  void disable_vad();
  void enable_agc();
  void disable_agc();

  // Reinit with a new mode string (e.g. "sr_low_cost", "voip_high_perf").
  // Caller must stop audio processing before calling this.
  bool reinit_by_name(const std::string &name);

  ~EspAfe() override;

 protected:
  // Derive aec_mode_t from afe_type + afe_mode
  aec_mode_t derive_aec_mode_() const;

  // AFE vtable, opaque data, and config (config must outlive afe_data)
  const esp_afe_sr_iface_t *afe_handle_{nullptr};
  esp_afe_sr_data_t *afe_data_{nullptr};
  afe_config_t *afe_config_{nullptr};

  // Feed buffer: interleaved [mic, ref, mic, ref, ...] or [mic1, mic2, ref, ...]
  int16_t *feed_buf_{nullptr};
  int feed_chunksize_{0};   // per-channel samples expected by feed()
  int fetch_chunksize_{0};  // mono output samples returned by fetch()
  int total_channels_{2};   // 2 for "MR", 3 for "MMR"

  // Config (set from Python, used in setup())
  int afe_type_{0};         // AFE_TYPE_SR
  int afe_mode_{0};         // AFE_MODE_LOW_COST
  int mic_num_{1};
  bool aec_enabled_{true};
  int aec_filter_length_{4};
  bool ns_enabled_{true};
  bool vad_enabled_{false};
  bool agc_enabled_{true};
  int agc_compression_gain_{9};
  int agc_target_level_{3};
  int task_core_{1};
  int task_priority_{5};
  int ringbuf_size_{8};

  int warmup_remaining_{3};
  uint32_t frame_count_{0};
};

// Switch platform classes
class AfeAecSwitch : public switch_::Switch, public Parented<EspAfe> {
 public:
  void write_state(bool state) override {
    if (state) { this->parent_->enable_aec(); } else { this->parent_->disable_aec(); }
    this->publish_state(state);
  }
};

class AfeNsSwitch : public switch_::Switch, public Parented<EspAfe> {
 public:
  void write_state(bool state) override {
    if (state) { this->parent_->enable_ns(); } else { this->parent_->disable_ns(); }
    this->publish_state(state);
  }
};

class AfeVadSwitch : public switch_::Switch, public Parented<EspAfe> {
 public:
  void write_state(bool state) override {
    if (state) { this->parent_->enable_vad(); } else { this->parent_->disable_vad(); }
    this->publish_state(state);
  }
};

class AfeAgcSwitch : public switch_::Switch, public Parented<EspAfe> {
 public:
  void write_state(bool state) override {
    if (state) { this->parent_->enable_agc(); } else { this->parent_->disable_agc(); }
    this->publish_state(state);
  }
};

// Action: esp_afe.set_mode
template<typename... Ts>
class SetModeAction : public Action<Ts...>, public Parented<EspAfe> {
 public:
  TEMPLATABLE_VALUE(std::string, mode)
  void play(const Ts &...x) override {
    this->parent_->reinit_by_name(this->mode_.value(x...));
  }
};

}  // namespace esp_afe
}  // namespace esphome

#endif  // USE_ESP32
