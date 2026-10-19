#!/usr/bin/env python3
"""Always-on, bidirectional speech translation with headphone playback."""

from array import array
from collections import deque
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from io import BytesIO
import json
import math
import os
from pathlib import Path
from queue import Empty, Full, Queue
import re
import subprocess
import threading
import time
from urllib.error import HTTPError
from urllib.parse import parse_qs, urlsplit
from urllib.request import Request, urlopen
import uuid
import wave


ROOT = Path(__file__).resolve().parents[1]
HTML = (ROOT / "web/live.html").read_bytes()
DEVICE = "hw:rockchipes8388,0"
PLAY_DEVICE = "hw:rockchipes8388,0"
CARD = "rockchipes8388"
CONFIG_PATH = ROOT / "config/live.json"
SAMPLE_RATE = 16000
FRAME_BYTES = 3200  # 100 ms of mono PCM16
SILENCE_FRAMES = 6
SHORT_SILENCE_FRAMES = 10
MAX_FRAMES = 120
STRONG_MAX_FRAMES = 80
WEAK_MIN_FRAMES = MAX_FRAMES
STRONG_RMS = 350.0
QUEUE_SIZE = 3


def headphone_connected() -> bool:
    result = subprocess.run(
        ["amixer", "-c", CARD, "cget", "numid=1"],
        capture_output=True, text=True, timeout=3, check=False,
    )
    return result.returncode == 0 and "values=on" in result.stdout


def utc_now() -> str:
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds")


def rms_pcm16(data: bytes) -> float:
    samples = array("h")
    samples.frombytes(data)
    if not samples:
        return 0.0
    return math.sqrt(sum(sample * sample for sample in samples) / len(samples))


def should_retry_ungated(reason: str, audio_sec: float, rms: float,
                         noise_rms: float) -> bool:
    return (reason in ("max_duration", "low_level_fallback") and
            audio_sec >= 10.0 and rms >= max(75.0, noise_rms * 1.7))


def plausible_fallback(text: str, source: str) -> bool:
    if source == "zh":
        return sum("\u4e00" <= character <= "\u9fff" for character in text) >= 6
    words = re.findall(r"[A-Za-z]+(?:'[A-Za-z]+)?", text)
    return len(words) >= 3 and sum(len(word.replace("'", "")) for word in words) >= 12


def usable_transcript(text: str, source: str) -> bool:
    if source == "zh":
        return bool(re.search(r"[\u4e00-\u9fff0-9]", text))
    return bool(re.search(r"[A-Za-z0-9]", text))


def wav_bytes(pcm: bytes) -> bytes:
    buffer = BytesIO()
    with wave.open(buffer, "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(SAMPLE_RATE)
        output.writeframes(pcm)
    return buffer.getvalue()


def prepare_playback(audio: bytes) -> tuple[bytes, float]:
    """Normalize and resample in memory before sending PCM to the 48 kHz DAC."""
    started = time.monotonic()
    result = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", "pipe:0",
         "-af", "loudnorm=I=-18:TP=-3:LRA=11,aresample=resampler=soxr:precision=28",
         "-ar", "48000", "-ac", "2", "-f", "s16le", "pipe:1"],
        input=audio, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        timeout=30, check=False,
    )
    if result.returncode or not result.stdout:
        raise RuntimeError(result.stderr.decode(errors="replace")[:200] or
                           f"ffmpeg exited {result.returncode}")
    return result.stdout, round((time.monotonic() - started) * 1000, 1)


