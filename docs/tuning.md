# Vapi voice assistant firmware

ESPHome firmware that turns the Home Assistant Voice PE into a **standalone Vapi
assistant**. Press the centre button (or say the wake word), talk, and the device
holds a Vapi call itself — no Home Assistant, no laptop.

**Status: working on hardware.** Audio flows both ways; Vapi transcribes the
microphone and the assistant is audible through the speaker.

## What it uses

Everything that makes this board good is kept:

| | |
|---|---|
| XMOS XU316 pipeline | hardware AEC, beamforming, noise suppression, AGC — channel 0 (fully processed) goes to Vapi, channel 1 to the wake word |
| AIC3204 codec + amplifier | speaker output, amp gated so it does not pop at boot |
| On-device wake word | `micro_wake_word`, no cloud round-trip to wake |
| 12-LED ring | call state, in the Vapi mint from `../../sensecap-watcher/DESIGN-palette.md` |
| Dial | volume |
| Mute switch | hangs up and blanks the ring |
| Centre button | push to talk, press again to hang up |
| ESPHome API | kept, so the device is still adoptable by Home Assistant |

Audio never gets resampled on the way in: the XMOS delivers 16 kHz mono, which
is exactly Vapi's wire format. On the way out a `resampler` speaker takes Vapi's
16 kHz up to the 48 kHz the hardware runs at.

## Setup

```sh
cp secrets.yaml.example secrets.yaml     # then fill it in
uvx --from esphome esphome run vapi-voice.yaml --device /dev/cu.usbmodem101
```

After the first flash it is on WiFi, so later updates can go over the air:

```sh
uvx --from esphome esphome run vapi-voice.yaml --device vapi-voice-xxxxxx.local
```

`secrets.yaml` needs `wifi_ssid`, `wifi_password`, `vapi_api_key` and
`vapi_assistant_id`. The Vapi key must be a **private** key — this firmware calls
`POST /call`, which a public key cannot do.

## Using it

- **Centre button** — start a call; press again to hang up.
- **Wake word** — see the placeholder note below.
- **Dial** — volume.
- **Mute switch** — hangs up immediately and blanks the ring.
- **`Start Vapi call` / `Hang up` buttons** are exposed over the API, so a call
  can be placed without being in the room:
  `../tools/press_button.py vapi-voice-xxxxxx.local "Start Vapi call"`.

## Voice and delivery

The default is ElevenLabs **Freya** (`jsCqWAovK2LkecY7zXl4`), and the `Vapi
voice` select switches between nine verified voices at runtime — a change takes
effect on the next call, and the choice survives reboots.

**Expressiveness is `stability` and `style`, not which voice you pick.** At
ElevenLabs' default stability every premade voice reads each line the same
measured way, which sounds flat no matter how characterful the voice is. This
ships `stability: 0.3`, `style: 0.6` on `eleven_turbo_v2_5`.

The prompt matters just as much: TTS delivery follows word choice and
punctuation, so a terse "just answer, no stage directions" prompt guarantees a
flat read. The shipped prompt asks for reactions, varied sentence length and
contractions.

Two ways to go if it needs adjusting — both need a reflash, unlike the voice:

| want | change |
|---|---|
| more animated | `stability: 0.15`, `style: 0.8`, voice `Gigi - animated` |
| less erratic | `stability: 0.45` — very low stability can slur |

### Choosing a voice safely

`tools/check_voices.py` is the only trustworthy check. Two traps it exists for:

1. **A 201 from `POST /call` proves nothing.** Vapi accepts any `voiceId` string
   at creation and only resolves it when the transport connects. A bad one ends
   the call instantly with `pipeline-error-eleven-labs-voice-not-found`, which
   on the device looks like the LED flashing and nothing happening.
2. **Receiving audio proves nothing either.** Vapi streams transport audio even
   when the voice failed — an earlier version of that script broke on the first
   bytes and cheerfully reported every voice as good, including a broken one.
   It now waits for the assistant to *finish an utterance*.

The 11labs provider takes its own preset names (`matilda`, `paula`, `andrea`) or
a raw 20-character ElevenLabs voice ID — but **not** ElevenLabs voice *names*.
`lily` fails; Lily's real ID works. So any voice in the ElevenLabs library, or
one you have cloned, can be dropped in by ID.

## Changing the wake word

`wake_word_model` in `vapi-voice.yaml` takes either a stock model name
(`okay_nabu`, `hey_jarvis`) or a path to a manifest in `firmware/models/`.
Swapping it is that one line, plus a reflash.

## Echo, and why AGC cannot be the lever

Symptom: the assistant answers itself. The transcript fills with "user" turns
that are verbatim repeats of what it just said.

The XMOS canceller works, but cancellation has finite depth and the surviving
residue scales with output level. **Volume is the lever.** At 0.9 enough echo
survived to be transcribed as speech; at 0.6 it did not.

