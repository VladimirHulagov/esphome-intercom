#include "i2s_audio_duplex.h"

#ifdef USE_ESP32

#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <cmath>

#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#ifdef USE_AUDIO_PROCESSOR
#include "../audio_processor/audio_processor.h"
#endif
#include "../audio_processor/audio_utils.h"
#include "../audio_processor/ring_buffer_caps.h"

namespace esphome {
namespace i2s_audio_duplex {

static const char *const TAG = "i2s_duplex";

static inline float compute_rms_dbfs(const int16_t *data, size_t samples, size_t stride = 1) {
  if (samples == 0) {
    return -120.0f;
  }
  uint64_t sumsq = 0;
  for (size_t i = 0; i < samples; i++) {
    int32_t s = data[i * stride];
    sumsq += static_cast<uint64_t>(s * s);
  }
  float mean = static_cast<float>(sumsq) / static_cast<float>(samples);
  float rms = std::sqrt(mean) / 32768.0f;
  if (rms < 1e-6f) {
    rms = 1e-6f;
  }
  return 20.0f * std::log10(rms);
}

// Helper: get MCLK multiple enum from integer value
static i2s_mclk_multiple_t get_mclk_multiple(uint32_t mult) {
  switch (mult) {
    case 128: return I2S_MCLK_MULTIPLE_128;
    case 384: return I2S_MCLK_MULTIPLE_384;
    case 512: return I2S_MCLK_MULTIPLE_512;
    default: return I2S_MCLK_MULTIPLE_256;
  }
}

// Helper: get STD slot config for the configured comm format (0=philips, 1=msb, 2=pcm_short, 3=pcm_long)
// Note: PCM short/long are TDM-only in ESP-IDF; falls back to Philips in STD mode
static i2s_std_slot_config_t get_std_slot_config(uint8_t fmt, i2s_data_bit_width_t bw, i2s_slot_mode_t mode) {
  switch (fmt) {
    case 1: return I2S_STD_MSB_SLOT_DEFAULT_CONFIG(bw, mode);
    default: return I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bw, mode);
  }
}

#if SOC_I2S_SUPPORTS_TDM
// Helper: get TDM slot config for the configured comm format
static i2s_tdm_slot_config_t get_tdm_slot_config(uint8_t fmt, i2s_data_bit_width_t bw,
                                                   i2s_slot_mode_t mode, i2s_tdm_slot_mask_t mask) {
  switch (fmt) {
    case 1: return I2S_TDM_MSB_SLOT_DEFAULT_CONFIG(bw, mode, mask);
    case 2: return I2S_TDM_PCM_SHORT_SLOT_DEFAULT_CONFIG(bw, mode, mask);
    case 3: return I2S_TDM_PCM_LONG_SLOT_DEFAULT_CONFIG(bw, mode, mask);
    default: return I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(bw, mode, mask);
  }
}
#endif  // SOC_I2S_SUPPORTS_TDM

// Audio parameters
static const size_t DMA_BUFFER_COUNT = 8;
static const size_t DMA_BUFFER_SIZE = 512;
static const size_t DEFAULT_FRAME_SIZE = 256;  // samples per frame at output rate (used when no AEC)
// Base buffer scaled by decimation_ratio_. At ratio=3 this provides ~256ms capacity,
// sufficient for stability while avoiding excessive latency from large playback backlogs.
// margin to absorb mixer jitter while saving ~24KB RAM.
static const size_t SPEAKER_BUFFER_BASE = 8192; // base speaker buffer, scaled by decimation_ratio_

// I2S new driver uses milliseconds directly, NOT FreeRTOS ticks
static const uint32_t I2S_IO_TIMEOUT_MS = 50;

void I2SAudioDuplex::setup() {
  ESP_LOGCONFIG(TAG, "Setting up I2S Audio Duplex...");

  // Compute decimation ratio: only active when output_sample_rate is explicitly set
  // and differs from sample_rate. If not set, ratio stays 1 (no decimation, zero overhead).
  if (this->output_sample_rate_ > 0 && this->output_sample_rate_ != this->sample_rate_) {
    this->decimation_ratio_ = this->sample_rate_ / this->output_sample_rate_;
    if (this->decimation_ratio_ * this->output_sample_rate_ != this->sample_rate_) {
      ESP_LOGE(TAG, "sample_rate (%u) must be an exact multiple of output_sample_rate (%u)",
               (unsigned)this->sample_rate_, (unsigned)this->output_sample_rate_);
      this->mark_failed();
      return;
    }
    if (this->decimation_ratio_ > 6) {
      ESP_LOGE(TAG, "Decimation ratio %u exceeds maximum of 6", (unsigned)this->decimation_ratio_);
      this->mark_failed();
      return;
    }
    this->mic_decimator_.init(this->decimation_ratio_);
    this->play_ref_decimator_.init(this->decimation_ratio_);
    // rx_decimator_ init deferred to audio_task_ where channel count is known
    ESP_LOGI(TAG, "Multi-rate: bus=%uHz, output=%uHz, ratio=%u",
             (unsigned)this->sample_rate_, (unsigned)this->output_sample_rate_,
             (unsigned)this->decimation_ratio_);
  }

  // Speaker ring buffer: stores data at bus rate (e.g. 48kHz).
  // Scale buffer size with decimation ratio to accommodate higher data rate.
  // PREFER_PSRAM: staging buffer between API play() and the i2s write path, not
  // realtime-critical itself (the task drains it at priority 19), so PSRAM is fine.
  this->speaker_buffer_size_ = SPEAKER_BUFFER_BASE * this->decimation_ratio_;
  this->speaker_buffer_ = audio_processor::create_prefer_psram(
      this->speaker_buffer_size_, "i2s_duplex.speaker");
  if (!this->speaker_buffer_) {
    ESP_LOGE(TAG, "Failed to create speaker ring buffer (%u bytes)", (unsigned)this->speaker_buffer_size_);
    this->mark_failed();
    return;
  }

  // AEC reference (mono mode only; stereo/TDM get ref from I2S RX).
  // Direct copy of previous TX frame, used for decimation on the next AEC pass.
  if (this->processor_ != nullptr && !this->use_stereo_aec_ref_ && !this->use_tdm_ref_) {
    size_t bus_frame_size = this->sample_rate_ / 1000 * 32;
    RAMAllocator<int16_t> alloc;
    this->direct_aec_ref_ = alloc.allocate(bus_frame_size);
    if (this->direct_aec_ref_ != nullptr) {
      memset(this->direct_aec_ref_, 0, bus_frame_size * sizeof(int16_t));
    }
    if (this->direct_aec_ref_) {
      ESP_LOGD(TAG, "AEC reference: direct from TX (%u samples)", (unsigned)bus_frame_size);
    } else {
      ESP_LOGE(TAG, "Failed to allocate direct AEC reference buffer");
    }
  }

  ESP_LOGI(TAG, "I2S Audio Duplex ready (speaker_buf=%u bytes)", (unsigned)this->speaker_buffer_size_);
}

void I2SAudioDuplex::set_processor(AudioProcessor *processor) {
  this->processor_ = processor;
  this->processor_enabled_.store(processor != nullptr, std::memory_order_relaxed);
  // Note: direct_aec_ref_ is allocated in setup() after decimation_ratio_ is computed
}

void I2SAudioDuplex::loop() {
  // Handle processor frame_spec change: audio task exited, restart with new
  // allocations. Consumer registration (mic_consumers_) survives stop()+start()
  // by construction, so no refcount save/restore is needed here.
  if (this->needs_restart_.exchange(false, std::memory_order_relaxed)) {
    ESP_LOGD(TAG, "Restarting audio task for frame_spec change");
    this->stop();
    this->start();
  }
}

void I2SAudioDuplex::dump_config() {
  ESP_LOGCONFIG(TAG, "I2S Audio Duplex:");
  ESP_LOGCONFIG(TAG, "  LRCLK Pin: %d", this->lrclk_pin_);
  ESP_LOGCONFIG(TAG, "  BCLK Pin: %d", this->bclk_pin_);
  ESP_LOGCONFIG(TAG, "  MCLK Pin: %d", this->mclk_pin_);
  ESP_LOGCONFIG(TAG, "  DIN Pin: %d", this->din_pin_);
  ESP_LOGCONFIG(TAG, "  DOUT Pin: %d", this->dout_pin_);
  ESP_LOGCONFIG(TAG, "  I2S Port: %u", this->i2s_num_);
  ESP_LOGCONFIG(TAG, "  I2S Role: %s", this->i2s_mode_secondary_ ? "secondary (slave)" : "primary (master)");
  ESP_LOGCONFIG(TAG, "  I2S Bus Rate: %u Hz", (unsigned)this->sample_rate_);
  ESP_LOGCONFIG(TAG, "  I2S Bits Per Sample: %u", this->bits_per_sample_);
  if (this->slot_bit_width_ > 0) {
    ESP_LOGCONFIG(TAG, "  Slot Bit Width: %u", this->slot_bit_width_);
  }
  ESP_LOGCONFIG(TAG, "  TX Channels: %u (%s)", this->num_channels_,
                this->num_channels_ == 2 ? "stereo" : "mono");
  ESP_LOGCONFIG(TAG, "  RX Mic Channel: %s", this->mic_channel_right_ ? "RIGHT" : "LEFT");
  static const char *const fmt_names[] = {"Philips", "MSB", "PCM Short", "PCM Long"};
  ESP_LOGCONFIG(TAG, "  Comm Format: %s", fmt_names[this->i2s_comm_fmt_ & 3]);
  ESP_LOGCONFIG(TAG, "  MCLK Multiple: %u", (unsigned)this->mclk_multiple_);
  if (this->use_apll_) {
    ESP_LOGCONFIG(TAG, "  APLL: enabled");
  }
  if (this->correct_dc_offset_) {
    ESP_LOGCONFIG(TAG, "  DC Offset Correction: enabled");
  }
  if (this->decimation_ratio_ > 1) {
    ESP_LOGCONFIG(TAG, "  Output Rate: %u Hz (decimation x%u)",
                  (unsigned)this->get_output_sample_rate(), (unsigned)this->decimation_ratio_);
  }
  ESP_LOGCONFIG(TAG, "  Speaker Buffer: %u bytes", (unsigned)this->speaker_buffer_size_);
  if (this->use_stereo_aec_ref_) {
    ESP_LOGCONFIG(TAG, "  Stereo AEC Reference: %s channel", this->ref_channel_right_ ? "RIGHT" : "LEFT");
  }
  if (this->use_tdm_ref_) {
    if (this->tdm_second_mic_slot_ >= 0) {
      ESP_LOGCONFIG(TAG, "  TDM Reference: %u slots, mic_slots=[%u,%d], ref_slot=%u",
                    this->tdm_total_slots_, this->tdm_mic_slot_,
                    this->tdm_second_mic_slot_, this->tdm_ref_slot_);
    } else {
      ESP_LOGCONFIG(TAG, "  TDM Reference: %u slots, mic_slot=%u, ref_slot=%u",
                    this->tdm_total_slots_, this->tdm_mic_slot_, this->tdm_ref_slot_);
    }
  }
  ESP_LOGCONFIG(TAG, "  AEC: %s", this->processor_ != nullptr ? "enabled" : "disabled");
  ESP_LOGCONFIG(TAG, "  Task: priority=%u, core=%d, stack=%u",
                this->task_priority_, this->task_core_, (unsigned)this->task_stack_size_);
#ifdef USE_DUPLEX_TELEMETRY
  ESP_LOGCONFIG(TAG, "  Telemetry Log Interval: %u frames", (unsigned) this->telemetry_log_interval_frames_);
#endif
}

bool I2SAudioDuplex::init_i2s_duplex_() {
  ESP_LOGCONFIG(TAG, "Initializing I2S in DUPLEX mode...");

  // Map configured bit depth to I2S enum
  // Note: 24-bit data is stored in 32-bit DMA containers (MSB-aligned)
  i2s_data_bit_width_t bit_width;
  switch (this->bits_per_sample_) {
    case 32: bit_width = I2S_DATA_BIT_WIDTH_32BIT; break;
    case 24: bit_width = I2S_DATA_BIT_WIDTH_24BIT; break;
    default: bit_width = I2S_DATA_BIT_WIDTH_16BIT; break;
  }

  // Slot bit width: auto = match data bit width, or explicit override
  i2s_slot_bit_width_t slot_bw = I2S_SLOT_BIT_WIDTH_AUTO;
  if (this->slot_bit_width_ > 0) {
    switch (this->slot_bit_width_) {
      case 32: slot_bw = I2S_SLOT_BIT_WIDTH_32BIT; break;
      case 24: slot_bw = I2S_SLOT_BIT_WIDTH_24BIT; break;
      case 16: slot_bw = I2S_SLOT_BIT_WIDTH_16BIT; break;
      default: slot_bw = I2S_SLOT_BIT_WIDTH_AUTO; break;
    }
  }

  bool need_tx = (this->dout_pin_ >= 0);
  bool need_rx = (this->din_pin_ >= 0);

  if (!need_tx && !need_rx) {
    ESP_LOGE(TAG, "At least one of din_pin or dout_pin must be configured");
    return false;
  }

  // Channel configuration
  // Clock source: APLL for accurate clocking (ESP32 original only)
  i2s_clock_src_t clk_src = I2S_CLK_SRC_DEFAULT;
#ifdef I2S_CLK_SRC_APLL
  if (this->use_apll_) clk_src = I2S_CLK_SRC_APLL;
#endif
  i2s_mclk_multiple_t mclk_mult = get_mclk_multiple(this->mclk_multiple_);

  // DMA descriptor limit is 4092 bytes. Compute max bytes per frame across
  // TX and RX configs, then clamp dma_frame_num to stay within the limit.
  // RX can be wider than TX (e.g., mono TX but stereo RX for AEC feedback).
  uint32_t bytes_per_sample = (this->bits_per_sample_ > 16) ? 4 : 2;  // 24/32-bit → 4-byte DMA container
  uint32_t tx_bytes_per_frame = this->num_channels_ * bytes_per_sample;
  uint32_t rx_bytes_per_frame = tx_bytes_per_frame;
  if (this->use_stereo_aec_ref_) {
    rx_bytes_per_frame = 2 * bytes_per_sample;  // stereo RX forced
  }
#if SOC_I2S_SUPPORTS_TDM
  if (this->use_tdm_ref_) {
    uint32_t tdm_frame = this->tdm_total_slots_ * bytes_per_sample;
    rx_bytes_per_frame = tdm_frame;
    tx_bytes_per_frame = tdm_frame;
  }
#endif
  uint32_t max_bytes_per_frame = std::max(tx_bytes_per_frame, rx_bytes_per_frame);
  uint32_t dma_frame_num = DMA_BUFFER_SIZE;  // 512 default
  if (max_bytes_per_frame > 0) {
    uint32_t max_frames = 4092 / max_bytes_per_frame;
    if (dma_frame_num > max_frames) {
      dma_frame_num = max_frames;
    }
  }
  i2s_chan_config_t chan_cfg = {
      .id = static_cast<i2s_port_t>(this->i2s_num_),
      .role = this->i2s_mode_secondary_ ? I2S_ROLE_SLAVE : I2S_ROLE_MASTER,
      .dma_desc_num = DMA_BUFFER_COUNT,
      .dma_frame_num = dma_frame_num,
      .auto_clear_after_cb = true,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
      .auto_clear_before_cb = false,
      .intr_priority = 0,
#endif
  };

  i2s_chan_handle_t *tx_ptr = need_tx ? &this->tx_handle_ : nullptr;
  i2s_chan_handle_t *rx_ptr = need_rx ? &this->rx_handle_ : nullptr;

  esp_err_t err = i2s_new_channel(&chan_cfg, tx_ptr, rx_ptr);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(err));
    return false;
  }

  ESP_LOGD(TAG, "I2S channel created: TX=%s RX=%s",
           this->tx_handle_ ? "yes" : "no",
           this->rx_handle_ ? "yes" : "no");

  auto pin_or_nc = [](int pin) -> gpio_num_t {
    return pin >= 0 ? static_cast<gpio_num_t>(pin) : GPIO_NUM_NC;
  };

