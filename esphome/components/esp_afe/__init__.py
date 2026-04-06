import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import CONF_ID, CONF_MODE, CONF_TYPE
from esphome.core import CORE
from esphome.components.esp32 import add_idf_component

CODEOWNERS = ["@n-IA-hane"]
DEPENDENCIES = ["esp32"]

_SUPPORTED_VARIANTS = ("ESP32S3", "ESP32P4")


def _validate_esp32_variant(config):
    if CORE.is_esp32:
        import esphome.components.esp32 as esp32

        variant = esp32.get_esp32_variant()
        if variant not in _SUPPORTED_VARIANTS:
            raise cv.Invalid(
                f"esp_afe requires {' or '.join(_SUPPORTED_VARIANTS)}, got {variant}"
            )
    return config


AecProcessor = cg.esphome_ns.class_("AecProcessor")

esp_afe_ns = cg.esphome_ns.namespace("esp_afe")
EspAfe = esp_afe_ns.class_("EspAfe", cg.Component, AecProcessor)
SetModeAction = esp_afe_ns.class_("SetModeAction", automation.Action)

CONF_ESP_AFE_ID = "esp_afe_id"
CONF_MIC_NUM = "mic_num"
CONF_AEC_ENABLED = "aec_enabled"
CONF_AEC_FILTER_LENGTH = "aec_filter_length"
CONF_NS_ENABLED = "ns_enabled"
CONF_VAD_ENABLED = "vad_enabled"
CONF_AGC_ENABLED = "agc_enabled"
CONF_AGC_COMPRESSION_GAIN = "agc_compression_gain"
CONF_AGC_TARGET_LEVEL = "agc_target_level"
CONF_TASK_CORE = "task_core"
CONF_TASK_PRIORITY = "task_priority"
CONF_RINGBUF_SIZE = "ringbuf_size"

AFE_TYPES = {
    "sr": 0,  # AFE_TYPE_SR: speech recognition, linear AEC
    "vc": 1,  # AFE_TYPE_VC: voice communication, nonlinear AEC
}

AFE_MODES = {
    "low_cost": 0,   # AFE_MODE_LOW_COST
    "high_perf": 1,  # AFE_MODE_HIGH_PERF
}

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(EspAfe),
            cv.Optional(CONF_TYPE, default="sr"): cv.enum(AFE_TYPES, lower=True),
            cv.Optional(CONF_MODE, default="low_cost"): cv.enum(AFE_MODES, lower=True),
            cv.Optional(CONF_MIC_NUM, default=1): cv.int_range(min=1, max=2),
            cv.Optional(CONF_AEC_ENABLED, default=True): cv.boolean,
            cv.Optional(CONF_AEC_FILTER_LENGTH, default=4): cv.int_range(min=1, max=8),
            cv.Optional(CONF_NS_ENABLED, default=True): cv.boolean,
            cv.Optional(CONF_VAD_ENABLED, default=False): cv.boolean,
            cv.Optional(CONF_AGC_ENABLED, default=True): cv.boolean,
            cv.Optional(CONF_AGC_COMPRESSION_GAIN, default=9): cv.int_range(min=0, max=30),
            cv.Optional(CONF_AGC_TARGET_LEVEL, default=3): cv.int_range(min=0, max=31),
            cv.Optional(CONF_TASK_CORE, default=1): cv.int_range(min=0, max=1),
            cv.Optional(CONF_TASK_PRIORITY, default=5): cv.int_range(min=1, max=24),
            cv.Optional(CONF_RINGBUF_SIZE, default=8): cv.int_range(min=2, max=32),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_esp32_variant,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_afe_type(config[CONF_TYPE]))
    cg.add(var.set_afe_mode(config[CONF_MODE]))
    cg.add(var.set_mic_num(config[CONF_MIC_NUM]))
    cg.add(var.set_aec_enabled(config[CONF_AEC_ENABLED]))
    cg.add(var.set_aec_filter_length(config[CONF_AEC_FILTER_LENGTH]))
    cg.add(var.set_ns_enabled(config[CONF_NS_ENABLED]))
    cg.add(var.set_vad_enabled(config[CONF_VAD_ENABLED]))
    cg.add(var.set_agc_enabled(config[CONF_AGC_ENABLED]))
    cg.add(var.set_agc_compression_gain(config[CONF_AGC_COMPRESSION_GAIN]))
    cg.add(var.set_agc_target_level(config[CONF_AGC_TARGET_LEVEL]))
    cg.add(var.set_task_core(config[CONF_TASK_CORE]))
    cg.add(var.set_task_priority(config[CONF_TASK_PRIORITY]))
    cg.add(var.set_ringbuf_size(config[CONF_RINGBUF_SIZE]))

    # Required for i2s_audio_duplex AEC code path (#ifdef USE_ESP_AEC)
    cg.add_define("USE_ESP_AEC")
    cg.add_define("USE_ESP_AFE")

    add_idf_component(name="espressif/esp-sr", ref="~2.3.0")


@automation.register_action(
    "esp_afe.set_mode",
    SetModeAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(EspAfe),
            cv.Required(CONF_MODE): cv.templatable(cv.string),
        }
    ),
    synchronous=True,
)
async def set_mode_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    parent = await cg.get_variable(config[CONF_ID])
    cg.add(var.set_parent(parent))
    templ = await cg.templatable(config[CONF_MODE], args, cg.std_string)
    cg.add(var.set_mode(templ))
    return var
