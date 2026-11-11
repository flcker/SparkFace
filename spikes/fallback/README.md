# Phase-0 Spike B2/B3/B4 fallback probe

This evidence-only probe measures the CPU fallback transport after the copy-free
Spike B path failed. It is not product code and must not be merged into `main`.
Spike B4 replaces the B3 synchronous `RequestSample` pacing experiments with an
event-driven media source; it does not by itself change the B/B2 NO-GO verdicts.

## Pinned baseline

- Windows 11 build 22000 or newer
- Microsoft Windows-Camera reference commit:
  `626f8b19c5f367602f2e89c6b314573d3776c9df`
- bgfx vcpkg port: `1.129.8940-496#1`
- MSVC x64 Developer PowerShell
- Output: NV12 1920x1080 at 30fps

Files derived from Microsoft Windows-Camera retain their Microsoft copyright
headers and are covered by `media-source/LICENSE-Microsoft.txt`.

The ring is backed by `C:\Users\Public\SparkFacePhase0B2Ring.bin` so the
interactive producer and the Frame Server media-source process map the same
pages across Windows sessions. The fixed path and permissive public location
are probe-only choices, not a product transport or security design. All frame
buffers and three ring slots are allocated before the measured loop.

## Build

```powershell
cmake -S . -B build/windows-x64-release -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DSPARKFACE_BUILD_ENV_GATE=OFF
cmake --build build/windows-x64-release --target `
  fallback_producer fallback_consumer fallback_mf_consumer fallback_registrar
```

Build the pinned Microsoft media source and apply the retained patch from a
Visual Studio Developer PowerShell:

```powershell
spikes/fallback/media-source/build-upstream.ps1
```

The script verifies the pinned upstream archive SHA-256, restores the exact
NuGet dependencies, applies `windows-camera-b2.patch`, and builds the x64
Release media-source DLL. No upstream checkout is stored in Git.

The pinned bgfx vcpkg build must retain its `shaderc.exe` build tool under
`$env:VCPKG_ROOT/buildtrees/bgfx`.

## Direct transport validation

Start the producer and consumer concurrently:

```powershell
build/windows-x64-release/spikes/fallback/fallback_producer.exe --frames 1020
build/windows-x64-release/spikes/fallback/fallback_consumer.exe --frames 1000
```

Ten-minute producer soak:

```powershell
build/windows-x64-release/spikes/fallback/fallback_producer.exe --soak-seconds 600
```

The producer reports D3D11 readback, bgfx roundtrip, NV12 conversion, ring
publication, total p50/p95/p99/max, achieved fps, process CPU, missed 33.3ms
deadlines, and content validation. These numbers are a synthetic transport
floor; they do not include a selected face model or production beauty shaders.

## Virtual-camera lifecycle

Install/remove the camera from an elevated terminal; producer and consumers run
without elevation. The DLL path below is emitted by `build-upstream.ps1`.

```powershell
build/windows-x64-release/spikes/fallback/fallback_registrar.exe install <media-source-dll>
build/windows-x64-release/spikes/fallback/fallback_registrar.exe list
build/windows-x64-release/spikes/fallback/fallback_registrar.exe remove
```

## Spike B4 asynchronous media-source pacing

The B4 media source makes `RequestSample` non-blocking. Each request is appended
to a fixed 16-entry queue and returns immediately. A private serial Media
Foundation work queue waits on both:

- the producer's auto-reset `Local\SparkFacePhase0B2FrameReady` event, signaled
  after each complete ring publication; and
- a 33ms periodic waitable timer, used as the no-frame deadline.

A frame-ready wake completes the oldest pending request only when a new,
complete, non-stale sequence is available. A deadline wake emits black when the
ring is absent or its newest sequence is stale. Input becomes stale after 66ms.
Pause, stop, and shutdown cancel both waits and flush pending requests. The
accepted v1 single-consumer contract is required because the auto-reset event
wakes one media-source waiter.

The B3 mode file is ignored by B4. Before each run, close OBS, Tencent Meeting,
and every other camera client, then restart Frame Server so it loads the rebuilt
media-source DLL:

```powershell
Remove-Item C:\Users\Public\SparkFacePhase0B2Mode.txt -ErrorAction SilentlyContinue
Get-Process FrameServer -ErrorAction SilentlyContinue | Stop-Process -Force
```

Run the 1,000-frame gate with the producer and consumer in separate terminals:

```powershell
# Terminal A
build/windows-x64-release/spikes/fallback/fallback_producer.exe --frames 1500
# Terminal B, after Terminal A starts
build/windows-x64-release/spikes/fallback/fallback_mf_consumer.exe `
  --frames 1000 --pacing-mode async
```