#if SOC_I2S_SUPPORTS_TDM
  if (this->use_tdm_ref_) {
    // ── TDM MODE: ES7210 multi-slot RX + ES8311 slot-0 TX ──
    // STEREO with 4 slots: DMA contains all 4 interleaved slots, BCLK/FS = 64.
    // ESP-IDF MONO only puts slot 0 in DMA; STEREO gives all active slots.
    // total_slot is derived from slot_mask (not slot_mode), so BCLK doesn't change.
    // ES8311 reads/writes slot 0 as standard I2S (first 16 bits after LRCLK edge).
    // DMA frame = tdm_total_slots × 2 bytes. At 4 slots, 256 frames = 2048 bytes/desc (< 4092 limit).
    i2s_tdm_slot_mask_t tdm_mask = I2S_TDM_SLOT0;
    for (int i = 1; i < this->tdm_total_slots_; i++)
      tdm_mask = static_cast<i2s_tdm_slot_mask_t>(tdm_mask | (I2S_TDM_SLOT0 << i));

    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = this->sample_rate_,
            .clk_src = clk_src,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = mclk_mult,
        },
        .slot_cfg = get_tdm_slot_config(this->i2s_comm_fmt_, bit_width, I2S_SLOT_MODE_STEREO, tdm_mask),
        .gpio_cfg = {
            .mclk = pin_or_nc(this->mclk_pin_),
            .bclk = pin_or_nc(this->bclk_pin_),
            .ws = pin_or_nc(this->lrclk_pin_),
            .dout = pin_or_nc(this->dout_pin_),
            .din = pin_or_nc(this->din_pin_),
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    // Apply slot_bit_width override BEFORE init
    if (slot_bw != I2S_SLOT_BIT_WIDTH_AUTO) {
      tdm_cfg.slot_cfg.slot_bit_width = slot_bw;
    }

    if (this->tx_handle_) {
      err = i2s_channel_init_tdm_mode(this->tx_handle_, &tdm_cfg);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init TDM TX: %s", esp_err_to_name(err));
        this->deinit_i2s_();
        return false;
      }
    }
    if (this->rx_handle_) {
      err = i2s_channel_init_tdm_mode(this->rx_handle_, &tdm_cfg);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init TDM RX: %s", esp_err_to_name(err));
        this->deinit_i2s_();
        return false;
      }
    }

    if (this->tdm_second_mic_slot_ >= 0) {
      ESP_LOGD(TAG, "TDM mode: %d slots, mic_slots=[%d,%d], ref_slot=%d, mask=0x%x",
               this->tdm_total_slots_, this->tdm_mic_slot_, this->tdm_second_mic_slot_,
               this->tdm_ref_slot_, (unsigned) tdm_mask);
    } else {
      ESP_LOGD(TAG, "TDM mode: %d slots, mic_slot=%d, ref_slot=%d, mask=0x%x",
               this->tdm_total_slots_, this->tdm_mic_slot_, this->tdm_ref_slot_, (unsigned) tdm_mask);
    }
  } else
