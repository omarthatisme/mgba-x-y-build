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

* The render thread keeps only the **newest** frame (a one-slot mailbox).
  The OpenGL path queues up to three frames and shows the oldest first.
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
