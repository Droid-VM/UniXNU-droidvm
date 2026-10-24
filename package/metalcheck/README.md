# Metal Check

Metal Check is a fixed, deterministic correctness probe for selected public Metal 2.3 APIs and the framebuffer rotation SPI used by the compositor. Each case checks results against CPU-computed bytes, values, or pixels. It is not a complete Metal conformance suite.

## Correctness cases

| # | Case |
|---:|---|
| 1 | shared buffer contents read/write |
| 2 | blit fill partial range and guards |
| 3 | shared-private-shared copy with offsets |
| 4 | affine compute dispatchThreads |
| 5 | affine compute dispatchThreadgroups |
| 6 | affine compute indirect dispatch with GPU-written arguments at byte offset 16 |
| 7 | RGBA8 replaceRegion/getBytes padded rows |
| 8 | buffer-texture-buffer blit offsets/stride/guards |
| 9 | texture-to-texture region copy |
| 10 | private RGBA8 clear/readback |
| 11 | fullscreen triangle vertex_id and fragment |
| 12 | vertex descriptor indexed quad |
| 13 | instanced left/right quads |
| 14 | alpha blend red over blue |
| 15 | scissor rectangle |
| 16 | depth less ordering |
| 17 | stencil replace/equal mask |
| 18 | multiple render targets |
| 19 | 4x MSAA resolve to mip 1, slice 1 |
| 20 | nearest 2x2 texture sampling |
| 21 | uniform mipmap generation |
| 22 | texture view nonzero mip/slice |
| 23 | compute atomic add |
| 24 | threadgroup sum |
| 25 | function constant scales 2 and 3 |
| 26 | MTLFence encoder dependency |
| 27 | MTLSharedEvent cross-queue dependency |
| 28 | IOSurface BGRA render/readback |
| 29 | IOSurface framebuffer rotation SPI: four rotations, non-square plane, four quadrant CPU pixel oracle |
| 30 | Rows not 64-byte aligned (widths 7 to 333, pitch rounded to the linear alignment): buffer-to-texture blit, and a texture over the buffer sampled 1:1; every byte checked. The compositor's small CPU-drawn bitmaps (labels, the status bar clock) arrive with such pitches |
| 31 | CPU writes to a no-copy buffer reach the GPU: 256 KiB of page-aligned memory rewritten eight times, each copied out by a blit at once; no 64-byte line may read stale. CoreAnimation wraps a client's CPU-drawn layer this way |
| 32 | New pages at a reused address: six rounds each map other pages where the last round's were (no-copy buffers, MAP_FIXED) or make a new IOSurface of the same size, and must read their own pattern. A freed backing store is often replaced at the same address |
| 33 | Small rectangles into a large texture, as the compositor packs layer images into an atlas: a cleared 1024x512 private render-target texture gets eight rectangles of odd size at odd places from buffers whose rows are not 64-byte aligned, and three copied out of larger textures; every texel of it is checked |
| 34 | One texture rewritten and drawn by 16 command buffers in flight (committed back to back): each writes its round's colour into the shared texture (blit, or a render pass clear) and draws it 1:1 into its own output, which must hold that colour only. The compositor rewrites and draws layer textures this way without waiting for the GPU |
| 35 | An IOSurface emptied (kIOSurfacePurgeableEmpty), made nonvolatile and drawn again on the CPU: the texture made of it before and a new one must read the new contents. A backing store the compositor does not show is volatile, and memory pressure takes its pages |
| 36 | Textures of other widths and row pitches over one reused page-aligned region, through the same no-copy buffer and through a new one each time, each drawn 1:1. A reused table cell's label gets another width over the same memory |
| 37 | One- and two-byte IOSurfaces as text reaches the compositor: 'LA08' as RG8Unorm (900x150 with rows 1824, and 333x24) and 'L008' as R8Unorm, rows rounded to 32 bytes; read by a blit to a buffer and drawn 1:1, every byte checked |
| 38 | The compositor's text masks: A8Unorm (and R8Unorm, RGBA8Unorm) textures over one no-copy buffer through the SPI newLinearTextureWithDescriptor:offset:bytesPerRow:bytesPerImage:, five mask sizes seen in the render server, rows tight or rounded to 16, 32 or 64 bytes, offsets 0, 64 and 256, the memory rewritten for each; drawn 1:1 and checked texel by texel |
| 39 | Text masks exactly as the render server got them on GFXBench's test list: A8 at offset 64 of whole-page shared memory with no more room, ten sizes and row pitches taken from it, drawn 1:1, then rewritten in place and drawn again through the same texture |
| 40 | New pages at an address an earlier no-copy buffer still covers: four rounds map new pages there (MAP_FIXED) while the earlier buffers stay alive, as the render server's caches keep them, and a blit from each new buffer, and an A8 mask over it, must read the new pages. On the guest the GPU read the earlier round's pages (the runtime put the new buffer in the live one's import) until NativeMetal reported such ranges remapped; that was GFXBench's garbled titles |

Only case 19 is optional: it may report `SKIP` when `supportsTextureSampleCount:4` is false. Every other case is required; an unexpected `SKIP`, missing resource or pipeline, command error, or oracle mismatch is a `FAIL`.

## Build and run

From this device repository, set `TREE_ROOT` to the build tree. The script builds the iOS UI app and macOS CLI, plus their offline Metal libraries:

```sh
TREE_ROOT=/path/to/tree
python3 scripts/build-droidvm-metalcheck.py "$TREE_ROOT"
```

Run the host CLI and save a correctness-plus-performance report:

```sh
"$TREE_ROOT/build/metalcheck/host/metalcheck" --cli \
  --library "$TREE_ROOT/build/metalcheck/host/default.metallib" \
  --output "$TREE_ROOT/out/metalcheck/host-baseline.json" \
  --benchmark
```

The iOS UI build output is `$TREE_ROOT/build/metalcheck/MetalCheck.app`. Its source defines **Correctness**, **Correctness + Performance**, and a five-minute **GPU Stress** action with 0/128/256 MiB memory choices and a stop-after-current-round control. The guest UI has been launched and its Correctness + Performance action passes all 28 cases and three workloads (`out/metalcheck/ui-performance.json` in the active rig). With the current no-container signing, it saves `/private/var/mobile/Documents/MetalCheck/latest.json`.

The enhanced guest run at `../../../droidvm/out/metalcheck-guest-enhanced/report.json` has 28 `PASS`, 0 `FAIL`, and 0 `SKIP`; its render, compute, and blit benchmark workloads pass, and the rig observed no guest GPU loss. The enhanced Mac M4 host correctness-and-benchmark baseline and the 15-second, 128-MiB host stress run also pass (`../../../droidvm/out/metalcheck/host-enhanced-baseline.json` and `../../../droidvm/out/metalcheck/host-stress.json`). The first five-minute guest run passed nine rounds in 303.98 seconds with 128 MiB extra resident buffers (`../../../droidvm/out/metalcheck-guest-stress-300s/report.json`), with no new GPU loss or display error/timeout. It included UI activity and an explicit SpringBoard restart while diagnosing app installation, so it is not an isolated performance baseline. After caching buffer length outside the byte-check loop, the deployed version passed 20 API/validation rounds in 301.76 seconds. However, that run retained stale Metal Check screen content during Home/GFXBench transitions and showed text corruption despite zero GPU-loss counters. Its reviewed overall result is FAIL (`out/metalcheck-guest-stress-final/report.json`); the original API/counter result is retained as `report-automated.json`. These probes do not validate the compositor or final displayed pixels. The UI Stop button was also verified: three completed rounds followed by CANCELLED, not PASS (`out/metalcheck/ui-stress-stop.json`).

The rotation fix has since passed all **29 correctness cases and all three workloads on both Mac and guest** (`out/rotation-fix/host-rotation.json` and `out/rotation-fix/after/report.json` in the active rig). The old guest plugin failed case 29 with an IOSurface bounds error; the new plugin passes its four-rotation pixel oracle. This confirms the bridge fix, while post-fix compositor and stress visual acceptance remain pending.

## Benchmark scope

The three fixed workloads submit for two seconds each: 512×512 render, 1M-float compute, and 16 MiB blit. Each uses at most three in-flight command buffers with a separate slot per output. Warmup and pipeline setup are outside the timed window; every queue is drained, then each slot's output is validated before the workload receives a score. A failed command buffer or output check yields `FAIL` with no valid score.

## Stress mode

The CLI accepts `--stress-seconds 1..86400` and `--memory-mib 0..512` (default `128`). Stress first runs all required correctness cases; if any fail, stress is not run. Each stress round runs render, compute, and blit workloads, with a two-second timed window for each and at most three in-flight command buffers. Setup and output verification are outside the timed windows. The run validates workload outputs and every byte in its extra resident buffers, and records process memory for each round. Stress scores are per-round rates under load, not an isolated performance baseline.

The suite does not cover argument buffers, indirect command buffers (ICB), sparse textures, ray tracing, mesh shaders, tessellation, function pointers and dynamic linking, heaps and aliasing, or counter sampling and performance counters. Passing these probes does not establish complete Metal conformance.

## Install on the current guest

After signing, adding the new CDHash to the owner trust bundle, and booting that trust bundle, stage the app under `/private/var/ngpu/apps/MetalCheck.app`, then use the trusted owner helper:

```sh
lsdiag -c owner-install /private/var/ngpu/apps/MetalCheck.app
lsdiag -c -u 501 owner-register /private/var/ngpu/apps/MetalCheck.app
lsdiag -c -u 501 app org.droidvm.metalcheck
lsdiag -c -u 501 launch org.droidvm.metalcheck
```

Unlock the guest before `launch`. `owner-install` moves the app into a real MCM bundle container and leaves the original path as a CLI symlink. It creates the data container without changing the signed no-container entitlement. It refuses an existing destination and does not modify MCM's container-root metadata. For a later update, stop the app, preserve a backup, and use the entitled `lsdiag copy` helper to replace its files in the existing bundle container, then register the updated path. Ordinary root toybox writes are still blocked by the bundle data vault. The installation helper does not add code to the kernel trust cache.

Running directly from `ngpu/apps` works for the owner CLI but the system blocks UI execution and icon reads at that location. A successful LaunchServices record alone does not prove an app can launch.