#endif  // SOC_I2S_SUPPORTS_TDM
  {
    // ── STANDARD MODE ──
    i2s_slot_mode_t tx_slot_mode = (this->num_channels_ == 2)
        ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO;
    i2s_std_config_t tx_cfg = {
        .clk_cfg = {
            .sample_rate_hz = this->sample_rate_,
            .clk_src = clk_src,
            .mclk_multiple = mclk_mult,
        },
        .slot_cfg = get_std_slot_config(this->i2s_comm_fmt_, bit_width, tx_slot_mode),
        .gpio_cfg = {
            .mclk = pin_or_nc(this->mclk_pin_),
            .bclk = pin_or_nc(this->bclk_pin_),
            .ws = pin_or_nc(this->lrclk_pin_),
            .dout = pin_or_nc(this->dout_pin_),
            .din = pin_or_nc(this->din_pin_),
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    tx_cfg.slot_cfg.slot_mask = (this->num_channels_ == 2)
        ? I2S_STD_SLOT_BOTH : (this->tx_slot_right_ ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT);
    // Apply slot_bit_width override
    if (slot_bw != I2S_SLOT_BIT_WIDTH_AUTO) {
      tx_cfg.slot_cfg.slot_bit_width = slot_bw;
    }

    // RX configuration - always independent of TX num_channels
    i2s_std_config_t rx_cfg = tx_cfg;
    if (this->use_stereo_aec_ref_) {
      rx_cfg.slot_cfg = get_std_slot_config(this->i2s_comm_fmt_, bit_width, I2S_SLOT_MODE_STEREO);
      rx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
      ESP_LOGD(TAG, "RX configured as STEREO for ES8311 digital feedback AEC");
    } else {
      rx_cfg.slot_cfg = get_std_slot_config(this->i2s_comm_fmt_, bit_width, I2S_SLOT_MODE_MONO);
      rx_cfg.slot_cfg.slot_mask = this->mic_channel_right_ ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT;
    }
    // Apply slot_bit_width override to RX
    if (slot_bw != I2S_SLOT_BIT_WIDTH_AUTO) {
      rx_cfg.slot_cfg.slot_bit_width = slot_bw;
    }

    if (this->tx_handle_) {
      err = i2s_channel_init_std_mode(this->tx_handle_, &tx_cfg);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init TX channel: %s", esp_err_to_name(err));
        this->deinit_i2s_();
        return false;
      }
      ESP_LOGD(TAG, "TX channel initialized");
    }

    if (this->rx_handle_) {
      err = i2s_channel_init_std_mode(this->rx_handle_, &rx_cfg);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init RX channel: %s", esp_err_to_name(err));
        this->deinit_i2s_();
        return false;
      }
      ESP_LOGD(TAG, "RX channel initialized (%s)", this->use_stereo_aec_ref_ ? "stereo" : "mono");
    }
  }

  if (this->tx_handle_) {
    err = i2s_channel_enable(this->tx_handle_);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to enable TX channel: %s", esp_err_to_name(err));
      this->deinit_i2s_();
      return false;
    }
  }
  if (this->rx_handle_) {
    err = i2s_channel_enable(this->rx_handle_);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to enable RX channel: %s", esp_err_to_name(err));
      this->deinit_i2s_();
      return false;
    }
  }

  ESP_LOGI(TAG, "I2S DUPLEX initialized (%s)", this->use_tdm_ref_ ? "TDM" : "standard");
  return true;
}

void I2SAudioDuplex::deinit_i2s_() {
  if (this->tx_handle_) {
    i2s_channel_disable(this->tx_handle_);
    i2s_del_channel(this->tx_handle_);
    this->tx_handle_ = nullptr;
  }
  if (this->rx_handle_) {
    i2s_channel_disable(this->rx_handle_);
    i2s_del_channel(this->rx_handle_);
    this->rx_handle_ = nullptr;
  }
  ESP_LOGD(TAG, "I2S deinitialized");
}

void I2SAudioDuplex::finish_stop_cleanup_() {
  if (this->tx_handle_) {
    i2s_del_channel(this->tx_handle_);
    this->tx_handle_ = nullptr;
  }
  if (this->rx_handle_) {
    i2s_del_channel(this->rx_handle_);
    this->rx_handle_ = nullptr;
  }
  this->stop_cleanup_pending_.store(false, std::memory_order_relaxed);
  this->audio_task_handle_ = nullptr;
  ESP_LOGI(TAG, "Duplex audio stopped (cleanup complete)");
}

void I2SAudioDuplex::start() {
  if (this->duplex_running_.load(std::memory_order_relaxed)) {
    ESP_LOGW(TAG, "Already running");
    return;
  }

  // Complete deferred cleanup from previous stop timeout
  if (this->stop_cleanup_pending_.load(std::memory_order_relaxed)) {
    if (this->task_exited_.load(std::memory_order_relaxed)) {
      ESP_LOGW(TAG, "Completing deferred stop cleanup before start");
      this->finish_stop_cleanup_();
    } else {
      ESP_LOGE(TAG, "Previous audio task still stuck, cannot start");
      this->has_i2s_error_.store(true, std::memory_order_relaxed);
      return;
    }
  }

  ESP_LOGI(TAG, "Starting duplex audio...");

  if (!this->init_i2s_duplex_()) {
    ESP_LOGE(TAG, "Failed to initialize I2S");
    return;
  }

  this->duplex_running_.store(true, std::memory_order_relaxed);
  this->task_exited_.store(false, std::memory_order_relaxed);
  this->has_i2s_error_.store(false, std::memory_order_relaxed);
  this->speaker_running_.store(this->tx_handle_ != nullptr, std::memory_order_relaxed);

  this->speaker_buffer_->reset();

  // Reset FIR decimators for clean state
  this->mic_decimator_.reset();
  this->rx_decimator_.reset();
  this->play_ref_decimator_.reset();

#ifdef USE_AUDIO_PROCESSOR
  if (this->use_stereo_aec_ref_) {
    ESP_LOGD(TAG, "ES8311 digital feedback - reference is sample-aligned");
  }
  if (this->use_tdm_ref_) {
    ESP_LOGD(TAG, "TDM hardware reference - slot %u is echo ref", this->tdm_ref_slot_);
  }
#endif

  // Audio task stack MUST stay in internal RAM (real-time, latency-sensitive)
  BaseType_t task_created = xTaskCreatePinnedToCore(
      audio_task,
      "i2s_duplex",
      this->task_stack_size_,
      this,
      this->task_priority_,
      &this->audio_task_handle_,
      this->task_core_ >= 0 ? this->task_core_ : tskNO_AFFINITY
  );

  if (task_created != pdPASS) {
    ESP_LOGE(TAG, "Failed to create audio task");
    this->duplex_running_.store(false, std::memory_order_relaxed);
    this->speaker_running_.store(false, std::memory_order_relaxed);
    this->deinit_i2s_();
    return;
  }

  ESP_LOGI(TAG, "Duplex audio started");
}

