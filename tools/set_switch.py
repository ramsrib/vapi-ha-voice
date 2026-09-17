#!/usr/bin/env -S uv run --quiet
# /// script
# requires-python = ">=3.11"
# dependencies = ["aioesphomeapi>=32"]
# ///
"""Turn a switch on or off over the ESPHome API.

Used to start and stop a microphone capture session without being in the room.

    ./set_switch.py vapi-voice-xxxxxx.local                  # list switches
    ./set_switch.py vapi-voice-xxxxxx.local "Mic capture" on
"""
import argparse, asyncio, sys
from aioesphomeapi import APIClient


async def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("name", nargs="?", help="switch name; omit to list switches")
    ap.add_argument("state", nargs="?", choices=["on", "off"])
    ap.add_argument("--port", type=int, default=6053)
    args = ap.parse_args()

    client = APIClient(args.host, args.port, None, client_info="vapi-switch")
    await client.connect(login=True)
    try:
        entities, _ = await client.list_entities_services()
        switches = [e for e in entities if type(e).__name__ == "SwitchInfo"]
        if not args.name:
            for s in switches:
                print(f"  {s.name}")
            return 0

        match = next((s for s in switches if s.name == args.name), None)
        if match is None:
            print(f"no switch named {args.name!r}; have: {[s.name for s in switches]}", file=sys.stderr)
            return 1
        if args.state is None:
            print("give a state: on | off", file=sys.stderr)
            return 1

        client.switch_command(match.key, args.state == "on")
        await asyncio.sleep(0.5)  # let the command reach the device before closing
        print(f"{match.name} -> {args.state}")
        return 0
    finally:
        await client.disconnect()


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
