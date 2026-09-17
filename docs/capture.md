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
the LAN. Turning it on also **ends any call that is already running**: blocking
new calls is not enough, because a call started beforehand survives as long as
someone keeps talking, and a capture session is nothing but talking — so the
assistant replies over every attempt and its voice lands in the recording. Passive matters: a passive source receives audio whenever the hardware
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

**Run one receiver at a time.** A stale one left over from an earlier session
still holds the port, and the device will connect to *it* — the capture succeeds
and the audio lands in that process's output directory, with nothing at all
looking wrong except a file that never appears where you expected. The receiver
now refuses to start in that situation and names the process to kill, rather than
letting the session go somewhere else.

Worth varying deliberately, because this is the only chance to: distance, angle,
volume, speed, and as many different voices as you can get. A model trained on
one person at one distance learns that person at that distance.

Recording the room *without* saying the phrase is just as useful — television,
music, conversation. Those become negatives from the correct tap, which is what
false-accept resistance is actually measured against.

## Never write to the socket from the audio path

This is the whole design, and it was learned the expensive way: the first two
versions sent straight from the microphone callback, and captures died after 29
and 45 seconds with `partial frame — network too slow`.

A non-blocking `send()` may accept only *part* of what it is given whenever the
socket buffer is momentarily full. That is normal, not an error. But a
half-written frame desynchronises the framing for the rest of the session, so
the only safe response from inside the audio callback is to end the stream —
which means any WiFi hiccup at all kills the capture. Retrying inside the
callback is not an escape either: that stalls the microphone task, which costs
real audio and risks the wake word.

So the audio path now only ever copies complete frames into a **4-second ring
buffer**, and `loop()` drains that ring to the socket. A partial write there is
harmless — the ring simply keeps the remainder for the next iteration, and the
framing is intact because the ring holds an already-framed byte stream. A
network stall shorter than four seconds is now invisible in the recording rather
than fatal to it.

Two smaller fixes came with it: `TCP_NODELAY`, because ~60 small writes a second
under Nagle build a backlog for no reason, and dropping whole frames rather than
partial ones when the ring does overflow, so the stream stays valid and only
loses that slice of audio.

`% buffered` in the device log is the health indicator. It should sit at 0.

## What it costs

16 kHz mono 16-bit is 32 KB/s, which is nothing on WiFi.

## Privacy

This streams room audio unencrypted over the local network. It is off unless
switched on, it never restores across a reboot (`restore_mode: ALWAYS_OFF`), and
it connects only to the single host compiled into the firmware. Leave it off
when you are not collecting.
