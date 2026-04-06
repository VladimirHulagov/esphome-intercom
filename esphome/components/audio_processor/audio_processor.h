#pragma once
#include <cstdint>
#include <cstddef>

namespace esphome {

/// Audio processing features that can be queried and toggled.
enum class AudioFeature : uint8_t {
  AEC = 0,  // Acoustic Echo Cancellation
  NS,       // Noise Suppression
  VAD,      // Voice Activity Detection
  AGC,      // Automatic Gain Control
  SE,       // Speech Enhancement (beamforming, requires 2+ mics)
};

/// How a feature can be controlled at runtime.
enum class FeatureControl : uint8_t {
  NOT_SUPPORTED,     // feature not available in this processor
  BOOT_ONLY,         // set at boot, cannot change at runtime
  RESTART_REQUIRED,  // toggle requires processor reinit (~70ms gap)
  LIVE_TOGGLE,       // toggle is immediate, no audio gap
};

/// Describes the frame layout expected by process().
struct FrameSpec {
  uint32_t sample_rate{16000};
  uint8_t mic_channels{1};     // number of mic channels in input
  uint8_t ref_channels{1};     // number of reference channels (0 or 1)
  size_t input_samples{512};   // per-channel samples expected by process()
  size_t output_samples{512};  // per-channel samples produced by process()
};

/// Snapshot of processor telemetry (thread-safe reads via atomics in impl).
struct ProcessorTelemetry {
  bool voice_present{false};
  float input_volume_dbfs{-120.0f};
  float output_rms_dbfs{-120.0f};
  float ringbuf_free_pct{1.0f};
  uint32_t glitch_count{0};
  uint32_t frame_count{0};
};

/// Abstract audio processor interface.
///
/// Implementations: EspAec (standalone AEC), EspAfe (full AFE pipeline).
/// Consumers: i2s_audio_duplex, intercom_api.
class AudioProcessor {
 public:
  virtual ~AudioProcessor() = default;

  virtual bool is_initialized() const = 0;

  /// Frame layout this processor expects and produces.
  virtual FrameSpec frame_spec() const = 0;

  /// Process one frame.
  /// @param in_mic  mic input, frame_spec().input_samples per channel
  /// @param in_ref  reference input (speaker loopback), same size. May be nullptr.
  /// @param out     output buffer, frame_spec().output_samples
  /// @return true if processed by DSP, false if passthrough (unprocessed copy)
  virtual bool process(const int16_t *in_mic, const int16_t *in_ref, int16_t *out) = 0;

  /// Query how a feature can be controlled.
  virtual FeatureControl feature_control(AudioFeature feature) const = 0;

  /// Toggle a feature on or off. Check feature_control() first.
  /// @return true if the change was applied successfully
  virtual bool set_feature(AudioFeature feature, bool enabled) = 0;

  /// Current telemetry snapshot.
  virtual ProcessorTelemetry telemetry() const = 0;

  /// Reconfigure the processor (e.g., switch SR/VC mode). Requires audio stop.
  /// @return true if reconfiguration succeeded
  virtual bool reconfigure(int type, int mode) = 0;
};

}  // namespace esphome
