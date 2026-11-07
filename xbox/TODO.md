# Xbox port: to do

Status and history live in `README.md`; this is what is left, roughly in
order. Measurements are from XEMU at 64 MB unless noted.

## 1. 60 fps at 720p

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
      buffer (three buffers plus depth); budget it against memory (section 4).
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

## 2. Readable text

- [ ] Fonts and UI art were converted at 512x512 at most, halving the
      1024x1024 atlas. Now (not yet seen in XEMU): up to 1024, and textures
      larger than 256 as DXT5 (`src/dxt.cpp`), so full resolution costs less
      memory than before. Check the text on screen.
- [ ] Converted textures now carry mipmaps (2x2 averages, DXT5 down to 4x4)
      with trilinear filtering: the game draws its text smaller than the
      textures at 640x480, and without mipmaps the glyphs alias. Not yet seen
      in XEMU. 720p would help further.
- [ ] Texture format 26 (seen as 18x12) is not converted yet.
- [x] The debug overlay ("Geometry Wars RE2 - Xbox frame N") covered the
      score; it is off unless `make OVERLAY=y`. The frame rate is in
      `gw2log.txt` on hardware.
- [ ] Grey textures (the fonts: white glyphs coloured by the vertices) are
      now kept exact as 16-bit alpha + grey instead of DXT5, whose 4x4
      blocks smear small letters. On hardware the Deadline rules screen and
      the menus read cleanly at 640x480; 720p is what would sharpen them
      further.

## 3. Glow

- [ ] Our own glow (`apply_glow` in `src/nv2a.cpp`, on by default, `make
      GLOW=n` without): the finished picture shrunk 4x, blurred, added back.
      Stand-in until the game's own chain below. It drew bright blocks in
      XEMU and nothing on hardware: the weights of the third and fourth
      samples went to combiner factors nothing reads. Fixed, and the GPU now
      finishes drawing each buffer before it is read back (needed on
      hardware). Seen on hardware: halos around the ships, enemies, HUD
      and menu panels; 19 fps in a level with it on.
- The game's chain (from the menu capture): the grid and the scene are
  drawn into float targets, resolved, shrunk and blurred through about 80
  small passes down to 2x1, summed per pyramid and composited with a tone
  curve. Its pixel shaders read as weighted texture sums
  (`xps_sum_of`, checked by `tools\vp_check.cmd` on `_ps_` captures), and
  `nv2a_sum_ps.ps.cg` sums four textures; what is missing is render targets
  other than the screen (pbkit only renders to the screen and its extra
  buffers) and resolves into textures.

- [ ] Render targets and resolves: the 360 draws the glow chain and the
      composite into EDRAM targets and resolves them to textures. On the NV2A
      they become surfaces in memory; resolves become no-ops or copies.
- [ ] Draw the glow-layer draws (vertex shader C5C713E2 for particles,
      8E1A294A for the in-level grid lines, whose three line buffers
      src/kernel_memory.cpp took for spark buffers), plus the blur and
      composite passes, likely at reduced resolution. The frame census now
      says why each skipped draw was skipped.
- [ ] Then give the glow-pass particle buffers and the grid-line buffers their
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

## 6. Sound

No XMA on the Xbox: the installer converts every sound to IMA ADPCM.

- [x] `installer\SoundConvert.cs` (and `tools\convert_banks.py`, the same for
      development): each wave of the banks (`audio\*.baf`; WAVE records of
      0x5C or 0x50 bytes) goes through ffmpeg once into 4-bit IMA ADPCM, written
      as `audio\<bank>.adpcm` next to the untouched banks, with a table keyed
      by each wave's place in its bank. 265 sounds, 1846 s, 76 MB (the XMA was
      38 MB); about 18 dB signal to noise on the noisiest effects. Needs
      ffmpeg on the player's PC (or ffmpeg.exe next to the installer); takes
      about 8 minutes.
- [ ] Runtime: the game's XAudio2 decodes through the 360's XMA unit (only
      XMACreateContext / XMAReleaseContext are imports; contexts are driven
      through the audio unit's registers). Emulate the unit: a context's input
      address lies in a loaded bank, which names the wave; "decoding" writes
      that wave's ADPCM samples (decoded, 16-bit) to the context's output.
      The ADPCM files stay on disc and are read as the sounds play.
- [ ] A render-driver thread calls the game's XAudio callback every 256
      samples (48 kHz) and sends the mixed frames (6 channels, float) to the
      AC97 through nxdk, mixed to stereo 16-bit. The mixer costs CPU every
      5.3 ms, so this waits until the frame rate allows it.
- [ ] Music is sequenced from the song banks' samples (Song_N.dat), so the
      same path plays it.

## 7. Windows installer

- [x] `xbox\tools\make_installer.ps1` builds `xbox\build\installer` (and a
      zip): `install.cmd <LIVE package> <new folder>` checks the title,
      extracts the package, writes and checks the game image, and fills the
      folder next to `default.xbe`. Its output matches the staged test disc.
- [x] Sound conversion (section 6).
- [x] The extractor prints every file; the installer now hides that list.

## 8. Housekeeping

- [ ] Saves and profile settings persist only for the session.
- [ ] Push the `xbox/nxdk-port` branch when ready (nothing pushed yet).