void I2SAudioDuplex::stop() {
  if (!this->duplex_running_.load(std::memory_order_relaxed)) {
    return;
  }

  ESP_LOGI(TAG, "Stopping duplex audio...");

  // Note: mic_consumers_ deliberately NOT cleared here. Consumers stay
  // registered across stop()/start() so the mic path is reconnected
  // automatically after an internal restart (frame_spec change).
  this->speaker_running_.store(false, std::memory_order_relaxed);
  this->duplex_running_.store(false, std::memory_order_relaxed);

  delay(60);

  esp_err_t err;
  if (this->tx_handle_) {
    err = i2s_channel_disable(this->tx_handle_);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "TX channel disable failed: %s", esp_err_to_name(err));
    }
  }
  if (this->rx_handle_) {
    err = i2s_channel_disable(this->rx_handle_);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "RX channel disable failed: %s", esp_err_to_name(err));
    }
  }

  if (this->audio_task_handle_) {
    int wait_count = 0;
    while (!this->task_exited_.load(std::memory_order_relaxed) && wait_count < 100) {
      delay(10);
      wait_count++;
    }
    if (!this->task_exited_.load(std::memory_order_relaxed)) {
      ESP_LOGE(TAG, "Audio task did not exit within 1s; channels disabled, deletion deferred");
      this->has_i2s_error_.store(true, std::memory_order_relaxed);
      this->stop_cleanup_pending_.store(true, std::memory_order_relaxed);
      this->audio_task_handle_ = nullptr;
      return;
    }
    this->audio_task_handle_ = nullptr;
  }

  this->finish_stop_cleanup_();
}

void I2SAudioDuplex::register_mic_consumer(void *token) {
  bool needs_start = false;
  size_t count_after = 0;
  bool first_consumer = false;
  {
    std::lock_guard<std::mutex> lock(this->mic_consumers_mutex_);
    auto it = std::find(this->mic_consumers_.begin(), this->mic_consumers_.end(), token);
    if (it != this->mic_consumers_.end()) {
      return;
    }
    first_consumer = this->mic_consumers_.empty();
    this->mic_consumers_.push_back(token);
    count_after = this->mic_consumers_.size();
    this->has_mic_consumers_.store(true, std::memory_order_relaxed);
    needs_start = !this->duplex_running_.load(std::memory_order_relaxed);
  }
  if (first_consumer) {
    ESP_LOGI(TAG, "Mic consumer registered (token=%p) — mic path active (consumers=%zu)",
             token, count_after);
  } else {
    ESP_LOGD(TAG, "Mic consumer registered (token=%p, consumers=%zu)", token, count_after);
  }
  if (needs_start) {
    this->start();
  }
}

void I2SAudioDuplex::unregister_mic_consumer(void *token) {
  size_t count_after = 0;
  bool removed = false;
  bool last_consumer_gone = false;
  {
    std::lock_guard<std::mutex> lock(this->mic_consumers_mutex_);
    auto it = std::find(this->mic_consumers_.begin(), this->mic_consumers_.end(), token);
    if (it != this->mic_consumers_.end()) {
      this->mic_consumers_.erase(it);
      removed = true;
    }
    count_after = this->mic_consumers_.size();
    last_consumer_gone = removed && this->mic_consumers_.empty();
    this->has_mic_consumers_.store(!this->mic_consumers_.empty(), std::memory_order_relaxed);
  }
  if (!removed) {
    return;
  }
  if (last_consumer_gone) {
    ESP_LOGI(TAG, "Last mic consumer removed (token=%p) — mic path idle", token);
  } else {
    ESP_LOGD(TAG, "Mic consumer unregistered (token=%p, consumers=%zu)", token, count_after);
  }
}

void I2SAudioDuplex::start_speaker() {
  if (!this->duplex_running_.load(std::memory_order_relaxed)) {
    this->start();
  }
  this->speaker_running_.store(true, std::memory_order_relaxed);

  this->play_ref_decimator_.reset();
}

void I2SAudioDuplex::stop_speaker() {
  this->speaker_running_.store(false, std::memory_order_relaxed);
  // Request audio task to reset ring buffers (avoids concurrent access).
  this->request_speaker_reset_.store(true, std::memory_order_relaxed);
}

size_t I2SAudioDuplex::play(const uint8_t *data, size_t len, TickType_t ticks_to_wait) {
  if (!this->speaker_buffer_) {
    return 0;
  }

  // Data arrives at bus rate (e.g. 48kHz from mixer/resampler). Write directly.
  size_t written = this->speaker_buffer_->write_without_replacement((void *) data, len, ticks_to_wait, true);

  if (written > 0) {
    this->last_speaker_audio_ms_.store(millis(), std::memory_order_relaxed);
  }

  return written;
}

bool I2SAudioDuplex::allocate_audio_buffers_(AudioTaskCtx &ctx) {
  if (this->audio_buffers_allocated_) {
    return true;
  }

  static constexpr size_t AEC_ALIGN = 16;
  const uint32_t buf_caps = this->buffers_in_psram_ ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL;

  // Worst-case processor mic channels: 2 if dual-mic TDM is available, else 1.
  // Allocating for 2ch unconditionally when dual-mic is possible lets the task
  // flip between MR (1 mic) and MMR (2 mic) without reallocating on reconfigure.
  const uint8_t worst_mic_ch = (this->tdm_second_mic_slot_ >= 0) ? 2 : 1;
  const size_t worst_processor_input_bytes = ctx.input_frame_bytes * worst_mic_ch;

  this->prealloc_rx_buffer_ = static_cast<int16_t *>(
      heap_caps_malloc(ctx.rx_frame_bytes, buf_caps));

  if (ctx.mic_separate) {
    this->prealloc_mic_buffer_ = static_cast<int16_t *>(
        heap_caps_aligned_alloc(AEC_ALIGN, ctx.input_frame_bytes, buf_caps));
  }

  if (worst_mic_ch > 1) {
    this->prealloc_processor_mic_buffer_ = static_cast<int16_t *>(
        heap_caps_aligned_alloc(AEC_ALIGN, worst_processor_input_bytes, buf_caps));
  }

  this->prealloc_spk_buffer_ = static_cast<int16_t *>(
      heap_caps_malloc(ctx.bus_frame_size * ctx.num_ch * ctx.i2s_bps, buf_caps));

  if (ctx.use_stereo_aec_ref || ctx.use_tdm_ref) {
    this->prealloc_spk_ref_buffer_ = static_cast<int16_t *>(
        heap_caps_aligned_alloc(AEC_ALIGN, ctx.input_frame_bytes, buf_caps));
  }

  if (ctx.use_tdm_ref) {
    const size_t tdm_tx_bytes = ctx.bus_frame_size * ctx.tdm_total_slots * ctx.i2s_bps;
    this->prealloc_tdm_tx_buffer_ = static_cast<int16_t *>(
        heap_caps_malloc(tdm_tx_bytes, buf_caps));
  }

#ifdef USE_AUDIO_PROCESSOR
  if (this->processor_ != nullptr && this->processor_->is_initialized()) {
    if (!this->prealloc_spk_ref_buffer_ && !ctx.use_tdm_ref) {
      this->prealloc_spk_ref_buffer_ = static_cast<int16_t *>(
          heap_caps_aligned_alloc(AEC_ALIGN, ctx.input_frame_bytes, buf_caps));
    }
    this->prealloc_aec_output_ = static_cast<int16_t *>(
        heap_caps_aligned_alloc(AEC_ALIGN, ctx.output_frame_bytes, buf_caps));

    // AEC reference ring buffer (TYPE2-style, no-codec setups). Also one-shot.
    if (this->aec_use_ring_buffer_ && !ctx.use_stereo_aec_ref && !ctx.use_tdm_ref &&
        !this->aec_ref_ring_buffer_) {
      size_t rb_bytes = (this->sample_rate_ * this->aec_ref_buffer_ms_ / 1000) * sizeof(int16_t);
      if (rb_bytes < ctx.bus_frame_bytes * 4) rb_bytes = ctx.bus_frame_bytes * 4;
      this->aec_ref_ring_buffer_ = audio_processor::create_prefer_psram(rb_bytes, "i2s_duplex.aec_ref");
      if (!this->aec_ref_ring_buffer_) {
        return false;
      }
      ESP_LOGI(TAG, "AEC reference: ring_buffer (%zu bytes, %ums capacity)",
               rb_bytes, (unsigned)this->aec_ref_buffer_ms_);
    } else if (!ctx.use_stereo_aec_ref && !ctx.use_tdm_ref && !this->aec_ref_ring_buffer_) {
      ESP_LOGI(TAG, "AEC reference: previous_frame");
    }
  }
#endif

  // Validate required allocations
  if (!this->prealloc_rx_buffer_ || !this->prealloc_spk_buffer_) return false;
  if (ctx.mic_separate && !this->prealloc_mic_buffer_) return false;
  if (worst_mic_ch > 1 && !this->prealloc_processor_mic_buffer_) return false;
  if (ctx.use_tdm_ref && !this->prealloc_tdm_tx_buffer_) return false;
#ifdef USE_AUDIO_PROCESSOR
  if (this->processor_ != nullptr && this->processor_->is_initialized()) {
    if (!this->prealloc_aec_output_) return false;
    if ((ctx.use_stereo_aec_ref || ctx.use_tdm_ref) && !this->prealloc_spk_ref_buffer_) return false;
  }
#endif

  this->audio_buffers_allocated_ = true;
  return true;
}