class AudioSegmenter:
    """Cut continuous speech without waiting for the speaker to stop."""

    def __init__(self) -> None:
        self.pre_roll = deque(maxlen=5)
        self.weak_roll = deque(maxlen=MAX_FRAMES)
        self.active = []
        self.hot_frames = 0
        self.voice_frames = 0
        self.quiet_frames = 0
        self.peak_level = 0.0
        self.noise = 45.0
        self.threshold = 100.0

    @property
    def speaking(self) -> bool:
        return bool(self.active)

    def feed(self, frame: bytes, level: float) -> tuple[bytes, str] | None:
        self.threshold = max(100.0, min(220.0, self.noise * 2.0))
        if not self.active:
            self.pre_roll.append(frame)
            self.weak_roll.append((frame, level))
            if level > self.threshold:
                self.hot_frames += 1
            else:
                self.hot_frames = 0
                self.noise = 0.995 * self.noise + 0.005 * level
            if self.hot_frames >= 2:
                self.active = list(self.pre_roll)
                self.voice_frames = self.hot_frames
                self.quiet_frames = 0
                self.peak_level = level
                self.weak_roll.clear()
                return None
            if len(self.weak_roll) == MAX_FRAMES:
                weak_threshold = max(80.0, self.noise * 1.25)
                levels = [item[1] for item in self.weak_roll]
                if (sum(value > weak_threshold for value in levels) >= 12 and
                        max(levels) > max(85.0, self.noise * 1.4)):
                    pcm = b"".join(item[0] for item in self.weak_roll)
                    self.weak_roll.clear()
                    self.pre_roll.clear()
                    return pcm, "low_level_fallback"
            return None

        self.active.append(frame)
        self.peak_level = max(self.peak_level, level)
        if level > self.threshold:
            self.voice_frames += 1
            self.quiet_frames = 0
        else:
            self.quiet_frames += 1
        strong = self.peak_level >= STRONG_RMS
        if len(self.active) >= (STRONG_MAX_FRAMES if strong else MAX_FRAMES):
            pcm = b"".join(self.active)
            # Keep context on strong speech; re-arm the gate after a weak window.
            tail = self.active[-3:]
            self.active = tail if strong else []
            self.pre_roll.clear()
            self.pre_roll.extend(tail)
            self.weak_roll.clear()
            self.hot_frames = 0
            self.voice_frames = 3 if strong else 0
            self.quiet_frames = 0
            self.peak_level = level if strong else 0.0
            return pcm, "max_duration"
        quiet_limit = SHORT_SILENCE_FRAMES if len(self.active) < MAX_FRAMES // 2 \
            else SILENCE_FRAMES
        if (self.quiet_frames >= quiet_limit and
                (self.peak_level >= STRONG_RMS or len(self.active) >= WEAK_MIN_FRAMES)):
            pcm = b"".join(self.active) if self.voice_frames >= 3 else b""
            self.active = []
            self.pre_roll.clear()
            self.weak_roll.clear()
            self.hot_frames = 0
            self.voice_frames = 0
            self.quiet_frames = 0
            self.peak_level = 0.0
            return (pcm, "silence") if pcm else None
        return None


