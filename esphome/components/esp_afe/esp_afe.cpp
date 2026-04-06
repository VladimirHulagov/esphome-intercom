#include "esp_afe.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace esphome {
namespace esp_afe {

static const char *const TAG = "esp_afe";
static const TickType_t CONFIG_MUTEX_TIMEOUT = pdMS_TO_TICKS(250);
static const TickType_t PROCESS_MUTEX_TIMEOUT = pdMS_TO_TICKS(20);

// Validate function pointer is a plausible executable address across S3/P4 builds.
static inline bool is_valid_func(const void *ptr) {
  auto addr = reinterpret_cast<uintptr_t>(ptr);
  return ptr != nullptr && addr >= 0x30000000 && addr < 0x60000000;
}

static inline void copy_passthrough_frame(const int16_t *in, int input_samples,
                                          int16_t *out, int output_samples) {
  size_t copy_samples = std::min(input_samples, output_samples);
  if (copy_samples > 0) {
    memcpy(out, in, copy_samples * sizeof(int16_t));
  }
  if (output_samples > static_cast<int>(copy_samples)) {
    memset(out + copy_samples, 0, (output_samples - copy_samples) * sizeof(int16_t));
  }
}

static inline float compute_rms_dbfs(const int16_t *data, int samples) {
  if (data == nullptr || samples <= 0) {
    return -120.0f;
  }

  double sum_sq = 0.0;
  for (int i = 0; i < samples; i++) {
    double s = static_cast<double>(data[i]);
    sum_sq += s * s;
  }

  double rms = std::sqrt(sum_sq / samples);
  if (rms <= 0.0) {
    return -120.0f;
  }

  double normalized = rms / 32768.0;
  return static_cast<float>(20.0 * std::log10(normalized));
}

aec_mode_t EspAfe::derive_aec_mode_() const {
  if (this->afe_type_ == AFE_TYPE_SR) {
    return (this->afe_mode_ == AFE_MODE_HIGH_PERF) ? AEC_MODE_SR_HIGH_PERF : AEC_MODE_SR_LOW_COST;
  }
  return (this->afe_mode_ == AFE_MODE_HIGH_PERF) ? AEC_MODE_VOIP_HIGH_PERF : AEC_MODE_VOIP_LOW_COST;
}

const char *EspAfe::memory_alloc_mode_to_str_() const {
  switch (this->memory_alloc_mode_) {
    case AFE_MEMORY_ALLOC_MORE_INTERNAL:
      return "MORE_INTERNAL";
    case AFE_MEMORY_ALLOC_INTERNAL_PSRAM_BALANCE:
      return "INTERNAL_PSRAM_BALANCE";
    case AFE_MEMORY_ALLOC_MORE_PSRAM:
      return "MORE_PSRAM";
    default:
      return "UNKNOWN";
  }
}

bool EspAfe::build_instance_(AfeInstance *instance) {
  if (instance == nullptr) {
    return false;
  }

  std::string fmt(this->mic_num_, 'M');
  fmt += 'R';

  afe_config_t *cfg = afe_config_init(fmt.c_str(), nullptr,
                                      static_cast<afe_type_t>(this->afe_type_),
                                      static_cast<afe_mode_t>(this->afe_mode_));

  if (cfg == nullptr) {
    ESP_LOGW(TAG, "afe_config_init returned NULL, using manual config");
    cfg = afe_config_alloc();
    if (cfg == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate AFE config");
      return false;
    }
    if (!afe_parse_input_format(fmt.c_str(), &cfg->pcm_config)) {
      ESP_LOGE(TAG, "Failed to parse input format: %s", fmt.c_str());
      afe_config_free(cfg);
      return false;
    }
    cfg->pcm_config.sample_rate = 16000;
    cfg->afe_mode = static_cast<afe_mode_t>(this->afe_mode_);
    cfg->afe_type = static_cast<afe_type_t>(this->afe_type_);
  }

  cfg->aec_init = this->aec_enabled_;
  cfg->aec_filter_length = this->aec_filter_length_;
  cfg->aec_mode = this->derive_aec_mode_();

  cfg->se_init = (this->mic_num_ >= 2);

  cfg->ns_init = this->ns_enabled_;
  cfg->ns_model_name = nullptr;
  cfg->afe_ns_mode = AFE_NS_MODE_WEBRTC;

  cfg->vad_init = this->vad_enabled_;
  cfg->vad_mode = static_cast<vad_mode_t>(this->vad_mode_);
  cfg->vad_model_name = nullptr;
  cfg->vad_min_speech_ms = this->vad_min_speech_ms_;
  cfg->vad_min_noise_ms = this->vad_min_noise_ms_;
  cfg->vad_delay_ms = this->vad_delay_ms_;
  cfg->vad_mute_playback = this->vad_mute_playback_;
  cfg->vad_enable_channel_trigger = this->vad_enable_channel_trigger_;

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
  cfg->memory_alloc_mode = static_cast<afe_memory_alloc_mode_t>(this->memory_alloc_mode_);
  cfg->afe_linear_gain = this->afe_linear_gain_;
  cfg->debug_init = false;
  cfg->fixed_first_channel = true;

  ESP_LOGI(TAG, "Config BEFORE check: aec=%d ns=%d vad=%d agc=%d se=%d wakenet=%d",
           cfg->aec_init, cfg->ns_init, cfg->vad_init, cfg->agc_init, cfg->se_init, cfg->wakenet_init);
  afe_config_check(cfg);
  ESP_LOGI(TAG, "Config AFTER check:  aec=%d ns=%d vad=%d agc=%d se=%d wakenet=%d",
           cfg->aec_init, cfg->ns_init, cfg->vad_init, cfg->agc_init, cfg->se_init, cfg->wakenet_init);

  const esp_afe_sr_iface_t *handle = esp_afe_handle_from_config(cfg);
  if (handle == nullptr) {
    ESP_LOGE(TAG, "esp_afe_handle_from_config returned NULL");
    afe_config_free(cfg);
    return false;
  }

  esp_afe_sr_data_t *data = handle->create_from_config(cfg);
  if (data == nullptr) {
    ESP_LOGE(TAG, "create_from_config returned NULL (insufficient memory?)");
    afe_config_free(cfg);
    return false;
  }

  int feed_chunksize = handle->get_feed_chunksize(data);
  int fetch_chunksize = handle->get_fetch_chunksize(data);
  int total_channels = cfg->pcm_config.total_ch_num;
  size_t feed_bytes = static_cast<size_t>(feed_chunksize) * total_channels * sizeof(int16_t);

  int16_t *feed_buf = static_cast<int16_t *>(heap_caps_aligned_alloc(16, feed_bytes, MALLOC_CAP_SPIRAM));
  if (feed_buf == nullptr) {
    feed_buf = static_cast<int16_t *>(heap_caps_aligned_alloc(16, feed_bytes, MALLOC_CAP_INTERNAL));
  }
  if (feed_buf == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate feed buffer (%u bytes)", static_cast<unsigned>(feed_bytes));
    handle->destroy(data);
    afe_config_free(cfg);
    return false;
  }

  instance->handle = handle;
  instance->data = data;
  instance->config = cfg;
  instance->feed_buf = feed_buf;
  instance->feed_chunksize = feed_chunksize;
  instance->fetch_chunksize = fetch_chunksize;
  instance->total_channels = total_channels;

  handle->print_pipeline(data);
  ESP_LOGI(TAG, "AFE prepared: feed_size=%d, fetch_size=%d, channels=%d, format=%s",
           feed_chunksize, fetch_chunksize, total_channels, fmt.c_str());
  return true;
}

void EspAfe::destroy_instance_(AfeInstance *instance) {
  if (instance == nullptr) {
    return;
  }
  if (instance->handle != nullptr && instance->data != nullptr) {
    instance->handle->destroy(instance->data);
    instance->data = nullptr;
  }
  if (instance->config != nullptr) {
    afe_config_free(instance->config);
    instance->config = nullptr;
  }
  if (instance->feed_buf != nullptr) {
    heap_caps_free(instance->feed_buf);
    instance->feed_buf = nullptr;
  }
  instance->handle = nullptr;
  instance->feed_chunksize = 0;
  instance->fetch_chunksize = 0;
  instance->total_channels = 0;
}

void EspAfe::install_instance_(AfeInstance *instance) {
  this->afe_handle_ = instance->handle;
  this->afe_data_ = instance->data;
  this->afe_config_ = instance->config;
  this->feed_buf_ = instance->feed_buf;
  this->feed_chunksize_ = instance->feed_chunksize;
  this->fetch_chunksize_ = instance->fetch_chunksize;
  this->total_channels_ = instance->total_channels;

  instance->handle = nullptr;
  instance->data = nullptr;
  instance->config = nullptr;
  instance->feed_buf = nullptr;
  instance->feed_chunksize = 0;
  instance->fetch_chunksize = 0;
  instance->total_channels = 0;
}

EspAfe::AfeInstance EspAfe::detach_instance_() {
  AfeInstance instance;
  instance.handle = this->afe_handle_;
  instance.data = this->afe_data_;
  instance.config = this->afe_config_;
  instance.feed_buf = this->feed_buf_;
  instance.feed_chunksize = this->feed_chunksize_;
  instance.fetch_chunksize = this->fetch_chunksize_;
  instance.total_channels = this->total_channels_;

  this->afe_handle_ = nullptr;
  this->afe_data_ = nullptr;
  this->afe_config_ = nullptr;
  this->feed_buf_ = nullptr;
  this->feed_chunksize_ = 0;
  this->fetch_chunksize_ = 0;
  this->total_channels_ = 0;

  return instance;
}

bool EspAfe::recreate_instance_(bool require_same_frame_sizes) {
  if (this->config_mutex_ == nullptr) {
    this->config_mutex_ = xSemaphoreCreateMutex();
    if (this->config_mutex_ == nullptr) {
      ESP_LOGE(TAG, "Failed to create config mutex");
      return false;
    }
  }

  if (xSemaphoreTake(this->config_mutex_, CONFIG_MUTEX_TIMEOUT) != pdTRUE) {
    ESP_LOGW(TAG, "Timed out waiting to rebuild AFE instance");
    return false;
  }

  int old_feed_chunksize = this->feed_chunksize_;
  int old_fetch_chunksize = this->fetch_chunksize_;

  AfeInstance old = this->detach_instance_();
  this->destroy_instance_(&old);

  AfeInstance next;
  if (!this->build_instance_(&next)) {
    xSemaphoreGive(this->config_mutex_);
    return false;
  }

  if (require_same_frame_sizes &&
      (next.feed_chunksize != old_feed_chunksize || next.fetch_chunksize != old_fetch_chunksize)) {
    ESP_LOGW(TAG, "Reinit changed frame sizes (%d/%d -> %d/%d) while audio is live",
             old_feed_chunksize, old_fetch_chunksize,
             next.feed_chunksize, next.fetch_chunksize);
    this->destroy_instance_(&next);
    xSemaphoreGive(this->config_mutex_);
    return false;
  }

  this->install_instance_(&next);
  this->warmup_remaining_ = 3;
  this->frame_count_ = 0;
  this->voice_present_.store(false, std::memory_order_relaxed);
  this->input_volume_dbfs_.store(-120.0f, std::memory_order_relaxed);
  this->output_rms_dbfs_.store(-120.0f, std::memory_order_relaxed);
  xSemaphoreGive(this->config_mutex_);
  return true;
}

bool EspAfe::set_aec_enabled_runtime_(bool enabled) {
  if (this->aec_enabled_ == enabled) {
    return true;
  }
  if (this->config_mutex_ == nullptr || !this->is_initialized()) {
    ESP_LOGW(TAG, "AEC toggle requested before initialization");
    return false;
  }
  if (xSemaphoreTake(this->config_mutex_, CONFIG_MUTEX_TIMEOUT) != pdTRUE) {
    ESP_LOGW(TAG, "Timed out waiting to toggle AEC");
    return false;
  }

  auto func = enabled ? this->afe_handle_->enable_aec : this->afe_handle_->disable_aec;
  if (!is_valid_func(reinterpret_cast<const void *>(func))) {
    xSemaphoreGive(this->config_mutex_);
    ESP_LOGW(TAG, "%s_aec not available (ptr=%p)",
             enabled ? "enable" : "disable", reinterpret_cast<const void *>(func));
    return false;
  }

  int ret = func(this->afe_data_);
  xSemaphoreGive(this->config_mutex_);
  if (ret < 0) {
    ESP_LOGW(TAG, "%s_aec failed (ret=%d)", enabled ? "enable" : "disable", ret);
    return false;
  }

  this->aec_enabled_ = enabled;
  return true;
}

bool EspAfe::set_reinit_flag_(bool &flag, bool enabled, const char *name) {
  if (flag == enabled) {
    return true;
  }
  bool old = flag;
  flag = enabled;
  if (this->recreate_instance_(true)) {
    return true;
  }
  flag = old;
  ESP_LOGW(TAG, "Failed to apply %s=%s", name, enabled ? "true" : "false");
  return false;
}

void EspAfe::setup() {
  ESP_LOGI(TAG, "Initializing AFE...");
  if (!this->recreate_instance_(false)) {
    this->mark_failed();
  }
}

void EspAfe::dump_config() {
  ESP_LOGCONFIG(TAG, "ESP AFE (Audio Front End):");
  ESP_LOGCONFIG(TAG, "  Type: %s", this->afe_type_ == AFE_TYPE_SR ? "SR" : "VC");
  ESP_LOGCONFIG(TAG, "  Mode: %s", this->afe_mode_ == AFE_MODE_LOW_COST ? "LOW_COST" : "HIGH_PERF");
  ESP_LOGCONFIG(TAG, "  Microphones: %d", this->mic_num_);
  ESP_LOGCONFIG(TAG, "  AEC: %s (filter_length=%d)", this->aec_enabled_ ? "ON" : "OFF", this->aec_filter_length_);
  ESP_LOGCONFIG(TAG, "  NS: %s (WebRTC)", this->ns_enabled_ ? "ON" : "OFF");
  ESP_LOGCONFIG(TAG, "  VAD: %s (mode=%d, speech=%dms, noise=%dms, delay=%dms)",
                this->vad_enabled_ ? "ON" : "OFF", this->vad_mode_, this->vad_min_speech_ms_,
                this->vad_min_noise_ms_, this->vad_delay_ms_);
  ESP_LOGCONFIG(TAG, "  AGC: %s (gain=%ddB, target=-%ddBFS)",
                this->agc_enabled_ ? "ON" : "OFF", this->agc_compression_gain_, this->agc_target_level_);
  ESP_LOGCONFIG(TAG, "  SE (Beamforming): %s", (this->mic_num_ >= 2) ? "ON" : "OFF");
  ESP_LOGCONFIG(TAG, "  Alloc: %s, linear_gain=%.2f", this->memory_alloc_mode_to_str_(), this->afe_linear_gain_);
  ESP_LOGCONFIG(TAG, "  Task: core=%d, priority=%d, ringbuf=%d", this->task_core_, this->task_priority_, this->ringbuf_size_);
  ESP_LOGCONFIG(TAG, "  Feed: %d samples, Fetch: %d samples, Channels: %d",
                this->feed_chunksize_, this->fetch_chunksize_, this->total_channels_);
  ESP_LOGCONFIG(TAG, "  Initialized: %s", this->is_initialized() ? "YES" : "NO");
}

void EspAfe::process(const int16_t *mic_in, const int16_t *ref_in,
                     int16_t *out, int frame_size) {
  int output_frame_size = this->fetch_chunksize_ > 0 ? this->fetch_chunksize_ : frame_size;
  if (!this->is_initialized() || this->config_mutex_ == nullptr) {
    copy_passthrough_frame(mic_in, frame_size, out, output_frame_size);
    return;
  }

  if (xSemaphoreTake(this->config_mutex_, PROCESS_MUTEX_TIMEOUT) != pdTRUE) {
    copy_passthrough_frame(mic_in, frame_size, out, output_frame_size);
    return;
  }

  output_frame_size = this->fetch_chunksize_ > 0 ? this->fetch_chunksize_ : frame_size;
  if (frame_size != this->feed_chunksize_) {
    ESP_LOGW(TAG, "Frame size mismatch: got %d, expected input %d", frame_size, this->feed_chunksize_);
    copy_passthrough_frame(mic_in, frame_size, out, output_frame_size);
    xSemaphoreGive(this->config_mutex_);
    return;
  }

  if (this->warmup_remaining_ > 0) {
    memset(this->feed_buf_, 0,
           static_cast<size_t>(this->feed_chunksize_) * this->total_channels_ * sizeof(int16_t));
    this->afe_handle_->feed(this->afe_data_, this->feed_buf_);
    this->warmup_remaining_--;
    copy_passthrough_frame(mic_in, frame_size, out, output_frame_size);
    xSemaphoreGive(this->config_mutex_);
    return;
  }

  if (this->mic_num_ == 1) {
    for (int i = 0; i < this->feed_chunksize_; i++) {
      this->feed_buf_[i * 2] = mic_in[i];
      this->feed_buf_[i * 2 + 1] = (ref_in != nullptr) ? ref_in[i] : 0;
    }
  } else {
    for (int i = 0; i < this->feed_chunksize_; i++) {
      this->feed_buf_[i * 3] = mic_in[i];
      this->feed_buf_[i * 3 + 1] = mic_in[i];
      this->feed_buf_[i * 3 + 2] = (ref_in != nullptr) ? ref_in[i] : 0;
    }
  }

  this->afe_handle_->feed(this->afe_data_, this->feed_buf_);

  afe_fetch_result_t *result = this->afe_handle_->fetch_with_delay(this->afe_data_, pdMS_TO_TICKS(100));
  if (result != nullptr && result->ret_value == ESP_OK && result->data != nullptr) {
    size_t output_bytes = static_cast<size_t>(output_frame_size) * sizeof(int16_t);
    size_t copy_bytes = std::min(static_cast<size_t>(result->data_size), output_bytes);
    memcpy(out, result->data, copy_bytes);
    if (copy_bytes < output_bytes) {
      memset(reinterpret_cast<uint8_t *>(out) + copy_bytes, 0, output_bytes - copy_bytes);
    }
    this->voice_present_.store(this->vad_enabled_ && result->vad_state == VAD_SPEECH, std::memory_order_relaxed);
    this->input_volume_dbfs_.store(result->data_volume, std::memory_order_relaxed);
    this->output_rms_dbfs_.store(compute_rms_dbfs(out, output_frame_size), std::memory_order_relaxed);

    if (++this->frame_count_ % 960 == 0) {
      ESP_LOGI(TAG, "AFE [frame %lu] vol=%.1fdB vad=%s ringbuf_free=%.0f%%",
               static_cast<unsigned long>(this->frame_count_),
               result->data_volume,
               result->vad_state == VAD_SPEECH ? "SPEECH" : "SILENCE",
               result->ringbuff_free_pct * 100.0f);
    }
  } else {
    copy_passthrough_frame(mic_in, frame_size, out, output_frame_size);
    this->output_rms_dbfs_.store(compute_rms_dbfs(out, output_frame_size), std::memory_order_relaxed);
    if (++this->frame_count_ % 960 == 0) {
      ESP_LOGW(TAG, "AFE fetch failed (ret=%d, ringbuf=%.0f%%)",
               result ? result->ret_value : -1,
               result ? result->ringbuff_free_pct * 100.0f : 0.0f);
    }
  }

  xSemaphoreGive(this->config_mutex_);
}

bool EspAfe::reinit_by_name(const std::string &name) {
  int new_type;
  int new_mode;
  if (name == "sr_low_cost") {
    new_type = AFE_TYPE_SR;
    new_mode = AFE_MODE_LOW_COST;
  } else if (name == "sr_high_perf") {
    new_type = AFE_TYPE_SR;
    new_mode = AFE_MODE_HIGH_PERF;
  } else if (name == "voip_low_cost") {
    new_type = AFE_TYPE_VC;
    new_mode = AFE_MODE_LOW_COST;
  } else if (name == "voip_high_perf") {
    new_type = AFE_TYPE_VC;
    new_mode = AFE_MODE_HIGH_PERF;
  } else {
    ESP_LOGW(TAG, "Unknown AFE mode: %s", name.c_str());
    return false;
  }

  int old_type = this->afe_type_;
  int old_mode = this->afe_mode_;
  this->afe_type_ = new_type;
  this->afe_mode_ = new_mode;
  ESP_LOGI(TAG, "Reinitializing AFE: type=%d mode=%d", new_type, new_mode);
  if (this->recreate_instance_(false)) {
    return true;
  }

  this->afe_type_ = old_type;
  this->afe_mode_ = old_mode;
  return false;
}

bool EspAfe::enable_aec() { return this->set_aec_enabled_runtime_(true); }
bool EspAfe::disable_aec() { return this->set_aec_enabled_runtime_(false); }
bool EspAfe::enable_ns() { return this->set_reinit_flag_(this->ns_enabled_, true, "ns_enabled"); }
bool EspAfe::disable_ns() { return this->set_reinit_flag_(this->ns_enabled_, false, "ns_enabled"); }
bool EspAfe::enable_vad() { return this->set_reinit_flag_(this->vad_enabled_, true, "vad_enabled"); }
bool EspAfe::disable_vad() { return this->set_reinit_flag_(this->vad_enabled_, false, "vad_enabled"); }
bool EspAfe::enable_agc() { return this->set_reinit_flag_(this->agc_enabled_, true, "agc_enabled"); }
bool EspAfe::disable_agc() { return this->set_reinit_flag_(this->agc_enabled_, false, "agc_enabled"); }

EspAfe::~EspAfe() {
  AfeInstance instance = this->detach_instance_();
  this->destroy_instance_(&instance);
  if (this->config_mutex_ != nullptr) {
    vSemaphoreDelete(this->config_mutex_);
    this->config_mutex_ = nullptr;
  }
}

}  // namespace esp_afe
}  // namespace esphome

#endif  // USE_ESP32
