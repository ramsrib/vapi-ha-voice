#!/usr/bin/env -S uv run --quiet
# /// script
# requires-python = ">=3.11"
# dependencies = ["aioesphomeapi>=32"]
# ///
"""Read or set a template number entity over the ESPHome API.

    ./set_number.py vapi-voice-xxxxxx.local                 # list numbers
    ./set_number.py vapi-voice-xxxxxx.local "Vapi volume" 0.75
"""
import argparse, asyncio, sys
from aioesphomeapi import APIClient


async def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("name", nargs="?")
    ap.add_argument("value", nargs="?", type=float)
    ap.add_argument("--port", type=int, default=6053)
    args = ap.parse_args()

    client = APIClient(args.host, args.port, None, client_info="vapi-number")
    await client.connect(login=True)
    try:
        entities, _ = await client.list_entities_services()
        numbers = [e for e in entities if type(e).__name__ == "NumberInfo"]
        if not args.name:
            for n in numbers:
                print(f"  {n.name}  [{n.min_value} .. {n.max_value} step {n.step}]")
            return 0
        match = next((n for n in numbers if n.name.lower() == args.name.lower()), None)
        if match is None:
            print(f"no number named {args.name!r}", file=sys.stderr)
            return 1
        if args.value is None:
            print(f"{match.name}: use a value to set it")
            return 0
        client.number_command(match.key, args.value)
        await asyncio.sleep(0.4)
        print(f"set {match.name!r} = {args.value}")
    finally:
        await client.disconnect()
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
