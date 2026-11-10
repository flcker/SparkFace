# Phase-0 Spike B2/B3 fallback probe

This evidence-only probe measures the CPU fallback transport after the copy-free
Spike B path failed. It is not product code and must not be merged into `main`.
Spike B3 compares source pacing only; it does not change the B/B2 NO-GO verdicts
or prove a product pipeline.

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

## Spike B3 pacing comparison

The media source reads the probe-only mode file
`C:\Users\Public\SparkFacePhase0B2Mode.txt` **once per source activation**.
A missing file, unreadable file, or any content other than a value beginning
with `immediate` selects the default `wait` mode. There is no per-frame
filesystem or registry lookup.

`wait` waits up to 35ms for a sequence newer than the last delivered sequence,
then retains the B2 stale/torn/idle-black protections. `immediate` selects the
latest complete sequence without that new-sequence wait and may deliberately
duplicate a valid latest sequence. Both modes reject torn slots, turn stale
input black, and recover when the producer resumes.

**Switching modes requires a new media-source activation.** Close OBS, Tencent
Meeting, and every other camera client, including `fallback_mf_consumer`, so
Frame Server releases the source. Then restart Frame Server before opening the
next client:

```powershell
Get-Process FrameServer -ErrorAction SilentlyContinue | Stop-Process -Force
```

Set the selected mode only after all clients are closed, then open a fresh
consumer. For the default wait experiment:

```powershell
Remove-Item C:\Users\Public\SparkFacePhase0B2Mode.txt -ErrorAction SilentlyContinue
# Terminal A
build/windows-x64-release/spikes/fallback/fallback_producer.exe --frames 1020
# Terminal B, after Terminal A starts
build/windows-x64-release/spikes/fallback/fallback_mf_consumer.exe --frames 1000 --pacing-mode wait
```

For the immediate-latest experiment, first close/release the existing source
and restart Frame Server as above, then:

```powershell
Set-Content C:\Users\Public\SparkFacePhase0B2Mode.txt immediate -NoNewline
# Terminal A
build/windows-x64-release/spikes/fallback/fallback_producer.exe --frames 1020
# Terminal B, after Terminal A starts
build/windows-x64-release/spikes/fallback/fallback_mf_consumer.exe --frames 1000 --pacing-mode immediate
```

Run both 1,000-frame experiments under the same baseline. The consumer's
`mf_pacing_mode` is the declared experiment label; it must match the mode file
used before the fresh activation. It preserves `--expect-recovery`:

```powershell
build/windows-x64-release/spikes/fallback/fallback_mf_consumer.exe `
  --frames 400 --expect-recovery --pacing-mode wait
```

While that consumer remains open, kill and restart the producer. The command
fails unless it observes at least three idle black frames and a valid recovered
frame.

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

The consumer reads B3 QPCs only when the sample has at least 72 bytes. A B3
diagnostic is valid when producer≤published≤sequence-acquired and
request-entry≤sequence-acquired≤copy-complete≤sample-payload-finalized.
It reports p50/p95/p99/max for producer-to-publish, source wait, source copy,
source-copy-to-sample-payload-finalized, Frame-Server/client delivery, and
producer-to-client frame age, plus sequence, content, duplicate, skipped,
idle-black, recovery, and declared pacing-mode results. Published age at
request is reported only when the frame was published at or before request
entry; frames published after request are counted explicitly as
`mf_published_after_request_sequences`. Empty segments print `n/a`.

`sample-payload-finalized` is deliberately not called sample enqueue: the
sample buffer must be unlocked before `QueueEventParamUnk`, so writing an exact
enqueue QPC would require unsafe post-unlock payload mutation. Consequently,
Frame-Server/client delivery starts at payload finalization and the
source-copy-to-payload-finalized gap is the nearest safe source-ready boundary.

Choose a provisional B3 winner only when both 1,000-frame runs have zero
marker/content errors and compare the valid `producer_to_client_frame_age_p95`.
Validate the winner's idle-black/recovery behavior with `--expect-recovery` and
OBS plus Tencent Meeting separately. Run its 10-minute soak only if that p95 is
at or below 33.3ms; otherwise record the comparison and retain the B/B2 NO-GO.
OBS and Tencent Meeting must each be tested separately because the accepted v1
contract permits one active consumer.
