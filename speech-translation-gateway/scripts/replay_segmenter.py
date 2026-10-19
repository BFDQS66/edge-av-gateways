#!/usr/bin/env python3
"""Replay a RAM-only mic capture through the live segmenter and STT."""

import json
from pathlib import Path
import subprocess
import sys
from urllib.request import Request, urlopen

from live_translate import (AudioSegmenter, FRAME_BYTES, SAMPLE_RATE,
                            plausible_fallback, rms_pcm16,
                            should_retry_ungated, wav_bytes)


def main(path: Path) -> None:
    converted = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", str(path),
         "-af", "pan=mono|c0=c0", "-ar", "16000", "-ac", "1", "-f", "s16le", "pipe:1"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30, check=False,
    )
    if converted.returncode:
        raise RuntimeError(converted.stderr.decode(errors="replace")[:300])
    segmenter = AudioSegmenter()
    results = []
    frame_levels = []
    for offset in range(0, len(converted.stdout) - FRAME_BYTES + 1, FRAME_BYTES):
        frame = converted.stdout[offset:offset + FRAME_BYTES]
        level = rms_pcm16(frame)
        frame_levels.append(level)
        emitted = segmenter.feed(frame, level)
        if not emitted:
            continue
        pcm, reason = emitted
        with urlopen(Request("http://127.0.0.1:8765/v1/stt?strategy=gate",
                             wav_bytes(pcm), {"Content-Type": "audio/wav"}),
                     timeout=40) as response:
            recognized = json.load(response)
        audio_sec = len(pcm) / (SAMPLE_RATE * 2)
        segment_rms = rms_pcm16(pcm)
        retry = not recognized["text"] and should_retry_ungated(
            reason, audio_sec, segment_rms, segmenter.noise)
        fallback = None
        if retry:
            with urlopen(Request("http://127.0.0.1:8765/v1/stt?strategy=off",
                                 wav_bytes(pcm), {"Content-Type": "audio/wav"}),
                         timeout=40) as response:
                fallback = json.load(response)["text"]
        accepted = bool(fallback and plausible_fallback(fallback, "zh"))
        translation = None
        if accepted:
            payload = json.dumps({"text": fallback, "source": "zh", "target": "en"},
                                 ensure_ascii=False).encode("utf-8")
            with urlopen(Request("http://127.0.0.1:8767/v1/translate", payload,
                                 {"Content-Type": "application/json"}),
                         timeout=180) as response:
                translation = json.load(response)["translation"]
        results.append({"ended_sec": round((offset + FRAME_BYTES) /
                                           (SAMPLE_RATE * 2), 1),
                        "audio_sec": round(audio_sec, 1),
                        "rms": round(segment_rms, 1),
                        "noise_rms": round(segmenter.noise, 1),
                        "reason": reason,
                        "text": recognized["text"],
                        "off_if_eligible": fallback,
                        "fallback_accepted": accepted,
                        "fallback_translation": translation,
                        "speech_sec": recognized["speech_sec"]})
    print(json.dumps({"max_frame_rms": round(max(frame_levels), 1),
                      "p95_frame_rms": round(sorted(frame_levels)[int(0.95 * len(frame_levels))], 1),
                      "segments": len(results),
                      "recognized": sum(bool(item["text"]) for item in results),
                      "empty": sum(not item["text"] for item in results),
                      "accepted_after_fallback": sum(bool(item["text"]) or
                                                     item["fallback_accepted"]
                                                     for item in results),
                      "results": results}, ensure_ascii=False, separators=(",", ":")))


if __name__ == "__main__":
    main(Path(sys.argv[1]))
