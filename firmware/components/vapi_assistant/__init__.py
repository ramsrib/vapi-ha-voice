"""Vapi voice assistant for ESPHome (ESP-IDF only).

Opens a Vapi call over the `vapi.websocket` transport and pipes raw PCM between
an ESPHome microphone source and speaker. Replaces the `voice_assistant`
component's role without replacing anything else on the device.
"""

import esphome.codegen as cg
from esphome.components import esp32, microphone, speaker
import esphome.config_validation as cv
from esphome import automation
from esphome.const import CONF_ID, CONF_MICROPHONE, CONF_SPEAKER, CONF_TRIGGER_ID

CODEOWNERS = ["@ramsrib"]
DEPENDENCIES = ["microphone", "speaker", "network", "esp32"]

vapi_assistant_ns = cg.esphome_ns.namespace("vapi_assistant")
VapiAssistant = vapi_assistant_ns.class_("VapiAssistant", cg.Component)

StartCallAction = vapi_assistant_ns.class_("StartCallAction", automation.Action)
StopCallAction = vapi_assistant_ns.class_("StopCallAction", automation.Action)
StartFromWakeWordAction = vapi_assistant_ns.class_(
    "StartFromWakeWordAction", automation.Action
)
IsActiveCondition = vapi_assistant_ns.class_("IsActiveCondition", automation.Condition)

StartTrigger = vapi_assistant_ns.class_("StartTrigger", automation.Trigger.template())
EndTrigger = vapi_assistant_ns.class_("EndTrigger", automation.Trigger.template())
ErrorTrigger = vapi_assistant_ns.class_("ErrorTrigger", automation.Trigger.template(cg.std_string))

CONF_API_KEY = "api_key"
CONF_API_URL = "api_url"
CONF_ASSISTANT_ID = "assistant_id"
CONF_FIRST_MESSAGE = "first_message"
CONF_SAMPLE_RATE = "sample_rate"
CONF_FRAME_MS = "frame_ms"
CONF_VOLUME = "volume"
CONF_SILENCE_TIMEOUT = "silence_timeout"
CONF_WAKE_SILENCE_TIMEOUT = "wake_silence_timeout"
CONF_MAX_DURATION = "max_duration"
CONF_STOP_SPEAKING_NUM_WORDS = "stop_speaking_num_words"
CONF_SYSTEM_PROMPT = "system_prompt"
CONF_MODEL_PROVIDER = "model_provider"
CONF_MODEL = "model"
CONF_VOICE_PROVIDER = "voice_provider"
CONF_VOICE_ID = "voice_id"
CONF_TRANSCRIBER_PROVIDER = "transcriber_provider"
CONF_TRANSCRIBER_MODEL = "transcriber_model"
CONF_VOICE_MODEL = "voice_model"
CONF_END_CALL_PHRASES = "end_call_phrases"
CONF_END_CALL_MESSAGE = "end_call_message"
CONF_STABILITY = "stability"
CONF_SIMILARITY_BOOST = "similarity_boost"
CONF_STYLE = "style"
CONF_USE_SPEAKER_BOOST = "use_speaker_boost"
CONF_ON_START = "on_start"
CONF_ON_END = "on_end"
CONF_ON_ERROR = "on_error"

