# ios-topaz → lavender (Redmi Note 7, SDM660)

Work-in-progress port of this repository's XNU bring-up to the Xiaomi Redmi Note 7
(`lavender`, Snapdragon 660, eMMC, 1080x2340). Branch `lavender`; the topaz work is
untouched on branch `topaz`.

| Path | What |
|---|---|
| `shim/` | Own boot shim (C + start.S): starts on Silver cpu0, hands off to Gold cpu4 (MPIDR 0x100, the only cores with a 16K granule), loads a `SWEETXNU` payload, prepares GIC-500 like topaz's shim, jumps to XNU. `build.ps1` (LLVM + Python on Windows). |
| `tools/mkadt_lavender.py` | Fake iPhone X (D22AP) device tree with lavender addresses. |
| `tools/mkpayload.py`, `adt.py` | From topaz, unchanged. |
| `tools/mktest.py`, `mkboot.py`, `dtbinfo.py`, `mkfakepayload.py` | Bare-metal test images, boot.img (header v1) packer, DTB splitter, loader self-test. |
| `PORT-PLAN.md` | Milestones (Ukrainian). |

Verified on the device (all via `fastboot boot`, nothing flashed):
ABL accepts header v1 + Image.gz + the real lavender DTB (msm-id 317, board-id 0x20008);
framebuffer 0x9d400000 1080x2340 stride 4320; Silver→Gold hand-off; 16K MMU on Gold;
shim loader runs a test payload at PA 0x100004000.
