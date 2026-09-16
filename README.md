# Vapi on HA Voice

Turn a **Home Assistant Voice: Preview Edition** into a standalone
[Vapi](https://vapi.ai) voice assistant. Press the centre button or say the wake
word, talk, and the device holds the Vapi call itself — no Home Assistant, no
laptop, no bridge in the middle.

It is an ESPHome external component plus a device config, so everything that
makes the hardware good is kept: the XMOS XU316 audio pipeline, the on-device
wake word, the LED ring, the dial and the mute switch.

> Working on hardware. Audio flows both ways, Vapi transcribes the microphone,
> and the assistant is audible through the speaker.

## Why this hardware

The Voice PE has two processors. The ESP32-S3 runs ESPHome; an **XMOS XU316**
owns the microphone array and the speaker, running hardware acoustic echo
cancellation, beamforming, noise suppression and automatic gain control.

That matters more than it sounds. On boards with two separate codecs there is no
echo reference, so on-device AEC is impossible and echo has to be *gated* —
which costs barge-in. Here the XMOS cancels echo against the real speaker signal
and hands the ESP32 clean audio over plain I2S, after two I2C writes.

It also happens to hand back **16 kHz mono**, which is bit-for-bit the format
Vapi's `vapi.websocket` transport carries. Nothing is transcoded inbound; a
`resampler` speaker takes Vapi's 16 kHz up to the 48 kHz the hardware runs at.

## What you need

- A Home Assistant Voice: Preview Edition
- A **private** Vapi API key — this calls `POST /call`, which a public key cannot do
- [ESPHome](https://esphome.io) (`uvx --from esphome esphome ...` works without installing)

## Setup

```sh
git clone https://github.com/ramsrib/vapi-ha-voice.git && cd vapi-ha-voice/firmware
cp secrets.yaml.example secrets.yaml     # fill in WiFi and your Vapi key
uvx --from esphome esphome run vapi-voice.yaml --device /dev/cu.usbmodemXXX
```

The first flash needs USB. After that the device is on WiFi and updates over the
air:

```sh
uvx --from esphome esphome run vapi-voice.yaml --device vapi-voice-xxxxxx.local
```

If the device is factory-fresh it has no WiFi credentials. You can provision it
from a browser with the [official installer](https://esphome.github.io/home-assistant-voice-pe/),
or without one:

```sh
tools/provision_improv.py /dev/cu.usbmodemXXX --env path/to/.env
```

## Using it

| control | does |
|---|---|
| Centre button | start a call; press again to hang up |
| Wake word | starts a call (see the caveat below) |
| Dial | volume |
| Mute switch | hangs up and blanks the ring |
| Saying "goodbye" | ends the call — see `end_call_phrases` |
| LED ring | call state |

Calls, volume and voice are also exposed over the ESPHome API, so you can drive
the device without being in the room:

```sh
tools/press_button.py vapi-voice-xxxxxx.local "Start Vapi call"
tools/set_number.py   vapi-voice-xxxxxx.local "Vapi volume" 0.8
tools/set_select.py   vapi-voice-xxxxxx.local "Vapi voice" "Freya - energetic"
```

The ESPHome `api:` block is kept, so the device can still be adopted by Home
Assistant for its LED ring, mute switch and dial even though speech goes to Vapi.

## Choosing a voice

Nine voices ship in a runtime selector — a change takes effect on the next call
and survives reboots. `tools/check_voices.py` verifies any other voice.

**Expressiveness comes from `stability` and `style`, not from which voice you
pick.** At ElevenLabs' default stability every premade voice reads each line the
same measured way. See [docs/tuning.md](docs/tuning.md).

Two traps worth knowing before you change the voice:

1. **A 201 from `POST /call` proves nothing.** Vapi accepts any `voiceId` string
   at creation and only resolves it when the transport connects. A bad one ends
   the call instantly with `pipeline-error-eleven-labs-voice-not-found`, which on
   the device looks like the LED flashing and nothing happening.
2. The 11labs provider takes its **preset names** or a **raw 20-character
   ElevenLabs voice ID** — but not ElevenLabs voice *names*. `lily` fails; Lily's
   actual ID works.

## The wake word

**"Hey Vapi"** runs on-device with no cloud round-trip —
`firmware/models/hey_vapi.{tflite,json}`, 61 KB, a `micro_wake_word` model.

To use a different phrase, point `wake_word_model` at any other model: a stock
one such as `okay_nabu` or `hey_jarvis`, or your own manifest in
`firmware/models/`. Sensitivity lives in the manifest rather than the YAML — see
[docs/tuning.md](docs/tuning.md).

A false trigger places a **billable call**, so a wake-initiated call is capped
with a short silence timeout while the button keeps the full one.

## Documentation

| | |
|---|---|
| [docs/hardware.md](docs/hardware.md) | What the hardware is, the XMOS pipeline, pin map, footguns |
| [docs/design.md](docs/design.md) | The four ways to integrate Vapi, and why this one |
| [docs/tuning.md](docs/tuning.md) | Voice, delivery, prompt, and the bugs worth not rediscovering |

## Recovery

This replaces the stock firmware. To go back, use the
[official installer](https://esphome.github.io/home-assistant-voice-pe/). The
device has two OTA slots, so a bad build is not fatal.

The only thing genuinely given up is official OTA updates from Nabu Casa —
you rebuild from their repo when they ship a new version.

## Licence

MIT — see [LICENSE](LICENSE).
