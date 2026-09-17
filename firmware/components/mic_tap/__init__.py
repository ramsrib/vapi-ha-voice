"""Stream the microphone to a host on the LAN, for collecting training data.

This exists because of a specific problem: the audio the wake-word engine hears
cannot be obtained any other way. The wake word fires *before* a call opens, so
the utterance never reaches Vapi and is not in any call recording — and the
engine listens to a different XMOS tap than Vapi does anyway (channel 1,
noise-suppressed without AGC, plus its own gain), so even call audio is the
wrong signal.

Point this at the same microphone source `micro_wake_word` uses and it captures
exactly the samples the model is scored on.

Debug/collection tool. It streams room audio unencrypted over the LAN, so it is
off until something calls `mic_tap.start` and it does not persist across reboots.
"""

import esphome.codegen as cg
from esphome.components import microphone
import esphome.config_validation as cv
from esphome import automation
from esphome.const import CONF_ID, CONF_MICROPHONE, CONF_PORT

CONF_LABEL = "label"

CODEOWNERS = ["@ramsrib"]
DEPENDENCIES = ["microphone", "network"]

mic_tap_ns = cg.esphome_ns.namespace("mic_tap")
MicTap = mic_tap_ns.class_("MicTap", cg.Component)

StartAction = mic_tap_ns.class_("StartAction", automation.Action)
StopAction = mic_tap_ns.class_("StopAction", automation.Action)
MarkAction = mic_tap_ns.class_("MarkAction", automation.Action)
IsStreamingCondition = mic_tap_ns.class_("IsStreamingCondition", automation.Condition)

CONF_HOST = "host"

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(MicTap),
            # Mirror the wake word's source exactly — same channel, same gain —
            # or the captured audio is not what the model will be scored on.
            cv.Required(CONF_MICROPHONE): microphone.microphone_source_schema(
                min_bits_per_sample=16,
                max_bits_per_sample=16,
                min_channels=1,
                max_channels=1,
            ),
            cv.Required(CONF_HOST): cv.string,
            cv.Optional(CONF_PORT, default=9000): cv.port,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_with_framework("esp-idf"),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # passive=True is not optional here. A non-passive source stops the *shared*
    # hardware microphone on stop(), which would kill the wake word and any live
    # call the moment capture ended. Passive sources receive audio whenever the
    # hardware is running and never start or stop it.
    mic_source = await microphone.microphone_source_to_code(
        config[CONF_MICROPHONE], passive=True
    )
    cg.add(var.set_microphone_source(mic_source))
    cg.add(var.set_host(config[CONF_HOST]))
    cg.add(var.set_port(config[CONF_PORT]))


@automation.register_action(
    "mic_tap.start", StartAction, cv.Schema({cv.GenerateID(): cv.use_id(MicTap)})
)
async def mic_tap_start_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_action(
    "mic_tap.stop", StopAction, cv.Schema({cv.GenerateID(): cv.use_id(MicTap)})
)
async def mic_tap_stop_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)


@automation.register_condition(
    "mic_tap.is_streaming",
    IsStreamingCondition,
    cv.Schema({cv.GenerateID(): cv.use_id(MicTap)}),
)
async def mic_tap_is_streaming_to_code(config, condition_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(condition_id, template_arg, parent)


@automation.register_action(
    "mic_tap.mark",
    MarkAction,
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(MicTap),
            cv.Required(CONF_LABEL): cv.templatable(cv.string),
        }
    ),
)
async def mic_tap_mark_to_code(config, action_id, template_arg, args):
    """Stamp a label into the stream at the current instant.

    Wire this to the wake-word detection trigger and every capture session comes
    back self-labelled: the marked moments are the hits, and the attempts
    between them are the misses. Those misses are the point — they are the only
    recording anyone has of the model failing, and they cannot be reconstructed
    after the fact.
    """
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    templ = await cg.templatable(config[CONF_LABEL], args, cg.std_string)
    cg.add(var.set_label(templ))
    return var