class LiveState:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.instance_id = uuid.uuid4().hex
        self.started = time.monotonic()
        self.last_audio = 0.0
        self.mode = "starting"
        self.rms = 0.0
        self.threshold = 150.0
        self.noise = 45.0
        self.seq = 0
        self.events = deque(maxlen=1000)
        self.queue = Queue(maxsize=QUEUE_SIZE)
        self.tts_queue = Queue(maxsize=QUEUE_SIZE)
        self.playback_queue = Queue(maxsize=QUEUE_SIZE)
        self.source = "zh"
        self.playback_enabled = True
        try:
            saved = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
            if saved.get("source") in ("zh", "en"):
                self.source = saved["source"]
            if isinstance(saved.get("playback_enabled"), bool):
                self.playback_enabled = saved["playback_enabled"]
        except (FileNotFoundError, ValueError, OSError):
            pass
        self.headphones_connected = False
        self.playback_state = "idle"
        self.inference_state = "idle"
        self.tts_state = "idle"
        self.segments = 0
        self.recognized = 0
        self.empty = 0
        self.errors = 0
        self.dropped = 0
        self.capture_restarts = 0
        self.played = 0
        self.playback_skipped = 0
        self.playback_dropped = 0
        self.playback_errors = 0
        self.busy_retries = 0
        self.stt_fallback_attempts = 0
        self.stt_fallback_accepted = 0
        self.stt_fallback_rejected = 0
        self.last_tts_ms = 0.0
        self.last_resample_ms = 0.0
        self.last_playback_ms = 0.0
        self.last_segment_reason = ""
        self.last_segment_rms = 0.0
        self.forced_chunks = 0
        self.weak_fallback_chunks = 0
        self.last_error = ""
        self.last_stt_ms = 0.0
        self.last_translation_ms = 0.0
        self.last_display_delay_ms = 0.0
        self.capture_processes = None
        self.capture_thread = None
        self.worker_thread = None
        self.tts_thread = None
        self.playback_thread = None

    def configure(self, source: str, playback_enabled: bool) -> None:
        if source not in ("zh", "en") or not isinstance(playback_enabled, bool):
            raise ValueError("source must be zh/en and playback_enabled must be boolean")
        CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
        temporary = CONFIG_PATH.with_suffix(".tmp")
        temporary.write_text(json.dumps({"source": source, "playback_enabled": playback_enabled}),
                             encoding="utf-8")
        os.replace(temporary, CONFIG_PATH)
        with self.lock:
            self.source = source
            self.playback_enabled = playback_enabled
        self.event("config_changed", source=source, target="en" if source == "zh" else "zh",
                   playback_enabled=playback_enabled)

    def headphone_test(self) -> None:
        with self.lock:
            source = self.source
            target = "en" if source == "zh" else "zh"
            if not self.playback_enabled or not self.headphones_connected:
                raise ValueError("enable playback and plug in headphones first")
        if not self.queue_tts("The camera is off. Please close the door." if target == "en"
                              else "摄像头已经关闭，请把门关上。", target):
            raise ValueError("TTS queue is full")
        self.event("headphone_test_requested", target=target)

    def event(self, kind: str, **fields) -> None:
        with self.lock:
            self.seq += 1
            self.events.append({"seq": self.seq, "time": utc_now(), "kind": kind, **fields})

    def fail(self, message: str) -> None:
        with self.lock:
            self.errors += 1
            self.last_error = message[:300]
        self.event("error", message=message[:300])

    def snapshot(self, since: int = 0) -> dict:
        with self.lock:
            age = round(time.monotonic() - self.last_audio, 1) if self.last_audio else None
            return {
                "instance_id": self.instance_id,
                "uptime_sec": round(time.monotonic() - self.started, 1),
                "mode": self.mode,
                "rms": round(self.rms, 1),
                "threshold": round(self.threshold, 1),
                "noise_rms": round(self.noise, 1),
                "last_audio_age_sec": age,
                "queue_depth": self.queue.qsize(),
                "tts_queue_depth": self.tts_queue.qsize(),
                "playback_queue_depth": self.playback_queue.qsize(),
                "source": self.source,
                "target": "en" if self.source == "zh" else "zh",
                "playback_enabled": self.playback_enabled,
                "headphones_connected": self.headphones_connected,
                "playback_state": self.playback_state,
                "inference_state": self.inference_state,
                "tts_state": self.tts_state,
                "segments": self.segments,
                "recognized": self.recognized,
                "empty": self.empty,
                "errors": self.errors,
                "dropped": self.dropped,
                "capture_restarts": self.capture_restarts,
                "played": self.played,
                "playback_skipped": self.playback_skipped,
                "playback_dropped": self.playback_dropped,
                "playback_errors": self.playback_errors,
                "busy_retries": self.busy_retries,
                "stt_fallback_attempts": self.stt_fallback_attempts,
                "stt_fallback_accepted": self.stt_fallback_accepted,
                "stt_fallback_rejected": self.stt_fallback_rejected,
                "last_error": self.last_error,
                "last_stt_ms": self.last_stt_ms,
                "last_translation_ms": self.last_translation_ms,
                "last_tts_ms": self.last_tts_ms,
                "last_resample_ms": self.last_resample_ms,
                "last_playback_ms": self.last_playback_ms,
                "last_segment_reason": self.last_segment_reason,
                "last_segment_rms": self.last_segment_rms,
                "forced_chunks": self.forced_chunks,
                "weak_fallback_chunks": self.weak_fallback_chunks,
                "last_display_delay_ms": self.last_display_delay_ms,
                "seq": self.seq,
                "events": [item for item in self.events if item["seq"] > since],
            }

    def metrics(self) -> dict:
        result = self.snapshot(self.seq)
        result.pop("events")
        return result

    def post_bytes(self, request: Request, timeout: int) -> bytes:
        for attempt in range(4):
            try:
                with urlopen(request, timeout=timeout) as response:
                    return response.read()
            except HTTPError as error:
                detail = error.read().decode(errors="replace")[:200]
                if error.code == 503 and attempt < 3:
                    with self.lock:
                        self.busy_retries += 1
                    time.sleep(0.25 * (attempt + 1))
                    continue
                raise RuntimeError(f"HTTP {error.code}: {detail}") from error
        raise RuntimeError("upstream remained busy")

    def capture_loop(self) -> None:
        while True:
            try:
                self.capture_once()
            except Exception as error:
                self.fail("audio capture: " + str(error))
            with self.lock:
                self.mode = "reconnecting"
                self.capture_restarts += 1
            time.sleep(2)

    def capture_once(self) -> None:
        recorder = subprocess.Popen(
            ["arecord", "-q", "-D", DEVICE, "-f", "S16_LE", "-r", "48000", "-c", "2",
             "-t", "raw", "-"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        )
        assert recorder.stdout is not None
        converter = subprocess.Popen(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "s16le", "-ar", "48000",
             "-ac", "2", "-i", "pipe:0", "-af", "pan=mono|c0=c0", "-f", "s16le",
             "-ar", "16000", "-ac", "1", "-flush_packets", "1", "pipe:1"],
            stdin=recorder.stdout, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        )
        recorder.stdout.close()
        assert converter.stdout is not None
        with self.lock:
            self.capture_processes = (recorder, converter)
            self.last_audio = time.monotonic()
            self.mode = "listening"
        self.event("capture_started", device=DEVICE)
        segmenter = AudioSegmenter()
        try:
            while True:
                frame = converter.stdout.read(FRAME_BYTES)
                if len(frame) != FRAME_BYTES:
                    raise RuntimeError(f"audio pipeline stopped (got {len(frame)} bytes)")
                level = rms_pcm16(frame)
                segment = segmenter.feed(frame, level)
                with self.lock:
                    self.last_audio = time.monotonic()
                    self.rms = level
                    self.threshold = segmenter.threshold
                    self.noise = segmenter.noise
                    self.mode = "speaking" if segmenter.speaking else "listening"
                if segment:
                    self.enqueue(segment[0], time.monotonic(), segment[1])
        finally:
            with self.lock:
                self.capture_processes = None
            for process in (converter, recorder):
                if process.poll() is None:
                    process.kill()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    pass

    def enqueue(self, pcm: bytes, ended: float, reason: str) -> None:
        segment_rms = round(rms_pcm16(pcm), 1)
        samples = array("h")
        samples.frombytes(pcm)
        peak_sample = max((abs(value) for value in samples), default=0)
        with self.lock:
            self.segments += 1
            source = self.source
            target = "en" if source == "zh" else "zh"
            noise_rms = self.noise
            self.last_segment_reason = reason
            self.last_segment_rms = segment_rms
            if reason == "max_duration":
                self.forced_chunks += 1
            elif reason == "low_level_fallback":
                self.weak_fallback_chunks += 1
        try:
            self.queue.put_nowait((pcm, ended, source, target, reason, segment_rms,
                                   noise_rms))
            self.event("segment_queued", reason=reason,
                       audio_sec=round(len(pcm) / (SAMPLE_RATE * 2), 1),
                       rms=segment_rms, noise_rms=round(noise_rms, 1),
                       peak_sample=peak_sample,
                       queue_depth=self.queue.qsize())
        except Full:
            with self.lock:
                self.dropped += 1
            self.event("dropped", reason="inference queue full")

    def worker_loop(self) -> None:
        while True:
            try:
                pcm, ended, source, target, reason, segment_rms, noise_rms = \
                    self.queue.get(timeout=1)
            except Empty:
                continue
            started = time.monotonic()
            with self.lock:
                self.inference_state = "processing"
            try:
                body = wav_bytes(pcm)
                request = Request(
                    f"http://127.0.0.1:8767/v1/speech-translate?source={source}&target={target}",
                    body, {"Content-Type": "audio/wav"},
                )
                result = json.loads(self.post_bytes(request, 180))
                transcript = result.get("transcript", "")
                translation = result.get("translation", "")
                if transcript and not usable_transcript(transcript, source):
                    self.event("recognition_rejected", source=source,
                               transcript=transcript[:80], reason="no source-language text")
                    transcript = ""
                    translation = ""
                stt_strategy = "gate"
                audio_sec = len(pcm) / (SAMPLE_RATE * 2)
                if not transcript and should_retry_ungated(
                        reason, audio_sec, segment_rms, noise_rms):
                    with self.lock:
                        self.stt_fallback_attempts += 1
                    off_request = Request("http://127.0.0.1:8765/v1/stt?strategy=off",
                                          body, {"Content-Type": "audio/wav"})
                    off_result = json.loads(self.post_bytes(off_request, 40))
                    candidate = off_result.get("text", "").strip()
                    if plausible_fallback(candidate, source):
                        payload = json.dumps({"text": candidate, "source": source,
                                              "target": target},
                                             ensure_ascii=False).encode("utf-8")
                        text_request = Request(
                            "http://127.0.0.1:8767/v1/translate", payload,
                            {"Content-Type": "application/json"},
                        )
                        translated = json.loads(self.post_bytes(text_request, 180))
                        transcript = candidate
                        translation = translated.get("translation", "")
                        result["stt_ms"] = (float(result.get("stt_ms", 0)) +
                                            float(off_result.get("latency_ms", 0)))
                        result["translation_ms"] = translated.get("translation_ms", 0)
                        result["translation_repaired"] = translated.get("translation_repaired", False)
                        stt_strategy = "off_fallback"
                        with self.lock:
                            self.stt_fallback_accepted += 1
                        self.event("stt_fallback_accepted", source=source,
                                   audio_sec=round(audio_sec, 1),
                                   segment_rms=segment_rms, transcript=candidate)
                    else:
                        with self.lock:
                            self.stt_fallback_rejected += 1
                        self.event("stt_fallback_rejected", source=source,
                                   audio_sec=round(audio_sec, 1),
                                   segment_rms=segment_rms, candidate=candidate[:80])
                delay = round((time.monotonic() - ended) * 1000, 1)
                with self.lock:
                    if transcript:
                        self.last_stt_ms = float(result.get("stt_ms", 0))
                        self.last_translation_ms = float(result.get("translation_ms", 0))
                        self.last_display_delay_ms = delay
                        self.recognized += 1
                        self.last_error = ""
                    else:
                        self.empty += 1
                if transcript:
                    self.event(
                        "translation", source=source, target=target,
                        transcript=transcript, translation=translation,
                        segment_reason=reason, segment_rms=segment_rms,
                        stt_strategy=stt_strategy,
                        translation_repaired=bool(result.get("translation_repaired", False)),
                        audio_sec=round(len(pcm) / (SAMPLE_RATE * 2), 1),
                        stt_ms=result.get("stt_ms", 0),
                        translation_ms=result.get("translation_ms", 0),
                        processing_ms=round((time.monotonic() - started) * 1000, 1),
                        display_delay_ms=delay,
                    )
                else:
                    self.event("recognition_empty", source=source,
                               segment_reason=reason, segment_rms=segment_rms,
                               audio_sec=round(len(pcm) / (SAMPLE_RATE * 2), 1),
                               stt_ms=result.get("stt_ms", 0))
                if translation:
                    with self.lock:
                        should_play = self.playback_enabled and self.headphones_connected
                        skip_reason = "headphones unplugged" if not self.headphones_connected \
                            else "playback disabled"
                    if should_play:
                        self.queue_tts(translation, target)
                    else:
                        with self.lock:
                            self.playback_skipped += 1
                        self.event("playback_skipped", reason=skip_reason, target=target)
            except Exception as error:
                self.fail("inference: " + str(error))
            finally:
                with self.lock:
                    self.inference_state = "idle"
                self.queue.task_done()

    def queue_tts(self, translation: str, target: str) -> bool:
        try:
            self.tts_queue.put_nowait((translation, target))
            self.event("tts_queued", target=target, queue_depth=self.tts_queue.qsize())
            return True
        except Full:
            with self.lock:
                self.playback_dropped += 1
            self.event("playback_dropped", reason="TTS queue full", target=target)
            return False

    def tts_loop(self) -> None:
        while True:
            try:
                translation, target = self.tts_queue.get(timeout=1)
            except Empty:
                continue
            with self.lock:
                self.tts_state = "synthesizing"
            try:
                self.synthesize_and_queue(translation, target)
            finally:
                with self.lock:
                    self.tts_state = "idle"
                self.tts_queue.task_done()

    def synthesize_and_queue(self, translation: str, target: str) -> None:
        try:
            started = time.monotonic()
            request = Request(
                f"http://127.0.0.1:8768/v1/tts/{target}",
                translation.encode("utf-8"),
                {"Content-Type": "text/plain; charset=utf-8"},
            )
            audio = self.post_bytes(request, 60)
            if not audio.startswith(b"RIFF"):
                raise RuntimeError("TTS did not return a WAV file")
            tts_ms = round((time.monotonic() - started) * 1000, 1)
            with self.lock:
                self.last_tts_ms = tts_ms
            try:
                self.playback_queue.put_nowait((audio, target))
                self.event("tts_ready", target=target, tts_ms=tts_ms, wav_bytes=len(audio))
            except Full:
                with self.lock:
                    self.playback_dropped += 1
                self.event("playback_dropped", reason="playback queue full", target=target)
        except Exception as error:
            with self.lock:
                self.playback_errors += 1
            self.fail("tts: " + str(error))

    def playback_loop(self) -> None:
        while True:
            try:
                audio, target = self.playback_queue.get(timeout=1)
            except Empty:
                continue
            try:
                plugged = headphone_connected()
                with self.lock:
                    self.headphones_connected = plugged
                    enabled = self.playback_enabled
                if not enabled or not plugged:
                    with self.lock:
                        self.playback_skipped += 1
                    self.event("playback_skipped", reason="headphones unplugged" if not plugged
                               else "playback disabled", target=target)
                    continue
                with self.lock:
                    self.playback_state = "playing"
                started = time.monotonic()
                pcm, resample_ms = prepare_playback(audio)
                result = subprocess.run(
                    ["aplay", "-q", "-D", PLAY_DEVICE, "-t", "raw", "-f", "S16_LE",
                     "-r", "48000", "-c", "2", "-"], input=pcm,
                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                    timeout=90, check=False,
                )
                if result.returncode:
                    raise RuntimeError(result.stderr.decode(errors="replace")[:200] or
                                       f"aplay exited {result.returncode}")
                playback_ms = round((time.monotonic() - started) * 1000, 1)
                with self.lock:
                    self.played += 1
                    self.last_resample_ms = resample_ms
                    self.last_playback_ms = playback_ms
                self.event("playback_finished", target=target,
                           resample_ms=resample_ms, playback_ms=playback_ms)
            except Exception as error:
                with self.lock:
                    self.playback_errors += 1
                self.fail("playback: " + str(error))
            finally:
                with self.lock:
                    self.playback_state = "idle"
                self.playback_queue.task_done()

    def watchdog_loop(self) -> None:
        while True:
            time.sleep(5)
            try:
                plugged = headphone_connected()
                with self.lock:
                    self.headphones_connected = plugged
            except Exception:
                with self.lock:
                    self.headphones_connected = False
            with self.lock:
                age = time.monotonic() - self.last_audio if self.last_audio else 0
                processes = self.capture_processes
            if age > 12 and processes:
                self.fail(f"audio capture stalled for {age:.1f}s; restarting pipeline")
                for process in processes:
                    if process.poll() is None:
                        process.kill()

    def start(self) -> None:
        try:
            self.headphones_connected = headphone_connected()
        except Exception:
            self.headphones_connected = False
        self.capture_thread = threading.Thread(target=self.capture_loop, name="capture", daemon=True)
        self.worker_thread = threading.Thread(target=self.worker_loop, name="inference", daemon=True)
        self.tts_thread = threading.Thread(target=self.tts_loop, name="tts", daemon=True)
        self.playback_thread = threading.Thread(target=self.playback_loop, name="playback", daemon=True)
        self.capture_thread.start()
        self.worker_thread.start()
        self.tts_thread.start()
        self.playback_thread.start()
        threading.Thread(target=self.watchdog_loop, name="watchdog", daemon=True).start()