# Pinned rather than floating: this is the transport, and a surprise major
# version here would break audio in a way that looks like a hardware fault.
ESP_WEBSOCKET_CLIENT_VERSION = "1.5.0"


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(VapiAssistant),
            # One channel only. The two microphone channels on this board are
            # different taps into the XMOS pipeline, not left and right, so
            # mixing them would average two differently-processed signals.
            cv.Required(CONF_MICROPHONE): microphone.microphone_source_schema(
                min_channels=1, max_channels=1
            ),
            cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
            cv.Required(CONF_API_KEY): cv.string_strict,
            cv.Optional(CONF_ASSISTANT_ID, default=""): cv.string_strict,
            cv.Optional(CONF_API_URL, default="https://api.vapi.ai"): cv.url,
            cv.Optional(CONF_FIRST_MESSAGE, default=""): cv.string,
            # Vapi's websocket transport carries raw PCM; 16 kHz mono is what
            # both ends of this board already speak.
            cv.Optional(CONF_SAMPLE_RATE, default=16000): cv.one_of(16000, int=True),
            cv.Optional(CONF_FRAME_MS, default=20): cv.int_range(min=10, max=60),
            cv.Optional(CONF_VOLUME, default=0.6): cv.float_range(min=0.0, max=1.0),
            cv.Optional(CONF_SILENCE_TIMEOUT, default="300s"): cv.All(
                cv.positive_time_period_seconds, cv.Range(min=cv.TimePeriod(seconds=10), max=cv.TimePeriod(seconds=3600))
            ),
            # Short, because a wake word can fire on the television.
            cv.Optional(CONF_WAKE_SILENCE_TIMEOUT, default="20s"): cv.All(
                cv.positive_time_period_seconds,
                cv.Range(min=cv.TimePeriod(seconds=10), max=cv.TimePeriod(seconds=3600)),
            ),
            cv.Optional(CONF_MAX_DURATION, default="1800s"): cv.All(
                cv.positive_time_period_seconds, cv.Range(min=cv.TimePeriod(seconds=10), max=cv.TimePeriod(seconds=43200))
            ),
            cv.Optional(CONF_STOP_SPEAKING_NUM_WORDS, default=2): cv.int_range(min=0, max=10),
            # Transient assistant. Used whenever assistant_id is absent, so the
            # device never depends on dashboard state it cannot see.
            cv.Optional(CONF_SYSTEM_PROMPT, default=""): cv.string,
            cv.Optional(CONF_MODEL_PROVIDER, default="anthropic"): cv.string_strict,
            cv.Optional(CONF_MODEL, default="claude-haiku-4-5-20251001"): cv.string_strict,
            cv.Optional(CONF_VOICE_PROVIDER, default="11labs"): cv.string_strict,
            cv.Optional(CONF_VOICE_ID, default="jsCqWAovK2LkecY7zXl4"): cv.string_strict,
            cv.Optional(CONF_TRANSCRIBER_PROVIDER, default="deepgram"): cv.string_strict,
            cv.Optional(CONF_TRANSCRIBER_MODEL, default="nova-3"): cv.string_strict,
            cv.Optional(CONF_VOICE_MODEL, default="eleven_turbo_v2_5"): cv.string_strict,
            cv.Optional(CONF_END_CALL_PHRASES, default=[]): cv.ensure_list(cv.string),
            cv.Optional(CONF_END_CALL_MESSAGE, default=""): cv.string,
            cv.Optional(CONF_STABILITY, default=0.3): cv.float_range(min=0.0, max=1.0),
            cv.Optional(CONF_SIMILARITY_BOOST, default=0.75): cv.float_range(min=0.0, max=1.0),
            cv.Optional(CONF_STYLE, default=0.6): cv.float_range(min=0.0, max=1.0),
            cv.Optional(CONF_USE_SPEAKER_BOOST, default=True): cv.boolean,
            cv.Optional(CONF_ON_START): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(StartTrigger)}
            ),
            cv.Optional(CONF_ON_END): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(EndTrigger)}
            ),
            cv.Optional(CONF_ON_ERROR): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ErrorTrigger)}
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_with_framework("esp-idf"),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # passive=True: micro_wake_word owns the microphone and must keep it
    # running to hear the wake word. A non-passive source here would stop the
    # *shared* hardware microphone when a call ends, silently killing the wake
    # word after the first call.
    mic_source = await microphone.microphone_source_to_code(
        config[CONF_MICROPHONE], passive=True
    )
    cg.add(var.set_microphone_source(mic_source))

    spk = await cg.get_variable(config[CONF_SPEAKER])
    cg.add(var.set_speaker(spk))

    cg.add(var.set_api_key(config[CONF_API_KEY]))
    cg.add(var.set_api_url(config[CONF_API_URL]))
    cg.add(var.set_assistant_id(config[CONF_ASSISTANT_ID]))
    cg.add(var.set_first_message(config[CONF_FIRST_MESSAGE]))
    cg.add(var.set_sample_rate(config[CONF_SAMPLE_RATE]))
    cg.add(var.set_frame_ms(config[CONF_FRAME_MS]))
    cg.add(var.set_volume(config[CONF_VOLUME]))
    cg.add(var.set_silence_timeout(int(config[CONF_SILENCE_TIMEOUT].total_seconds)))
    cg.add(var.set_wake_silence_timeout(int(config[CONF_WAKE_SILENCE_TIMEOUT].total_seconds)))
    cg.add(var.set_max_duration(int(config[CONF_MAX_DURATION].total_seconds)))
    cg.add(var.set_stop_speaking_num_words(config[CONF_STOP_SPEAKING_NUM_WORDS]))
    cg.add(var.set_system_prompt(config[CONF_SYSTEM_PROMPT]))
    cg.add(var.set_model_provider(config[CONF_MODEL_PROVIDER]))
    cg.add(var.set_model(config[CONF_MODEL]))
    cg.add(var.set_voice_provider(config[CONF_VOICE_PROVIDER]))
    cg.add(var.set_voice_id(config[CONF_VOICE_ID]))
    cg.add(var.set_transcriber_provider(config[CONF_TRANSCRIBER_PROVIDER]))
    cg.add(var.set_transcriber_model(config[CONF_TRANSCRIBER_MODEL]))
    cg.add(var.set_voice_model(config[CONF_VOICE_MODEL]))
    for phrase in config[CONF_END_CALL_PHRASES]:
        cg.add(var.add_end_call_phrase(phrase))
    cg.add(var.set_end_call_message(config[CONF_END_CALL_MESSAGE]))
    cg.add(var.set_stability(config[CONF_STABILITY]))
    cg.add(var.set_similarity_boost(config[CONF_SIMILARITY_BOOST]))
    cg.add(var.set_style(config[CONF_STYLE]))
    cg.add(var.set_use_speaker_boost(config[CONF_USE_SPEAKER_BOOST]))

    for conf in config.get(CONF_ON_START, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
    for conf in config.get(CONF_ON_END, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
    for conf in config.get(CONF_ON_ERROR, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "error")], conf)

    esp32.add_idf_component(
        name="espressif/esp_websocket_client", ref=ESP_WEBSOCKET_CLIENT_VERSION
    )


VAPI_ACTION_SCHEMA = automation.maybe_simple_id(
    {cv.GenerateID(): cv.use_id(VapiAssistant)}
)


@automation.register_action("vapi_assistant.start", StartCallAction, VAPI_ACTION_SCHEMA)
@automation.register_action(
    "vapi_assistant.start_from_wake_word", StartFromWakeWordAction, VAPI_ACTION_SCHEMA
)
@automation.register_action("vapi_assistant.stop", StopCallAction, VAPI_ACTION_SCHEMA)
async def vapi_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_condition(
    "vapi_assistant.is_active", IsActiveCondition, VAPI_ACTION_SCHEMA
)
async def vapi_is_active_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
