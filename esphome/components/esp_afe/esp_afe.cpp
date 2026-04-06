#include "esp_afe.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"
#include <esp_heap_caps.h>
#include <cstring>
#include <algorithm>
#include <string>

namespace esphome {
namespace esp_afe {

static const char *const TAG = "esp_afe";

// Validate function pointer is a plausible code address (ESP32-S3: 0x40000000-0x4FFFFFFF)
static inline bool is_valid_func(const void *ptr) {
  auto addr = reinterpret_cast<uintptr_t>(ptr);
  return ptr != nullptr && addr >= 0x40000000 && addr < 0x50000000;
}

aec_mode_t EspAfe::derive_aec_mode_() const {
  if (this->afe_type_ == AFE_TYPE_SR) {
    return (this->afe_mode_ == AFE_MODE_HIGH_PERF) ? AEC_MODE_SR_HIGH_PERF : AEC_MODE_SR_LOW_COST;
  }
  // AFE_TYPE_VC or AFE_TYPE_VC_8K
  return (this->afe_mode_ == AFE_MODE_HIGH_PERF) ? AEC_MODE_VOIP_HIGH_PERF : AEC_MODE_VOIP_LOW_COST;
}

void EspAfe::setup() {
  ESP_LOGI(TAG, "Initializing AFE...");

  // Build format string: "MR" for 1 mic, "MMR" for 2 mics
  std::string fmt(this->mic_num_, 'M');
  fmt += 'R';

  // Try afe_config_init first (gets good defaults, handles NULL models gracefully)
  afe_config_t *cfg = afe_config_init(fmt.c_str(), nullptr,
                                      static_cast<afe_type_t>(this->afe_type_),
                                      static_cast<afe_mode_t>(this->afe_mode_));

  if (cfg == nullptr) {
    // Fallback: manual allocation
    ESP_LOGW(TAG, "afe_config_init returned NULL, using manual config");
    cfg = afe_config_alloc();
    if (cfg == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate AFE config");
      this->mark_failed();
      return;
    }
    if (!afe_parse_input_format(fmt.c_str(), &cfg->pcm_config)) {
      ESP_LOGE(TAG, "Failed to parse input format: %s", fmt.c_str());
      afe_config_free(cfg);
      this->mark_failed();
      return;
    }
    cfg->pcm_config.sample_rate = 16000;
    cfg->afe_mode = static_cast<afe_mode_t>(this->afe_mode_);
    cfg->afe_type = static_cast<afe_type_t>(this->afe_type_);
  }

  // Override with our settings
  cfg->aec_init = this->aec_enabled_;
  cfg->aec_filter_length = this->aec_filter_length_;
  cfg->aec_mode = this->derive_aec_mode_();

  cfg->se_init = (this->mic_num_ >= 2);

  cfg->ns_init = this->ns_enabled_;
  cfg->ns_model_name = nullptr;
  cfg->afe_ns_mode = AFE_NS_MODE_WEBRTC;

  cfg->vad_init = this->vad_enabled_;
  cfg->vad_mode = VAD_MODE_3;
  cfg->vad_model_name = nullptr;
  cfg->vad_min_speech_ms = 128;
  cfg->vad_min_noise_ms = 1000;
  cfg->vad_delay_ms = 128;
  cfg->vad_mute_playback = false;
  cfg->vad_enable_channel_trigger = false;

  cfg->wakenet_init = false;
  cfg->wakenet_model_name = nullptr;
  cfg->wakenet_model_name_2 = nullptr;

  cfg->agc_init = this->agc_enabled_;
  cfg->agc_mode = AFE_AGC_MODE_WEBRTC;
  cfg->agc_compression_gain_db = this->agc_compression_gain_;
  cfg->agc_target_level_dbfs = this->agc_target_level_;

  cfg->afe_perferred_core = this->task_core_;
  cfg->afe_perferred_priority = this->task_priority_;
  cfg->afe_ringbuf_size = this->ringbuf_size_;
  cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
  cfg->afe_linear_gain = 1.0f;
  cfg->debug_init = false;
  cfg->fixed_first_channel = true;

  // Validate (may adjust conflicting settings)
  ESP_LOGI(TAG, "Config BEFORE check: aec=%d ns=%d vad=%d agc=%d se=%d wakenet=%d",
           cfg->aec_init, cfg->ns_init, cfg->vad_init, cfg->agc_init, cfg->se_init, cfg->wakenet_init);
  afe_config_check(cfg);
  ESP_LOGI(TAG, "Config AFTER check:  aec=%d ns=%d vad=%d agc=%d se=%d wakenet=%d",
           cfg->aec_init, cfg->ns_init, cfg->vad_init, cfg->agc_init, cfg->se_init, cfg->wakenet_init);

  // Get vtable
  this->afe_handle_ = esp_afe_handle_from_config(cfg);
  if (this->afe_handle_ == nullptr) {
    ESP_LOGE(TAG, "esp_afe_handle_from_config returned NULL");
    afe_config_free(cfg); cfg = nullptr;
    this->mark_failed();
    return;
  }

  // Create AFE instance
  this->afe_data_ = this->afe_handle_->create_from_config(cfg);
  if (this->afe_data_ == nullptr) {
    ESP_LOGE(TAG, "create_from_config returned NULL (insufficient memory?)");
    afe_config_free(cfg); cfg = nullptr;
    this->mark_failed();
    return;
  }

  // Query frame sizes
  this->feed_chunksize_ = this->afe_handle_->get_feed_chunksize(this->afe_data_);
  this->fetch_chunksize_ = this->afe_handle_->get_fetch_chunksize(this->afe_data_);
  this->total_channels_ = cfg->pcm_config.total_ch_num;

  // Allocate feed buffer in PSRAM, 16-byte aligned
  size_t feed_bytes = this->feed_chunksize_ * this->total_channels_ * sizeof(int16_t);
  this->feed_buf_ = static_cast<int16_t *>(
      heap_caps_aligned_alloc(16, feed_bytes, MALLOC_CAP_SPIRAM));
  if (this->feed_buf_ == nullptr) {
    // Fallback: internal RAM
    this->feed_buf_ = static_cast<int16_t *>(
        heap_caps_aligned_alloc(16, feed_bytes, MALLOC_CAP_INTERNAL));
  }
  if (this->feed_buf_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate feed buffer (%u bytes)", (unsigned) feed_bytes);
    this->afe_handle_->destroy(this->afe_data_);
    this->afe_data_ = nullptr;
    afe_config_free(cfg); cfg = nullptr;
    this->mark_failed();
    return;
  }

  // Initialize warmup counter
  this->warmup_remaining_ = 3;

  // Keep config alive (create_from_config may hold a reference)
  this->afe_config_ = cfg;

  this->afe_handle_->print_pipeline(this->afe_data_);
  ESP_LOGI(TAG, "AFE initialized: feed_size=%d, fetch_size=%d, channels=%d, format=%s",
           this->feed_chunksize_, this->fetch_chunksize_, this->total_channels_, fmt.c_str());
}

void EspAfe::dump_config() {
  ESP_LOGCONFIG(TAG, "ESP AFE (Audio Front End):");
  ESP_LOGCONFIG(TAG, "  Type: %s", this->afe_type_ == 0 ? "SR" : "VC");
  ESP_LOGCONFIG(TAG, "  Mode: %s", this->afe_mode_ == 0 ? "LOW_COST" : "HIGH_PERF");
  ESP_LOGCONFIG(TAG, "  Microphones: %d", this->mic_num_);
  ESP_LOGCONFIG(TAG, "  AEC: %s (filter_length=%d)", this->aec_enabled_ ? "ON" : "OFF", this->aec_filter_length_);
  ESP_LOGCONFIG(TAG, "  NS: %s (WebRTC)", this->ns_enabled_ ? "ON" : "OFF");
  ESP_LOGCONFIG(TAG, "  VAD: %s (WebRTC)", this->vad_enabled_ ? "ON" : "OFF");
  ESP_LOGCONFIG(TAG, "  AGC: %s (gain=%ddB, target=-%ddBFS)",
                this->agc_enabled_ ? "ON" : "OFF", this->agc_compression_gain_, this->agc_target_level_);
  ESP_LOGCONFIG(TAG, "  SE (Beamforming): %s", (this->mic_num_ >= 2) ? "ON" : "OFF");
  ESP_LOGCONFIG(TAG, "  Task: core=%d, priority=%d", this->task_core_, this->task_priority_);
  ESP_LOGCONFIG(TAG, "  Feed: %d samples, Fetch: %d samples, Channels: %d",
                this->feed_chunksize_, this->fetch_chunksize_, this->total_channels_);
  ESP_LOGCONFIG(TAG, "  Initialized: %s", this->is_initialized() ? "YES" : "NO");
}

void EspAfe::process(const int16_t *mic_in, const int16_t *ref_in,
                     int16_t *out, int frame_size) {
  if (!this->is_initialized() || frame_size != this->fetch_chunksize_) {
    if (this->is_initialized() && frame_size != this->fetch_chunksize_) {
      ESP_LOGW(TAG, "Frame size mismatch: got %d, expected %d", frame_size, this->fetch_chunksize_);
    }
    memcpy(out, mic_in, frame_size * sizeof(int16_t));
    return;
  }

  // Warmup: feed silent frames to prime AFE ring buffers
  if (this->warmup_remaining_ > 0) {
    memset(this->feed_buf_, 0,
           this->feed_chunksize_ * this->total_channels_ * sizeof(int16_t));
    this->afe_handle_->feed(this->afe_data_, this->feed_buf_);
    this->warmup_remaining_--;
    memcpy(out, mic_in, frame_size * sizeof(int16_t));
    return;
  }

  // Interleave mic + ref into feed buffer
  if (this->mic_num_ == 1) {
    // "MR" format: [mic0, ref0, mic1, ref1, ...]
    for (int i = 0; i < this->feed_chunksize_; i++) {
      this->feed_buf_[i * 2] = mic_in[i];
      this->feed_buf_[i * 2 + 1] = (ref_in != nullptr) ? ref_in[i] : 0;
    }
  } else {
    // "MMR" format for dual-mic: [mic1_0, mic2_0, ref0, mic1_1, mic2_1, ref1, ...]
    // For now, duplicate single mic channel into both (placeholder for process_multi)
    for (int i = 0; i < this->feed_chunksize_; i++) {
      this->feed_buf_[i * 3] = mic_in[i];
      this->feed_buf_[i * 3 + 1] = mic_in[i];  // duplicate until process_multi
      this->feed_buf_[i * 3 + 2] = (ref_in != nullptr) ? ref_in[i] : 0;
    }
  }

  // Feed: non-blocking, writes to internal ring buffer
  this->afe_handle_->feed(this->afe_data_, this->feed_buf_);

  // Fetch: blocks until AFE processing completes (100ms timeout)
  afe_fetch_result_t *result = this->afe_handle_->fetch_with_delay(
      this->afe_data_, pdMS_TO_TICKS(100));

  if (result != nullptr && result->ret_value == ESP_OK && result->data != nullptr) {
    size_t copy_bytes = std::min(static_cast<size_t>(result->data_size),
                                 static_cast<size_t>(frame_size * sizeof(int16_t)));
    memcpy(out, result->data, copy_bytes);

    // Periodic diagnostics (~every 3s at 32ms/frame)
    if (++this->frame_count_ % 960 == 0) {  // ~30s
      ESP_LOGI(TAG, "AFE [frame %lu] vol=%.1fdB vad=%s ringbuf_free=%.0f%%",
               (unsigned long) this->frame_count_,
               result->data_volume,
               result->vad_state == VAD_SPEECH ? "SPEECH" : "SILENCE",
               result->ringbuff_free_pct * 100.0f);
    }
  } else {
    // Timeout or error: passthrough raw mic
    memcpy(out, mic_in, frame_size * sizeof(int16_t));
    if (++this->frame_count_ % 960 == 0) {  // ~30s
      ESP_LOGW(TAG, "AFE fetch failed (ret=%d, ringbuf=%.0f%%)",
               result ? result->ret_value : -1,
               result ? result->ringbuff_free_pct * 100.0f : 0.0f);
    }
  }
}

bool EspAfe::reinit_by_name(const std::string &name) {
  // Map mode string to afe_type + afe_mode
  int new_type, new_mode;
  if (name == "sr_low_cost") { new_type = AFE_TYPE_SR; new_mode = AFE_MODE_LOW_COST; }
  else if (name == "sr_high_perf") { new_type = AFE_TYPE_SR; new_mode = AFE_MODE_HIGH_PERF; }
  else if (name == "voip_low_cost") { new_type = AFE_TYPE_VC; new_mode = AFE_MODE_LOW_COST; }
  else if (name == "voip_high_perf") { new_type = AFE_TYPE_VC; new_mode = AFE_MODE_HIGH_PERF; }
  else {
    ESP_LOGW(TAG, "Unknown AFE mode: %s", name.c_str());
    return false;
  }

  ESP_LOGI(TAG, "Reinitializing AFE: type=%d mode=%d", new_type, new_mode);

  // Destroy old instance
  if (this->afe_handle_ != nullptr && this->afe_data_ != nullptr) {
    this->afe_handle_->destroy(this->afe_data_);
    this->afe_data_ = nullptr;
  }
  if (this->afe_config_ != nullptr) {
    afe_config_free(this->afe_config_);
    this->afe_config_ = nullptr;
  }
  if (this->feed_buf_ != nullptr) {
    heap_caps_free(this->feed_buf_);
    this->feed_buf_ = nullptr;
  }

  // Update config and re-run setup
  this->afe_type_ = new_type;
  this->afe_mode_ = new_mode;
  this->setup();

  return this->is_initialized();
}

// Runtime toggles (validate function pointers; some may be unimplemented in the binary)
#define AFE_CALL_IF_VALID(func_name)                                                          \
  if (this->afe_handle_ && this->afe_data_ &&                                                \
      is_valid_func(reinterpret_cast<const void *>(this->afe_handle_->func_name))) {          \
    this->afe_handle_->func_name(this->afe_data_);                                            \
  } else {                                                                                     \
    ESP_LOGW(TAG, #func_name " not available (ptr=%p)",                                       \
             this->afe_handle_ ? reinterpret_cast<const void *>(this->afe_handle_->func_name) \
                               : nullptr);                                                     \
  }

void EspAfe::enable_aec() { AFE_CALL_IF_VALID(enable_aec) }
void EspAfe::disable_aec() { AFE_CALL_IF_VALID(disable_aec) }
void EspAfe::enable_ns() { AFE_CALL_IF_VALID(enable_ns) }
void EspAfe::disable_ns() { AFE_CALL_IF_VALID(disable_ns) }
void EspAfe::enable_vad() { AFE_CALL_IF_VALID(enable_vad) }
void EspAfe::disable_vad() { AFE_CALL_IF_VALID(disable_vad) }
void EspAfe::enable_agc() { AFE_CALL_IF_VALID(enable_agc) }
void EspAfe::disable_agc() { AFE_CALL_IF_VALID(disable_agc) }

#undef AFE_CALL_IF_VALID

EspAfe::~EspAfe() {
  if (this->afe_handle_ != nullptr && this->afe_data_ != nullptr) {
    this->afe_handle_->destroy(this->afe_data_);
    this->afe_data_ = nullptr;
  }
  if (this->afe_config_ != nullptr) {
    afe_config_free(this->afe_config_);
    this->afe_config_ = nullptr;
  }
  if (this->feed_buf_ != nullptr) {
    heap_caps_free(this->feed_buf_);
    this->feed_buf_ = nullptr;
  }
}

}  // namespace esp_afe
}  // namespace esphome

#endif  // USE_ESP32
