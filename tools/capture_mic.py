#!/usr/bin/env python3
"""Receive the device's microphone stream and write it as a labelled WAV.

This captures the one signal that cannot be obtained any other way: what the
wake-word engine actually hears. The wake word fires *before* a call opens, so
"Hey Vapi" is in no call recording; and the engine listens to XMOS channel 1
(noise-suppressed, no AGC, plus its own gain) while Vapi listens to channel 0,
so even call audio is the wrong tap.

Every detection the device reports is written to a sidecar label file. That is
what makes a session useful: the marked moments are the hits, and the attempts
in between are the misses — the only recording anyone has of the model failing.

    ./capture_mic.py                      # listen, write into ./captures/
    ./capture_mic.py --out-dir ~/wav      # somewhere else

Then switch "Mic capture" on (esphome logs / Home Assistant / tools/set_switch),
say the phrase, and switch it off. Ctrl-C also closes the file cleanly.
"""
import argparse
import datetime
import errno
import shutil
import socket
import struct
import subprocess
import sys
import wave
from pathlib import Path

HEADER = b"MICTAP01"
FRAME_PCM, FRAME_LABEL = 0, 1


def port_holder(port):
    """Describe whatever is already listening on the port, if we can tell."""
    if shutil.which("lsof") is None:
        return None
    try:
        out = subprocess.run(
            ["lsof", "-nP", f"-iTCP:{port}", "-sTCP:LISTEN"],
            capture_output=True, text=True, timeout=5,
        ).stdout.strip().splitlines()
    except (subprocess.SubprocessError, OSError):
        return None
    return out[1:] if len(out) > 1 else None


def recv_exactly(sock, n):
    """Read n bytes, or return None if the peer closed mid-frame."""
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return bytes(buf)


def session(conn, out_dir):
    head = recv_exactly(conn, 16)
    if head is None or head[:8] != HEADER:
        print(f"!! not a mic_tap stream (got {head[:8] if head else None!r})", file=sys.stderr)
        return
    rate, channels, bits = struct.unpack("<IHH", head[8:16])
    print(f"   format: {rate} Hz, {channels}ch, {bits}-bit")
    if bits != 16:
        print(f"!! expected 16-bit, refusing to guess", file=sys.stderr)
        return

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    wav_path = out_dir / f"capture-{stamp}.wav"
    lbl_path = out_dir / f"capture-{stamp}.labels.txt"

    frames = 0
    labels = []
    with wave.open(str(wav_path), "wb") as wav:
        wav.setnchannels(channels)
        wav.setsampwidth(bits // 8)
        wav.setframerate(rate)
        try:
            while True:
                head = recv_exactly(conn, 4)
                if head is None:
                    break
                ftype, _pad, length = head[0], head[1], struct.unpack("<H", head[2:4])[0]
                payload = recv_exactly(conn, length) if length else b""
                if payload is None:
                    break
                if ftype == FRAME_PCM:
                    wav.writeframes(payload)
                    frames += len(payload) // (channels * bits // 8)
                elif ftype == FRAME_LABEL:
                    # Position it at the audio written so far — the device sends
                    # the mark inline, so this is the instant it fired.
                    at = frames / rate
                    text = payload.decode("utf-8", "replace")
                    labels.append((at, text))
                    print(f"   [{at:7.2f}s] {text}")
        except KeyboardInterrupt:
            print("\n   interrupted, closing the file cleanly")

    secs = frames / rate if rate else 0
    if labels:
        # Audacity label format, so the session can be reviewed by ear.
        lbl_path.write_text("".join(f"{a:.6f}\t{a:.6f}\t{t}\n" for a, t in labels))

    print(f"\n   {wav_path}  ({secs:.1f}s, {len(labels)} detections)")
    if labels:
        print(f"   {lbl_path}")
        print("   Import the labels alongside the WAV in Audacity to see which")
        print("   attempts fired. The unmarked ones are the misses — those are")
        print("   the clips worth training on.")
    elif secs > 0:
        print("   No detections. If you spoke the phrase, every attempt was a miss —")
        print("   which is exactly the material this is for.")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--out-dir", type=Path, default=Path("captures"))
    args = ap.parse_args()
    # Line-buffer, so progress is visible when this is redirected to a log
    # rather than appearing only once the process exits.
    try:
        sys.stdout.reconfigure(line_buffering=True)
    except AttributeError:
        pass
    args.out_dir.mkdir(parents=True, exist_ok=True)

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        srv.bind(("0.0.0.0", args.port))
    except OSError as exc:
        if exc.errno != errno.EADDRINUSE:
            raise
        # A receiver left running from an earlier session still holds the port,
        # and the device will happily connect to *it* — so the capture succeeds
        # and the audio lands in that process's output directory instead of this
        # one. Nothing looks wrong except a missing file. Fail loudly and name
        # the culprit rather than letting the session go somewhere else.
        print(f"!! port {args.port} is already in use.\n", file=sys.stderr)
        holder = port_holder(args.port)
        if holder:
            print("   Already listening:", file=sys.stderr)
            for line in holder:
                print(f"     {line}", file=sys.stderr)
            pids = sorted({parts[1] for l in holder if len(parts := l.split()) > 1})
            if pids:
                print(f"\n   That is almost certainly an earlier capture_mic.py.", file=sys.stderr)
                print(f"   It would have received this session and written the audio", file=sys.stderr)
                print(f"   into ITS output directory, not {args.out_dir}.\n", file=sys.stderr)
                print(f"   Stop it:  kill {' '.join(pids)}", file=sys.stderr)
                print(f"   Or run here on another port:  --port {args.port + 1}", file=sys.stderr)
        else:
            print(f"   Stop whatever is listening, or use --port {args.port + 1}.", file=sys.stderr)
        return 1

    srv.listen(1)
    print(f"listening on :{args.port} — turn on the device's 'Mic capture' switch")
    print(f"writing to {args.out_dir.resolve()}")
    print("(Ctrl-C to quit)\n")

    try:
        while True:
            conn, addr = srv.accept()
            print(f">> {addr[0]} connected")
            with conn:
                session(conn, args.out_dir)
            print(">> disconnected, waiting for the next session\n")
    except KeyboardInterrupt:
        print("\nbye")
    return 0


if __name__ == "__main__":
    sys.exit(main() or 0)
