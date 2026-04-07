#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "../audio_processor/audio_processor.h"

#ifdef USE_ESP32

#include <esp_afe_sr_iface.h>
#include <esp_afe_sr_models.h>
#include <esp_afe_config.h>
#include <freertos/semphr.h>

#include <atomic>
#include <numeric>

namespace esphome {
namespace esp_afe {

class EspAfe : public Component, public AudioProcessor {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::PROCESSOR; }

  // AudioProcessor interface
  bool is_initialized() const override { return this->afe_data_ != nullptr; }
  FrameSpec frame_spec() const override;
  bool process(const int16_t *in_mic, const int16_t *in_ref, int16_t *out,
               uint8_t mic_channels_in = 1) override;
  uint32_t frame_spec_revision() const override {
    return this->frame_spec_revision_.load(std::memory_order_relaxed);
  }
  FeatureControl feature_control(AudioFeature feature) const override;
  bool set_feature(AudioFeature feature, bool enabled) override;
  ProcessorTelemetry telemetry() const override;
  bool reconfigure(int type, int mode) override;

  // Config setters (called from Python codegen)
  void set_afe_type(int type) { this->afe_type_ = type; }
  void set_afe_mode(int mode) { this->afe_mode_ = mode; }
  void set_mic_num(int num) { this->mic_num_ = num; }
  void set_aec_enabled(bool en) { this->aec_enabled_ = en; }
  void set_aec_filter_length(int len) { this->aec_filter_length_ = len; }
  void set_se_enabled(bool en) { this->se_enabled_ = en; }
  void set_ns_enabled(bool en) { this->ns_enabled_ = en; }
  void set_vad_enabled(bool en) { this->vad_enabled_ = en; }
  void set_vad_mode(int mode) { this->vad_mode_ = mode; }
  void set_vad_min_speech_ms(int ms) { this->vad_min_speech_ms_ = ms; }
  void set_vad_min_noise_ms(int ms) { this->vad_min_noise_ms_ = ms; }
  void set_vad_delay_ms(int ms) { this->vad_delay_ms_ = ms; }
  void set_vad_mute_playback(bool en) { this->vad_mute_playback_ = en; }
  void set_vad_enable_channel_trigger(bool en) { this->vad_enable_channel_trigger_ = en; }
  void set_agc_enabled(bool en) { this->agc_enabled_ = en; }
  void set_agc_compression_gain(int gain) { this->agc_compression_gain_ = gain; }
  void set_agc_target_level(int level) { this->agc_target_level_ = level; }
  void set_memory_alloc_mode(int mode) { this->memory_alloc_mode_ = mode; }
  void set_afe_linear_gain(float gain) { this->afe_linear_gain_ = gain; }
  void set_task_core(int core) { this->task_core_ = core; }
  void set_task_priority(int prio) { this->task_priority_ = prio; }
  void set_ringbuf_size(int size) { this->ringbuf_size_ = size; }
  void set_input_volume_sensor_enabled(bool en) { this->input_volume_sensor_enabled_ = en; }
  void set_output_rms_sensor_enabled(bool en) { this->output_rms_sensor_enabled_ = en; }

  // Runtime toggles (for switches and automations)
  bool enable_aec();
  bool disable_aec();
  bool enable_ns();
  bool disable_ns();
  bool enable_se();
  bool disable_se();
  bool enable_vad();
  bool disable_vad();
  bool enable_agc();
  bool disable_agc();

  bool is_aec_enabled() const { return this->aec_enabled_; }
  bool is_se_enabled() const { return this->mic_num_ >= 2 && this->se_enabled_; }
  bool is_ns_enabled() const { return this->ns_enabled_; }
  bool is_vad_enabled() const { return this->vad_enabled_; }
  bool is_agc_enabled() const { return this->agc_enabled_; }
  bool is_voice_present() const { return this->voice_present_.load(std::memory_order_relaxed); }
  float get_input_volume_dbfs() const { return this->input_volume_dbfs_.load(std::memory_order_relaxed); }
  float get_output_rms_dbfs() const { return this->output_rms_dbfs_.load(std::memory_order_relaxed); }

