# Phase-0 Spike D: cross-process D3D11 shared-texture handoff

This evidence-only probe determines whether SparkFace can publish GPU-rendered NV12 frames from one process, import them in the custom Media Foundation source, and deliver them through the Windows 11 virtual-camera path without CPU frame copies in the producer-to-media-source transport.

It is not product code and must not merge into `main`. The retained executable evidence belongs on `spikes`; only the measured verdict belongs on `main`.

## Pinned baseline

- Windows 11 build 22000 or newer
- Microsoft Windows-Camera reference commit `626f8b19c5f367602f2e89c6b314573d3776c9df`
- Microsoft archive SHA-256 `d4c4217684d76e6e5934114a4d9ade0d0e6249fef70462d5cc451e3613674eec`
- NuGet `7.9.0.83`, downloaded from the versioned NuGet distribution URL
- NuGet SHA-256 `992d70cac5b06c38efec91806caba64cdcc07e6d963a0959dbbbaf264d33b800`
- MSVC x64 Developer PowerShell
- Output format: NV12 1920x1080 at 30 fps

Files derived from Microsoft Windows-Camera retain their Microsoft copyright headers and are covered by `media-source/LICENSE-Microsoft.txt`.

## Architecture under test

1. `d3d11_handoff_producer` renders a deterministic NV12 pattern into three D3D11 textures on the selected GPU adapter.
2. Each texture is created with `D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX`, exported under a stable named NT handle, and synchronized with an `IDXGIKeyedMutex`.
3. A compact file-backed control mapping under `C:\Users\Public` publishes slot, sequence, producer QPC, publication QPC, producer PID, and producer adapter LUID. It contains no frame pixels and remains visible across the interactive producer and Frame Server sessions.
4. The patched out-of-process Media Foundation source creates a D3D11-aware custom sample allocator, compares the allocator texture's adapter LUID with the producer LUID, then opens and copies the named texture only when they match.
5. The producer and media-source transport never map texture pixels to CPU memory. The diagnostic consumer maps the final client sample only to validate the payload marker and record end-to-end frame age.

The producer must select the Intel Iris Xe adapter on the reference laptop. WARP and the Microsoft Basic Render Driver are rejected.

## Build

From a normal PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-phase0-d.ps1
```

The script builds the producer, diagnostic consumer, registrar, and the patched Windows-Camera media-source DLL. Build outputs remain under `build/` and are not evidence.
The reference workstation already retains the B2 virtual camera and media-source COM identity. A newly introduced probe CLSID did not enumerate through Windows Frame Server, and a second friendly name on the B2 source identity resolved to the existing B2 camera. Spike D therefore exercises the retained B2 camera while temporarily pointing its COM registration at the D media-source DLL. The run script saves and restores the original B2 DLL path in `finally`. Do not run B2 measurement or another camera client concurrently.


## Automated gate

Run from an elevated PowerShell because the probe temporarily updates the retained media source under HKLM:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run-phase0-d.ps1
```

The default gate captures 1,000 output frames and passes only when all of the following hold:

- the selected producer adapter contains `Intel` and is not a software adapter;
- exactly 1,000 frames arrive at 1920x1080 NV12;
- output cadence is at least 29.5 fps;
- p95 producer-to-client frame age is no more than 33.3 ms;
- every received sequence is newer than the previous sequence;
- every decoded NV12 payload marker matches the published sequence and magic value.

The script restores the original B2 media-source DLL registration in `finally` unless `-KeepInstalled` is supplied for the conditional application checks.

## Measured result

The default 1,000-frame gate ran on 2026-11-13 and failed its first required
same-adapter check before any frame could be delivered.

| Gate | Result |
|---|---|
| Producer adapter | Intel Iris Xe, LUID `00000000:0000ffd8` |
| Frame Server source allocator adapter | LUID `00000000:000178d8` |
| Same adapter | Fail |
| Delivered frames | 0 of 1,000 requested |
| FPS / producer-to-client p95 | `n/a` |
| Registration cleanup | Original B2 media-source path restored |

The media source successfully created a D3D11-aware custom allocator and
received `IMFDXGIBuffer` samples. The allocated texture belonged to a different
adapter LUID from the interactive producer. D3D11 NT shared textures require a
same-adapter path, so the source rejected the handoff before
`OpenSharedResourceByName`; earlier direct open attempts returned
`E_INVALIDARG (0x80070057)` for the same mismatch.

This is a **NO-GO** for the tested cross-process D3D11 shared-texture design.
The producer-restart, 30-second stall, application, and soak checks were not
run because the automated gate is their explicit prerequisite.


## Conditional checks

Only after the automated 1,000-frame gate passes:

1. Kill `d3d11_handoff_producer` while consuming. Start it again and verify delivery resumes without reinstalling the camera.
2. Keep the producer stopped for at least 30 seconds. Verify the client remains responsive, then restart the producer and verify delivery resumes.
3. Test the camera separately in OBS and Tencent Meeting at 1920x1080/30 fps.
4. Only when both applications receive a valid image, run each for 10 minutes and record device removal, client hangs, black frames, malformed frames, and producer restart recovery.

## Evidence

Retained evidence is written to `results/summary.json`. Raw client and producer logs may be retained when they are small and contain no machine secrets. Build outputs, the upstream source tree, downloaded binaries, and media-source DLLs are never committed.

## Decision rule

- `GO`: all automated gates pass on Intel Iris Xe; producer restart and 30-second stall recovery pass; OBS and Tencent Meeting both receive valid 1080p30 output and complete their 10-minute soaks without device removal, hangs, black frames, or malformed frames.
- `NO-GO`: any required automated, recovery, application, or soak gate fails.
- `INCONCLUSIVE`: the exact approved environment cannot be exercised; this is not a pass and does not authorize v1 implementation.