The gate requires `mf_frames=1000`, `mf_b4_diagnostic_frames=1000`,
`mf_pacing_mode=async`, zero marker and duplicate errors, at least 29.5 achieved
fps, and `producer_to_client_frame_age_p95_ms` at or below 33.3ms. The elapsed
time and achieved fps are measured around the complete source-reader loop, not
inferred from sample timestamps.

The producer count intentionally leaves 500 frames of startup and under-rate
headroom. The consumer releases the Source Reader and media-source references
before `IMFActivate::ShutdownObject`; releasing the Source Reader already shuts
down its source, so an additional direct `IMFMediaSource::Shutdown` call returns
`MF_E_SHUTDOWN` and can hide an otherwise complete gate result.

Validate idle black and producer recovery separately:

```powershell
build/windows-x64-release/spikes/fallback/fallback_mf_consumer.exe `
  --frames 400 --expect-recovery --pacing-mode async
```

While that consumer remains open, stop and restart the producer. The command
fails unless it observes at least three idle black frames followed by a valid
recovered frame.

### Payload diagnostics and metrics

The diagnostic NV12 payload preserves the B2 marker and checksum:

| Bytes | Field | Writer boundary |
| --- | --- | --- |
| 0..3 | `"SFB2"` marker | producer |
| 4..11 | sequence | producer |
| 12..15 | checksum | producer |
| 16..23 | producer QPC | producer |
| 24..31 | source copy-complete QPC | media source (B2 field) |
| 32..39 | published QPC | producer value copied by media source |
| 40..47 | request-entry QPC | media-source `RequestSample` entry |
| 48..55 | sequence-acquired QPC | complete, non-stale ring slot accepted |
| 56..63 | source copy-complete QPC | payload copy completed |
| 64..71 | sample-payload-finalized QPC | payload metadata written before buffer unlock |

The consumer reads B4 QPCs only when the sample has at least 72 bytes. A B4
diagnostic is valid when producer≤published≤sequence-acquired and
request-entry≤sequence-acquired≤copy-complete≤sample-payload-finalized. It
reports p50/p95/p99/max for producer-to-publish, source wait, source copy,
source-copy-to-sample-payload-finalized, Frame-Server/client delivery, and
producer-to-client frame age, plus elapsed time, achieved fps, sequence,
content, duplicate, skipped, idle-black, recovery, and pacing-mode results.
Published age at request is reported only when the frame was published at or
before request entry; frames published after request are counted explicitly as
`mf_published_after_request_sequences`. Empty segments print `n/a`.

`sample-payload-finalized` is deliberately not called sample enqueue: the
sample buffer must be unlocked before `QueueEventParamUnk`, so writing an exact
enqueue QPC would require unsafe post-unlock payload mutation. Consequently,
Frame-Server/client delivery starts at payload finalization and the
source-copy-to-payload-finalized gap is the nearest safe source-ready boundary.

Only after the 1,000-frame gate passes should B4 be tested with OBS and Tencent
Meeting separately. Run the 10-minute soak only if both real applications move
frames and the automated p95/fps gate passes; otherwise retain the B/B2 NO-GO.
The applications must remain separate because v1 permits one active consumer.

### B4 result

The 1,000-frame gate completed on the Intel Iris Xe reference machine after the
consumer shutdown-order fix. The clean, non-tracing media source produced the
following result:

| Gate | Required | Observed | Result |
| --- | ---: | ---: | --- |
| Valid frames | 1,000 | 1,000 | pass |
| B4 diagnostic frames | 1,000 | 1,000 | pass |
| Marker errors | 0 | 0 | pass |
| Duplicate sequences | 0 | 0 | pass |
| Achieved frame rate | >=29.5fps | 26.644fps | fail |
| Producer-to-client frame-age p95 | <=33.3ms | 48.758ms | fail |

The run skipped 125 producer sequences. Its producer remained healthy at
29.992fps with `transport_total_p95_ms=22.920`, while the media-source consumer
reported `source_wait_p95_ms=63.205`. B4 is therefore **NO-GO**: asynchronous
`RequestSample` removes the synchronous stall but does not meet the 30fps or
33.3ms end-to-end gates. Recovery, OBS, Tencent Meeting, and the 10-minute soak
were not run after the automated gate failed. The B/B2 NO-GO verdict remains.
