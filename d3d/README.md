# Direct3D display drivers

Adds two display drivers to the mGBA Qt frontend (Windows only), next to the
existing *Software (Qt)*, *OpenGL* and *OpenGL (force version 1.x)*:

* **Direct3D 11 (low latency)** – flip-model swap chain with a frame-latency
  wait, so at most one frame is queued between the emulator and the screen.
  Falls back step by step to older swap-chain modes if the system lacks them.
* **Direct3D 9** – Direct3D 9Ex with the FlipEx swap effect where available.
  Fixed-function only, meant as a compatible fallback.

Select one in **Settings → Audio/Video → Display driver**. The default driver
is unchanged. If the chosen driver can't start on a machine, mGBA logs why and
falls back to the Software (Qt) display, as it does for OpenGL.

## Why it is lower latency than the OpenGL path

* The render thread keeps at most **two** frames waiting and shows them in
  order, one per screen refresh. Frames beyond that are dropped oldest-first.
  The OpenGL path queues up to three frames *and* re-times them with a timer.
  A short queue is enough to absorb frames that arrive in bursts (some audio
  devices wake the emulator every ~30 ms, which delivers frames in pairs)
  without dropping any, and while fast-forwarding only the newest is kept.
* The thread waits for the swap chain to be ready *before* it picks up the
  frame, so the picture shown is as fresh as possible.
* With audio/video sync on it presents with VSync; while fast-forwarding it
  presents without waiting and is limited to the monitor's refresh rate.
* Direct3D/DXGI/compiler DLLs are loaded at run time from System32, so the
  executable gains no new import dependencies.

## Layout

| File | Purpose |
| --- | --- |
| `D3DRenderer.h/.cpp` | Qt-free Direct3D 11 and 9Ex renderers |
| `DisplayD3D.h/.cpp` | Qt `Display` subclass, render thread, OSD overlay |
| `../.github/apply_d3d.py` | Copies these in and patches upstream (enum, factory, settings list, CMake) |

The sources are compiled only when CMake defines `BUILD_D3D` (Windows builds
only); on other platforms they compile to nothing.

## Diagnosing choppy playback

Set `MGBA_D3D_STATS=1` before starting mGBA to record what the display is
doing:

```
set MGBA_D3D_STATS=1
mGBA.exe
```

Play for about 20 seconds, then close mGBA. `mgba-d3d-stats.txt` is written
next to `mGBA.exe` (or in `%TEMP%` if that folder is read-only). The first line
names the swap chain mode the system accepted; every following line covers
about two seconds:

* `refresh` – the monitor refresh rate Qt reports. A GBA runs at 59.73 fps, so
  anything other than ~60 Hz cannot show frames evenly without variable refresh.
* `frames in` – frames handed over by the emulator, with the time between them.
  Large gaps here mean the frames arrive unevenly (audio or GUI thread timing).
* `dropped` – frames replaced before they could be shown.
* `presented` – frames put on screen, with the time between them, and how long
  `present()` blocked.

Nothing is recorded unless the variable is set.

## Tuning

`MGBA_D3D_QUEUE` sets how many frames may wait (1-4, default 2). `1` restores
the previous "newest frame only" behaviour, which has the lowest possible delay
on machines whose frames arrive evenly; higher values trade a little delay for
more tolerance of bursty frame delivery.
