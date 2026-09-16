# Talking to Vapi from the Home Assistant Voice PE

Four ways in, and they are not close. The reasoning matters more than the
ranking, because it turns on one fact about the hardware.

## The fact that decides it

**The thing worth having on this device is not the ESP32.** It is the XMOS
XU316: a hardware AEC / beamforming / noise-suppression / AGC pipeline, wired to
a real microphone array and a real speaker with an amplifier, whose output tap
point is selectable over I2C (see [`NOTES.md`](hardware.md)). Plus on-device wake
words, an LED ring that already expresses assistant state, a dial, and a mute
switch.

All of that is reachable **without touching the firmware**. The XMOS hands clean
audio to whatever runs on the S3, and the stock firmware already exposes that
audio over its API. So the question is not "how do I get Vapi onto this chip",
it is "how little do I have to throw away".

Two supporting facts:

- The mic stream is **16 kHz mono** — bit-for-bit Vapi's `vapi.websocket`
  format. No transcoding inbound.
- Playback is 48 kHz, so outbound needs a **1:3 integer upsample**. Not a
  resampler, a loop.

## The options

| | approach | firmware work | what you keep | what you lose |
|---|---|---|---|---|
| **A** | **Bridge over the ESPHome API** | **none** | everything | a laptop/server stays in the loop |
| B | ESPHome external component | moderate | everything | nothing, once it works |
| C | Custom ESP-IDF firmware | large | the XMOS | wake word, LEDs, dial, OTA, safe mode, HA |
| D | Vapi as a Home Assistant conversation agent | none | everything | Vapi's voice, turn-taking, interruption |

### A — Bridge over the ESPHome API *(start here)*

The device is a server on **port 6053** speaking ESPHome's native API, and
`aioesphomeapi` — the same client Home Assistant itself uses — has the complete
voice-assistant surface:

```
subscribe_voice_assistant       wake word fires, device starts streaming
_on_voice_assistant_audio       mic audio arrives (16 kHz, already XMOS-processed)
send_voice_assistant_audio      assistant audio back to the device
send_voice_assistant_event      drive the LED ring through its states
```

So a Python process connects to the device, waits for "Okay Nabu", opens a Vapi
call, and pipes PCM both ways. **No firmware is flashed and nothing is at risk.**
The wake word, the LED feedback, the dial and the mute switch all keep working
because the stock firmware is still the thing running them.

It is also the only option that can be stood up and debugged with print
statements on a laptop, which matters for the part nobody has done before — the
actual audio plumbing.

### B — ESPHome external component *(where it should end up)*

The same logic, compiled into the firmware, so the device talks to Vapi directly
with no laptop. You add a component that opens the Vapi websocket and pumps PCM
between ESPHome's `microphone` and `speaker` abstractions, and you keep every
other component in the stock YAML: wake word, LEDs, dial, OTA, safe mode.

This is the destination, not the starting point — the audio contract is easier to
get right in Python first, and an ESPHome component is a slower edit-run loop.
The second OTA slot and the official web installer mean a bad build is
recoverable.

### C — Custom ESP-IDF firmware

Tempting because `vapi_client.c` from the AtomS3R project is board-independent
and would drop in unchanged. But that is the easy part. You would also
reimplement the AIC3204 codec bring-up, I2S in **secondary** mode on both links,
the XMOS pipeline configuration and DFU check, the WS2812 ring, the dial, the
buttons, the mute switch, OTA and safe mode — to end up with less than the stock
firmware already gives you.

The gain over B is only "no ESPHome", and the XMOS — the actual reason to want
this device — is equally available in A and B. Reach for this only if something
in ESPHome's audio path turns out to be a hard blocker.

### D — Vapi as a Home Assistant conversation agent

Zero work, and the wrong shape. Home Assistant's Assist pipeline is
STT → conversation agent → TTS, so plugging Vapi in as the conversation agent
means **HA does the speech and Vapi only does the text**. You would lose Vapi's
voice, its turn-taking and its interruption handling, which is most of what Vapi
is. Worth knowing it exists; not worth building.

## Outcome: B, built and working

The bridge was skipped. Once the audio contract was understood it was clear the
Python would be throwaway — the Vapi protocol logic already existed in proven C
in the AtomS3R project, which is far closer to an ESPHome component than any
Python bridge is. So the component was written directly: [`firmware/`](../firmware/).

Everything the device offers is still in use — XMOS pipeline, wake word, LED
ring, dial, mute switch — and the ESPHome API is kept, so it can still be
adopted by Home Assistant for those. The only thing genuinely given up is
official OTA updates from Nabu Casa.

## Determined: API_AUDIO is supported

Measured against the live device with [`tools/probe_device.py`](../tools/):

```
voice assistant feature flags (29)
  yes  VOICE_ASSISTANT      no  SPEAKER            yes  API_AUDIO
  yes  TIMERS              yes  ANNOUNCE            no  START_CONVERSATION
   no  MULTI_CHANNEL_AUDIO
```

So assistant audio can be streamed straight over the API — **no HTTP media
server is needed**, and the return path is a direct PCM pipe. The device also
reports `encryption supported: False` while un-adopted, so the API takes no key
today.

For the record, the original wording of this section is kept below because the
question it poses is the right one to ask of any ESPHome voice device; it is
simply answered for this one.

## The question that decided it (now answered)

Stock `voice_assistant` is configured with `media_player: external_media_player`,
which in Home Assistant's normal flow plays TTS **from a URL**. `aioesphomeapi`
also offers `send_voice_assistant_audio` for streaming audio over the API. Which
of the two this firmware (2024.12.2) actually accepts decides the shape of the
return path:

- **streaming accepted** → a direct PCM pipe, and the bridge is small
- **URL only** → the bridge also serves an HTTP audio stream, and latency gets
  more interesting

This is discoverable at runtime from the device's advertised voice-assistant
feature flags, and it is the first thing [`tools/probe_device.py`](../tools/)
answers. Everything else in the design follows from it.

## Before any of this

The unit is **unprovisioned** — no WiFi, so no API to connect to. Give it
credentials over USB with the Improv web installer, or over BLE from the HA
companion app. See [`bridge/README.md`](../README.md).

Note that pairing it with Home Assistant sets an **API encryption key**. The
bridge connects with no key today; if the device is later adopted by HA, the
bridge needs that key.
