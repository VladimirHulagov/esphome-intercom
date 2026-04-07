#include "esp_aec.h"

#ifdef USE_ESP32

#include <cstring>

#include "esphome/core/log.h"

namespace esphome {
namespace esp_aec {

static const char *const TAG = "esp_aec";

void EspAec::setup() {
  ESP_LOGI(TAG, "Initializing AEC...");
  this->handle_ = aec_create(this->sample_rate_, this->filter_length_, 1, this->mode_);
  if (this->handle_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create AEC instance");
    this->mark_failed();
    return;
  }
  this->cached_frame_size_ = aec_get_chunksize(this->handle_);
  ESP_LOGI(TAG, "AEC initialized: rate=%d, filter=%d, frame=%d (%dms)",
           this->sample_rate_, this->filter_length_, this->cached_frame_size_,
           this->cached_frame_size_ * 1000 / this->sample_rate_);
}

EspAec::~EspAec() {
  if (this->handle_ != nullptr) {
    aec_destroy(this->handle_);
    this->handle_ = nullptr;
  }
}

void EspAec::dump_config() {
  ESP_LOGCONFIG(TAG, "ESP AEC (ESP-SR standalone):");
  ESP_LOGCONFIG(TAG, "  Sample Rate: %d Hz", this->sample_rate_);
  ESP_LOGCONFIG(TAG, "  Filter Length: %d", this->filter_length_);
  ESP_LOGCONFIG(TAG, "  Frame Size: %d samples", this->cached_frame_size_);
  ESP_LOGCONFIG(TAG, "  Mode: %d", (int) this->mode_);
  ESP_LOGCONFIG(TAG, "  Initialized: %s", this->is_initialized() ? "YES" : "NO");
}

FrameSpec EspAec::frame_spec() const {
  FrameSpec spec;
  spec.sample_rate = this->sample_rate_;
  spec.mic_channels = 1;
  spec.ref_channels = 1;
  spec.input_samples = this->cached_frame_size_;
  spec.output_samples = this->cached_frame_size_;
  return spec;
}

bool EspAec::process(const int16_t *in_mic, const int16_t *in_ref, int16_t *out,
                     uint8_t mic_channels_in) {
  // EspAec is single-mic only, mic_channels_in parameter accepted for interface compatibility
  if (this->handle_ == nullptr) {
    memcpy(out, in_mic, this->cached_frame_size_ * sizeof(int16_t));
    this->glitch_count_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  aec_process(this->handle_,
              const_cast<int16_t *>(in_mic),
              const_cast<int16_t *>(in_ref),
              out);
  this->frame_count_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

FeatureControl EspAec::feature_control(AudioFeature feature) const {
  if (feature == AudioFeature::AEC)
    return FeatureControl::RESTART_REQUIRED;  // reinit needed for mode change
  return FeatureControl::NOT_SUPPORTED;
}

bool EspAec::set_feature(AudioFeature feature, bool enabled) {
  // Standalone AEC has no per-feature toggles. Use reconfigure() for mode changes.
  return false;
}

ProcessorTelemetry EspAec::telemetry() const {
  ProcessorTelemetry t;
  t.frame_count = this->frame_count_.load(std::memory_order_relaxed);
  t.glitch_count = this->glitch_count_.load(std::memory_order_relaxed);
  return t;
}

bool EspAec::reconfigure(int type, int mode) {
  aec_mode_t new_mode;
  if (type == 0) {  // SR
    new_mode = (mode == 1) ? AEC_MODE_SR_HIGH_PERF : AEC_MODE_SR_LOW_COST;
  } else {  // VC
    new_mode = (mode == 1) ? static_cast<aec_mode_t>(4) : static_cast<aec_mode_t>(3);
  }
  return this->reinit_(new_mode);
}

bool EspAec::reinit_(aec_mode_t new_mode) {
  ESP_LOGI(TAG, "Reinitializing AEC: mode %d -> %d", (int) this->mode_, (int) new_mode);
  if (this->handle_ != nullptr) {
    aec_destroy(this->handle_);
    this->handle_ = nullptr;
  }
  this->mode_ = new_mode;
  this->handle_ = aec_create(this->sample_rate_, this->filter_length_, 1, this->mode_);
  if (this->handle_ == nullptr) {
    ESP_LOGE(TAG, "Failed to recreate AEC instance");
    return false;
  }
  this->cached_frame_size_ = aec_get_chunksize(this->handle_);
  if (this->cached_frame_size_ != this->last_frame_size_) {
    this->last_frame_size_ = this->cached_frame_size_;
    this->frame_spec_revision_.fetch_add(1, std::memory_order_relaxed);
  }
  ESP_LOGI(TAG, "AEC reinitialized: mode=%d, frame=%d (%dms)",
           (int) this->mode_, this->cached_frame_size_,
           this->cached_frame_size_ * 1000 / this->sample_rate_);
  return true;
}

}  // namespace esp_aec
}  // namespace esphome

#endif  // USE_ESP32
