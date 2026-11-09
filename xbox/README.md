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
| `src/nv2a.cpp`, `xenos_vp.cpp`, `xenos_vs.cpp`, `dxt.cpp` | NV2A renderer (pbkit): game vertex shaders translated to NV2A vertex programs, textures converted (DXT5, mipmaps), our own glow |
| `src/input.cpp` | Controllers (SDL), plus a test-only remote pad for `tools/xemu_pad.py` |
| `src/*_native.cpp`, `particle_buffers.cpp` | Native rewrites of the game's hottest routines (grid, particles, starfield, sine), each checked against the recompiled original by `tools/native_check.cmd`; memory-saving buffer sharing |
| `src/wait_stats.cpp` | Wait times by thread and call site, logged every 10 s |
| `installer/` | Windows installer: prepares the game from the player's own LIVE package |

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
   Reached (console, 2026-11-08): the game updates at full speed, 58.6
   updates a second median with four players, and draws every second update
   (`GW_DRAW_EVERY` in `src/render_native.cpp`), 30 pictures a second at 720p.
   Per update in a level: grid about 2.2 ms, particles 0-4, stars 1.2, sound
   2.0, commands 0.1.

## Status (2026-11-05)

**Milestone 1 reached.** In XEMU at 64 MB the game boots, loads all its data,
title images, sound effects and the six songs, and runs its own loop: about 11
frames a second (some 120 draw calls each), with 35 MB of guest memory
committed. The status page shows the frame counter rising. Nothing is drawn
and no sound plays yet.

How it fits in 64 MB:

- **Commit on first touch.** The game reserves two vertex-buffer rings
  (18 × 4 MB and 18 × 3.5 MB) and 256 buffers of 128 KB, about 167 MB sized
  for the 360's 512 MB. Allocations of 128 KB and up get address space only;
  the first touch of each 16 KB backs it with memory.
- **Music stays on the disc.** The game reads each song bank whole (about
  33 MB for the six) and copies the sample data into its sound buffers. Large
  reads into untouched memory are recorded instead of performed, the game's
  memcpy/XMemCpy hand the record on to the destination, and a page is read
  from the disc only if something touches it. The sound layer can stream music
  from the same records later.
- **Physical memory at 0xC0000000.** The game's Direct3D converts GPU
  addresses back with "0xC0000000 + physical", so that view is the real one.

Other fixes on the way:

- The game's heap commits 64 KB pages and relies on the rounded-up tail.
- XMP (the music player) polling is answered as on ReXGlue.
- Two GPU waits on memory at 0x20B000 are never released by the game; with
  no rendering they are skipped after a short spin.

## Milestone 2: what the title screen draws

`xbox/src/gpu_capture.cpp` records one frame (swap 120) of the command stream;
`python xbox/tools/gpu_capture.py` saves it from a running XEMU and writes
`xbox/test-artifacts/gpu/report.txt` plus every shader's microcode.

One title frame (2026-11-05):

| Part | Draws |
| --- | --- |
| Background grid: one indexed quad list, 46,464 indices | 1 |
| Starfield: 6,000 point sprites (3×3 texture) | 1 |
| Logo pieces (8 quads) and the 1280×720 title image | 2 |
| Glow: downsample/blur passes from 640×360 to 2×1, 8-bit targets, then the composite with the float 1280×720 scene | ~54 |
| EDRAM resolves (copy to texture) | 57 |
| Clears | 3 |

Only 7 distinct vertex shaders and 12 distinct pixel shaders are used (the
resolve vertex shaders are inline copies of one program).

### Progress (2026-11-06)

Deadline is playable in XEMU at 64 MB at about 30 frames a second (the game is
a 60 Hz game, so it plays in slow motion), and a level ran 6900 frames without
running out of memory. What got it there:

- Vertex shaders become NV2A vertex programs (`src/xenos_vp.cpp`; any that
  do not translate run on the CPU through `src/xenos_vs.cpp`).
- The Xbox codegen (`codegen.toml`) keeps the PowerPC registers in C++
  locals, and `tools/prepare_generated.py` swaps SIMDe's byte-loop vector
  loads and the x87 rounding-mode switches for cheap equivalents: 11.6 to
  17 fps, and the XBE from 17.2 to 9.2 MB.
- The grid physics, particles, starfield and sine are native C++
  (`src/*_native.cpp`), checked against the recompiled originals.
- Memory: copies of buffers nothing draws yet share one buffer
  (`src/particle_buffers.cpp`, `src/kernel_memory.cpp`).

Built since that test and not yet run in XEMU: our own glow, the in-level
grid lines, mipmapped full-resolution textures (readable text), and the
native starfield and particles. `TODO.md` lists what is left (60 fps at 720p, the game's own
glow chain, sound).

Fixed on the way: the CP's read-pointer writeback was little-endian (hang at
frame 453); the 256-buffer vertex pool cycles through 32 buffers; guest
addresses were computed with an overflowing pointer sum that clang folded to
0 in one place.

Plan for the NV2A renderer (pbkit):

1. Disassemble the 19 shaders (Xenos microcode) and write an NV2A
   equivalent for each: vertex programs (or a CPU transform) and register
   combiner setups.
2. Render targets become NV2A surfaces in memory, so EDRAM resolves turn into
   no-ops or simple copies; the float scene target becomes 8-bit ARGB.
3. First picture: grid, stars, logo and title image without the glow. Then the
   glow chain at reduced resolution.

## Building and testing

Prerequisites: nxdk in `C:\nxdk`, LLVM in `C:\Program Files\LLVM`, MSYS2 in
`C:\msys64`, XEMU in `C:\Games\Emulators\Xemu`, Visual Studio 2022 (for the
one-off image dump).

1. `.\tools\extract_stfs.ps1 -Package <LIVE package> -OutDir game\extracted`
2. `.\tools\codegen.ps1` (downloads the SDK, generates the PC `generated\`),
   then `xbox\tools\codegen.ps1` (the Xbox variant in `xbox\build\generated`,
   registers as C++ locals; see `xbox\codegen.toml`).
3. `xbox\tools\dump_image.cmd` writes the decrypted game image to
   `game\analysis\default.image.bin` and lists the variable imports.
4. `bash xbox/tools/build.sh` builds `xbox\build\release\default.xbe`.
5. `powershell -ExecutionPolicy Bypass -File xbox\tools\boot_xemu.ps1` stages
   `xbox\build\staged-disc`, builds `xbox\test-artifacts\gw2-current.iso` and
   boots it in an isolated XEMU (own config, test EEPROM, hard disk snapshot).
   `-MemoryMb 128` boots a development-kit Xbox (test only).
6. `python xbox\tools\xemu_pad.py "press A" "wait 120" "shot menu"` presses
   buttons through a test-only remote pad and saves screenshots to
   `xbox\test-artifacts\pad` (a real controller works too).
7. `python xbox\tools\read_log.py` prints the counters and the log tail from
   the running emulator. Every 10 s without a frame the log lists what each
   game thread last called.

For players: `powershell -ExecutionPolicy Bypass -File xbox\tools\make_installer.ps1`
assembles `xbox\build\installer` (and `gw2-xbox-installer.zip`); in it,
`install.cmd <LIVE package> <new folder>` prepares the game from the
player's own package next to `default.xbe`.

Everything built or captured stays under `xbox\build\` and
`xbox\test-artifacts\` (both ignored by git).
