#!/usr/bin/env -S uv run --quiet
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyserial>=3.5"]
# ///
"""Give the device WiFi credentials over USB, using Improv Serial.

The stock firmware compiles in `improv_serial`, which is the same protocol the
web installer uses — so credentials can be pushed from a script instead of a
browser. Nothing is flashed and no firmware is changed.

Credentials are read from a .env file (WIFI_SSID / WIFI_PASSWORD) and are never
printed.

    ./provision_improv.py /dev/cu.usbmodem101 --env ../../vapi-atoms3r-voice/.env
    ./provision_improv.py /dev/cu.usbmodem101 --env <file> --net 2   # WIFI_SSID_2
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import serial

HEADER = b"IMPROV"
VERSION = 0x01

TYPE_CURRENT_STATE = 0x01
TYPE_ERROR_STATE = 0x02
TYPE_RPC = 0x03
TYPE_RPC_RESULT = 0x04

CMD_WIFI_SETTINGS = 0x01
CMD_REQUEST_STATE = 0x02
CMD_REQUEST_INFO = 0x03

STATES = {
    0x01: "authorization required",
    0x02: "authorized / ready",
    0x03: "provisioning",
    0x04: "provisioned",
}
ERRORS = {
    0x00: "none",
    0x01: "invalid RPC packet",
    0x02: "unknown RPC command",
    0x03: "unable to connect (wrong credentials, or AP out of range)",
    0xFF: "unknown error",
}


def build_packet(pkt_type: int, data: bytes) -> bytes:
    body = HEADER + bytes([VERSION, pkt_type, len(data)]) + data
    return body + bytes([sum(body) & 0xFF])


def rpc(command: int, payload: bytes = b"") -> bytes:
    return build_packet(TYPE_RPC, bytes([command, len(payload)]) + payload)


def wifi_payload(ssid: str, password: str) -> bytes:
    s, p = ssid.encode(), password.encode()
    return bytes([len(s)]) + s + bytes([len(p)]) + p


def split_strings(blob: bytes) -> list[str]:
    """Improv RPC results are a sequence of length-prefixed strings."""
    out, i = [], 0
    while i < len(blob):
        n = blob[i]
        out.append(blob[i + 1 : i + 1 + n].decode("utf-8", "replace"))
        i += 1 + n
    return out


def parse_stream(buf: bytearray):
    """Pull whole Improv packets out of a byte stream that also carries logs."""
    while True:
        idx = buf.find(HEADER)
        if idx < 0:
            # keep a tail in case a header is split across reads
            del buf[: max(0, len(buf) - len(HEADER))]
            return
        if len(buf) < idx + 9:
            del buf[:idx]
            return
        length = buf[idx + 8]
        end = idx + 9 + length
        if len(buf) < end + 1:
            del buf[:idx]
            return
        packet = bytes(buf[idx:end])
        checksum = buf[end]
        del buf[: end + 1]
        if (sum(packet) & 0xFF) != checksum:
            print("  (dropped a packet with a bad checksum)")
            continue
        yield packet[7], packet[9:]


def load_env(path: Path, index: int) -> tuple[str, str]:
    suffix = "" if index == 1 else f"_{index}"
    want = {f"WIFI_SSID{suffix}", f"WIFI_PASSWORD{suffix}"}
    found: dict[str, str] = {}
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        key = key.strip()
        if key in want:
            found[key] = value.strip().strip('"').strip("'")
    missing = want - found.keys()
    if missing:
        raise SystemExit(f"{path}: missing {', '.join(sorted(missing))}")
    return found[f"WIFI_SSID{suffix}"], found[f"WIFI_PASSWORD{suffix}"]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("port")
    ap.add_argument("--env", type=Path, required=True, help="file holding WIFI_SSID / WIFI_PASSWORD")
    ap.add_argument("--net", type=int, default=1, help="1 = WIFI_SSID, 2 = WIFI_SSID_2, ...")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=float, default=60.0)
    args = ap.parse_args()

    ssid, password = load_env(args.env, args.net)
    print(f"provisioning {args.port} with SSID {ssid!r} (password hidden)")

    # Do not assert DTR/RTS: on a native USB-serial-JTAG port they drive reset
    # and boot mode, and resetting mid-provision loses the exchange.
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = args.port, args.baud, 0.2
    s.dtr = False
    s.rts = False
    s.open()
    s.dtr = False
    s.rts = False

    buf = bytearray()
    deadline = time.time() + args.timeout
    sent_credentials = False

    s.write(rpc(CMD_REQUEST_INFO))
    s.write(rpc(CMD_REQUEST_STATE))
    s.flush()

    # Don't wait for a CURRENT_STATE packet before acting. The stock firmware
    # answers REQUEST_INFO but was observed not to emit a state packet, and
    # WIFI_SETTINGS is accepted regardless — so ask, then send anyway.
    send_at = time.time() + 1.5

    while time.time() < deadline:
        if not sent_credentials and time.time() >= send_at:
            print("  sending credentials...")
            s.write(rpc(CMD_WIFI_SETTINGS, wifi_payload(ssid, password)))
            s.flush()
            sent_credentials = True
            deadline = time.time() + args.timeout

        chunk = s.read(4096)
        if chunk:
            buf.extend(chunk)
        for pkt_type, data in parse_stream(buf):
            if pkt_type == TYPE_CURRENT_STATE:
                state = data[0] if data else 0
                print(f"  state: {STATES.get(state, hex(state))}")
                if state in (0x02, 0x04) and not sent_credentials:
                    # 0x04 means it already has credentials; sending new ones
                    # replaces them, which is what we want.
                    print("  sending credentials...")
                    s.write(rpc(CMD_WIFI_SETTINGS, wifi_payload(ssid, password)))
                    s.flush()
                    sent_credentials = True
                    deadline = time.time() + args.timeout
            elif pkt_type == TYPE_ERROR_STATE:
                code = data[0] if data else 0xFF
                if code != 0x00:
                    print(f"  ERROR: {ERRORS.get(code, hex(code))}")
                    if code == 0x03:
                        s.close()
                        return 1
            elif pkt_type == TYPE_RPC_RESULT:
                command = data[0] if data else 0
                strings = split_strings(data[2:]) if len(data) > 2 else []
                if command == CMD_REQUEST_INFO and strings:
                    print(f"  device: {' / '.join(strings)}")
                elif command == CMD_WIFI_SETTINGS:
                    print("  PROVISIONED.")
                    if strings:
                        print(f"  reachable at: {', '.join(strings)}")
                    s.close()
                    return 0
        if not chunk:
            time.sleep(0.05)

    s.close()
    print("timed out without a provisioning result", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
