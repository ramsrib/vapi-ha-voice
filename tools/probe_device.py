#!/usr/bin/env -S uv run --quiet
# /// script
# requires-python = ">=3.11"
# dependencies = ["aioesphomeapi>=32"]
# ///
"""Ask the Home Assistant Voice PE what it can do, over its own API.

This answers the question that decides the shape of a Vapi bridge: can the
device take assistant audio streamed over the API, or does it only accept a
media URL for playback?

The answer is a flag the device advertises — VoiceAssistantFeature.API_AUDIO.
Everything else printed here is context: which wake words it has, what the audio
settings default to, whether the API wants an encryption key.

Nothing is flashed and no call is placed; this only reads.

    ./probe_device.py home-assistant-voice-xxxxxx.local
    ./probe_device.py 10.0.0.x --key <base64 noise psk>
"""

from __future__ import annotations

import argparse
import asyncio

from aioesphomeapi import APIClient
from aioesphomeapi.model import VoiceAssistantFeature


async def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("host", help="device hostname or IP")
    ap.add_argument("--port", type=int, default=6053)
    ap.add_argument(
        "--key",
        default=None,
        help="API encryption key (noise PSK). Only needed once Home Assistant "
        "has adopted the device — a factory-fresh unit has none.",
    )
    ap.add_argument("--password", default=None)
    args = ap.parse_args()

    client = APIClient(
        args.host,
        args.port,
        args.password,
        noise_psk=args.key,
        client_info="vapi-bridge-probe",
    )

    await client.connect(login=True)
    try:
        info = await client.device_info()

        print("== device ==")
        for label, value in (
            ("name", info.name),
            ("friendly name", info.friendly_name),
            ("model", info.model),
            ("project", f"{info.project_name} {info.project_version}"),
            ("esphome", info.esphome_version),
            ("mac", info.mac_address),
            ("encryption supported", info.api_encryption_supported),
        ):
            print(f"  {label:22} {value}")

        flags = info.voice_assistant_feature_flags_compat(client.api_version)
        print(f"\n== voice assistant feature flags ({flags}) ==")
        for feature in VoiceAssistantFeature:
            mark = "yes" if flags & feature else " no"
            print(f"  {mark}  {feature.name}")

        api_audio = bool(flags & VoiceAssistantFeature.API_AUDIO)
        print("\n== the answer ==")
        if api_audio:
            print("  API_AUDIO is supported.")
            print("  Assistant audio can be streamed straight over the API with")
            print("  send_voice_assistant_audio(). The bridge is a direct PCM pipe:")
            print("  16 kHz mono in from the XMOS, 16 kHz mono back out.")
        else:
            print("  API_AUDIO is NOT supported by this firmware.")
            print("  Playback has to go through the media player as a URL, so the")
            print("  bridge must also serve Vapi's audio over HTTP. Expect to think")
            print("  about latency and about when the stream is considered finished.")

        try:
            cfg = await client.get_voice_assistant_configuration(timeout=5.0)
            print("\n== wake words ==")
            print(f"  max active: {cfg.max_active_wake_words}")
            for ww in cfg.available_wake_words:
                active = "*" if ww.id in cfg.active_wake_words else " "
                print(f"  {active} {ww.id:24} {ww.wake_word}")
        except Exception as exc:  # noqa: BLE001 - informational only
            print(f"\n== wake words ==\n  unavailable: {exc!r}")

        entities, _services = await client.list_entities_services()
        print(f"\n== entities ({len(entities)}) ==")
        for e in entities:
            print(f"  {type(e).__name__:28} {getattr(e, 'name', '')}")
    finally:
        await client.disconnect()
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
