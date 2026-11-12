# GPU, display, touch and memory work log (Nov 9–12, 2026)

Goal: a Mac-like experience on the Pixel 8a — 60 fps, no visual glitches. Each test image was booted from RAM
(`fastboot boot`); "fullosNNN" is the image number. Window-drag fps is measured with `scripts/drag60.py`, which drags a
window through the USB input channel while `NativeGOPDisplay` logs fps every 10 s.

## The pipeline

```
WindowServer → Metal → NativeMaliMetal plugin → UniXNU frontend + metal2vulkan (Rust)
  → PanVK → darwin-drm → MaliClient (IOKit) → Mali CSF queues → GPU
WindowServer swap → NativeGOPDisplay → copies the frame into the panel's framebuffer (1200×540 landscape on a 1080×2400 panel)
```

## Results

| Change | Where | Effect |
|---|---|---|
| Batched queue submits (`MALI_GROUP_SUBMIT_BATCH`, up to 32 jobs per call) | `Mali.cpp`, `darwin-drm` | A submit cost ~36 µs (≈20 µs IOKit + 16 µs kernel) and a frame made ~170 of them |
| GPU-side cross-queue waits (`SYNC_WAIT64` in the queue, not a CPU wait) | `Mali.cpp`, `darwin-drm` | Fewer CPU stalls between queues |
| Flush-ID page (`MALI_MAP_FLUSH_ID`, read-only, 16 KiB on macOS) | `Mali.cpp`, `darwin-drm` | PanVK skips cache flushes the GPU already did |
| Fault scan at most every 5 ms | `Mali.cpp` | Less kernel time per submit |
| Early swap completion; copy after the GPU fence | `display-0019` | WindowServer is not held by the copy |
| Wide `stp` stores in the copy; timeline only for heavy frames | `display-0020`, `display-0021` | Faster copy, less logging |
| Render-target descriptors packed locally and stored once; push constants built locally then copied | PanVK `pan_fb.c`, `panvk_vX_cmd_push_constant.c` | GPU memory is uncached: scattered small stores were slow |
| 128-byte alignment for tiled (u-interleaved) images; linear 64 | PanVK `panvk_image.c` | Metal's heap layout matches, so the bridge stops keeping shadow copies of heap textures |
| `PANVK_DEBUG=noafbc` in WindowServer | plugin | Needed with the in-place heap textures |
| Shader cache keyed on a fixed PanVK UUID (`keep-uuid.py`) and fixed bridge runtime id | build scripts | Rebuilt drivers keep the shader cache warm |
| Thermal regulator with hysteresis (step down a third at ≥85 °C, boot speed at ≥95 °C, step up after 5 s below 75 °C) | `AkitaAcpm.cpp` | Replaced a one-way guard that left CPUs at boot speed forever |
| Ring slack and wait-on-full | `Mali.cpp` (fullos181) | Fixed `CS_CONFIG_FAULT` (fatal 0x40) that happened with exactly 256 jobs outstanding (a full 64 KiB ring) |
| Slack counted from the start of the job the CS is in, two jobs | `Mali.cpp` (fullos194) | fullos193 hit fatal 0x40 again: the read pointer was mid-job, so a 256th job still fit. 15 min of drag stress since: 0 faults |
| Touch driver with gestures | `AkitaTouch.inc`, `NativeRecoveryInput.cpp` | Tap, drag, long press = right click, two-finger scroll |
| Dark mode and transparency at boot | `kern_exec.c` (rc.server) | `AppleInterfaceStyle=Dark`; no forced reduce-transparency |
| Ask DarwinBoot for up to 8 GiB (`AKITA_OWNED_MEMORY=0x200000000`) | `package-akita-macos.py` | macOS sees 5.26 GiB (was 4 GiB), fullos183 |

**Window drag: 15.7 fps → ~22 fps.**

## Things that did not help

- Lowering render encoders on metal2vulkan's worker thread (`NATIVE_METAL_LOWER_WORKER=1`): no gain, because opening the next
  encoder drains the worker on the main thread (`settle_end`) and Core Animation opens encoders back to back. Kept opt-in.
- Restricting WindowServer to the big cores (`AKITA_CPUMASK`): no fps gain. The fixes it needed (display-0018, ArmCpu skipping
  cores not in the topology) were kept.

## Where the time goes now

WindowServer's main thread is CPU-bound in the Metal → Vulkan bridge, about 150 µs per draw. Getting to 60 fps needs fewer or
cheaper draws: caching lowered state between frames, or doing more of the lowering ahead of time.

## Tools

- `scripts/stacks.py`, `allstacks.py`, `sysyms.py`: the plugin samples WindowServer's stacks
  (`/tmp/akita-prof-stacks.txt`), and these resolve and rank them.
- `scripts/drag60.py`: drag test (`DRAG_WAIT`, datagrams of ≤60 steps; longer ones were dropped).
- `scripts/phase4-patch-mali.py`: regenerates `Mali.cpp` / `mali_uapi.h` from the base driver.
- `scripts/build-bridge.sh`, `build-drm.sh`, `build-mesa-main.sh`, `keep-uuid.py`, `bridge-runtime-id.py`, `compile-Mali.sh`,
  `build-acpm.sh`, `rebuild_ufs_driver.py`, `build-fullos.sh`.
