# Xbox port: to do

Status and history live in `README.md`; this is what is left, roughly in
order. Measurements are from XEMU at 64 MB unless noted.

## 1. 60 fps at 720p

- [x] **Four players at full speed (console, 2026-11-08).** 15-minute
      four-player soak: median 58.6 updates a second (30 drawn), 50 at the
      worst 5-second window, no crash, 4.9 MB free. Was 42-47 (21-23 drawn).
      Particles in place (their mirrors were registered at the buffers'
      headers, so nothing ever matched), the particle update's constants
      read once per job, its particles prefetched four ahead, its selects
      without branches (640 cycles a particle, was about 1,000; four at once
      in SSE measured slower, 1,150), the stars four at once, square roots
      as sqrtss (-fno-math-errno). Per update now: grid about 4 ms
      (memory-bound), particles 3-5, sound 2.5, stars 1.3.
- [ ] **Pausing freezes the game (found 2026-11-07).** Pressing Start in a
      level stops the picture: the main thread spins in the game's
      BlockOnFence (sub_820A4CE0 / sub_820A1B18), waiting for a GPU fence.
      Read on the console: progress (device +10896's word) 0x430F, the
      device's fence counter (+10908) 0x43CB: 188 fences inserted that never
      ran. During play the progress keeps pace (two behind, logged by the
      temporary audit in src/gpu.cpp, audit_progress). Not the native draws
      (native_enabled off: same) nor the interrupt threads' processors.
      Suspects: the commands now running on the thread that writes them
      (run_commands) and a fence the game expects the GPU to reach without
      another kick (the 360's GPU runs on its own), or older: pausing was
      not tested for a long time. Next: try the last commit (197d89a); log
      what the pause path writes after the last kick.

### Native drawing (replacing the 360 graphics path)

The game builds every frame as Xenos (360 GPU) command packets through its
Direct3D, and src/gpu.cpp + src/nv2a.cpp decode and re-issue them for the
NV2A: the frame is built twice. The plan, in order:

- [ ] 1. Commands run on the thread that writes them (src/gpu.cpp
      run_commands), no command processor thread; its interrupts are
      delivered by a small interrupt thread.
- [ ] 2. Native Direct3D draw calls (DrawVertices 820A08B8,
      DrawIndexedVertices 820A0CA0, BeginVertices/EndVertices
      820A0408/820A08A8) that read the device's state shadows and draw on
      the NV2A at once: no draw packets built (draw setup 820A76B0, constant
      flush 820A91A0) or decoded.
- [ ] 3. Native code writes its vertices (line batches, text) in host order
      where the NV2A reads them: no byte swaps.
- [ ] 4. The game's frame rendering (sub_820653C8 and the passes under it)
      native, on top of 1-3.


On hardware (Deadline, moving and shooting): 29-33 fps.
`tools/xbdm_profile.py` samples the console's threads through xbdm, and the
log's `cpu ms/frame` line (src/status.cpp) times the largest parts. Per
frame now: grid 7 ms (physics 4.6, lines 2.2), renderer 8-10, particles 2,
starfield 2, the rest (the game's own code) about 17.

- [x] Grid: one job thread does every row (the other five return at once);
      points kept in host floats (byte swaps through memory were most of
      its time on the Pentium III); the lines drawn by the NV2A as smoothed
      lines from their end points. 14 -> 7 ms.
- [x] Renderer (command processor thread): 8-10 -> 6-7 ms. The vertex copy
      waited on memory (the game's vertices have left the cache by then):
      now one straight copy with read-ahead; pbkit blocks of 512 words, not
      32 (each pb_end flushes the write-combining buffer and polls the GPU).
- [x] The game's Direct3D itself is cheap: most samples under it were the
      main thread switched out while the command processor ran (6 kicks a
      frame). The command processor must stay above the game's threads:
      below them the game hangs (something spins on the GPU's progress).
- [ ] Renderer: the packet parser (13% of its samples), gw_nv2a_swap (8%).
- [ ] Profile the game's own code (main thread) with xbdm_profile.py.

- [ ] **Find the 30 fps ceiling.** Deadline runs at 30-32 fps. The game is a
      60 Hz game (its clock advances 1/60 s per frame, so at 30 fps it plays
      in slow motion). The wait log (`src/wait_stats.cpp`, every 10 s) shows
      the renderer and command processor hardly wait, the job threads idle,
      and the main thread busy about two thirds of the time, while the
      sampled CPU is in kernel code about 60% of the time. Suspect: a stream
      of memory faults (kernel exception dispatch); the 10 s log now also
      counts commit-on-touch faults by allocation. If it is faults, stop them;
      if it is real work, the main thread needs about 21 ms -> 16 ms.
- [ ] **Cost of the job threads.** The game splits work over 6 job threads for
      the 360's 6 hardware threads; the Xbox has one core, so every hand-off
      is a context switch. Try running the jobs inline on the calling thread.
- [ ] **Output 1280x720.** The back buffer is 640x480 today (`nv2a: ready,
      640x480`), the game's 1280x720 scene scaled down. `make VIDEO=720p`
      (touch `src/status.cpp` first) asks for 1280x720 and falls back to
      640x480; try it once the memory census shows 6 MB to spare. 720p needs the
      component/HDMI video modes on hardware and about 1.8 MB per 16-bit
      buffer (three buffers plus depth); budget it against memory (section 2).
- [ ] Guest memory accesses are no longer volatile, with a compiler barrier
      at every label (`src/gen/pch.h`, `tools/prepare_generated.py`): 27%
      faster recompiled code on the grid in the PC check. Not yet run in
      XEMU; if the game hangs or misbehaves after it, undo this first.
- [ ] The memory base is a compile-time constant in the generated code
      (`REX_HOST` in `src/gen/pch.h`): 5-9% faster grid code on the PC check
      but 0.8 MB more XBE (8.3 -> 9.1 MB). Weigh it against memory once the
      memory census shows the headroom; measure on the Pentium III.
- [ ] Measure on real hardware: XEMU's speed is not the Xbox's.
- [ ] Remaining hot spots after that (last profile): the grid job, now native
      (`src/grid_native.cpp`, about 10%); the particle job
      (guest 0x82063448, about 3.5%); vertex copies in `draw_on_gpu`.

## 2. Fit in 64 MB

- [x] A level ran out of memory after about 2000 frames. With the undrawn
      particle copies sharing one buffer it ran 6900 frames (two minutes of
      game time) and was still going.
- [ ] Main-picture particle buffers: 2 frames x 6 jobs of up to 4 MB, filled
      as particles grow. If the jobs run inline (section 1) one buffer per
      frame may do.
- [ ] Vertex buffer pool: 32 x 128 KB fully used (4 MB); try 16.
- [ ] Converted textures: 1 MB each at 512x512 ARGB.

## 3. Picture still missing

- [x] **Menu flicker on idle menus (seen 2026-11-08) — fixed, confirmed on
      the TV.** Not the 2026-11-07 kind (no frame lost
      draws): the logo's two ends dimmed for 1-3 frames every few frames.
      Found with the capture build's flicker detector (make SPAWN_CAPTURE=y:
      every frame checked in 20x20-pixel cells, logged as "capture: frame
      ..."). The glow's full-screen add quad lost its upper-right triangle
      in those frames (the changed pixels all above the screen diagonal; gone
      with GLOW=n). Its corners now go inline in the command stream instead
      of the vertex arena (glow_quad): 3+ minutes idle, no flicker. Clearing
      the vertex cache, an extra wait after a target change and a separate
      area for the last blur did not help, so the cause in the arena path is
      not known; other arena draws could have the same fault.
      Still seen on the TV after that: the whole screen blinking. pbkit
      hands out its three buffers in turn whatever is on screen, so with two
      finished frames waiting for vblank the next frame's black fill wiped
      the buffer the TV was showing (the capture build reads frames as drawn,
      so it never saw this). begin_frame now waits while the display shows
      its back buffer: on the idle menu that happened about every 8 s.

- [ ] Background grid: drawn into its own small target and distorted
      (vertex shader 8518CDFB, 46464 vertices); not drawn yet.
- [x] Grid lines (vertex shader 8E1A294A, pixel shader B16AAC17): they were
      drawn but invisible (the stand-in combiner read a colour the program
      never wrote) and the 65528-vertex half was dropped (too big for the
      vertex arena). Now their own translation (`line_i` in xvp_get) and
      combiner (`use_line_combiner`): colour = (line colour x brightness)^2,
      faded as (1 - |edge|)^3 across each line, as the game's shader. The
      game's 18x12 colour map (a GPU render target) is one fixed colour for
      now. Big draws go to the GPU in pieces. Seen on hardware: blue grid,
      every eighth line bright, bending around the ship and shots. The grid
      job writes the lines straight into a contiguous host mirror in the
      NV2A's order (`gw_grid_line_host`), drawn in place: copying them cost
      19 -> 8 fps; in place a level runs at 25 fps. Still to do: a second
      mirror if the one shared by the frames in flight shows tearing.
- [ ] Grid colour field (`field_begin` / `field_end` in `src/nv2a.cpp`, from
      a level capture on hardware, `tools/gpu_capture.py --xbdm`): the game
      splats a light per object into a 150x100 field (vertex shader
      C5C713E2, HDR colours up to about 20), shrinks and blurs it to 2x1,
      sums the levels and keeps half of last frame's field; the grid lines
      read it. Reproduced at 75x50 in pbkit's extra buffers (16-bit, a
      quarter of the true value), 4-tap blurs, no movement with the
      distortion. It comes out dimmer than the game's, so a dim blue floor
      keeps the grid visible; tints show near explosions. Costs about 3 fps
      (25 -> 22). To match the game: brighter/wider blurs (its are 15-tap),
      the distortion's movement (needs the grid displacements, now not
      written), and checking against the original on a 360 or in a capture. `make GRID_CHECK=y` compares the guest buffer,
      which the native code no longer writes.
- [ ] Vertex arena overflows on some menu frames ("vertex arena full").

## 4. Sound

No XMA on the Xbox: the installer converts every sound to IMA ADPCM. None of
the game's audio engine, its XAudio2 or the XMA unit runs: the game's sound
layer is cut over at its events and parameters (src/sound_game.cpp) to a
native engine that plays the game's sound graphs (src/sound_native.cpp).

- [x] **Native engine (2026-11-07).** The nine sound graphs (audio\*.dat:
      System, GW3_FX, UIMusic, Song_1..6; 1342 components of 47 kinds) are
      interpreted as the game's engine does (timing, scripts, every component
      kind; derived from the game's code). Sound effects' bank in memory, the
      music streamed from the disc. Surround is carried as stereo (the
      Xbox's output); chains of mixers are combined so each sound is mixed
      once; silent layers are skipped undecoded and keep their place. The
      reverb send goes through a reverb of our own (the 360 used XAudio's
      built-in one). Pause stops the six songs' graphs, as the original.
- [x] PC check (`xbox\tools\sound_check.cmd`): plays the graphs from the
      staged disc through a scripted menu, Song 1 with shots and explosions,
      and Song 3, and writes `xbox\build\sound_check\menu.wav`, `game.wav`,
      `song3.wav` with each second's level and cost. Streaming in real time
      matches an all-in-memory run (SOUND_RESIDENT=1) to rounding.
      Cost on the PC: about 22 us per 5.3 ms frame in the menu, 30 us in a
      busy game.
- [ ] **Cost on the console (2026-11-07).** It plays steadily (187.5 frames
      a second, 12 queued). Mix: about 0.16 ms per 5.3 ms frame in the menu,
      0.55-0.7 ms in a level (it was 1.9 ms): control 0.09, sounds 0.16,
      effects 0.10, combining the mixer chains 0.09, mixing 0.05, reverb
      0.06 ms (the log's "audio: us a frame" lines). What makes the Xbox
      slow and the PC not: data fetched from memory (the game's threads push
      the engine's data out of the cache between frames; the reverb's
      arithmetic alone takes 17 us warm, 60-70 in the game), and nxdk's
      -fno-builtin turning fabsf/fmaxf/sqrtf into library calls. Frames are
      mixed 4 at a time (the later ones find the data in the cache). Prefetch
      instructions did not help. Next: a smaller working set (hot fields of
      components and voices together; combining only what changed).
      Not yet listened to on the TV.
- [x] **Lock-up after a few minutes (2026-11-07):** the console ran out of
      memory (the game commits memory as it plays). The effects' long waves
      (34 of 104, 3.7 MB) now stream like the music; about 11 MB free after
      start-up (was 7.7). The first three fatal errors of a run are saved to
      E:\GW2\gw2crash1.txt ..3 (the log ring is soon overwritten).
- [x] **"Unanticipated CPU_INTERRUPT" / call to 0BADF00D (2026-11-07):** the
      commands run ahead of the interrupts' delivery, so a later interrupt's
      callback (scratch registers 4 and 5) overwrote an earlier one's before
      its delivery; the graphics interrupt thread then died. Each INTERRUPT
      packet now records its callback and the scratch write-back is repeated
      just before its delivery (src/gpu.cpp).
- [ ] TransitionSwitch (Song 3's output the game waits on) and the music's
      section changes: heard only in the PC check so far.
- [x] `installer\SoundConvert.cs` (and `tools\convert_banks.py`, the same for
      development): each wave of the banks (`audio\*.baf`; WAVE records of
      0x5C or 0x50 bytes) goes through ffmpeg once into 4-bit IMA ADPCM, written
      as `audio\<bank>.adpcm` next to the untouched banks, with a table keyed
      by each wave's place in its bank. 265 sounds, 1873 s, 77 MB (the XMA was
      38 MB); about 18 dB signal to noise on the noisiest effects. Needs
      ffmpeg on the player's PC (or ffmpeg.exe next to the installer); takes
      about 8 minutes. One entry (GW3 LeaderChange2) says more samples than
      its data holds; the engine plays what is there.

## 5. Windows installer

- [x] `xbox\tools\make_installer.ps1` builds `xbox\build\installer` (and a
      zip): `install.cmd <LIVE package> <new folder>` checks the title,
      extracts the package, writes and checks the game image, and fills the
      folder next to `default.xbe`. Its output matches the staged test disc.
- [x] Sound conversion (section 4).
- [x] The extractor prints every file; the installer now hides that list.

## 6. Housekeeping

- [x] Push `main` (first pushed 2026-11-08; all work is on `main`, no
      branches).

## 7. Console integration

- [ ] Saving and loading: confirm on the console that saves and profile
      settings are written and read back after a restart (today they persist
      only for the session). Give the XBE the 360 game's title ID,
      `0x584108FF` (the STFS header, the XEX execution info and the XDBF all
      agree on it; see `docs\package-analysis.md`), so the saves have their
      own folder on the hard disk. The XBE sets no title ID today (only
      `XBE_TITLE` in the `Makefile`), so cxbe's default is used.
- [ ] Dashboard icon: use the 360 game's icon as the XBE's title image (the
      original Xbox dashboard's game icon). Source: the icon in the XEX's XDBF
      resource or the package's thumbnail.
- [ ] Menu items for 360-only features (Xbox LIVE, leaderboards,
      marketplace and the like): selecting one opens a popup in the game's
      own style saying NOT IMPLEMENTED, instead of doing nothing or failing.
