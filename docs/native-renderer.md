# Native renderer: roadmap

The port runs the game's own Direct3D code, which writes Xenos (Xbox 360 GPU) command buffers, and
the runtime emulates that GPU on Direct3D 12: it decodes the PM4 packets, translates every shader
and pipeline state, emulates the 10 MB of EDRAM and keeps a copy of guest memory on the host GPU,
tracking CPU writes with page protection. UnleashedRecomp goes the other way: it replaces the
title's Direct3D calls with a native renderer and precompiles every shader offline (XenosRecomp),
so there is no GPU to emulate. This page is the plan to get there with Black Ops.

## Why

Split screen draws ~8,300 times per frame. Emulating that costs one CPU core (the "GPU Commands"
thread), measured in a two player match on an i5-9600K:

| Share of the thread | Work |
|---|---|
| ~30% | Driver (Direct3D 12 calls, replaying the command lists) |
| ~30% | Guest memory uploads: ~650-800 per frame, each write-protecting pages again |
| ~40% | Register writes, state translation, bindings, textures, render targets |

With the runtime work done so far (game threads that sleep instead of polling, asynchronous
submission, hot vertex streaming, profile-guided optimization) a match runs at 58-60 FPS on that
CPU, with no headroom left. A native renderer removes the emulation itself: no PM4 decoding, no
EDRAM, and resources the renderer owns.

## What it takes

1. **Find the Direct3D layer in the executable.** The XDK's Direct3D is linked statically into
   `default.xex` / `default_mp.xex` without symbols. Entry points to identify: device creation,
   `SetRenderState`/`SetSamplerState`, `SetTexture`, `SetStreamSource`, `SetIndices`,
   `SetVertexDeclaration`, shader and constant setters, `DrawVertices`/`DrawIndexedVertices`,
   `Resolve`, `BeginTiling`/`EndTiling` (predicated tiling) and `Swap`. Found so far: the internal
   helpers that build `DRAW_INDX_2` packets directly (campaign `sub_824912E0`, `sub_824928A0`,
   `sub_824929F8`, `sub_82499708`, used by clears and resolves). The main draw path builds its
   packet headers with variable counts and has to be traced from the renderer's surface drawing
   functions down.
2. **Check how the engine talks to Direct3D.** Call of Duty engines on the Xbox 360 also write
   registers and constants straight into the command buffer and reuse precompiled command
   buffers. Every such path needs its own replacement, which is the main difference from Sonic
   Unleashed (whose renderer used Direct3D calls throughout).
3. **Shaders offline.** The game's shaders live in its fast files. XenosRecomp (UnleashedRecomp)
   translates Xenos microcode to HLSL/DXIL ahead of time; it needs the shaders extracted from the
   fast files and an index from microcode hash to the translated shader.
4. **Render targets and resolves.** EDRAM tiles, predicated tiling and resolves become host render
   targets and copies; MSAA (the game uses 2x/4x) maps to host MSAA.
5. **Memory.** The game writes vertex buffers and textures straight into memory it shares with the
   GPU (no `Lock`/`Unlock` to hook on the console). The renderer still has to learn about CPU
   writes, through the engine's own allocators (the dynamic vertex buffers and the skinned
   vertex cache) instead of page protection.

## Plan

Each stage keeps the game playable:

1. Done: runtime-side improvements (thread sleeping, asynchronous submission, streaming of the
   vertex buffers the CPU rewrites every frame).
2. Map the Direct3D layer (step 1 above) and log, per frame, which entry points the game uses and
   how often, to size the work.
3. Replace the engine's dynamic geometry path (dynamic vertex buffers, skinned vertices) with
   host buffers the game writes through hooks: removes the page protection of the hottest memory.
4. Replace whole passes (UI and 2D first, then the world) with native draws, keeping Xenos
   emulation for everything not replaced yet.
5. Precompiled shaders; then remove the emulation from the replaced passes.

This is a project of months: the steps above are the order in which it pays off.
