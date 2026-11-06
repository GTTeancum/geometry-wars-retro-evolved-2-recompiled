# Xbox port: to do

Status and history live in `README.md`; this is what is left, roughly in
order. Measurements are from XEMU at 64 MB unless noted.

## 1. 60 fps at 720p

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
      buffer (three buffers plus depth); budget it against memory (section 4).
- [ ] Measure on real hardware: XEMU's speed is not the Xbox's.
- [ ] Remaining hot spots after that (last profile): the grid job, now native
      (`src/grid_native.cpp`, about 10%); the particle job
      (guest 0x82063448, about 3.5%); vertex copies in `draw_on_gpu`.

## 2. Readable text

- [ ] Fonts and UI art were converted at 512x512 at most, halving the
      1024x1024 atlas. Now (not yet seen in XEMU): up to 1024, and textures
      larger than 256 as DXT5 (`src/dxt.cpp`), so full resolution costs less
      memory than before. Check the text on screen.
- [ ] Texture filtering and the 640x480 output also soften text; 720p helps.
- [ ] Texture format 26 (seen as 18x12) is not converted yet.
- [ ] Remove the debug overlay ("Geometry Wars RE2 - Xbox frame N") once
      hangs no longer need reporting by frame.

## 3. Glow

- [ ] Render targets and resolves: the 360 draws the glow chain and the
      composite into EDRAM targets and resolves them to textures. On the NV2A
      they become surfaces in memory; resolves become no-ops or copies.
- [ ] Draw the glow pass (vertex shaders C5C713E2 for particles, 8E1A294A for
      sparks, plus the blur/composite passes), likely at reduced resolution.
- [ ] Then give the glow-pass particle buffers and the spark buffers their
      own memory again (`src/particle_buffers.cpp`, `src/kernel_memory.cpp`
      share them while nothing draws them): about 10 MB that section 4 must
      find first.
- [ ] Pixel shaders: today only analysed for which interpolator feeds the
      texture; the glow composite needs real combiner setups.

## 4. Fit in 64 MB

- [x] A level ran out of memory after about 2000 frames. With the undrawn
      particle copies sharing one buffer it ran 6900 frames (two minutes of
      game time) and was still going.
- [ ] Main-picture particle buffers: 2 frames x 6 jobs of up to 4 MB, filled
      as particles grow. If the jobs run inline (section 1) one buffer per
      frame may do.
- [ ] Vertex buffer pool: 32 x 128 KB fully used (4 MB); try 16.
- [ ] Converted textures: 1 MB each at 512x512 ARGB.

## 5. Picture still missing

- [ ] Background grid: drawn into its own small target and distorted
      (vertex shader 8518CDFB, 46464 vertices); not drawn yet.
- [ ] Vertex arena overflows on some menu frames ("vertex arena full").

## 6. Sound

- [ ] Audio output (XAudio render driver callback) and the game's XMA music
      and effects: decode XMA on the Xbox or convert in the installer.

## 7. Windows installer

- [x] `xbox	ools\make_installer.ps1` builds `xboxuild\installer` (and a
      zip): `install.cmd <LIVE package> <new folder>` checks the title,
      extracts the package, writes and checks the game image, and fills the
      folder next to `default.xbe`. Its output matches the staged test disc.
- [ ] Format conversions for the Xbox (audio) once sound needs them.
- [ ] The extractor prints every file; quieter output.

## 8. Housekeeping

- [ ] Saves and profile settings persist only for the session.
- [ ] Push the `xbox/nxdk-port` branch when ready (nothing pushed yet).
