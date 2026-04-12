#include "esp_afe.h"

#ifdef USE_ESP32

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <esp_heap_caps.h>
#include <esp_memory_utils.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace esphome {
namespace esp_afe {

static const char *const TAG = "esp_afe";
static const TickType_t CONFIG_MUTEX_TIMEOUT = pdMS_TO_TICKS(250);
static const TickType_t PROCESS_MUTEX_TIMEOUT = pdMS_TO_TICKS(20);

// Validate function pointer using ESP-IDF's memory map knowledge.
static inline bool is_valid_func(const void *ptr) {
  return ptr != nullptr && esp_ptr_executable(ptr);
}

static inline void copy_passthrough_frame(const int16_t *in, int input_samples,
                                          int mic_channels, int16_t *out, int output_samples) {
  if (out == nullptr || output_samples <= 0) {
    return;
  }
  if (in == nullptr || input_samples <= 0) {
    memset(out, 0, output_samples * sizeof(int16_t));
    return;
  }
  size_t copy_samples = std::min(input_samples, output_samples);
  if (copy_samples > 0) {
    if (mic_channels <= 1) {
      memcpy(out, in, copy_samples * sizeof(int16_t));
    } else {
      for (size_t i = 0; i < copy_samples; i++) {
        out[i] = in[i * mic_channels];
      }
    }
  }
  if (output_samples > static_cast<int>(copy_samples)) {
    memset(out + copy_samples, 0, (output_samples - copy_samples) * sizeof(int16_t));
  }
}

// 20*log10(sqrt(mean)/32768) = 10*log10(mean) - 20*log10(32768)
static constexpr float RMS_DBFS_OFFSET = 90.30899870f;  // 20 * log10(32768)

static inline float compute_rms_dbfs(const int16_t *data, int samples) {
  if (data == nullptr || samples <= 0)
    return -120.0f;
  uint64_t sumsq = 0;
  for (int i = 0; i < samples; i++) {
    int32_t s = data[i];
    sumsq += static_cast<uint64_t>(s * s);
  }
  float mean = static_cast<float>(sumsq) / static_cast<float>(samples);
  if (mean <= 0.0f)
    return -120.0f;
  return 10.0f * log10f(mean) - RMS_DBFS_OFFSET;
}

aec_mode_t EspAfe::derive_aec_mode_() const {
  if (this->afe_type_ == AFE_TYPE_SR) {
    return (this->afe_mode_ == AFE_MODE_HIGH_PERF) ? AEC_MODE_SR_HIGH_PERF : AEC_MODE_SR_LOW_COST;
  }
  return (this->afe_mode_ == AFE_MODE_HIGH_PERF) ? AEC_MODE_VOIP_HIGH_PERF : AEC_MODE_VOIP_LOW_COST;
}

int EspAfe::afe_mic_channels_() const {
  if (this->mic_num_ < 2) {
    return 1;
  }
  return this->se_enabled_ ? 2 : 1;
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

  const int afe_mic_channels = this->afe_mic_channels_();
  // Stack-allocated format string: "MR" (1 mic) or "MMR" (2 mic). No heap alloc.
  char fmt[4];
  for (int i = 0; i < afe_mic_channels && i < 2; i++) fmt[i] = 'M';
  fmt[afe_mic_channels] = 'R';
  fmt[afe_mic_channels + 1] = '\0';

  afe_config_t *cfg = afe_config_init(fmt, nullptr,
                                      static_cast<afe_type_t>(this->afe_type_),
                                      static_cast<afe_mode_t>(this->afe_mode_));

  if (cfg == nullptr) {
    ESP_LOGW(TAG, "afe_config_init returned NULL, using manual config");
    cfg = afe_config_alloc();
    if (cfg == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate AFE config");
      return false;
    }
    if (!afe_parse_input_format(fmt, &cfg->pcm_config)) {
      ESP_LOGE(TAG, "Failed to parse input format: %s", fmt);
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

  cfg->se_init = afe_mic_channels >= 2 && this->se_enabled_;

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

  afe_config_check(cfg);

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
  int process_chunksize = std::gcd(feed_chunksize, fetch_chunksize);
  if (process_chunksize <= 0) {
    process_chunksize = (fetch_chunksize > 0) ? fetch_chunksize : feed_chunksize;
  }
  // Use official API for feed channel count instead of config struct (more robust
  // if esp-sr changes internal channel mapping in future versions).
  int total_channels = handle->get_feed_channel_num(data);
  if (total_channels <= 0) {
    ESP_LOGW(TAG, "get_feed_channel_num returned %d, falling back to cfg->pcm_config.total_ch_num=%d",
             total_channels, cfg->pcm_config.total_ch_num);
    total_channels = cfg->pcm_config.total_ch_num;  // fallback
  }
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
  instance->process_chunksize = process_chunksize;
  instance->total_channels = total_channels;

  ESP_LOGI(TAG, "AFE ready: process=%d feed=%d fetch=%d ch=%d fmt=%s (transport_mics=%d)",
           process_chunksize, feed_chunksize, fetch_chunksize, total_channels, fmt, this->mic_num_);
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
  instance->process_chunksize = 0;
  instance->total_channels = 0;
}

bool EspAfe::install_instance_(AfeInstance *instance) {
  this->afe_handle_ = instance->handle;
  this->afe_data_ = instance->data;
  this->afe_config_ = instance->config;
  this->feed_buf_ = instance->feed_buf;
  this->feed_chunksize_ = instance->feed_chunksize;
  this->fetch_chunksize_ = instance->fetch_chunksize;
  this->process_chunksize_ = instance->process_chunksize;
  this->total_channels_ = instance->total_channels;
  this->staged_input_samples_ = 0;

  instance->handle = nullptr;
  instance->data = nullptr;
  instance->config = nullptr;
  instance->feed_buf = nullptr;
  instance->feed_chunksize = 0;
  instance->fetch_chunksize = 0;
  instance->process_chunksize = 0;
  instance->total_channels = 0;

  // The fetch bridge is mandatory: without it process() would read from an
  // empty ring forever and the caller would only ever see passthrough.
  if (!this->start_fetch_task_()) {
    ESP_LOGE(TAG, "Installed AFE instance but fetch task failed to start");
    return false;
  }
  return true;
}

EspAfe::AfeInstance EspAfe::detach_instance_() {
  // Stop the fetch task first so no one is calling afe->fetch() on the
  // handle we're about to hand back to destroy_instance_.
  this->stop_fetch_task_();

  AfeInstance instance;
  instance.handle = this->afe_handle_;
  instance.data = this->afe_data_;
  instance.config = this->afe_config_;
  instance.feed_buf = this->feed_buf_;
  instance.feed_chunksize = this->feed_chunksize_;
  instance.fetch_chunksize = this->fetch_chunksize_;
  instance.process_chunksize = this->process_chunksize_;
  instance.total_channels = this->total_channels_;

  this->afe_handle_ = nullptr;
  this->afe_data_ = nullptr;
  this->afe_config_ = nullptr;
  this->feed_buf_ = nullptr;
  this->feed_chunksize_ = 0;
  this->fetch_chunksize_ = 0;
  this->process_chunksize_ = 0;
  this->total_channels_ = 0;
  this->staged_input_samples_ = 0;

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

  // esp-sr FFT resources are global: only one AFE instance can exist.
  // Must destroy old before creating new.
  int old_process = this->process_chunksize_;
  int old_fetch = this->fetch_chunksize_;
  AfeInstance old = this->detach_instance_();
  this->destroy_instance_(&old);

  AfeInstance next;
  if (!this->build_instance_(&next)) {
    ESP_LOGE(TAG, "Failed to build new AFE instance. AFE is DOWN until successful rebuild.");
    xSemaphoreGive(this->config_mutex_);
    return false;
  }

  if (require_same_frame_sizes && old_process > 0 && old_fetch > 0 &&
      (next.process_chunksize != old_process || next.fetch_chunksize != old_fetch)) {
    ESP_LOGW(TAG, "Reinit changed external frame sizes (%d/%d -> %d/%d), rejecting",
             old_process, old_fetch, next.process_chunksize, next.fetch_chunksize);
    this->destroy_instance_(&next);
    xSemaphoreGive(this->config_mutex_);
    return false;
  }

  // Compare against the last successfully-installed spec, not against the
  // (already-detached) `this->*_chunksize_` fields which are zero at this
  // point. Using the last_spec_* members means a rollback to the previous
  // config does not spuriously bump frame_spec_revision_, which would make
  // i2s_audio_duplex try to restart its audio task concurrently with our
  // fetch task recreation and race inside FreeRTOS.
  int new_mic_ch = this->afe_mic_channels_();
  bool spec_changed = (new_mic_ch != this->last_spec_mic_ch_ ||
                       next.process_chunksize != this->last_spec_process_size_ ||
                       next.fetch_chunksize != this->last_spec_fetch_size_);
  (void) old_process;
  (void) old_fetch;

  if (!this->install_instance_(&next)) {
    AfeInstance failed = this->detach_instance_();
    this->destroy_instance_(&failed);
    xSemaphoreGive(this->config_mutex_);
    return false;
  }

  this->last_spec_process_size_ = this->process_chunksize_;
  this->last_spec_fetch_size_ = this->fetch_chunksize_;

  if (spec_changed) {
    this->last_spec_mic_ch_ = new_mic_ch;
    // H2 partial fix: release barrier ensures new frame_spec stores (install_instance_
    // above) happen-before consumers observe the bumped revision via acquire load.
    this->frame_spec_revision_.fetch_add(1, std::memory_order_release);
  }
  this->warmup_remaining_ = 3;
  this->frame_count_.store(0, std::memory_order_relaxed);
  this->glitch_count_.store(0, std::memory_order_relaxed);
  this->ringbuf_free_pct_.store(1.0f, std::memory_order_relaxed);
  this->voice_present_.store(false, std::memory_order_relaxed);
  this->input_volume_dbfs_.store(-120.0f, std::memory_order_relaxed);
  this->output_rms_dbfs_.store(-120.0f, std::memory_order_relaxed);
  xSemaphoreGive(this->config_mutex_);
  return true;
}

bool EspAfe::set_aec_enabled_runtime_(bool enabled) {
  // esp-sr 2.3.1 disable_aec() tears down the AEC internal state; a later
  // enable_aec() fails with "AEC is not initialized". Use the recreate path
  // (same as NS/VAD/AGC/SE) so the AEC is rebuilt from scratch when toggled.
  return this->set_reinit_flag_(this->aec_enabled_, enabled, "aec_enabled");
}

bool EspAfe::set_reinit_flag_(bool &flag, bool enabled, const char *name) {
  if (flag == enabled) {
    return true;
  }
  if (!this->is_initialized() || this->config_mutex_ == nullptr || this->feed_chunksize_ == 0 ||
      this->fetch_chunksize_ == 0) {
    // Not yet running: commit immediately, build_instance_ will use it at setup/start
    flag = enabled;
    ESP_LOGD(TAG, "Deferring %s=%s until AFE is initialized",
             name, enabled ? "true" : "false");
    return true;
  }
  // Staged config: set flag, rebuild, rollback on failure.
  // Flag must be set before rebuild because build_instance_ reads it.
  // The mutex in recreate_instance_ ensures process() either sees the old
  // instance (passthrough) or the new one, never a mix.
  bool old_value = flag;
  flag = enabled;
  if (this->recreate_instance_(true)) {
    return true;
  }
  // Rebuild failed: restore flag and try to rebuild with old config
  ESP_LOGW(TAG, "Failed to apply %s=%s, rolling back", name, enabled ? "true" : "false");
  flag = old_value;
  if (!this->recreate_instance_(true)) {
    ESP_LOGE(TAG, "Rollback also failed for %s, AFE is down", name);
  }
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
  ESP_LOGCONFIG(TAG, "  Microphones: transport=%d, afe=%d", this->mic_num_, this->afe_mic_channels_());
  ESP_LOGCONFIG(TAG, "  AEC: %s (filter_length=%d)", this->aec_enabled_ ? "ON" : "OFF", this->aec_filter_length_);
  ESP_LOGCONFIG(TAG, "  NS: %s (WebRTC)", this->ns_enabled_ ? "ON" : "OFF");
  ESP_LOGCONFIG(TAG, "  VAD: %s (mode=%d, speech=%dms, noise=%dms, delay=%dms)",
                this->vad_enabled_ ? "ON" : "OFF", this->vad_mode_, this->vad_min_speech_ms_,
                this->vad_min_noise_ms_, this->vad_delay_ms_);
  ESP_LOGCONFIG(TAG, "  AGC: %s (gain=%ddB, target=-%ddBFS)",
                this->agc_enabled_ ? "ON" : "OFF", this->agc_compression_gain_, this->agc_target_level_);
  if (this->mic_num_ >= 2) {
    ESP_LOGCONFIG(TAG, "  SE (Beamforming): %s", this->se_enabled_ ? "ON" : "OFF");
  } else {
    ESP_LOGCONFIG(TAG, "  SE (Beamforming): unavailable (mic_num < 2)");
  }
  ESP_LOGCONFIG(TAG, "  Alloc: %s, linear_gain=%.2f", this->memory_alloc_mode_to_str_(), this->afe_linear_gain_);
  ESP_LOGCONFIG(TAG, "  Task: core=%d, priority=%d, ringbuf=%d", this->task_core_, this->task_priority_, this->ringbuf_size_);
  ESP_LOGCONFIG(TAG, "  Process: %d samples, Feed: %d samples, Fetch: %d samples, Channels: %d",
                this->process_chunksize_, this->feed_chunksize_, this->fetch_chunksize_, this->total_channels_);
  ESP_LOGCONFIG(TAG, "  Initialized: %s", this->is_initialized() ? "YES" : "NO");
}

FrameSpec EspAfe::frame_spec() const {
  FrameSpec spec;
  spec.sample_rate = 16000;
  // Dynamic mic contract: expose the actual channel count the AFE needs.
  // When SE (beamforming) is off, only 1 mic is needed even on 2-mic boards.
  spec.mic_channels = this->afe_mic_channels_();
  spec.ref_channels = 1;
  spec.input_samples = this->process_chunksize_ > 0 ? this->process_chunksize_ : this->fetch_chunksize_;
  spec.output_samples = this->fetch_chunksize_;
  return spec;
}

FeatureControl EspAfe::feature_control(AudioFeature feature) const {
  switch (feature) {
    case AudioFeature::AEC:
      // esp-sr 2.3.1 disable_aec tears down the internal state, so AEC must
      // be rebuilt via recreate_instance_ to come back.
      return FeatureControl::RESTART_REQUIRED;
    case AudioFeature::NS:
    case AudioFeature::AGC:
    case AudioFeature::VAD:
      return FeatureControl::RESTART_REQUIRED;
    case AudioFeature::SE:
      return this->mic_num_ >= 2 ? FeatureControl::RESTART_REQUIRED : FeatureControl::NOT_SUPPORTED;
    default:
      return FeatureControl::NOT_SUPPORTED;
  }
}

bool EspAfe::set_feature(AudioFeature feature, bool enabled) {
  switch (feature) {
    case AudioFeature::AEC: return enabled ? this->enable_aec() : this->disable_aec();
    case AudioFeature::SE:  return enabled ? this->enable_se()  : this->disable_se();
    case AudioFeature::NS:  return enabled ? this->enable_ns()  : this->disable_ns();
    case AudioFeature::VAD: return enabled ? this->enable_vad() : this->disable_vad();
    case AudioFeature::AGC: return enabled ? this->enable_agc() : this->disable_agc();
    default: return false;
  }
}

ProcessorTelemetry EspAfe::telemetry() const {
  ProcessorTelemetry t;
  t.voice_present = this->voice_present_.load(std::memory_order_relaxed);
  t.input_volume_dbfs = this->input_volume_dbfs_.load(std::memory_order_relaxed);
  t.output_rms_dbfs = this->output_rms_dbfs_.load(std::memory_order_relaxed);
  t.ringbuf_free_pct = this->ringbuf_free_pct_.load(std::memory_order_relaxed);
  t.glitch_count = this->glitch_count_.load(std::memory_order_relaxed);
  t.frame_count = this->frame_count_.load(std::memory_order_relaxed);
  return t;
}

bool EspAfe::reconfigure(int type, int mode) {
  int old_type = this->afe_type_;
  int old_mode = this->afe_mode_;
  this->afe_type_ = type;
  this->afe_mode_ = mode;
  if (this->recreate_instance_(false)) {
    return true;
  }
  // MEDIUM fix (Codex): reconfigure() rollback. Rebuild failed; restore old config
  // values AND try to rebuild with them so the DSP isn't left permanently down.
  ESP_LOGW(TAG, "reconfigure: new type=%d mode=%d build failed, rolling back to type=%d mode=%d",
           type, mode, old_type, old_mode);
  this->afe_type_ = old_type;
  this->afe_mode_ = old_mode;
  if (!this->recreate_instance_(false)) {
    ESP_LOGE(TAG, "reconfigure: rollback rebuild ALSO failed - AFE is DOWN");
  }
  return false;
}

bool EspAfe::process(const int16_t *in_mic, const int16_t *in_ref, int16_t *out,
                     uint8_t mic_channels_in) {
  const int transport_mic_channels = mic_channels_in;
  int qs = this->process_chunksize_ > 0 ? this->process_chunksize_ : this->fetch_chunksize_;
  int os = this->fetch_chunksize_;
  if (!this->is_initialized() || this->config_mutex_ == nullptr) {
    if (qs > 0 && os > 0) copy_passthrough_frame(in_mic, qs, transport_mic_channels, out, os);
    return false;
  }

  if (xSemaphoreTake(this->config_mutex_, PROCESS_MUTEX_TIMEOUT) != pdTRUE) {
    copy_passthrough_frame(in_mic, qs, transport_mic_channels, out, os);
    return false;
  }

  // H1 fix: read afe_mic_channels INSIDE the mutex so it stays consistent with
  // total_channels_ / feed_chunksize_ after an SE toggle recreate.
  const int afe_mic_channels = this->afe_mic_channels_();
  qs = this->process_chunksize_ > 0 ? this->process_chunksize_ : this->fetch_chunksize_;
  os = this->fetch_chunksize_;
  int fs = this->feed_chunksize_;
  if (qs <= 0 || os <= 0 || fs <= 0 || this->feed_buf_ == nullptr) {
    copy_passthrough_frame(in_mic, qs, transport_mic_channels, out, os);
    xSemaphoreGive(this->config_mutex_);
    return false;
  }

  // Step 1: stage new input and feed it to AFE when a full frame is assembled.
  int offset = this->staged_input_samples_;
  if (offset + qs > fs) {
    ESP_LOGW(TAG, "AFE staging overflow (%d + %d > %d), dropping staged input", offset, qs, fs);
    offset = 0;
  }

  bool in_warmup = (this->warmup_remaining_ > 0) && (offset + qs >= fs);

  if (!in_warmup) {
    const int tc = this->total_channels_;
    int16_t *dst = this->feed_buf_ + offset * tc;
    if (afe_mic_channels == 1) {
      if (in_ref != nullptr) {
        for (int i = 0; i < qs; i++) {
          *dst++ = in_mic[i * transport_mic_channels];
          *dst++ = in_ref[i];
        }
      } else {
        for (int i = 0; i < qs; i++) {
          *dst++ = in_mic[i * transport_mic_channels];
          *dst++ = 0;
        }
      }
    } else if (transport_mic_channels >= 2) {
      if (in_ref != nullptr) {
        for (int i = 0; i < qs; i++) {
          *dst++ = in_mic[i * 2];
          *dst++ = in_mic[i * 2 + 1];
          *dst++ = in_ref[i];
        }
      } else {
        for (int i = 0; i < qs; i++) {
          *dst++ = in_mic[i * 2];
          *dst++ = in_mic[i * 2 + 1];
          *dst++ = 0;
        }
      }
    } else {
      // AFE wants 2 mic channels but caller sent 1 (SE transition): zero-fill.
      if (in_ref != nullptr) {
        for (int i = 0; i < qs; i++) {
          *dst++ = in_mic[i];
          *dst++ = 0;
          *dst++ = in_ref[i];
        }
      } else {
        for (int i = 0; i < qs; i++) {
          *dst++ = in_mic[i];
          *dst++ = 0;
          *dst++ = 0;
        }
      }
    }
  }
  offset += qs;

  if (offset == fs) {
    if (this->warmup_remaining_ > 0) {
      memset(this->feed_buf_, 0, static_cast<size_t>(fs) * this->total_channels_ * sizeof(int16_t));
      this->warmup_remaining_--;
    }
    int feed_ret = this->afe_handle_->feed(this->afe_data_, this->feed_buf_);
    if (feed_ret > 0) {
      if (this->feed_signal_ != nullptr) {
        xSemaphoreGive(this->feed_signal_);
      }
    } else {
      // AFE rejected the frame (input ring full). Do not arm the fetch task:
      // no new output will be produced for this attempt.
      this->feed_rejected_.fetch_add(1, std::memory_order_relaxed);
    }
    offset = 0;
  }
  this->staged_input_samples_ = offset;

  // Step 2: try to pull a processed frame that the fetch task has pushed into
  // our side of the bridge. Non-blocking: if nothing is ready we emit
  // passthrough for this call. The one-frame latency is a consequence of the
  // decoupled feed/fetch topology mandated by esp-sr.
  size_t output_bytes = static_cast<size_t>(os) * sizeof(int16_t);
  bool processed = false;
  bool vad_speech = false;
  float input_vol = -120.0f;
  float ringbuf_pct = this->ringbuf_free_pct_.load(std::memory_order_relaxed);
  if (this->fetch_output_ring_) {
    size_t got = this->fetch_output_ring_->read(reinterpret_cast<uint8_t *>(out), output_bytes, 0);
    if (got == output_bytes) {
      processed = true;
    }
  }
  if (!processed) {
    copy_passthrough_frame(in_mic, qs, transport_mic_channels, out, os);
    this->glitch_count_.fetch_add(1, std::memory_order_relaxed);
  }

  xSemaphoreGive(this->config_mutex_);

  // Output-side RMS depends on the samples handed to the caller. VAD / input
  // volume / ringbuf_free_pct are written by fetch_task_loop_ when it pulls a
  // frame from AFE, so this function only needs to refresh output RMS.
  (void) vad_speech;
  (void) input_vol;
  (void) ringbuf_pct;
  if (processed && this->output_rms_sensor_enabled_) {
    this->output_rms_dbfs_.store(compute_rms_dbfs(out, os), std::memory_order_relaxed);
  }
  this->frame_count_.fetch_add(1, std::memory_order_relaxed);
  return processed;
}

bool EspAfe::reinit_by_name(const std::string &name) {
  return this->reinit_by_name(name.c_str());
}

bool EspAfe::reinit_by_name(const char *name) {
  int type, mode;
  if (strcmp(name, "sr_low_cost") == 0) { type = AFE_TYPE_SR; mode = AFE_MODE_LOW_COST; }
  else if (strcmp(name, "sr_high_perf") == 0) { type = AFE_TYPE_SR; mode = AFE_MODE_HIGH_PERF; }
  else if (strcmp(name, "voip_low_cost") == 0) { type = AFE_TYPE_VC; mode = AFE_MODE_LOW_COST; }
  else if (strcmp(name, "voip_high_perf") == 0) { type = AFE_TYPE_VC; mode = AFE_MODE_HIGH_PERF; }
  else {
    ESP_LOGW(TAG, "Unknown AFE mode: %s", name);
    return false;
  }
  return this->reconfigure(type, mode);
}

bool EspAfe::enable_aec() { return this->set_aec_enabled_runtime_(true); }
bool EspAfe::disable_aec() { return this->set_aec_enabled_runtime_(false); }
bool EspAfe::enable_se() {
  if (this->mic_num_ < 2) {
    ESP_LOGW(TAG, "SE requires mic_num >= 2");
    return false;
  }
  return this->set_reinit_flag_(this->se_enabled_, true, "se_enabled");
}
bool EspAfe::disable_se() {
  if (this->mic_num_ < 2) {
    ESP_LOGW(TAG, "SE requires mic_num >= 2");
    return false;
  }
  return this->set_reinit_flag_(this->se_enabled_, false, "se_enabled");
}
bool EspAfe::enable_ns() { return this->set_reinit_flag_(this->ns_enabled_, true, "ns_enabled"); }
bool EspAfe::disable_ns() { return this->set_reinit_flag_(this->ns_enabled_, false, "ns_enabled"); }
bool EspAfe::enable_vad() { return this->set_reinit_flag_(this->vad_enabled_, true, "vad_enabled"); }
bool EspAfe::disable_vad() { return this->set_reinit_flag_(this->vad_enabled_, false, "vad_enabled"); }
bool EspAfe::enable_agc() { return this->set_reinit_flag_(this->agc_enabled_, true, "agc_enabled"); }
bool EspAfe::disable_agc() { return this->set_reinit_flag_(this->agc_enabled_, false, "agc_enabled"); }

// ---- Fetch task: decouples AFE's async output from the synchronous caller ----

void EspAfe::fetch_task_trampoline(void *arg) {
  static_cast<EspAfe *>(arg)->fetch_task_loop_();
  vTaskDelete(nullptr);
}

void EspAfe::fetch_task_loop_() {
  // One AFE output frame lasts fetch_chunksize_/16 ms @ 16kHz. Adding 10ms
  // slack gives the internal worker enough time even under load, and makes
  // sure the task exits within ~frame_ms on shutdown (no forced delete).
  const int frame_ms = (this->fetch_chunksize_ > 0) ? (this->fetch_chunksize_ / 16) : 32;
  const TickType_t fetch_timeout = pdMS_TO_TICKS(frame_ms + 10);

  while (this->fetch_task_running_.load(std::memory_order_acquire)) {
    // Wait for process() to confirm at least one feed was accepted.
    if (this->feed_signal_ != nullptr) {
      if (xSemaphoreTake(this->feed_signal_, pdMS_TO_TICKS(100)) != pdTRUE) {
        continue;
      }
    }
    if (!this->fetch_task_running_.load(std::memory_order_acquire)) {
      break;
    }
    // fetch_with_delay() with a finite timeout blocks only until AFE produces
    // output (or the timeout fires), avoiding both busy polling and an
    // unkillable portMAX_DELAY wait on shutdown.
    afe_fetch_result_t *result =
        this->afe_handle_->fetch_with_delay(this->afe_data_, fetch_timeout);
    if (!this->fetch_task_running_.load(std::memory_order_acquire)) {
      break;
    }
    if (result == nullptr || result->ret_value != ESP_OK || result->data == nullptr) {
      this->fetch_timeout_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    this->fetch_ok_.fetch_add(1, std::memory_order_relaxed);

    // Telemetry from the frame the worker just handed us.
    this->ringbuf_free_pct_.store(result->ringbuff_free_pct, std::memory_order_relaxed);
    this->voice_present_.store(this->vad_enabled_ && result->vad_state == VAD_SPEECH,
                               std::memory_order_relaxed);
    if (this->input_volume_sensor_enabled_) {
      this->input_volume_dbfs_.store(result->data_volume, std::memory_order_relaxed);
    }

    if (this->fetch_output_ring_) {
      const size_t want = static_cast<size_t>(result->data_size);
      size_t wrote = this->fetch_output_ring_->write_without_replacement(result->data, want,
                                                                         pdMS_TO_TICKS(5));
      if (wrote != want) {
        this->output_ring_drop_.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
}

bool EspAfe::start_fetch_task_() {
  if (this->fetch_task_handle_ != nullptr) {
    return true;  // already running
  }
  if (this->afe_handle_ == nullptr || this->afe_data_ == nullptr ||
      this->fetch_chunksize_ <= 0) {
    return false;
  }

  if (!this->fetch_output_ring_) {
    // Enough capacity for two full output frames (~64ms @ 16kHz) so a brief
    // stall in process() does not back-pressure the worker.
    const size_t frame_bytes = static_cast<size_t>(this->fetch_chunksize_) * sizeof(int16_t);
    this->fetch_output_ring_ = RingBuffer::create(frame_bytes * 2);
    if (!this->fetch_output_ring_) {
      ESP_LOGE(TAG, "Failed to allocate AFE fetch output ring buffer");
      return false;
    }
  }

  if (this->feed_signal_ == nullptr) {
    this->feed_signal_ = xSemaphoreCreateCounting(8, 0);
    if (this->feed_signal_ == nullptr) {
      ESP_LOGE(TAG, "Failed to create AFE feed signal semaphore");
      return false;
    }
  }

  if (this->fetch_task_stack_ == nullptr) {
    RAMAllocator<StackType_t> internal_stack(RAMAllocator<StackType_t>::ALLOC_INTERNAL);
    this->fetch_task_stack_ = internal_stack.allocate(kFetchTaskStackWords);
    if (this->fetch_task_stack_ == nullptr) {
      ESP_LOGE(TAG, "Failed to allocate AFE fetch task stack");
      return false;
    }
  }

  this->fetch_task_running_.store(true, std::memory_order_release);
  this->fetch_task_handle_ = xTaskCreateStaticPinnedToCore(
      &EspAfe::fetch_task_trampoline, "afe_fetch", kFetchTaskStackWords, this,
      this->task_priority_ > 0 ? this->task_priority_ - 1 : 1,
      this->fetch_task_stack_, &this->fetch_task_tcb_,
      this->task_core_ >= 0 ? this->task_core_ : tskNO_AFFINITY);
  if (this->fetch_task_handle_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create AFE fetch task");
    this->fetch_task_running_.store(false, std::memory_order_release);
    return false;
  }
  ESP_LOGI(TAG, "AFE fetch task started (core=%d, priority=%d)",
           this->task_core_, this->task_priority_ - 1);
  return true;
}

void EspAfe::stop_fetch_task_() {
  if (this->fetch_task_handle_ == nullptr) {
    return;
  }
  this->fetch_task_running_.store(false, std::memory_order_release);
  // Wake the task if it is waiting on feed_signal_, then let the fetch
  // timeout (frame_ms + slack) finish naturally on the next iteration.
  // The task calls vTaskDelete(nullptr) in fetch_task_trampoline.
  if (this->feed_signal_ != nullptr) {
    xSemaphoreGive(this->feed_signal_);
  }
  // Wait up to ~250ms for the task to mark itself deleted.
  for (int i = 0; i < 25; i++) {
    if (eTaskGetState(this->fetch_task_handle_) == eDeleted) {
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  this->fetch_task_handle_ = nullptr;
  // Give FreeRTOS IDLE time to reclaim the deleted TCB before it is reused
  // by the next xTaskCreateStatic call.
  vTaskDelay(pdMS_TO_TICKS(30));
  // Do NOT zero fetch_task_tcb_ manually — FreeRTOS may still be holding
  // back-links into the struct. xTaskCreateStaticPinnedToCore will
  // reinitialize the fields it cares about.
  this->fetch_output_ring_.reset();
  if (this->feed_signal_ != nullptr) {
    vSemaphoreDelete(this->feed_signal_);
    this->feed_signal_ = nullptr;
  }
}

EspAfe::~EspAfe() {
  // Quiesce: acquire mutex to ensure process() is not mid-frame
  if (this->config_mutex_ != nullptr) {
    xSemaphoreTake(this->config_mutex_, pdMS_TO_TICKS(500));
  }
  AfeInstance instance = this->detach_instance_();
  this->destroy_instance_(&instance);
  if (this->config_mutex_ != nullptr) {
    xSemaphoreGive(this->config_mutex_);
    vSemaphoreDelete(this->config_mutex_);
    this->config_mutex_ = nullptr;
  }
}

}  // namespace esp_afe
}  // namespace esphome

#endif  // USE_ESP32
