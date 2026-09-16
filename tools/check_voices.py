#!/usr/bin/env -S uv run --quiet
# /// script
# requires-python = ">=3.11"
# dependencies = ["websockets>=12"]
# ///
"""Find which ElevenLabs voices actually work, by running the pipeline.

Vapi accepts any `voiceId` string when the call is created — a 201 proves
nothing. The voice is only resolved once the transport connects, and an invalid
one shows up as `pipeline-error-eleven-labs-voice-not-found` a moment later.
That is what made the device flash its LED and go straight back to idle.

So: create a call per candidate, connect, read the control channel briefly, and
report what the pipeline said. Each call is billable but lasts ~2 seconds.
"""
import asyncio, json, ssl, subprocess, sys, tempfile
import websockets

API = "https://api.vapi.ai/call"


# Extra fields merged into the voice object, as JSON, e.g.
#   VOICE_SETTINGS='{"stability":0.3,"style":0.6}' ./check_voices.py ...
EXTRA = json.loads(__import__("os").environ.get("VOICE_SETTINGS", "{}"))


def split_voice(spec: str) -> tuple[str, str]:
    """Accept "provider:voiceId", defaulting to 11labs."""
    if ":" in spec:
        provider, _, voice = spec.partition(":")
        return provider, voice
    return "11labs", spec


def create_call(key: str, voice: str) -> str:
    provider, voice_id = split_voice(voice)
    body = {
        "assistant": {
            "firstMessage": "Hi.",
            "model": {"provider": "anthropic", "model": "claude-haiku-4-5-20251001",
                      "messages": [{"role": "system", "content": "Say hi."}]},
            "voice": {"provider": provider, "voiceId": voice_id, **EXTRA},
            "transcriber": {"provider": "deepgram", "model": "nova-3"},
            "silenceTimeoutSeconds": 10, "maxDurationSeconds": 10,
        },
        "transport": {"provider": "vapi.websocket",
                      "audioFormat": {"format": "pcm_s16le", "container": "raw", "sampleRate": 16000}},
    }
    # curl, not urllib: Cloudflare answers urllib's default client with a 403.
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
        json.dump(body, f)
        path = f.name
    out = subprocess.run(
        ["curl", "-s", "--max-time", "30", "-X", "POST", API,
         "-H", f"Authorization: Bearer {key}", "-H", "Content-Type: application/json",
         "-d", f"@{path}"],
        capture_output=True, text=True, check=True).stdout
    return json.loads(out)["transport"]["websocketCallUrl"]


async def probe(key: str, voice: str) -> str:
    try:
        url = create_call(key, voice)
    except Exception as exc:
        return f"create failed: {exc}"
    verdict = "inconclusive — no assistant speech and no error"
    try:
        async with websockets.connect(url, ssl=ssl.create_default_context(), open_timeout=20) as ws:
            deadline = asyncio.get_event_loop().time() + 12
            while asyncio.get_event_loop().time() < deadline:
                try:
                    msg = await asyncio.wait_for(ws.recv(), timeout=3)
                except asyncio.TimeoutError:
                    continue
                if isinstance(msg, bytes):
                    # NOT proof of anything: Vapi streams transport audio even
                    # when the voice failed to resolve. An earlier version of
                    # this script broke here and reported every voice as good.
                    continue
                data = json.loads(msg)
                if data.get("type") == "status-update" and data.get("status") == "ended":
                    return f"FAILED — {data.get('endedReason')}"
                if data.get("type") == "speech-update" and data.get("role") == "assistant" \
                        and data.get("status") == "stopped":
                    # The assistant finished an utterance, so the voice really
                    # did synthesise. This is the only trustworthy signal.
                    return "OK — assistant spoke"
    except Exception as exc:
        return f"connect failed: {exc}"
    return verdict


async def main() -> int:
    key = sys.argv[1]
    for voice in sys.argv[2:]:
        print(f"  {voice:<18} {await probe(key, voice)}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