The obvious-looking alternative is a trap. The microphone tap Vapi listens to is
the AGC stage, and AGC makes quiet signals louder — including residual echo — so
moving the tap to noise-suppression-without-AGC looks like the clean fix. It was
tried, and it **broke the device**: without automatic gain, ordinary speech from
a metre away never reaches a level the transcriber acts on, so calls connect,
greet, and then hear nothing at all.

AGC is not amplifying echo instead of your voice; it is amplifying your voice and
the echo comes along. Keep it, and turn the speaker down.

## Wake-word sensitivity

`probability_cutoff` and `sliding_window_size` live in the model manifest
(`models/hey_vapi.json`), not the YAML, so changing them needs a reflash.

Ship values are **0.27 / 5**. A cutoff that low looks alarming next to the
official models' ~0.97, and it is not comparable: it is the threshold at which
*this* model was measured to produce zero false accepts per hour on held-out
ambient audio. A cutoff only means something relative to the model's own score
distribution.

The history is worth knowing, because the sensitivity was the bug twice over.
The first model shipped at 0.94 / 8 — stricter than any official model, since
Nabu Casa's ship at a window of 5 — and needed several attempts to wake. Relaxing
it to 0.85 / 5 helped and was still not enough: measured against real recordings
rather than the synthetic validation set, that model answered **45% of the time**
while its own validation claimed 96%. A wake word that works slightly less than
half the time is exactly what "it takes a few tries" feels like.

The current model answers **82%** of real utterances at 0.27, with no false
accepts across 84 real negative clips recorded through this device. The previous
model false-accepted on 4 of those 84 at *every* cutoff tried, which is the part
a synthetic validation set never showed.

If you retrain, take the cutoff from the training run's own measurement rather
than from any published figure, and check it against recordings of real people
before shipping. Validation recall on synthetic speech does not predict this.

**Rolling back.** The previous model is kept as
`models/hey_vapi.v1.{tflite,json}.bak`. Point `wake_word_model` at a restored
copy of `hey_vapi.v1.json` and reflash.

## Things worth knowing

**`play()` returns how much it accepted.** Ignoring that return value silently
drops the remainder whenever the speaker buffer is briefly full, and it is heard
as distortion rather than as a clean gap. That was the cause of the first working
build sounding rough. The component now writes in a loop against a wall-clock
budget and counts anything it still could not place, logging it once a second —
so "it sounds bad" can be attributed instead of guessed at.

**The websocket delivers audio in bursts**, not at a steady 20 ms cadence, which
is why both the speaker and the resampler are given 500 ms of buffer.

**A call must not be started twice.** `is_active()` is false during the couple of
seconds `POST /call` takes, so a naive check placed four billable calls in six
seconds on an earlier project. The component claims the transition with an atomic
compare-exchange instead.

**The wake-word engine keeps running for the whole call**, and must. It shares
one hardware microphone with the Vapi client, and a non-passive `MicrophoneSource`
stops that hardware on `stop()` — so whichever component ends its session kills
audio for the other. Ownership goes to the wake word, which has to listen
continuously anyway; the Vapi client's source is `passive=True` and never starts
or stops the device.

Two settings follow from that, and both defaults are wrong here:
`stop_after_detection` defaults to `true`, which stops the shared microphone the
moment the wake word fires — the assistant greets you and then hears nothing
until the call dies on a silence timeout. And `micro_wake_word` has **no start in
its own lifecycle**: something must call `micro_wake_word.start:`. The stock
config does it from `voice_assistant.on_client_connected`, so removing that
automation silently removes the only start and the wake word never fires at all,
no matter how good the model is. It is started from `on_boot` here.

**Checking a flash over serial breaks the flash.** Reading the USB-serial-JTAG
console drives reset, and ESPHome only marks a boot successful after ~60 seconds.
Captures every 30–40 seconds therefore log repeated *unsuccessful boot attempts*
until safe mode rolls back to the previous image — leaving you reading an old
firmware's config dump and concluding the upload failed. It had not. Verify over
the network instead: `esphome logs vapi-voice.yaml --device <host>.local` touches
no reset lines.

**ESPHome excludes some built-in ESP-IDF components by default.** `json`,
`esp_http_client` and `esp-tls` are in `DEFAULT_EXCLUDED_IDF_COMPONENTS` — it
uses ArduinoJson and pulls the HTTP client in only for its own `http_request`
component — so a component that includes `cJSON.h` fails to build with a bare
`No such file or directory`, which reads like a missing dependency rather than a
deliberate exclusion. `include_builtin_idf_component()` puts them back.

**Echo cancellation is the XMOS's job, not ours.** Do not port the echo-gating
from the AtomS3R firmware — it exists there because that board has two codecs and
no echo reference, and here it would cost barge-in for nothing.

## Recovery

This replaces the stock firmware. To go back, use the official installer at
<https://esphome.github.io/home-assistant-voice-pe/>. `../backup/` also holds
this unit's bootloader/partition table and its NVS partition.
