# Metal / Adreno full port (decade track)

This is the long path the user asked for. It is separate from the daily
GUI smoothness work (Track A / DPU present in `docs/GPU.md`).

## Why it is hard

iOS 16 ships **precompiled AGX machine code** (`default.metallib` in every
framework, plus per-app metallibs). There is no AIR/Metal source and no
compiler we can retarget to Adreno a6xx. A complete IOAccelerator ABI
alone does **not** run those binaries. Phase 4 without Phase 5 will make
CoreAnimation choose Metal and then hang or scramble the screen — that is
why `sweetgpu=3` stays off on daily images.

## Roadmap (honest order)

| Phase | What | Depends on | Est. |
| --- | --- | --- | --- |
| A | Smooth CPU present (cache copy → DPU flip) | nothing | **now** (0086–0089) |
| B0 | Pull GMU / SQE / ZAP from vendor (`tools/gpu-fw.py`) | Android or OrangeFox + /vendor | days |
| B1 | CX/GMU power, load FW, read chip id stably | B0 | weeks |
| B2 | Ringbuffer + IB + clear/blit into IOSurface | B1 | months |
| B3 | IOAccelerator / IOGPU user-client ABI (trace-driven) | B2 | months |
| B4 | Tiny custom Metal path (our shaders only) | B3 | months |
| B5 | AGX ISA decode → a6xx emit (research) | B4 + RE | **years** |
| B6 | Replace enough of QuartzCore's metallib surface that SB/apps run | B5 | **years** |

B5/B6 are the "even if it takes 10 years" items. Progress is measured by
artifacts (fw hashes, ring submits in the log, a known-good clear on the
panel), not by registering a fake `AGXAccelerator`.

## Rules

1. Never enable `sweetgpu=3` on a daily boot until B4 can draw a frame.
2. Never claim "Metal works" when CA is still on `CA::OGL::SW`.
3. Keep Track A (DPU flip) as the path that makes the phone *feel* fast
   while B* runs in parallel.
4. Firmware blobs stay in `fw/` (gitignored). Only `fw/manifest.txt`
   hashes may be committed.

## Immediate next (B0)

With the phone in Android or OrangeFox (`/vendor` mounted):

```text
python tools/gpu-fw.py
```

That unblocks B1. Without it, every "Metal" change is theater.