void I2SAudioDuplex::audio_task(void *param) {
  I2SAudioDuplex *self = static_cast<I2SAudioDuplex *>(param);
  self->audio_task_();
  vTaskDelete(nullptr);
}

void I2SAudioDuplex::audio_task_() {
  AudioTaskCtx ctx{};
#ifdef USE_DUPLEX_TELEMETRY
  uint32_t t_frame_count = 0;
  uint32_t t_spk_underruns = 0;
#ifdef USE_AUDIO_PROCESSOR
  ProcessorTelemetry prev_processor_telem{};
#endif
#endif

  // ── Populate invariants ──
  ctx.ratio = this->decimation_ratio_;
  ctx.i2s_bps = (this->bits_per_sample_ > 16) ? 4 : 2;
  ctx.num_ch = this->num_channels_;
  ctx.use_stereo_aec_ref = this->use_stereo_aec_ref_;
  ctx.use_tdm_ref = this->use_tdm_ref_;
  ctx.ref_channel_right = this->ref_channel_right_;
  ctx.correct_dc_offset = this->correct_dc_offset_;
  ctx.tdm_total_slots = this->tdm_total_slots_;
  ctx.tdm_mic_slot = this->tdm_mic_slot_;
  ctx.tdm_second_mic_slot = this->tdm_second_mic_slot_;
  ctx.tdm_ref_slot = this->tdm_ref_slot_;

  ESP_LOGI(TAG, "Audio task started (stereo=%s, tdm=%s, decimation=%ux)",
           ctx.use_stereo_aec_ref ? "YES" : "no",
           ctx.use_tdm_ref ? "YES" : "no", (unsigned)ctx.ratio);

  // Determine frame sizes: processors may consume and produce different frame lengths.
  ctx.input_frame_size = DEFAULT_FRAME_SIZE;
  ctx.output_frame_size = DEFAULT_FRAME_SIZE;
#ifdef USE_AUDIO_PROCESSOR
  if (this->processor_ != nullptr && this->processor_->is_initialized()) {
    auto spec = this->processor_->frame_spec();
    ctx.input_frame_size = spec.input_samples;
    ctx.output_frame_size = spec.output_samples;
    ctx.processor_mic_channels = std::max<uint8_t>(1, spec.mic_channels);
    ctx.processor_spec_revision = this->processor_->frame_spec_revision();
    uint32_t out_rate = this->get_output_sample_rate();
    ESP_LOGI(TAG, "Processor: input=%u, output=%u samples, mic_ch=%u (%ums @ %uHz), revision=%u",
             (unsigned) ctx.input_frame_size, (unsigned) ctx.output_frame_size,
             (unsigned) ctx.processor_mic_channels,
             (unsigned) (ctx.input_frame_size * 1000 / out_rate), (unsigned) out_rate,
             (unsigned) ctx.processor_spec_revision);
  }
#endif

  // Init multi-channel RX decimator now that we know channel count
  if (ctx.use_tdm_ref || ctx.use_stereo_aec_ref) {
    uint8_t rx_ch = ctx.use_tdm_ref
        ? (ctx.processor_mic_channels > 1 ? 3 : 2)  // MMR or MR
        : 2;  // stereo: mic + ref
    this->rx_decimator_.init(ctx.ratio, rx_ch);
  }

  // ── Frame sizing ──
  ctx.bus_frame_size = ctx.input_frame_size * ctx.ratio;
  ctx.input_frame_bytes = ctx.input_frame_size * sizeof(int16_t);
  ctx.processor_input_frame_bytes = ctx.input_frame_bytes * ctx.processor_mic_channels;
  ctx.output_frame_bytes = ctx.output_frame_size * sizeof(int16_t);
  ctx.bus_frame_bytes = ctx.bus_frame_size * sizeof(int16_t);
  if (ctx.use_tdm_ref) {
    ctx.rx_frame_bytes = ctx.bus_frame_size * ctx.tdm_total_slots * ctx.i2s_bps;
  } else if (ctx.use_stereo_aec_ref) {
    ctx.rx_frame_bytes = ctx.bus_frame_size * 2 * ctx.i2s_bps;
  } else {
    ctx.rx_frame_bytes = ctx.bus_frame_size * ctx.i2s_bps;
  }

  // ── Buffer setup ──
  // Working buffers are owned by the component and pre-allocated worst-case
  // on first task entry (see allocate_audio_buffers_). Subsequent restarts
  // (frame_spec change, feature toggle) reuse the same pointers without any
  // heap_caps_alloc calls, eliminating SPIRAM fragmentation that previously
  // caused "Failed to allocate AEC output buffer" after a few reconfigures.
  ctx.mic_separate = (ctx.ratio > 1) || ctx.use_stereo_aec_ref || ctx.use_tdm_ref;

  auto alloc_fail = [this](const char *what) {
    ESP_LOGE(TAG, "Failed to allocate %s", what);
    this->has_i2s_error_.store(true, std::memory_order_relaxed);
    this->task_exited_.store(true, std::memory_order_relaxed);
    this->duplex_running_.store(false, std::memory_order_relaxed);
    this->speaker_running_.store(false, std::memory_order_relaxed);
  };

  if (ctx.processor_mic_channels > 2) {
    alloc_fail("unsupported processor mic channel count");
    goto cleanup;
  }
  if (ctx.processor_mic_channels > 1 && !ctx.use_tdm_ref) {
    alloc_fail("dual-mic processor requires TDM microphone slots");
    goto cleanup;
  }
  if (ctx.processor_mic_channels > 1 && ctx.tdm_second_mic_slot < 0) {
    alloc_fail("dual-mic processor requires tdm_mic_slots with two slots");
    goto cleanup;
  }

  if (!this->allocate_audio_buffers_(ctx)) {
    alloc_fail("audio buffers");
    goto cleanup;
  }

  ctx.rx_buffer = this->prealloc_rx_buffer_;
  ctx.mic_buffer = ctx.mic_separate ? this->prealloc_mic_buffer_ : ctx.rx_buffer;
  if (ctx.processor_mic_channels > 1) {
    ctx.processor_mic_buffer = this->prealloc_processor_mic_buffer_;
  }
  ctx.spk_buffer = this->prealloc_spk_buffer_;
  ctx.spk_ref_buffer = this->prealloc_spk_ref_buffer_;
  ctx.tdm_tx_buffer = this->prealloc_tdm_tx_buffer_;
  ctx.aec_output = this->prealloc_aec_output_;
  if (ctx.use_tdm_ref) {
    ctx.tdm_tx_frame_bytes = ctx.bus_frame_size * ctx.tdm_total_slots * ctx.i2s_bps;
  }

#ifdef USE_DUPLEX_TELEMETRY
  ESP_LOGI(TAG, "Heap after audio init: internal=%u, PSRAM=%u",
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#endif

  // ── Main loop ──
  while (this->duplex_running_.load(std::memory_order_relaxed)) {
    // Service ring buffer operations requested by main thread
    if (this->request_speaker_reset_.exchange(false, std::memory_order_relaxed)) {
      this->speaker_buffer_->reset();
      this->direct_aec_ref_valid_ = false;
      if (this->aec_ref_ring_buffer_) {
        this->aec_ref_ring_buffer_->reset();
      }
    }
    // Reset per-frame state
    ctx.output_buffer = nullptr;
    ctx.current_output_frame_size = ctx.input_frame_size;
    ctx.current_output_frame_bytes = ctx.input_frame_bytes;

    // Snapshot atomic state for this frame (avoids repeated .load() in sample loops)
    ctx.mic_attenuation = this->mic_attenuation_.load(std::memory_order_relaxed);
    ctx.mic_gain = this->mic_gain_.load(std::memory_order_relaxed);
    ctx.speaker_volume = this->speaker_volume_.load(std::memory_order_relaxed);
    ctx.speaker_running = this->speaker_running_.load(std::memory_order_relaxed);
    ctx.speaker_paused = this->speaker_paused_.load(std::memory_order_relaxed);
    ctx.mic_running = this->has_mic_consumers_.load(std::memory_order_relaxed);

    this->process_rx_path_(ctx);

    ctx.processor_enabled = this->processor_enabled_.load(std::memory_order_relaxed);
#ifdef USE_AUDIO_PROCESSOR
    ctx.processor_ready = ctx.processor_enabled && this->processor_ != nullptr &&
                          this->processor_->is_initialized();
#endif
    ctx.now_ms = millis();

    this->process_aec_and_callbacks_(ctx);
    this->process_tx_path_(ctx);

#ifdef USE_AUDIO_PROCESSOR
    // Check if processor's frame_spec changed (e.g., SE toggled).
    // If so, exit the loop for task restart with new allocations.
    if (this->processor_ != nullptr) {
      uint32_t rev = this->processor_->frame_spec_revision();
      if (rev != ctx.processor_spec_revision) {
        ESP_LOGD(TAG, "Processor frame_spec changed (rev %u -> %u), restarting audio task",
                 (unsigned) ctx.processor_spec_revision, (unsigned) rev);
        this->needs_restart_.store(true, std::memory_order_relaxed);
        break;
      }
    }
#endif

#ifdef USE_DUPLEX_TELEMETRY
    {
      // Lightweight per-frame cycle snapshot (only when telemetry: true)
      // Note: includes I2S blocking time in rx/tx, separate compute-only measurement
      // would require bracketing inside the path functions.
      // t_frame_count and t_spk_underruns declared before the loop (reset on task restart)
      t_spk_underruns += ctx.speaker_underrun ? 1 : 0;
      t_frame_count++;
      if (t_frame_count >= this->telemetry_log_interval_frames_) {
        ESP_LOGD(TAG, "Perf[%u frames]: spk_underrun=%u, heap_int=%u, heap_ps=%u",
                 (unsigned) t_frame_count, (unsigned) t_spk_underruns,
                 (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned) heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#ifdef USE_AUDIO_PROCESSOR
        if (this->processor_ != nullptr) {
          ProcessorTelemetry telem = this->processor_->telemetry();
          const uint32_t audio_stack_high_water =
              uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t);
          auto delta_u32 = [](uint32_t current, uint32_t previous) -> uint32_t {
            return current >= previous ? (current - previous) : current;
          };
          ESP_LOGD(TAG,
                   "AFE[%u]: +glitch=%u +in_drop=%u +feed_ok=%u +feed_rej=%u "
                   "+fetch_ok=%u +fetch_to=%u +out_drop=%u rb=%.0f%% "
                   "q(feed=%u pk=%u, fetch=%u pk=%u)",
                   (unsigned) telem.frame_count,
                   (unsigned) delta_u32(telem.glitch_count, prev_processor_telem.glitch_count),
                   (unsigned) delta_u32(telem.input_ring_drop, prev_processor_telem.input_ring_drop),
                   (unsigned) delta_u32(telem.feed_ok, prev_processor_telem.feed_ok),
                   (unsigned) delta_u32(telem.feed_rejected, prev_processor_telem.feed_rejected),
                   (unsigned) delta_u32(telem.fetch_ok, prev_processor_telem.fetch_ok),
                   (unsigned) delta_u32(telem.fetch_timeout, prev_processor_telem.fetch_timeout),
                   (unsigned) delta_u32(telem.output_ring_drop, prev_processor_telem.output_ring_drop),
                   telem.ringbuf_free_pct * 100.0f,
                   (unsigned) telem.feed_queue_frames,
                   (unsigned) telem.feed_queue_peak,
                   (unsigned) telem.fetch_queue_frames,
                   (unsigned) telem.fetch_queue_peak);
          ESP_LOGD(TAG,
                   "AFE timing: proc last/max=%u/%uus feed last/max=%u/%uus "
                   "fetch last/max=%u/%uus stackB audio/feed/fetch=%u/%u/%u",
                   (unsigned) telem.process_us_last,
                   (unsigned) telem.process_us_max,
                   (unsigned) telem.feed_us_last,
                   (unsigned) telem.feed_us_max,
                   (unsigned) telem.fetch_us_last,
                   (unsigned) telem.fetch_us_max,
                   (unsigned) audio_stack_high_water,
                   (unsigned) telem.feed_stack_high_water,
                   (unsigned) telem.fetch_stack_high_water);
          prev_processor_telem = telem;
        }
#endif
        t_frame_count = 0;
        t_spk_underruns = 0;
      }
    }
#endif

    // Yield: I2S read/write already block on DMA, so taskYIELD suffices.
    if (ctx.consecutive_i2s_errors > 0) {
      delay(1);
    } else {
      taskYIELD();
    }
  }

  this->task_exited_.store(true, std::memory_order_relaxed);

cleanup:
  // Buffers live on as component-owned preallocations; nothing to free here.
  ESP_LOGI(TAG, "Audio task stopped");
}

// ════════════════════════════════════════════════════════════════════════════
// RX PATH: I2S read → deinterleave/decimate → mic_buffer + spk_ref_buffer
// ════════════════════════════════════════════════════════════════════════════
void I2SAudioDuplex::process_rx_path_(AudioTaskCtx &ctx) {
  if (!this->rx_handle_)
    return;

  size_t bytes_read;
  esp_err_t err = i2s_channel_read(this->rx_handle_, ctx.rx_buffer, ctx.rx_frame_bytes,
                                    &bytes_read, I2S_IO_TIMEOUT_MS);
  if (err != ESP_OK && err != ESP_ERR_TIMEOUT && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "i2s_channel_read failed: %s", esp_err_to_name(err));
    if (++ctx.consecutive_i2s_errors > 100) {
      ESP_LOGE(TAG, "Persistent I2S read errors (%d)", ctx.consecutive_i2s_errors);
      this->has_i2s_error_.store(true, std::memory_order_relaxed);
      this->duplex_running_.store(false, std::memory_order_relaxed);
    }
    return;
  }
  if (err != ESP_OK || bytes_read != ctx.rx_frame_bytes)
    return;

  ctx.consecutive_i2s_errors = 0;

  // Convert 32-bit I2S samples to 16-bit only when FIR strided does NOT handle it.
  // When ratio > 1, the FIR decimator reads 32-bit directly via process_strided_32.
  if (ctx.i2s_bps == 4 && ctx.ratio <= 1) {
    auto *src32 = reinterpret_cast<int32_t *>(ctx.rx_buffer);
    size_t total_i2s_samples = bytes_read / sizeof(int32_t);
    for (size_t i = 0; i < total_i2s_samples; i++) {
      ctx.rx_buffer[i] = static_cast<int16_t>(src32[i] >> 16);
    }
  }

  if (ctx.use_tdm_ref) {
    this->update_tdm_slot_levels_(ctx);
  }

  ctx.output_buffer = ctx.mic_buffer;  // Default: no AEC processing
  ctx.processor_input = ctx.mic_buffer;

#if SOC_I2S_SUPPORTS_TDM
  if (ctx.use_tdm_ref) {
    const uint8_t ts = ctx.tdm_total_slots;
    const bool dual_mic = ctx.processor_mic_channels > 1 && ctx.tdm_second_mic_slot >= 0;
    uint8_t ch_offsets[MC_FIR_MAX_CH];
    uint8_t num_mic_ch;
    if (dual_mic) {
      ch_offsets[0] = ctx.tdm_mic_slot;
      ch_offsets[1] = static_cast<uint8_t>(ctx.tdm_second_mic_slot);
      ch_offsets[2] = ctx.tdm_ref_slot;
      num_mic_ch = 2;
    } else {
      ch_offsets[0] = ctx.tdm_mic_slot;
      ch_offsets[1] = ctx.tdm_ref_slot;
      num_mic_ch = 1;
    }
    if (ctx.i2s_bps == 4) {
      auto *src32 = reinterpret_cast<const int32_t *>(ctx.rx_buffer);
      this->rx_decimator_.process_multi_32(src32, ctx.input_frame_size, ts, ch_offsets,
          dual_mic ? ctx.processor_mic_buffer : nullptr, ctx.mic_buffer,
          ctx.spk_ref_buffer, num_mic_ch);
    } else {
      this->rx_decimator_.process_multi(ctx.rx_buffer, ctx.input_frame_size, ts, ch_offsets,
          dual_mic ? ctx.processor_mic_buffer : nullptr, ctx.mic_buffer,
          ctx.spk_ref_buffer, num_mic_ch);
    }
    if (dual_mic) {
      ctx.processor_input = ctx.processor_mic_buffer;
    }
  } else
#endif
  if (ctx.use_stereo_aec_ref) {
    // Stereo: mic + ref via multi-channel FIR
    const uint8_t mi = ctx.ref_channel_right ? 0 : 1;
    const uint8_t ri = ctx.ref_channel_right ? 1 : 0;
    uint8_t ch_offsets[2] = {mi, ri};
    if (ctx.i2s_bps == 4) {
      auto *src32 = reinterpret_cast<const int32_t *>(ctx.rx_buffer);
      this->rx_decimator_.process_multi_32(src32, ctx.input_frame_size, 2, ch_offsets,
          nullptr, ctx.mic_buffer, ctx.spk_ref_buffer, 1);
    } else {
      this->rx_decimator_.process_multi(ctx.rx_buffer, ctx.input_frame_size, 2, ch_offsets,
          nullptr, ctx.mic_buffer, ctx.spk_ref_buffer, 1);
    }
  } else if (ctx.ratio > 1) {
    // Mono with decimation
    if (ctx.i2s_bps == 4) {
      auto *src32 = reinterpret_cast<const int32_t *>(ctx.rx_buffer);
      this->mic_decimator_.process_strided_32(src32, ctx.mic_buffer, ctx.input_frame_size, 1, 0);
    } else {
      this->mic_decimator_.process(ctx.rx_buffer, ctx.mic_buffer, ctx.bus_frame_size);
    }
  }
  // else: Mono without decimation: mic_buffer == rx_buffer (aliased), nothing to do

  // Fused loop: DC offset + mic attenuation in one pass.
  // For dual-mic: mic1 is in mic_buffer, mic2 is in processor_mic_buffer[i*2+1]
  // (both filled by the multi-channel FIR). Apply DC+atten on both, update in-place.
  const bool do_dc = ctx.correct_dc_offset;
  const bool do_atten = ctx.mic_attenuation != 1.0f;
  const bool dual_mic = ctx.processor_mic_channels > 1 && ctx.processor_mic_buffer != nullptr;

  if (do_dc || do_atten) {
    const float atten = ctx.mic_attenuation;
    for (size_t i = 0; i < ctx.input_frame_size; i++) {
      int16_t s1 = ctx.mic_buffer[i];

      if (do_dc) {
        int32_t inp = (int32_t) s1 << 16;
        int32_t out = inp - ctx.dc_prev_input + ctx.dc_prev_output - (ctx.dc_prev_output >> 10);
        ctx.dc_prev_input = inp;
        ctx.dc_prev_output = out;
        s1 = static_cast<int16_t>(out >> 16);
      }
      if (do_atten) {
        s1 = scale_sample(s1, atten);
      }
      ctx.mic_buffer[i] = s1;

      if (dual_mic) {
        // mic2 already in processor_mic_buffer interleaved by multi-channel FIR
        int16_t s2 = ctx.processor_mic_buffer[i * 2 + 1];
        if (do_dc) {
          int32_t inp2 = (int32_t) s2 << 16;
          int32_t out2 = inp2 - ctx.dc_prev_input_secondary + ctx.dc_prev_output_secondary -
                         (ctx.dc_prev_output_secondary >> 10);
          ctx.dc_prev_input_secondary = inp2;
          ctx.dc_prev_output_secondary = out2;
          s2 = static_cast<int16_t>(out2 >> 16);
        }
        if (do_atten) {
          s2 = scale_sample(s2, atten);
        }
        // Update both mic1 and mic2 in the interleaved buffer
        ctx.processor_mic_buffer[i * 2] = s1;
        ctx.processor_mic_buffer[i * 2 + 1] = s2;
      }
    }
  } else if (dual_mic) {
    // No DC or attenuation, but mic1 in processor_mic_buffer needs updating
    // (FIR wrote raw values, mic_buffer has same raw values)
    // processor_mic_buffer already has correct values from FIR, nothing to do
  }
}

