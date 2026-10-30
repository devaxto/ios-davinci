# Native drivers — quality bar (davinci / sweet)

The phone must feel good **today** with software CoreAnimation. Adreno /
Metal is a parallel lab track (`docs/METAL-PORT.md`). Mixing them made the
GUI worse: `sweetgpu=5` boots poke GMU/SQE during bring-up and do not paint
SpringBoard.

## Hard rule

| Image | Boot-args | Purpose |
| --- | --- | --- |
| **Daily** (`shim-boot-0093.img`, `…-0091.img`) | `sweetgpu=1` (or omit) | GUI, touch, feel |
| **Lab metal** (`…-metal.img`) | `sweetgpu=4` / `5` | GMU/SQE only; expect jank |

Never flash/boot a metal image when chasing smoothness.

## Driver scoreboard (honest)

| Area | Status | What “perfect” means next |
| --- | --- | --- |
| Display present | **good** (0091 flip + cache) | Stable 60 Hz flip; no memcpy; measure `flips N` in log |
| Touch (GT9896) | **works** | Edge gestures, no drops, low latency |
| Keys / buttons | **works** | Volume + power |
| USB gadget | **works** (device enum) | Stable adb-over-USB later; no reset loops |
| UFS root | **works** | No I/O stalls on present path |
| SMP (8× A55) | **works** | No idle imbalance killing frames |
| Battery / SPMI | **basic** | Real capacity, charging icon truthful |
| GPU Adreno | **lab only** | Ring IDLE then blit; **not** on daily |
| Audio / modem / BT / Wi‑Fi | **missing / stub** | Separate milestones; do not block GUI |

## Order of work (feel first)

1. Always recover with a **daily** image if the panel feels worse.
2. Pull a boot log; confirm `SWEET FB: … flip` and rising `flips`.
3. Only then touch lab GPU (`sweetgpu=5`) on a throwaway boot.
4. Broaden other drivers only when the home screen stays smooth.

## Lab GPU note

Vendor FW is in `fw/` (`a618_gmu.bin`, `a630_sqe.fw`, `a615_zap.*`). Embedded
in 0093+. Bring-up timeouts are soft (milliseconds). Failures must not
hang the CPUs or the display path.