  // Reinit with a new mode string (e.g. "sr_low_cost", "voip_high_perf").
  // Caller must stop audio processing before calling this.
  bool reinit_by_name(const std::string &name);

  ~EspAfe() override;

 protected:
  // Derive aec_mode_t from afe_type + afe_mode
  aec_mode_t derive_aec_mode_() const;
  int afe_mic_channels_() const;

  struct AfeInstance {
    const esp_afe_sr_iface_t *handle{nullptr};
    esp_afe_sr_data_t *data{nullptr};
    afe_config_t *config{nullptr};
    int16_t *feed_buf{nullptr};
    int feed_chunksize{0};
    int fetch_chunksize{0};
    int process_chunksize{0};
    int total_channels{0};
  };

  bool build_instance_(AfeInstance *instance);
  bool recreate_instance_(bool require_same_frame_sizes);
  bool set_aec_enabled_runtime_(bool enabled);
  bool set_reinit_flag_(bool &flag, bool enabled, const char *name);
  void destroy_instance_(AfeInstance *instance);
  void install_instance_(AfeInstance *instance);
  AfeInstance detach_instance_();
  const char *memory_alloc_mode_to_str_() const;

  // AFE vtable, opaque data, and config (config must outlive afe_data)
  const esp_afe_sr_iface_t *afe_handle_{nullptr};
  esp_afe_sr_data_t *afe_data_{nullptr};
  afe_config_t *afe_config_{nullptr};

  // Feed buffer: interleaved [mic, ref, mic, ref, ...] or [mic1, mic2, ref, ...]
  int16_t *feed_buf_{nullptr};
  int feed_chunksize_{0};   // per-channel samples expected by feed()
  int fetch_chunksize_{0};  // mono output samples returned by fetch()
  int process_chunksize_{0};  // external process() input chunk size
  int total_channels_{2};   // 2 for "MR", 3 for "MMR"
  int staged_input_samples_{0};
  bool fetch_started_{false};

  // Config (set from Python, used in setup())
  int afe_type_{0};         // AFE_TYPE_SR
  int afe_mode_{0};         // AFE_MODE_LOW_COST
  int mic_num_{1};  // physical microphone channels available from transport
  bool aec_enabled_{true};
  int aec_filter_length_{4};
  bool se_enabled_{false};
  bool ns_enabled_{true};
  bool vad_enabled_{false};
  int vad_mode_{VAD_MODE_3};
  int vad_min_speech_ms_{128};
  int vad_min_noise_ms_{1000};
  int vad_delay_ms_{128};
  bool vad_mute_playback_{false};
  bool vad_enable_channel_trigger_{false};
  bool agc_enabled_{true};
  int agc_compression_gain_{9};
  int agc_target_level_{3};
  int memory_alloc_mode_{AFE_MEMORY_ALLOC_MORE_PSRAM};
  float afe_linear_gain_{1.0f};
  int task_core_{1};
  int task_priority_{5};
  int ringbuf_size_{8};

  SemaphoreHandle_t config_mutex_{nullptr};
  std::atomic<bool> voice_present_{false};
  std::atomic<float> input_volume_dbfs_{-120.0f};
  std::atomic<float> output_rms_dbfs_{-120.0f};
  bool input_volume_sensor_enabled_{false};
  bool output_rms_sensor_enabled_{false};
  int warmup_remaining_{3};
  std::atomic<uint32_t> frame_count_{0};
  std::atomic<uint32_t> glitch_count_{0};
  std::atomic<float> ringbuf_free_pct_{1.0f};
  std::atomic<uint32_t> frame_spec_revision_{0};
  int last_spec_mic_ch_{0};  // last published mic_channels for revision tracking
};

class AfeSwitchBase : public switch_::Switch, public Component, public Parented<EspAfe> {
 public:
  float get_setup_priority() const override { return setup_priority::DATA; }