void I2SAudioDuplex::update_tdm_slot_levels_(const AudioTaskCtx &ctx) {
  uint8_t enabled_slots[8];
  size_t enabled_count = 0;
  const uint8_t slot_limit = std::min<uint8_t>(ctx.tdm_total_slots, 8);
  for (uint8_t slot = 0; slot < slot_limit; slot++) {
    if (this->tdm_slot_level_sensor_enabled_[slot]) {
      enabled_slots[enabled_count++] = slot;
    }
  }
  if (enabled_count == 0) {
    return;
  }

  // Probe only every ~256 ms at 16 kHz / 512-sample cadence to keep overhead low.
  this->tdm_slot_level_divider_++;
  if (this->tdm_slot_level_divider_ < 8) {
    return;
  }
  this->tdm_slot_level_divider_ = 0;

  const size_t frame_samples = ctx.bus_frame_size;
  const size_t slot_stride = ctx.tdm_total_slots;
  for (size_t i = 0; i < enabled_count; i++) {
    uint8_t slot = enabled_slots[i];
    float dbfs;
    if (ctx.i2s_bps == 4 && ctx.ratio > 1) {
      // 32-bit mode with decimation: rx_buffer has not been converted to 16-bit
      auto *src32 = reinterpret_cast<const int32_t *>(ctx.rx_buffer);
      uint64_t sumsq = 0;
      for (size_t j = 0; j < frame_samples; j++) {
        int32_t s = src32[j * slot_stride + slot] >> 16;
        sumsq += static_cast<uint64_t>(s * s);
      }
      float mean = static_cast<float>(sumsq) / static_cast<float>(frame_samples);
      float rms = std::sqrt(mean) / 32768.0f;
      dbfs = (rms < 1e-6f) ? -120.0f : 20.0f * std::log10(rms);
    } else {
      dbfs = compute_rms_dbfs(ctx.rx_buffer + slot, frame_samples, slot_stride);
    }
    this->tdm_slot_level_dbfs_[slot].store(dbfs, std::memory_order_relaxed);
  }
}

