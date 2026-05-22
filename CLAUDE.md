# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

ZLMediaKit is a C++11 high-performance streaming media framework and server (`MediaServer`). It bridges live streaming and surveillance protocols (RTSP/RTMP/HLS/HTTP-FLV/HTTP-TS/HTTP-fMP4/MP4/WebRTC/SRT/GB28181) and provides protocol conversion among them. It ships as:

- A static library `zlmediakit` (built from `src/`) — the core engine.
- A shared library `mk_api` (`api/`) — C ABI SDK over the core, usable from C/C++ and other languages (a Go binding lives in `golang/`).
- A `MediaServer` executable (`server/`) wiring all servers + REST API + webhooks + optional Python plugin.

The project is built on the sibling [`ZLToolKit`](https://github.com/ZLMediaKit/ZLToolKit) network/util library, which is pulled in as a submodule and supplies the `toolkit::` namespace (event poller, sockets, `TcpServer`, `UdpServer`, `mINI` config, `NoticeCenter` for broadcasts, `RingBuffer`, logging macros `InfoL/WarnL/ErrorL`).

## Build & development

### Required submodules

`git clone` alone is not sufficient — third-party code under `3rdpart/` (`ZLToolKit`, `media-server`, `jsoncpp`, `pybind11`) and `www/webassist` are git submodules and must be fetched:

```bash
git submodule update --init
```

CI uses `.gitmodules_github` (GitHub mirrors) instead of `.gitmodules` (Gitee defaults); if Gitee URLs hang in a foreign network: `mv -f .gitmodules_github .gitmodules && git submodule sync && git submodule update --init`.

WebRTC additionally needs `libsrtp`, `usrsctp`, and OpenSSL installed system-wide (see `.github/workflows/linux.yml` for the exact recipe used in CI).

### Standard build (Linux/macOS)

The `build_for_linux.sh` / `build_for_mac.sh` scripts referenced in the README are not present in-tree; use CMake directly:

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Build artifacts (libraries, the `MediaServer` executable, and tests) land in `release/${os}/${BuildType}/` (e.g., `release/linux/Release/`). `cmake` also copies `conf/config.ini`, `default.pem`, and `www/` into that directory — `MediaServer` reads `config.ini` from its own directory by default, so **edit `release/linux/${BuildType}/config.ini`, not `conf/config.ini`**, unless you pass `-c /path/to/config.ini`.

### Key CMake options

Defined at the top of `CMakeLists.txt`. Defaults shown:

| Option | Default | Notes |
|---|---|---|
| `ENABLE_OPENSSL` | ON | Required for HTTPS/RTSPS/RTMPS/WebRTC. If absent, `ENABLE_WEBRTC` is force-disabled. |
| `ENABLE_WEBRTC` | ON | Requires OpenSSL + libsrtp. |
| `ENABLE_SRT` | ON | |
| `ENABLE_HLS` / `ENABLE_MP4` / `ENABLE_RTPPROXY` | ON | |
| `ENABLE_API` | ON | Builds `mk_api` shared lib. `ENABLE_API_STATIC_LIB=ON` to build static. |
| `ENABLE_CXX_API` | OFF | Install C++ headers (mostly unused). |
| `ENABLE_SERVER` | ON | Builds `MediaServer` executable. `ENABLE_SERVER_LIB=ON` builds a static `MediaServer` library instead (used by the Android build). |
| `ENABLE_TESTS` | ON | Builds the demos in `tests/` as separate executables. |
| `ENABLE_FFMPEG` | OFF | Required for `ENABLE_VIDEOSTACK` and the FFmpeg-based proxy / transcode / snapshot in `MediaServer`. |
| `ENABLE_PYTHON` | OFF | Builds the `pyinvoker` plugin into `MediaServer`. |
| `ENABLE_ASAN` | OFF | Adds `-fsanitize=address`. |

### Running a single test

Each `.cpp` file at the top level of `tests/` is compiled into its own executable of the same name (`tests/CMakeLists.txt` iterates with `aux_source_directory`). To build and run just one, e.g. `test_rtp`:

```bash
cd build
make test_rtp -j$(nproc)
./release/linux/Release/test_rtp
```

`tests/DeviceHK/` is **not** auto-compiled (it depends on the Hikvision SDK). The `test_rtp_pcap` target is skipped if libpcap is missing; `test_rtcp_nack` is skipped if WebRTC is disabled. The `tests/` directory is demos/benchmarks, not a unit-test suite — there is no `ctest` integration.

### Docker

`dockerfile` (root) and `dockerfile_py` build the production image; `docker/centos7/Dockerfile.runtime` and `docker/ubuntu18.04/{Dockerfile.devel,Dockerfile.runtime}` are also maintained. `build_docker_images.sh` builds them locally.

## Code architecture

### Two-namespace split

- `toolkit::` — everything imported from `ZLToolKit` (under `3rdpart/ZLToolKit`). Network IO, event loop, sockets, INI config, logger, the broadcast `NoticeCenter`.
- `mediakit::` — everything in this repo. Protocols, muxers, demuxers, sources, the server.

When implementing new protocol features, you'll inherit from toolkit primitives like `toolkit::Session` / `toolkit::TcpServer` / `toolkit::UdpServer` and plug into the mediakit `MediaSource` / `MediaSink` graph.

### Module layout under `src/`

| Module | Purpose |
|---|---|
| `Common/` | `MediaSource` (the registry/abstraction every protocol exposes), `MediaSink`, `MultiMediaSourceMuxer` (the fan-out muxer that re-packages one input into many output protocols), `config.h` (all config keys + all `kBroadcastXxx` event names), `Stamp` (DTS/PTS smoothing), `Parser` (URL/header parsing). |
| `Extension/` | `Frame` (raw NAL/audio frame abstraction), `Track` (per-codec descriptor), `Factory` (creates RTP/RTMP codec adapters for each `CodecId`). |
| `Rtsp/`, `Rtmp/`, `Http/`, `Rtp/`, `Rtcp/`, `TS/`, `FMP4/`, `Record/` | Per-protocol stack. Each typically has: `*Session` (server-side per-connection state), `*Player` + `*PlayerImp` (client), `*Pusher` (publish client), `*Muxer` + `*MediaSource` + `*MediaSourceImp` + `*MediaSourceMuxer` (output side), `*Demuxer` (input side), and a protocol `*Splitter`/`*Protocol`. `Record/` covers MP4/HLS/FLV recording, MP4 VOD playback, and the HLS maker. |
| `Player/`, `Pusher/` | High-level `MediaPlayer`/`PlayerProxy` and `MediaPusher`/`PusherProxy` that auto-select the protocol from a URL. |
| `Codec/` | Optional encoders (AAC, H264) — only built when matching deps are present. |
| `Onvif/`, `Shell/` | ONVIF SOAP helpers and the telnet shell server. |

### Sibling top-level modules

- `ext-codec/` — codec-specific RTP/RTMP packer/unpacker pairs (`H264Rtp`, `AACRtmp`, `Opus`, `VP9Rtp`, `JPEGRtp`, `AV1Rtp`, …). Adding a new codec means dropping `<Codec>.{h,cpp}`, `<Codec>Rtp.{h,cpp}`, optionally `<Codec>Rtmp.{h,cpp}` here, adding it to `CODEC_MAP` in `src/Extension/Frame.h`, and registering it in `src/Extension/Factory.cpp`.
- `webrtc/` — DTLS/SRTP/ICE/SCTP/STUN, `WebRtcTransport` hierarchy, `WebRtcSession`, `WebRtcPusher`/`WebRtcPlayer`/`WebRtcEchoTest`/`WebRtcTalk` plus WHIP/WHEP signaling sessions. Only compiled when `ENABLE_WEBRTC` is on.
- `srt/` — SRT protocol (handshake, packet queues, NACK, ACK, crypto) and `SrtTransport`/`SrtSession`/`SrtPlayer`/`SrtPusher`.
- `api/` — `mk_api` C SDK. Headers in `api/include/mk_*.h`, one umbrella header `mk_mediakit.h`. C example consumers in `api/tests/`.
- `server/` — `MediaServer` main. `main.cpp` wires up TCP/UDP servers for every protocol; `WebApi.cpp` implements the REST API; `WebHook.cpp` posts events to user-configured HTTP endpoints; `FFmpegSource.cpp` shells out to FFmpeg for pull-as-proxy; `Process.cpp` manages child processes; `pyinvoker.cpp` exposes pybind11-based Python plugins.
- `player/` — A small SDL2/FFmpeg-based desktop player demo (`ENABLE_PLAYER AND ENABLE_FFMPEG`).
- `tests/` — Demos and benchmarks; not a unit-test suite.
- `golang/` — Cgo wrapper over `mk_api`.
- `Android/` — Gradle/AAR project that consumes `MediaServer` built as a static lib (`ENABLE_SERVER_LIB=ON`).

### Central data-flow pattern

For nearly every input-to-output pipeline:

1. A protocol input (e.g. `RtspSession` receiving RTP, `RtmpSession` receiving FLV tags, `RtpProcess` receiving GB28181 PS) demuxes the byte stream into `Frame::Ptr` objects per `Track`.
2. Frames are pushed into a `MultiMediaSourceMuxer` (one per `vhost/app/stream` tuple).
3. The muxer fans frames out to per-protocol `*MediaSourceMuxer` instances (RTSP/RTMP/TS/FMP4/HLS/MP4-record/RTP-send), each of which packs into the wire format and exposes a `*MediaSource` registered globally via `MediaSource::findAsync` / `MediaSource::regist`.
4. Output protocol sessions / players resolve the `MediaSource` by URL and subscribe through its `RingBuffer<Packet>` consumer.

Whether each fan-out branch actually runs is gated by `ProtocolOption` (per-stream toggle: `enable_rtsp`, `enable_rtmp`, `enable_hls`, `enable_mp4`, `enable_audio`, `add_mute_audio`, …) and by demand — most branches are lazily started when a reader appears (`onReaderChanged`) and torn down when the reader count drops to zero. Keep this lifecycle in mind: don't assume a downstream muxer exists at any given moment, use the standard `getOrCreate*` accessors.

### Event/broadcast bus

Server-side hooks are dispatched via `toolkit::NoticeCenter`. Every event name lives as a `kBroadcastXxx` constant in `src/Common/config.h`, with its callback signature defined by an adjacent `#define BroadcastXxxArgs ...` macro. Subscribe in code with `NoticeCenter::Instance().addListener(tag, kBroadcastXxx, [](BroadcastXxxArgs) { ... })`. `MediaServer`'s `WebHook.cpp` translates each of these into an outbound HTTP POST. When adding a new server-side event, add the `kBroadcast...` + `Args` macro pair in `config.h`, fire it from the protocol layer, and (if it should be exposed to users) wire it into `WebHook.cpp`.

### Config system

`mediakit::mINI` (from ZLToolKit) is a global singleton holding all config values. Each subsystem registers its keys and defaults in an `onceToken` block (see the top of `server/main.cpp` and the `namespace`d blocks throughout `src/Common/config.h`, `src/Rtsp/`, etc.). `conf/config.ini` is a *documented sample* that gets copied to the build output; the canonical defaults live in the C++ source. Reloading at runtime fires `kBroadcastReloadConfig`.

## Conventions

### Comments are bilingual

Chinese-original comments are kept and English translations are placed **immediately below** the Chinese, with the marker `[AUTO-TRANSLATED:<hash>]` on the last line of the Chinese block. This format is enforced by the project's translation skill (`.claude/skills/translation/SKILL.md`) — when adding or editing comments in a file that already follows this pattern, preserve it. For any Chinese→English translation work, **follow `.claude/skills/translation/SKILL.md` strictly**: it has a hardcoded terminology dictionary (e.g. `源站`→`Origin server`, `合并写`→`Write coalescing`, `花屏`→`Visual artifacts (glitches)`, never "Screen tearing"), bans Chinglish patterns, and requires block-uniform (not line-interleaved) bilingual formatting.

### UTF-8 BOM is required on C/C++ source

`.github/workflows/style.yml` (the `style check` PR action) **fails the build if any modified `.c`/`.cc`/`.cpp`/`.h` file is missing a UTF-8 BOM** (`file <name>` should report `UTF-8 (with BOM) text`). When creating a new source file, ensure it has a BOM — the simplest path is to copy an existing header and edit, rather than writing one from scratch. The `Write` tool produces BOM-less files; if you create files this way, prepend the BOM (`printf '\xef\xbb\xbf' > file` then append, or use `sed -i '1s/^/\xef\xbb\xbf/' file`) before committing.

### Formatting

`clang-format` ≥ 9 with `.clang-format` (WebKit-based, 4-space indent, 160-column limit, pointer-right `T *x`, braces on same line for control structures, namespaces unindented). Use `clang-format -i <files>` when adding non-trivial new code.

### Standard file header

Every source file in the project starts with the MIT-license header found in `src/Common/MediaSource.h`. Keep it on new files you add.

### Naming

- C++ classes are `UpperCamelCase`; methods are `lowerCamelCase`; member fields are `_snake_case` (leading underscore).
- The C API in `api/` uses `mk_<module>_<verb>` exclusively (e.g. `mk_media_create`, `mk_player_play`).
- Config keys use dot notation (`http.port`, `rtsp.authBasic`) and the C++ constants follow `kXxx` naming (`Http::kPort`).

## CI

See `.github/workflows/`:

- `linux.yml`, `macos.yml`, `windows.yml`, `android.yml` — release builds with WebRTC/OpenSSL/SRTP/SCTP; upload artifacts to a tracking issue.
- `linux_py.yml`, `macos_py.yml`, `windows_py.yml`, `docker_py.yml` — same with `ENABLE_PYTHON=ON`.
- `docker.yml` — pushes `zlmediakit/zlmediakit:<branch>` images.
- `codeql.yml` — security scan.
- `style.yml` — the BOM check above. Runs only on PRs.
- `issue_lint.yml` — issue template enforcement.
