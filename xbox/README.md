# Geometry Wars: Retro Evolved 2 — original Xbox port (nxdk)

The ReXGlue recompilation of the Xbox 360 game, rebuilt for the original Xbox
with nxdk and tested in XEMU. The recompiled game code is unchanged; this
directory replaces the ReXGlue PC runtime with a small Xbox runtime:

| File | Role |
| --- | --- |
| `src/gen/pch.h` | Replaces the generated PCH: big-endian guest memory as `base + address` with 32-bit wraparound, host-side indirect-call table, setjmp map |
| `include/rex/` | Minimal stand-ins for the SDK headers the generated code includes (PPC context and vector helpers are copied from ReXGlue 0.10.0, BSD-3, with i386 paths added) |
| `src/guest_memory.cpp` | Guest windows (virtual heap, system heap, stacks, image, physical), commit-on-first-touch for large allocations, memory census |
| `src/kernel_threads.cpp` | Guest threads (KTHREAD/PCR/TLS/stack), events, semaphores, mutants, critical sections, spin locks, TLS, time |
| `src/kernel_memory.cpp`, `kernel_io.cpp`, `kernel_misc.cpp`, `xam.cpp` | The other kernel/XAM imports (153 in all) |
| `src/gpu.cpp` | Xenos stand-in: PM4 command processor for fences, writebacks, swaps and interrupts; 60 Hz vblank. Draws are counted, not rendered |
| `src/status.cpp` | On-screen status page, log ring read by `tools/read_log.py` |

## Goal

**Milestone 1: the game runs its own loop on a 64 MB Xbox.** It boots from
the disc, loads its data, and completes frames; the status page shows the
frame counter rising. No picture yet.

Later milestones:

2. **Title screen picture**: translate the game's draw packets and its ~18
   shaders to the NV2A (pbkit), with the bloom redone at 8-bit precision.
3. **Playable with sound**: controller input through nxdk, music and effects
   (the game's XMA audio decoded or converted for the Xbox).
4. **Full speed on hardware**, within 64 MB.

## Status (2026-11-05)

Booting in XEMU at 64 MB, the game:

- sets up its graphics (ring buffer, interrupt callback), starts its worker,
  audio and loader threads (18 threads), and issues its first draw calls;
- loads its text, the five title images and its data file;
- starts its audio engine and loads the sound effects and menu music.

It does not yet complete a frame. The blocker is memory:

| Use | Size |
| --- | --- |
| Program (recompiled code) | 18.6 MB |
| Title screen images | 12 MB |
| Music: six song banks, all kept in memory | ~33 MB |
| Sound effects and menu music | ~5 MB |

At 128 MB (a development kit, `boot_xemu.ps1 -MemoryMb 128`) it loads all six
songs and then runs out of guest address space during the last one. Music has
to be streamed from the disc or converted to a smaller format before the game
can fit in 64 MB; that is the next step.

Fixed on the way:

- The game's heap commits 64 KB pages and relies on the rounded-up tail.
- Two vertex-buffer rings of 18 × 4 MB and 18 × 3.5 MB, and 256 buffers of
  128 KB (about 167 MB in all, sized for the 360's 512 MB) are now reserved
  and backed only when touched. They use a few MB.

## Building and testing

Prerequisites: nxdk in `C:\nxdk`, LLVM in `C:\Program Files\LLVM`, MSYS2 in
`C:\msys64`, XEMU in `C:\Games\Emulators\Xemu`, Visual Studio 2022 (for the
one-off image dump).

1. `.\tools\extract_stfs.ps1 -Package <LIVE package> -OutDir game\extracted`
2. `.\tools\codegen.ps1` (generates `generated\`)
3. `xbox\tools\dump_image.cmd` writes the decrypted game image to
   `game\analysis\default.image.bin` and lists the variable imports.
4. `bash xbox/tools/build.sh` builds `xbox\build\release\default.xbe`.
5. `powershell -ExecutionPolicy Bypass -File xbox\tools\boot_xemu.ps1` stages
   `xbox\build\staged-disc`, builds `xbox\test-artifacts\gw2-current.iso` and
   boots it in an isolated XEMU (own config, test EEPROM, hard disk snapshot).
6. `python xbox\tools\read_log.py` prints the counters and the log tail from
   the running emulator. Every 10 s without a frame the log lists what each
   game thread last called.

Everything built or captured stays under `xbox\build\` and
`xbox\test-artifacts\` (both ignored by git).