// ════════════════════════════════════════════════════════════════════════════
// AEC + CALLBACKS: raw callbacks → AEC processing → gain → post callbacks
// ════════════════════════════════════════════════════════════════════════════
void I2SAudioDuplex::process_aec_and_callbacks_(AudioTaskCtx &ctx) {
  if (!this->rx_handle_ || ctx.output_buffer == nullptr)
    return;

  // Raw mic callbacks: pre-AEC audio for MWW
  if (ctx.mic_running && !this->raw_mic_callbacks_.empty()) {
    for (auto &callback : this->raw_mic_callbacks_) {
      callback((const uint8_t *) ctx.mic_buffer, ctx.input_frame_bytes);
    }
  }

#ifdef USE_AUDIO_PROCESSOR
#if SOC_I2S_SUPPORTS_TDM
  if (ctx.use_tdm_ref && ctx.processor_ready &&
      ctx.spk_ref_buffer != nullptr && ctx.aec_output != nullptr) {
    // TDM: hardware-synced reference, no speaker gating needed.
    // TDM analog ref already reflects DAC volume. No extra scaling on ref.
    this->processor_->process(ctx.processor_input, ctx.spk_ref_buffer, ctx.aec_output, ctx.processor_mic_channels);
    ctx.output_buffer = ctx.aec_output;
    ctx.current_output_frame_size = ctx.output_frame_size;
    ctx.current_output_frame_bytes = ctx.output_frame_bytes;
  } else
#endif
  if (!ctx.use_tdm_ref && ctx.processor_ready &&
      ctx.spk_ref_buffer != nullptr && ctx.aec_output != nullptr &&
      // Stereo AEC: reference is embedded in I2S RX (L=DAC loopback), always available.
      // Mono AEC: reference comes from speaker ring buffer, only valid when speaker is active.
      (ctx.use_stereo_aec_ref ||
       (ctx.speaker_running &&
        (ctx.now_ms - this->last_speaker_audio_ms_.load(std::memory_order_relaxed) <= AEC_ACTIVE_TIMEOUT_MS)))) {

    // Mono mode: get AEC reference (direct from TX or ring buffer)
    // Reference is post-volume PCM, no additional scaling (Espressif TYPE2 pattern).
    if (!ctx.use_stereo_aec_ref) {
      bool ref_filled = false;

      if (this->aec_ref_ring_buffer_) {
        // Ring buffer mode: read one frame, zero-fill if not enough data (TYPE2 timeout pattern)
        size_t ref_bytes = ctx.bus_frame_size * sizeof(int16_t);
        size_t avail = this->aec_ref_ring_buffer_->available();
        if (avail >= ref_bytes) {
          if (ctx.ratio > 1 && this->direct_aec_ref_ != nullptr) {
            // Read at bus rate into direct_aec_ref_ (temp), then decimate to output rate
            this->aec_ref_ring_buffer_->read(this->direct_aec_ref_, ref_bytes, 0);
            this->play_ref_decimator_.process(this->direct_aec_ref_, ctx.spk_ref_buffer, ctx.bus_frame_size);
          } else {
            // No decimation or no temp buffer: read directly into spk_ref_buffer
            size_t read_bytes = (ctx.ratio > 1) ? ctx.input_frame_bytes : ref_bytes;
            this->aec_ref_ring_buffer_->read(ctx.spk_ref_buffer, read_bytes, 0);
          }
          ref_filled = true;
        }
      } else if (this->direct_aec_ref_ != nullptr && this->direct_aec_ref_valid_) {
        // Previous frame mode: decimate from bus rate to output rate
        this->play_ref_decimator_.process(this->direct_aec_ref_, ctx.spk_ref_buffer, ctx.bus_frame_size);
        ref_filled = true;
      }

      if (!ref_filled) {
        memset(ctx.spk_ref_buffer, 0, ctx.input_frame_bytes);
      }
    }
    // Stereo mode: spk_ref_buffer already filled from deinterleave. No extra scaling.
    // TDM mode: spk_ref_buffer filled from TDM deinterleave. No extra scaling.

    this->processor_->process(ctx.processor_input, ctx.spk_ref_buffer, ctx.aec_output, ctx.processor_mic_channels);
    ctx.output_buffer = ctx.aec_output;
    ctx.current_output_frame_size = ctx.output_frame_size;
    ctx.current_output_frame_bytes = ctx.output_frame_bytes;
  }
#endif

  // Apply mic gain (snapshot value)
  if (ctx.mic_gain != 1.0f) {
    for (size_t i = 0; i < ctx.current_output_frame_size; i++) {
      ctx.output_buffer[i] = scale_sample(ctx.output_buffer[i], ctx.mic_gain);
    }
  }

  // Post-AEC callbacks (VA/STT)
  if (ctx.mic_running) {
    for (auto &callback : this->mic_callbacks_) {
      callback((const uint8_t *) ctx.output_buffer, ctx.current_output_frame_bytes);
    }
  }
}