STATE = LiveState()


class Handler(BaseHTTPRequestHandler):
    def do_POST(self) -> None:
        route = urlsplit(self.path).path
        if route == "/api/headphone-test":
            try:
                STATE.headphone_test()
                self.send_json({"queued": True})
            except ValueError as error:
                self.send_bytes(409, str(error).encode(), "text/plain; charset=utf-8")
            return
        if route != "/api/config":
            self.send_bytes(404, b"not found\n", "text/plain")
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 1 <= length <= 1024 or not self.headers.get("Content-Type", "").startswith("application/json"):
                raise ValueError("expected a JSON body of at most 1024 bytes")
            data = json.loads(self.rfile.read(length))
            STATE.configure(data.get("source"), data.get("playback_enabled"))
            self.send_json(STATE.metrics())
        except (ValueError, AttributeError) as error:
            self.send_bytes(400, str(error).encode(), "text/plain; charset=utf-8")
        except OSError as error:
            self.send_bytes(500, str(error).encode(), "text/plain; charset=utf-8")

    def do_GET(self) -> None:
        route = urlsplit(self.path)
        if route.path == "/":
            self.send_bytes(200, HTML, "text/html; charset=utf-8")
        elif route.path == "/api/state":
            try:
                since = max(0, int(parse_qs(route.query).get("since", ["0"])[0]))
            except ValueError:
                since = 0
            self.send_json(STATE.snapshot(since))
        elif route.path == "/api/metrics":
            self.send_json(STATE.metrics())
        elif route.path == "/healthz":
            status = STATE.metrics()
            healthy = (status["last_audio_age_sec"] is not None and
                       status["last_audio_age_sec"] < 12 and
                       STATE.capture_thread.is_alive() and STATE.worker_thread.is_alive() and
                       STATE.tts_thread.is_alive() and
                       STATE.playback_thread.is_alive())
            self.send_bytes(200 if healthy else 503, b"ok\n" if healthy else b"unhealthy\n",
                            "text/plain")
        else:
            self.send_bytes(404, b"not found\n", "text/plain")

    def send_json(self, value: dict) -> None:
        self.send_bytes(200, json.dumps(value, ensure_ascii=False).encode(), "application/json; charset=utf-8")

    def send_bytes(self, status: int, body: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, format: str, *args) -> None:
        pass


def main() -> None:
    if os.sys.byteorder != "little":
        raise RuntimeError("PCM16 decoder requires a little-endian host")
    STATE.start()
    server = ThreadingHTTPServer(("127.0.0.1", 8770), Handler)
    server.daemon_threads = True
    server.serve_forever(poll_interval=0.5)


if __name__ == "__main__":
    main()
