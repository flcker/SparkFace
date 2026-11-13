# Spike D — cross-process D3D11 virtual-camera handoff

## Verdict

**NO-GO for the tested D3D11 NT shared-texture design.** The interactive Iris
Xe producer and the out-of-process Windows Frame Server media source exposed
different adapter LUIDs. D3D11 shared textures require both devices to address
the same adapter, so the handoff failed before one frame reached the client.

This result closes the proposed GPU-resident recovery for B–B4. It does not
claim that every possible GPU transport is impossible; it rejects the tested
named D3D11 texture path across the current `MFCreateVirtualCamera` Frame Server
boundary.

## Baseline

| Item | Tested value |
| --- | --- |
| Date | 2026-11-13 |
| OS | Windows 11 Pro build 26300 |
| GPU | Intel Iris Xe, driver `32.0.101.7092` |
| Media-source reference | `microsoft/Windows-Camera@626f8b19c5f367602f2e89c6b314573d3776c9df` |
| Camera media type | NV12, 1920×1080, 30fps |
| Automated request | 1,000 frames |
| Product gate | At least 29.5fps and producer-to-client p95 no more than 33.3ms |

Executable evidence is retained on `spikes` under `spikes/d3d11-handoff/` by
[evidence PR #11](https://github.com/flcker/SparkFace/pull/11). The evidence
includes the producer, diagnostic consumer, registrar, pinned upstream patch,
build/run scripts, strict manifest, reproduction instructions, and result
summary. Build products and media-source binaries remain out of Git.

## Design tested

1. The interactive producer rendered deterministic NV12 frames into three
   preallocated D3D11 textures on the Intel Iris Xe.
2. Each texture used
   `D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX`
   and a globally named NT handle.
3. A file-backed control mapping published the latest slot, sequence, timing,
   producer PID, and producer adapter LUID across Windows sessions without
   copying frame pixels through CPU memory.
4. The patched out-of-process media source created a D3D11-aware custom
   `IMFVideoSampleAllocatorEx`, obtained allocator-owned `IMFDXGIBuffer`
   textures, and compared their adapter LUID with the producer before opening a
   shared texture.

## Measured result

| Gate | Result |
| --- | --- |
| Producer adapter | Intel Iris Xe, LUID `00000000:0000ffd8` |
| Frame Server source allocator | LUID `00000000:000178d8` |
| Same-adapter prerequisite | Fail |
| Requested / delivered frames | 1,000 / 0 |
| Output cadence | `n/a` |
| Producer-to-client p95 | `n/a` |
| Payload validation | Not run; no frame crossed the adapter boundary |
| Cleanup | Original B2 media-source COM path restored; no probe process remained |

The custom allocator resolved the earlier `E_NOINTERFACE` result from the
Frame Server-provided sample: the source did receive an `IMFDXGIBuffer`.
However, that texture belonged to LUID `00000000:000178d8`, while the
interactive Iris Xe producer used `00000000:0000ffd8`. Earlier direct
`OpenSharedResourceByName` attempts returned `E_INVALIDARG (0x80070057)`; the
final probe records both LUIDs and fails the same-adapter gate before attempting
the invalid open.

## Skipped conditional checks

Producer restart, a 30-second producer stall, OBS, Tencent Meeting, and the two
10-minute application soaks were not run. The approved decision rule permits
those checks only after the automated 1,000-frame gate passes. Running them
without a deliverable first frame could not change the verdict.

## Decision

Do not use this cross-session D3D11 named-texture path for the v1 virtual-camera
transport, and do not report FPS or latency for a path that delivered zero
frames. A future proposal must define and prove a different Frame Server
boundary or explicitly revise the architecture/product constraints. The
detailed v1 build plan remains blocked by this transport failure and the
independent Spike C face-model NO-GO.