// ════════════════════════════════════════════════════════════════════════════
// TX PATH: ring buffer read → volume → format expand → I2S write
// ════════════════════════════════════════════════════════════════════════════
void I2SAudioDuplex::process_tx_path_(AudioTaskCtx &ctx) {
  if (!this->tx_handle_)
    return;

  if (ctx.speaker_running) {
    ctx.speaker_got = this->speaker_buffer_->read((void *) ctx.spk_buffer, ctx.bus_frame_bytes, 0);
    size_t got = ctx.speaker_got;
    ctx.speaker_underrun = (got == 0 && !ctx.speaker_paused);

    if (ctx.speaker_paused) {
      memset(ctx.spk_buffer, 0, ctx.bus_frame_bytes);
    } else if (got > 0) {
      if (ctx.speaker_volume != 1.0f) {
        size_t got_samples = got / sizeof(int16_t);
        for (size_t i = 0; i < got_samples; i++) {
          ctx.spk_buffer[i] = scale_sample(ctx.spk_buffer[i], ctx.speaker_volume);
        }
      }
      if (got < ctx.bus_frame_bytes) {
        memset(((uint8_t *) ctx.spk_buffer) + got, 0, ctx.bus_frame_bytes - got);
      }
    } else {
      memset(ctx.spk_buffer, 0, ctx.bus_frame_bytes);
    }
  } else {
    memset(ctx.spk_buffer, 0, ctx.bus_frame_bytes);
    ctx.speaker_got = 0;
  }

  // Save post-volume TX data as AEC reference (skip if processor is off)
#ifdef USE_AUDIO_PROCESSOR
  if (this->aec_ref_ring_buffer_ && ctx.processor_enabled) {
    // Ring buffer mode: write post-volume PCM for TYPE2-style reference
    if (ctx.speaker_running && !ctx.speaker_paused) {
      size_t frame_bytes = ctx.bus_frame_size * sizeof(int16_t);
      size_t written = this->aec_ref_ring_buffer_->write(
          (void *) ctx.spk_buffer, frame_bytes);
      if (written == 0) {
        // Buffer full: discard one frame to make room (deterministic backlog trim)
        // Reuse direct_aec_ref_ as discard buffer (same size: bus_frame_size samples)
        if (this->direct_aec_ref_) {
          this->aec_ref_ring_buffer_->read(this->direct_aec_ref_, frame_bytes, 0);
        } else {
          this->aec_ref_ring_buffer_->reset();
        }
        this->aec_ref_ring_buffer_->write((void *) ctx.spk_buffer, frame_bytes);
      }
    }
  } else if (this->direct_aec_ref_ != nullptr && ctx.processor_enabled) {
    // Previous frame mode: save for next iteration
    memcpy(this->direct_aec_ref_, ctx.spk_buffer, ctx.bus_frame_size * sizeof(int16_t));
    this->direct_aec_ref_valid_ = ctx.speaker_running && !ctx.speaker_paused;
  }
#endif

  // Prepare TX: format expansion + TDM interleave
  const void *tx_data;
  size_t tx_bytes;
#if SOC_I2S_SUPPORTS_TDM
  if (ctx.use_tdm_ref && ctx.tdm_tx_buffer != nullptr) {
    if (ctx.i2s_bps == 4) {
      auto *tdm32 = reinterpret_cast<int32_t *>(ctx.tdm_tx_buffer);
      memset(tdm32, 0, ctx.tdm_tx_frame_bytes);
      for (size_t i = 0; i < ctx.bus_frame_size; i++) {
        tdm32[i * ctx.tdm_total_slots] = static_cast<int32_t>(ctx.spk_buffer[i]) << 16;
      }
    } else {
      memset(ctx.tdm_tx_buffer, 0, ctx.tdm_tx_frame_bytes);
      for (size_t i = 0; i < ctx.bus_frame_size; i++) {
        ctx.tdm_tx_buffer[i * ctx.tdm_total_slots] = ctx.spk_buffer[i];
      }
    }
    tx_data = ctx.tdm_tx_buffer;
    tx_bytes = ctx.tdm_tx_frame_bytes;
  } else
#endif
  {
    size_t total_tx_samples = ctx.bus_frame_size * ctx.num_ch;
    if (ctx.num_ch == 2 && ctx.i2s_bps == 4) {
      // Fused mono->stereo + 16->32 in one backward pass
      auto *dst32 = reinterpret_cast<int32_t *>(ctx.spk_buffer);
      for (int i = static_cast<int>(ctx.bus_frame_size) - 1; i >= 0; i--) {
        int32_t s = static_cast<int32_t>(ctx.spk_buffer[i]) << 16;
        dst32[i * 2 + 1] = s;
        dst32[i * 2] = s;
      }
    } else if (ctx.num_ch == 2) {
      for (int i = static_cast<int>(ctx.bus_frame_size) - 1; i >= 0; i--) {
        ctx.spk_buffer[i * 2 + 1] = ctx.spk_buffer[i];
        ctx.spk_buffer[i * 2] = ctx.spk_buffer[i];
      }
    } else if (ctx.i2s_bps == 4) {
      auto *dst32 = reinterpret_cast<int32_t *>(ctx.spk_buffer);
      for (int i = static_cast<int>(total_tx_samples) - 1; i >= 0; i--) {
        dst32[i] = static_cast<int32_t>(ctx.spk_buffer[i]) << 16;
      }
    }
    tx_data = ctx.spk_buffer;
    tx_bytes = total_tx_samples * ctx.i2s_bps;
  }

  size_t bytes_written;
  esp_err_t err = i2s_channel_write(this->tx_handle_, tx_data, tx_bytes, &bytes_written, I2S_IO_TIMEOUT_MS);
  if (err != ESP_OK && err != ESP_ERR_TIMEOUT && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "i2s_channel_write failed: %s", esp_err_to_name(err));
    if (++ctx.consecutive_i2s_errors > 100) {
      ESP_LOGE(TAG, "Persistent I2S write errors (%d)", ctx.consecutive_i2s_errors);
      this->has_i2s_error_.store(true, std::memory_order_relaxed);
      this->duplex_running_.store(false, std::memory_order_relaxed);
    }
  } else if (err == ESP_OK) {
    ctx.consecutive_i2s_errors = 0;
  }

  // Report frames actually consumed from the ring buffer (not silence/pad frames).
  // Using got (ring buffer read) instead of bytes_written (I2S output) prevents
  // counting silence frames as "played" during underruns.
  if (err == ESP_OK && ctx.speaker_got > 0 && !this->speaker_output_callbacks_.empty()) {
    uint32_t frames_played = ctx.speaker_got / sizeof(int16_t);
    int64_t timestamp = esp_timer_get_time();
    for (auto &cb : this->speaker_output_callbacks_) {
      cb(frames_played, timestamp);
    }
  }
}

size_t I2SAudioDuplex::get_speaker_buffer_available() const {
  if (!this->speaker_buffer_) return 0;
  return this->speaker_buffer_->available();
}

size_t I2SAudioDuplex::get_speaker_buffer_size() const {
  return this->speaker_buffer_size_;
}

}  // namespace i2s_audio_duplex
}  // namespace esphome

#endif  // USE_ESP32