  void setup() override {
    if (this->parent_ != nullptr) {
      this->publish_state(this->get_parent_state_());
    }
  }

 protected:
  virtual bool get_parent_state_() const = 0;

  void publish_parent_state_() {
    if (this->parent_ != nullptr) {
      this->publish_state(this->get_parent_state_());
    }
  }
};

// Switch platform classes
class AfeAecSwitch : public AfeSwitchBase {
 public:
  void write_state(bool state) override {
    if (this->parent_ == nullptr)
      return;
    if ((state && this->parent_->enable_aec()) || (!state && this->parent_->disable_aec())) {
      this->publish_state(state);
    } else {
      this->publish_parent_state_();
    }
  }

 protected:
  bool get_parent_state_() const override { return this->parent_->is_aec_enabled(); }
};

class AfeNsSwitch : public AfeSwitchBase {
 public:
  void write_state(bool state) override {
    if (this->parent_ == nullptr)
      return;
    if ((state && this->parent_->enable_ns()) || (!state && this->parent_->disable_ns())) {
      this->publish_state(state);
    } else {
      this->publish_parent_state_();
    }
  }

 protected:
  bool get_parent_state_() const override { return this->parent_->is_ns_enabled(); }
};

class AfeSeSwitch : public AfeSwitchBase {
 public:
  void write_state(bool state) override {
    if (this->parent_ == nullptr)
      return;
    if ((state && this->parent_->enable_se()) || (!state && this->parent_->disable_se())) {
      this->publish_state(state);
    } else {
      this->publish_parent_state_();
    }
  }

 protected:
  bool get_parent_state_() const override { return this->parent_->is_se_enabled(); }
};

class AfeVadSwitch : public AfeSwitchBase {
 public:
  void write_state(bool state) override {
    if (this->parent_ == nullptr)
      return;
    if ((state && this->parent_->enable_vad()) || (!state && this->parent_->disable_vad())) {
      this->publish_state(state);
    } else {
      this->publish_parent_state_();
    }
  }

 protected:
  bool get_parent_state_() const override { return this->parent_->is_vad_enabled(); }
};

class AfeAgcSwitch : public AfeSwitchBase {
 public:
  void write_state(bool state) override {
    if (this->parent_ == nullptr)
      return;
    if ((state && this->parent_->enable_agc()) || (!state && this->parent_->disable_agc())) {
      this->publish_state(state);
    } else {
      this->publish_parent_state_();
    }
  }

 protected:
  bool get_parent_state_() const override { return this->parent_->is_agc_enabled(); }
};

class AfeVadBinarySensor : public binary_sensor::BinarySensor, public PollingComponent, public Parented<EspAfe> {
 public:
  float get_setup_priority() const override { return setup_priority::DATA; }

  void setup() override {
    if (this->parent_ != nullptr) {
      this->publish_state(this->parent_->is_voice_present());
    }
  }

  void update() override {
    if (this->parent_ != nullptr) {
      this->publish_state(this->parent_->is_voice_present());
    }
  }
};

class AfeInputVolumeSensor : public sensor::Sensor, public PollingComponent, public Parented<EspAfe> {
 public:
  float get_setup_priority() const override { return setup_priority::DATA; }

  void update() override {
    if (this->parent_ != nullptr) {
      this->publish_state(this->parent_->get_input_volume_dbfs());
    }
  }
};

class AfeOutputRmsSensor : public sensor::Sensor, public PollingComponent, public Parented<EspAfe> {
 public:
  float get_setup_priority() const override { return setup_priority::DATA; }

  void update() override {
    if (this->parent_ != nullptr) {
      this->publish_state(this->parent_->get_output_rms_dbfs());
    }
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
