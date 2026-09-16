#!/usr/bin/env -S uv run --quiet
# /// script
# requires-python = ">=3.11"
# dependencies = ["aioesphomeapi>=32"]
# ///
"""Read or set a template select entity over the ESPHome API.

    ./set_select.py vapi-voice-xxxxxx.local                      # list
    ./set_select.py vapi-voice-xxxxxx.local "Vapi voice" vapi:Hana
"""
import argparse, asyncio, sys
from aioesphomeapi import APIClient


async def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("name", nargs="?")
    ap.add_argument("value", nargs="?")
    ap.add_argument("--port", type=int, default=6053)
    args = ap.parse_args()

    client = APIClient(args.host, args.port, None, client_info="vapi-select")
    await client.connect(login=True)
    try:
        entities, _ = await client.list_entities_services()
        selects = [e for e in entities if type(e).__name__ == "SelectInfo"]
        if not args.name:
            for s in selects:
                print(f"  {s.name}: {', '.join(s.options)}")
            return 0
        match = next((s for s in selects if s.name.lower() == args.name.lower()), None)
        if match is None:
            print(f"no select named {args.name!r}", file=sys.stderr)
            return 1
        if args.value is None:
            print(f"{match.name} options: {', '.join(match.options)}")
            return 0
        if args.value not in match.options:
            print(f"{args.value!r} not an option; have: {', '.join(match.options)}", file=sys.stderr)
            return 1
        client.select_command(match.key, args.value)
        await asyncio.sleep(0.4)
        print(f"set {match.name!r} = {args.value}")
    finally:
        await client.disconnect()
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
