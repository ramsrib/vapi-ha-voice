# Capturing training audio from the device

The wake word is only as good as the audio it was trained on, and until now
none of that audio came from this device.

## Why this is needed at all

Three facts, and together they mean the useful recording cannot be obtained any
other way.

**The wake-word utterance never reaches Vapi.** The wake word fires *before* a
call opens. By the time anything is being recorded, "Hey Vapi" has already been
said. It is in no call artifact, and no amount of harvesting from the Vapi API
will ever produce one.

**Vapi hears a different signal than the wake word does.** The XMOS XU316
exposes two taps. Channel 0 is the fully processed one — AEC → IC → NS → AGC —
and is what the call streams. The wake-word engine listens to **channel 1**,
which stops at noise suppression, and applies its own `gain_factor: 4` on top.
Even the call audio that *does* exist is the wrong signal to train on.

**A miss leaves no trace whatsoever.** On detection the device acts. On a failed
attempt nothing happens at all — no event, no log line, no audio. Yet the misses
are the most valuable training material there is: they are the only record of
the model failing, and they cannot be reconstructed afterwards.

## What the tap does

`mic_tap` registers a **passive** listener on the same microphone source
`micro_wake_word` uses — same channel, same gain — and streams it to a host on
the LAN. Passive matters: a passive source receives audio whenever the hardware
microphone is running and never starts or stops it, so the tap cannot disturb
the wake word or a live call. (A non-passive source stops the *shared* hardware
microphone on `stop()`, which is the bug that made early builds go deaf
mid-call.)

Every detection is stamped into the stream as it happens, so a session comes
back self-labelled: **the marks are the hits, and the attempts between them are
the misses.**

## Running a session

Set `capture_host` in `vapi-voice.yaml` to your machine's LAN address, flash, then:

```bash
tools/capture_mic.py                                     # listens on :9000
tools/set_switch.py vapi-voice-xxxxxx.local "Mic capture" on
#   ... say "Hey Vapi" thirty or forty times ...
tools/set_switch.py vapi-voice-xxxxxx.local "Mic capture" off
```

You get `captures/capture-<stamp>.wav` and, if anything fired, a
`.labels.txt` in Audacity's label format — import both and the hits are marked.

Worth varying deliberately, because this is the only chance to: distance, angle,
volume, speed, and as many different voices as you can get. A model trained on
one person at one distance learns that person at that distance.

Recording the room *without* saying the phrase is just as useful — television,
music, conversation. Those become negatives from the correct tap, which is what
false-accept resistance is actually measured against.

## What it costs

16 kHz mono 16-bit is 32 KB/s, which is nothing on WiFi. The component writes
non-blocking and drops whole frames rather than stalling the audio path, and
logs a warning if it ever does — a dropped frame means a gap in the recording,
which you want to know about before training on it.

## Privacy

This streams room audio unencrypted over the local network. It is off unless
switched on, it never restores across a reboot (`restore_mode: ALWAYS_OFF`), and
it connects only to the single host compiled into the firmware. Leave it off
when you are not collecting.
