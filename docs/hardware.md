# Home Assistant Voice PE — device knowledge

Everything established about the **Home Assistant Voice: Preview Edition**,
independent of any one firmware project. Read off the device's own boot log and
out of the official firmware source
([esphome/home-assistant-voice-pe](https://github.com/esphome/home-assistant-voice-pe)),
not from the product page.

## What it is

```
Chip           ESP32-S3, silicon revision v0.2, dual core, WiFi b/g/n + BLE
Flash          16 MB  — two 7.75 MB OTA app slots, NVS at 0xF90000
PSRAM          8 MB
Firmware       ESPHome project "home-assistant-voice", version 2024.12.2
ESP-IDF        5.1.5
EFuse MAC      <device MAC>   (hostname home-assistant-voice-xxxxxx)
USB descriptor 0x303A / 0x1001, serial <USB descriptor serial>
```

It appears over USB as a **native USB-Serial-JTAG device** — no bridge chip —
at `/dev/cu.usbmodem*`. The console reads at 115200, and unlike the AtomS3R
boards, scripted capture works fine.

> The USB descriptor's serial number and the chip's EFuse MAC disagree, and the
> hostname confirms the EFuse MAC is the operative identity. Most likely a
> vendor-burned custom MAC with the ROM descriptor still showing the Espressif
> factory one. **Unconfirmed** — settling it needs an esptool download-mode
> reset, which resets the device.

## The part that matters: an XMOS does the hard audio work

There are two processors. The ESP32-S3 runs ESPHome; an **XMOS XU316** runs
XMOS's `sln_voice` FFVA firmware (DFU version 1.3.1) and owns the microphone
array and the speaker path.

The XU316 exposes a **configurable DSP pipeline** over I2C, with these stages:

| stage | value |
|---|---|
| NONE | 0 |
| AEC — acoustic echo cancellation | 1 |
| IC — interference cancellation | 2 |
| NS — noise suppression | 3 |
| AGC — automatic gain control | 4 |

Two I2S channels come back to the ESP32, and **each channel's tap point into
that pipeline is independently selectable**. Stock firmware sets:

- **channel 0 → AGC** (the whole pipeline: AEC, IC, NS, AGC) — fed to the voice
  assistant
- **channel 1 → NS** (no AGC) — fed to the wake-word engine, which applies its
  own `gain_factor: 4`

Selecting a stage is four bytes to I2C `0x42`:

```c
/* {CONFIGURATION_SERVICER_RESID, channel resid, length, stage} */
uint8_t set[] = { 241, 0x30 /* ch0; 0x40 = ch1 */, 1, stage };
```

**This is the whole reason the device is interesting.** The echo problem that
forced hard mic-gating on the AtomS3R — two codecs, no echo reference, so AEC was
impossible — does not exist here. The XU316 cancels echo in hardware against the
real speaker signal and hands the ESP32 already-clean audio. Barge-in becomes
possible rather than something to design around.

Just as importantly: that processing is **downstream of nothing on the ESP32**.
Any firmware on the S3, including a custom ESP-IDF build, gets the clean audio
over plain I2S after two I2C writes.

## Audio formats and the I2S direction

Both I2S links have the ESP32 as **secondary (slave)** — the XMOS drives the
clocks in both directions.

| link | pins | rate | format |
|---|---|---|---|
| mic in (from XMOS) | BCLK 13, LRCLK 14, DIN 15 | 16 kHz | 32-bit, stereo |
| speaker out (to XMOS) | BCLK 8, LRCLK 7, DOUT 10 | 48 kHz | 32-bit, stereo |

The mic rate is already **exactly Vapi's wire format** (16 kHz mono). The
speaker runs at 48 kHz, so the only signal processing a Vapi client needs is a
1:3 integer upsample on playback, plus 16→32-bit widening and mono→stereo
duplication. No resampler library required.

## Pin map

| function | pin |
|---|---|
| Internal I2C — SDA / SCL | GPIO5 / GPIO6 |
| XMOS XU316 — I2C address | **0x42** |
| XMOS XU316 — reset | GPIO4 |
| AIC3204 codec — I2C address | **0x18** |
| I2S in — BCLK / LRCLK / DIN | GPIO13 / GPIO14 / GPIO15 |
| I2S out — BCLK / LRCLK / DOUT | GPIO8 / GPIO7 / GPIO10 |
| Internal speaker amp enable | GPIO47 |
| Centre button | GPIO0 (inverted) |
| Hardware mute switch (side) | GPIO3 |
| Headphone jack detect | GPIO17 |
| LED ring (WS2812) | GPIO21 |
| LED ring power supply | GPIO45 |
| Rotary dial A / B | GPIO16 / GPIO18 |

An I2C scan on the internal bus finds exactly two devices, 0x18 and 0x42, which
matches: codec and XMOS.

## What the stock firmware provides

`aic3204`, `voice_kit`, `i2s_audio` (mic + speaker), `nabu_media_player`,
`micro_wake_word`, `esp32_rmt_led_strip`, `rotary_encoder`, `power_supply`,
`esp32_ble` / `esp32_ble_server`, `esp32_improv`, `improv_serial`, `api`,
`esphome.ota`, `http_request.ota`, `safe_mode`, `factory_reset.button`, `mdns`,
`psram`, `debug`.

**Wake words run on-device** (`micro_wake_word`): Okay Nabu, Hey Jarvis, Hey
Mycroft, Stop, plus a VAD model. No cloud round-trip to wake.

The ESPHome API listens on **port 6053**.

## Current state of this unit

Factory-fresh and **not provisioned**:

```
SSID: ''    BSSID: 00:00:00:00:00:00    Subnet: 0.0.0.0
Hostname: home-assistant-voice-xxxxxx
Using noise encryption: NO
```

Both `improv_serial` (over this USB port) and `esp32_improv` (over BLE) are
compiled in, so it can be given WiFi credentials without Home Assistant.

API encryption is currently **off** because no key has been set. Pairing it with
Home Assistant sets one — worth knowing before writing anything that connects to
the API expecting no key.

## Things most likely to cost you a day

**Opening the USB port resets the ESP32.** Reset reason reads `USB UART Reset
Digital Core`. ESPHome counts a boot as unsuccessful if it does not survive 60
seconds, and this unit already logged *"2 suspected unsuccessful boot attempts"*
purely from console captures. Safe mode triggers at 10, and the counter clears
after a minute of uninterrupted running — so leave it alone between captures
rather than reconnecting in a loop.

**The ESP32 is the I2S secondary on both links.** Custom firmware that assumes
it should generate BCLK/LRCLK will get silence. The XMOS is master.

**The mic stream is stereo, and the channels are not left/right.** They are two
different tap points into the XMOS pipeline. Treating it as a stereo recording
and downmixing would average a fully-processed signal with a differently
processed one.

**Do not assume the AIC3204 is optional.** It is the DAC for the speaker path;
custom firmware has to bring it up over I2C at 0x18 before anything is audible.

**There are two firmwares on this device.** The XMOS has its own image, updated
by the ESP32 over I2C DFU (`voice_kit`). Replacing the ESP32 firmware does not
touch it — which is good, but it also means the XMOS version is a variable that
custom firmware should read and check rather than assume.
