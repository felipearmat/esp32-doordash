import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import output
from esphome.const import CONF_ID

CODEOWNERS = []
DEPENDENCIES = ["wifi", "esp32"]

intercom_ns = cg.esphome_ns.namespace("intercom_remote")
IntercomRemote = intercom_ns.class_("IntercomRemote", cg.Component)

CONF_ENDPOINT = "endpoint"
CONF_GATEWAY_HOST = "gateway_host"
CONF_GATEWAY_PORT = "gateway_port"
CONF_SAMPLE_RATE = "sample_rate"
CONF_FRAME_MS = "frame_ms"
CONF_MIC_BIAS_MV = "mic_bias_mv"
CONF_INPUT_GAIN = "input_gain"
CONF_OUTPUT_GAIN = "output_gain"
CONF_AMP_ENABLE = "amp_enable"
CONF_AMP_WARMUP_MS = "amp_warmup_ms"
CONF_AMP_IDLE_TIMEOUT_MS = "amp_idle_timeout_ms"
CONF_MICROPHONE_PIN = "microphone_pin"
CONF_DAC_PIN = "dac_pin"

# No duplex_mode, vox_*, nlms_*, i2s_* here — this component only does raw
# capture/playback. All the audio intelligence (when to talk, echo, noise)
# lives on the central gateway. See the project README.

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(IntercomRemote),
    # "A" is always the outdoor module. Internal modules use B, C, D... — one
    # letter per physical module, with no practical limit beyond the
    # alphabet (and the UDP port scheme, see server.py: port = 6056 + (letter - 'A')).
    cv.Required(CONF_ENDPOINT): cv.All(cv.string, cv.Length(min=1, max=1), cv.upper),
    cv.Required(CONF_MICROPHONE_PIN): cv.int_range(min=32, max=39),
    cv.Required(CONF_DAC_PIN): cv.one_of(25, 26, int=True),
    cv.Required(CONF_GATEWAY_HOST): cv.string,
    cv.Optional(CONF_GATEWAY_PORT, default=6055): cv.port,
    cv.Optional(CONF_SAMPLE_RATE, default=8000): cv.one_of(8000, 16000, int=True),
    cv.Optional(CONF_FRAME_MS, default=20): cv.one_of(10, 20, 30, int=True),
    cv.Optional(CONF_MIC_BIAS_MV, default=1650): cv.int_range(min=0, max=3300),
    cv.Optional(CONF_INPUT_GAIN, default=3.0): cv.float_range(min=0.1, max=20.0),
    cv.Optional(CONF_OUTPUT_GAIN, default=0.7): cv.float_range(min=0.0, max=1.0),
    cv.Optional(CONF_AMP_ENABLE): cv.use_id(output.BinaryOutput),
    cv.Optional(CONF_AMP_WARMUP_MS, default=10): cv.int_range(min=0, max=200),
    cv.Optional(CONF_AMP_IDLE_TIMEOUT_MS, default=400): cv.int_range(min=50, max=5000),
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_endpoint(config[CONF_ENDPOINT]))
    cg.add(var.set_microphone_pin(config[CONF_MICROPHONE_PIN]))
    cg.add(var.set_dac_pin(config[CONF_DAC_PIN]))
    cg.add(var.set_gateway_host(config[CONF_GATEWAY_HOST]))
    cg.add(var.set_gateway_port(config[CONF_GATEWAY_PORT]))
    cg.add(var.set_sample_rate(config[CONF_SAMPLE_RATE]))
    cg.add(var.set_frame_ms(config[CONF_FRAME_MS]))
    cg.add(var.set_mic_bias_mv(config[CONF_MIC_BIAS_MV]))
    cg.add(var.set_input_gain(config[CONF_INPUT_GAIN]))
    cg.add(var.set_output_gain(config[CONF_OUTPUT_GAIN]))
    cg.add(var.set_amp_warmup_ms(config[CONF_AMP_WARMUP_MS]))
    cg.add(var.set_amp_idle_timeout_ms(config[CONF_AMP_IDLE_TIMEOUT_MS]))

    if CONF_AMP_ENABLE in config:
        amp = await cg.get_variable(config[CONF_AMP_ENABLE])
        cg.add(var.set_amp_enable(amp))
