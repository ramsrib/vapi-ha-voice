#!/usr/bin/env -S uv run --quiet
# /// script
# requires-python = ">=3.11"
# dependencies = ["aioesphomeapi>=32"]
# ///
"""Press a template button on the device over the ESPHome API.

Used to place or end a Vapi call without being in the room.

    ./press_button.py vapi-voice-xxxxxx.local "Start Vapi call"
"""
import argparse, asyncio, sys
from aioesphomeapi import APIClient


async def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("name", nargs="?", help="button name; omit to list buttons")
    ap.add_argument("--port", type=int, default=6053)
    args = ap.parse_args()

    client = APIClient(args.host, args.port, None, client_info="vapi-press")
    await client.connect(login=True)
    try:
        entities, _ = await client.list_entities_services()
        buttons = [e for e in entities if type(e).__name__ == "ButtonInfo"]
        if not args.name:
            for b in buttons:
                print(f"  {b.name}")
            return 0
        match = next((b for b in buttons if b.name.lower() == args.name.lower()), None)
        if match is None:
            print(f"no button named {args.name!r}; have: {[b.name for b in buttons]}", file=sys.stderr)
            return 1
        client.button_command(match.key)
        await asyncio.sleep(0.5)
        print(f"pressed {match.name!r}")
    finally:
        await client.disconnect()
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
