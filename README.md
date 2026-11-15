# Edge Audio/Video Gateways

Two C++17/Linux projects developed for an embedded audio and video pipeline.
This repository tracks source code, tests, deployment scripts, and engineering
notes. Model weights, generated recordings, build products, board images, and
local development caches are excluded.

| Project | Current state | Main components |
| --- | --- | --- |
| [Speech Translation Gateway](speech-translation-gateway/README.md) | Runs on Ubuntu and Orange Pi 5 Pro (RK3588S) | SenseVoice STT, Silero VAD, Qwen3 translation, Kokoro TTS, C++ HTTP services, live microphone UI, systemd monitoring |
| [Video Event Gateway](video-event-gateway/README.md) | OBS RTMP → C++ delayed gate → HLS/RTSP on Orange Pi; publish auth and experimental STT text rules verified; [Qt VM viewer](video-event-gateway/qt-viewer/README.md) verified; real moderation categories still planned | FFmpeg packet gate and audio decode, ZLMediaKit HLS, SenseVoice STT reuse, Qt/GStreamer client, earlier OpenCV event prototype |

The speech project supports Chinese↔English translation with headphone playback.
Its [board deployment notes](speech-translation-gateway/docs/board_deployment.md)
include measured latency, recognition errors, thermal limits, and the recovery
steps used during testing. The video project's [design notes](video-event-gateway/docs/design.md)
describe its pipeline and current limitations.

Each project has its own build and test instructions. Downloads of open model
weights and third-party runtime packages are handled by project scripts; they
are not redistributed here.
