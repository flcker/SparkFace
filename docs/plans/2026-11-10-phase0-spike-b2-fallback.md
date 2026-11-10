# Spike B/B2 — GPU handoff and CPU fallback evidence

## Verdict

- **Spike B: NO-GO.** The planned GPU-resident D3D11 capture → DirectML D3D12 → bgfx D3D12 → virtual-camera chain is not supported by the tested APIs and driver.
- **Spike B2: NO-GO.** The preallocated CPU fallback is correct, sustains producer-side 1080p30, survives producer loss, and renders in OBS and Tencent Meeting. It does not satisfy the product frame budget: the real producer-to-Frame-Server-client path measured `78.099ms` p95, above the `33.3ms` frame budget, before a real face model or production beauty shaders are added.

This result does not change the accepted v1 single-consumer contract from Spike A/A2. Product implementation and Spike C remain blocked pending an explicit architecture/product decision.

## Baseline

| Item | Tested value |
|---|---|
| Date | 2026-11-10 |
| OS | Windows 11 build 26300 |
| GPU | Intel Iris Xe, driver `32.0.101.7092` |
| Media-source reference | `microsoft/Windows-Camera@626f8b19c5f367602f2e89c6b314573d3776c9df` |
| Reference archive SHA-256 | `d4c4217684d76e6e5934114a4d9ade0d0e6249fef70462d5cc451e3613674eec` |
| bgfx | vcpkg `1.129.8940-496#1`, D3D12 |
| Camera media type | NV12, 1920×1080, 30fps |
| Camera | `SparkFace Phase0 B2 Camera` |
| Applications | OBS Studio 33.0.0; Tencent Meeting 3.47.2.405 |

The retained probe lives on branch `spikes` under `spikes/fallback/`. Microsoft-derived files retain their license and attribution in `spikes/fallback/media-source/LICENSE-Microsoft.txt`.

## Spike B GPU-resident findings

| Gate | Result |
|---|---|
| Same adapter | Yes; all tested components used the Iris Xe |
| D3D11/D3D12 shared formats | D3D12→D3D11 opened BGRA8, RGBA8, and R8; NV12 failed; D3D11→D3D12 failed for all four formats |
| Shared-fence protocol | Passed 1,000 known-pattern round trips; synchronization p95 `3.107ms` |
| DirectML direct resource binding | Failed with `0xC0000005`; the working path copied data |
| bgfx imported native resource | Failed with `0xC0000005`; bgfx-owned compute worked |
| GPU-backed camera sample | D3D11 NV12 could be wrapped in an `IMFSample`, but Frame Server surface preservation was not proven; D3D12 NV12 returned `E_NOINTERFACE` |

These failures reject the copy-free hypothesis. The CPU fallback was measured separately rather than presenting a partial component sum as an end-to-end result.

## B2 implementation

The probe uses a fixed, preallocated path:

1. Render a deterministic frame into a D3D11 texture on the Iris Xe.
2. Read into a persistent staging buffer.
3. Upload to a bgfx-owned D3D12 texture, run the synthetic compute round trip, and read into a persistent output buffer.
4. Convert RGBA to NV12 in a persistent buffer.
5. Publish into a three-slot sequence-validated ring backed by `C:\Users\Public\SparkFacePhase0B2Ring.bin`.
6. Have the patched Windows-Camera media source wait for the next producer sequence, copy the latest valid NV12 frame into the requested sample, and emit paced black frames when the producer is absent or stale.
7. Deliver the sample through `MFCreateVirtualCamera` and Windows Frame Server.

There is no per-frame heap allocation after initialization. The public file path and permissive location are probe-only and are not an accepted product security design.

## Reproduction

From an MSVC x64 Developer PowerShell with the pinned vcpkg environment:

```powershell
cmake -S . -B build/windows-x64-release -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows `
  -DSPARKFACE_BUILD_ENV_GATE=OFF
cmake --build build/windows-x64-release --target `
  fallback_producer fallback_consumer fallback_mf_consumer fallback_registrar
spikes/fallback/media-source/build-upstream.ps1
```

Install/remove requires elevation; producer and consumers run without elevation:

```powershell
build/windows-x64-release/spikes/fallback/fallback_registrar.exe install <media-source-dll>
build/windows-x64-release/spikes/fallback/fallback_producer.exe --frames 1200
build/windows-x64-release/spikes/fallback/fallback_mf_consumer.exe --frames 1000
build/windows-x64-release/spikes/fallback/fallback_registrar.exe remove
```

## Measured results

### Producer-side transport

A final 1,080-frame run used the complete synthetic fallback path.

| Metric | Result |
|---|---:|
| Achieved rate | 29.992fps |
| Process CPU | 37.143% |
| D3D11 readback p95 | 6.375ms |
| bgfx upload/compute/readback p95 | 10.932ms |
| RGBA→NV12 p95 | 5.999ms |
| Ring publication p95 | 0.359ms |
| Producer transport total p50 / p95 / p99 | 16.140 / 22.014 / 25.333ms |
| Missed 33.3ms deadlines | 2 |
| Content validation | Pass |

The previously completed 18,000-frame prerequisite soak recorded four missed producer-side deadlines. It did not include the media source or Frame Server, so it is supporting sustained-transport evidence only.

### Real Frame Server path

The final diagnostic MF run consumed 1,000 valid frames from the installed virtual camera.

| Metric | Result |
|---|---:|
| Valid frames | 1,000 |
| Skipped / duplicate sequences | 0 / 23 |
| Marker/content errors | 0 |
| Source enqueue p50 / p95 / p99 | 13.872 / 16.439 / 47.356ms |
| Frame Server delivery p50 / p95 / p99 | 32.666 / 48.826 / 49.373ms |
| Producer-to-client p50 / p95 / p99 | 62.638 / 78.099 / 79.222ms |
| Maximum producer-to-client latency | 80.876ms |

The duplicate count reflects polling timeout alignment between two 30fps loops; no invalid or torn frame reached the client. Throughput remained 30fps, but the latency result fails the product budget.

### Producer loss and recovery

The MF consumer remained open while the producer was killed for two seconds and restarted with a new sequence epoch.

| Metric | Result |
|---|---:|
| First idle black frame | 46.442ms after the last valid frame |
| Idle black frames observed | 49 |
| Recovery without reopening camera | Yes |
| Recovery-run skipped / duplicate sequences | 1 / 8 |
| Marker/content errors | 0 |
| Recovery-run producer-to-client p95 | 78.328ms |

### Real applications

| Application | Observation |
|---|---|
| OBS Studio 33.0.0 | Selected `SparkFace Phase0 B2 Camera`; rendered the moving deterministic pattern across the full 16:9 preview at NV12 1920×1080/30. A recorded MP4 was H.264 1920×1080 at 30fps. |
| Tencent Meeting 3.47.2.405 | Enumerated the B2 camera through Media Foundation, selected NV12 1920×1080/30, produced its first frame in 63ms, rendered the full moving pattern, and stabilized at 29–30fps in its capture log. |

Applications were tested separately, consistent with the accepted one-active-consumer v1 contract.

## Decision

B2 proves compatibility, correctness, idle behavior, recovery, and 1080p30 throughput for a synthetic CPU fallback. It does **not** prove the product workload or the required frame budget. The durable decision remains **NO-GO**; do not begin the v1 build plan or Spike C until the architecture/product constraint is explicitly resolved.
