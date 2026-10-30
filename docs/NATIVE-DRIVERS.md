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
| Display present | **0095** logical→panel crop flip | `flips` rising; present us near 0; no 10 MB memcpy |
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

## GPU vs smoothness (honest)

Until AGX→a6xx (years), “GPU support” does **not** make SpringBoard smooth.
What does: DPU **flip** of the buffer CA already drew (0095 crop flip).
Adreno ring/blit is lab-only proof we can move pixels later for *our*
compositor path — never enable `sweetgpu>=2` on a feel boot.

Vendor FW is in `fw/` (`a618_gmu.bin`, `a630_sqe.fw`, `a615_zap.*`). Embedded
in 0093+. Bring-up timeouts are soft (milliseconds).
