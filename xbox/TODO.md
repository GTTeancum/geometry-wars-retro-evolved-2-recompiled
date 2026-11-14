# Xbox port: to do

Status and history live in `README.md`; this is what is left, roughly in
order. Measurements are from the console unless noted: since 2026-11-12 a
retail Xbox in debug mode (CerBIOS), 64 MB, at 10.0.0.231.

The open items come from the v0.8.0 beta's first reports (the r/originalxbox
release thread, 2026-11-12).

## Done in the working tree, not yet committed

- **480p crashed at start** (reported). At 720 or 640 pixels wide (16-bit)
  pbkit drew with the theoretical line length (1472 or 1280 bytes) inside a
  tile laid out with the next tile pitch (1536): the NV2A wrote out of bounds
  and the console hung, debugger included. Fixed in our copy of pbkit's core
  (`third_party/pbkit/pbkit.c`, two lines marked "GW2 Xbox"), linked ahead of
  nxdk's. 720x480 (480p) checked on the console and on a TV. 640x480 (480i)
  has the same fix but has not been run on hardware (needs 480p switched off
  in the dashboard).
  - Not a fault: on the test capture device the 480p picture looked shifted
    left. The dashboard shows the same offset there (the capture trims about
    20 pixels on the left); the kernel's own 480p timing is kept.
- **High score lost after switching off** (reported). The game keeps no
  scores in its profile; it sends them to its Live leaderboards and reads
  its best back from them, and the port answered neither. `src/live_scores.cpp`
  ports the PC port's local scores: the player reported signed in to Live,
  the best score per view kept in `E:\UDATA\584108FF\000000000000\scores.dat`,
  the game's leaderboard reads answered from it. XLiveBase messages answered
  as ReXGlue does (the friends request fails; a bare success crashed the
  game). Saves are now flushed to the disk (file and volume) when written and
  before the in-game reset.
  - Checked: a Deadline score kept through a cold restart.
- **Local leaderboard with initials** (requested), in `src/live_scores.cpp`:
  a top 10 per mode with three initials each, in `scores.dat` (version 2;
  version 1 read as "---"). Every finished single-player game's score is
  taken from the game's score task before the game's own "only if it beats
  this session's best" check. A top-10 score is entered on the game-over
  screen: the prompt bar's line reads "Enter your initials: ABC" (the bar's
  own label, its style), up/down change the letter, A/right next, B/left
  back, A on the last (or Start) keeps them; the game sees a neutral
  controller meanwhile, and for 1.5 s after the game ends nothing counts.
  The last initials come pre-filled; the table is saved on every entry.
  Shown by the game itself: the Leaderboards screen's Overall and My score
  boards list the table (ranks, initials, scores), the mode select panel,
  the game-over board, the Friends board and the in-game high score show
  the best with its initials. Checked on the console with the remote pad
  (entries at ranks 1, 2 and 4, the boards above).
  - Not seen yet: the blink of the letter being entered (screenshots are
    too slow to catch it).
  - Score screens (asked by the user, checked on the console): the
    game-over screen's Previous Scores rows show each game's initials (the
    game's own session records, their names drawn by the text call
    returning to 0x82088D24 replaced by the initials entered for that score,
    or the last initials); its Friends Leaderboard is empty (its rows' text
    calls, returning to 0x82088FF0 and 0x82089030, skipped); the mode select
    panel lists the top three of each mode with their initials
    (XUserReadStats answers three rows, those after the first with XUIDs of
    their own). Both boards come through the same stats reader, so they are
    told apart where they are drawn.
  - Polish, if wanted: My score highlights the last row (every row carries
    the local player's XUID); the game-over board shows the best, not the
    entry being made (the game reads that board as a game starts).
  - The port is offline only: friends mean nothing. The Leaderboards screen
    still opens on its Friends filter (one row, the best) and the game-over
    panel is headed "Friends Leaderboard". Possible: open on Overall, and
    the game-over panel headed as a plain leaderboard.
- `make TEXT_TRACE=y` (`src/text_trace.cpp`): logs which game code draws
  which text, and where (how the game-over screen and prompt bar were found).
- Test tools: `make TEST_PATTERN=y` (lines every 8 pixels in from each edge),
  `tools/xbdm_log.py` (the log ring read from the console's memory, works
  after a hang), `tools/xbdm_watch.py` (exceptions and breaks from xbdm,
  optionally launching the game).
- Commit, and update the README's known issues (a retail console has now run
  it; 480p fixed; high scores kept).

## Open

1. **Slowdown late in a game** (reported, v1.1 console): King playable at
   first, then slow motion by about 1,700,000 points; worse in Evolved
   (snakes, about 5,600,000), Pacifism the same. Not bombs.
   - Benchmark: `make TEST_CHEATS=y` (`src/test_cheats.cpp`: invincible,
     every mode shown, the save untouched), then
     `bash xbox/tools/stress_evolved.sh <name>`: Evolved, nobody firing, the
     enemies piling up; speed every 10 s (`tools/xbdm_speed.py`) into
     `test-artifacts/stress-<name>.log`. Baseline: 60 updates/s for a
     minute, 37 at 90 s, 24 at 3 min, 12 at 6 min.
   - Where it goes (6 min): the main thread is the limit (sound is about a
     fifth of the CPU by its own counters, on its own thread). Main thread:
     object outlines about 40% (line builder 23, transforms 9, outline 6),
     the renderer's vertex copy and draw 13%, separation 9% (after its
     rewrite), grid 3%, then many 1-2% recompiled entity functions.
   - The benchmark repeats exactly (the test build seeds the game's random
     generator with 12345): compare builds at the same game update, not
     the same second. Baseline worktree (HEAD + the 480p fix + test
     cheats) built at D:\Programming\GitHub\gw2-baseline.
   - `tools/stress_play.sh`: the same, the ship moving and firing
     (`tools/stress_fire.txt`, steps in game updates; repeats closely, not
     exactly). In play the game's own counters show, per update at about
     25 updates/s: sound 8-10 ms, particles 6-8, grid 2.5, stars 1.3.
   - Done, all bit-identical in `tools\native_check.cmd` (cosine within
     1.5e-7, as the sine): separation (`src/entity_native.cpp`, with
     prefetching), the object outline fused (one routine: transforms, line
     math, streaming stores), cosine and the rotation matrix
     (`src/math_native.cpp`). Pile-up benchmark at the same game update,
     updates/s: original 46 / 31 / 23 / 12 at updates 4000 / 6000 / 8000 /
     10000; with the rewrites 57 / 41 / 32 / 15.
   - Done: adaptive drawing (`src/render_native.cpp`; make ADAPTIVE=n for
     the fixed every-second frame): a picture every 2, 3 or 4 frames, the
     most often that fits 60 frames a second (measured cost of drawn and
     undrawn frames). Same points: 60 / 60 / 43 / 23 updates/s, pictures
     30 down to 15 as the crowd grows; full speed held to about update
     7000 (the original lost it by 3000). Beyond that the update alone is
     over budget. To decide with the user: 15 pictures a second at the
     worst is choppier; the alternative is slow motion.
   - Done (second round, all checked bit-identical or byte-identical):
     - Deferred outlines (`src/shapes_native.cpp`, `src/nv2a.cpp`): a crowd
       makes about 23,000 outline segments a drawn frame, 12 vertices (384
       bytes) each, about 9 MB, which were written to the game's chunks and
       read back, byte-swapped and copied by the renderer. The game keeps
       every chunk until the end of the frame, so host copies of the chunks
       (tried first, `gw_vertex_mirror`-style, per lock) cannot fit in 64 MB.
       Now a chunk the native builder fills from its start keeps only each
       segment's inputs (56 bytes) plus a tag in its first vertex, and the
       renderer builds the vertices from them straight into its vertex arena
       with the same arithmetic. Falls back to the guest chunk when other
       code writes into it or the segment store (up to 2.6 MB, grown only
       while 6 MB stays free) runs out.
     - Vertex arena as a fenced ring (`src/nv2a.cpp`, `third_party/pbkit`):
       eight segments, each released by a GPU semaphore (pbkit's unused
       DMA object 8, now uncached); the CPU waits only for the oldest
       segment instead of the whole GPU when the arena fills. A lost fence
       (100 ms) falls back to the old idle wait.
     - Particles four at a time (`src/particle_native.cpp`,
       particle_update4): the same single-precision operations in SSE
       lanes; the job batches four live particles (compaction never moves a
       particle onto another's old slot, so reading all four first is safe).
     - Sound: stereo ADPCM decoded both channels in one loop (two
       independent chains), the effects' filters with coefficients and
       state in registers and both channels in one loop, and the effects
       held in memory decoded once and cached (1 MB, least recently played
       given up; each block decodes the same wherever it starts)
       (`src/sound_native.cpp`); PC sound check output byte-identical. The
       audio report gives voices a pass and decoding time.
     - 24 recompiled enemy/world functions native (`src/enemy_native.cpp`,
       about 13% of the main thread in a crowd, each with a checker case):
       nearest target 0x82052860, Entity::Tick 0x820453E0, hash cell
       0x82046BD8, the enemy updates (homing, wanderer, dodger, pulsing,
       four-way roller, drifter), grid pushers (fused), arena clamps,
       look-at/scale/world matrices, nearest player, the 3D line, sprite
       and glow vertex builders (0x82060CC0, 0x8205FA98, 0x82045B18).
       Unverified work goes under GW_ENEMY_WIP until its case passes.
   - Measured: in the pile-up benchmark the GPU is now the limit at the
     heaviest point (hundreds of piled-up enemies' outlines, about 70,000
     quads a drawn frame): deferring the outlines made the CPU reach the
     GPU's queue sooner and wait there. The pile-up no longer measures CPU
     work past update 7000; the playing benchmark is the measure.
   - Next: the white shape (below, also a GPU cost), the GPU side of the
     strokes (vertex program length or fill and overdraw), the sound
     graph's combining (about 150 us a pass) and control, the voices on the
     APU (shelved, git 52c1b75; voices now cost more than when it was
     judged); separation's cost is mostly memory latency over the hash
     lists (left as is).
   - Fixed: during play a music stream was late on nearly every sound pass
     (60,000 late passes in 6 minutes): the disc reader thread ran at
     normal priority and a busy game thread kept it from running. It runs
     at the highest priority now (it needs little time); late passes
     stopped growing (554 in a whole run, at the start). The audio report
     now says why a stream was late.
   - The white shape: found by switching draw kinds off on the running game
     (make DRAW_SKIP=y, tools/xbdm_poke.py gw_draw_skip <hash>): the outline
     strokes (vertex shader 6FA9F218), both passes. Two causes:
     - A renderer fault, fixed: points far off the screen (ordinary geometry
       just outside the view, or debris 220 units off the plane) landed past
       the NV2A's coordinate range and their triangles wrapped into wedges
       across the screen. The translated programs now keep the screen
       position within +-2000 pixels (src/xenos_vp.cpp epilogue, constant
       65 set in begin_frame); nothing on or near the screen changes. (Not
       the near plane: the game's clipping is on, the camera is 512 units
       above the plane, z / w clipping changed nothing.)
     - A test artifact: the shape and the remaining white mass appear only
       with the test build's invincible ship, which ignores hits, so the
       enemies that reach it are never cleared and pile into it by the
       hundreds for minutes, their additive outlines saturating. A real ship
       dies at the first. With the new default cheat (lives never run out:
       hits, deaths and screen clears as in a real game; lives at guest
       0x8219D098 topped up, src/test_cheats.cpp, gw_cheat_invincible 2), a
       6-minute playing run never showed it. Invincibility is
       gw_cheat_invincible 1 (poke it while the game runs).
   - 30 ticks a second, a picture each: now the default (make TICK30=n for
     60). The game sets its time step from the display (guest 0x8205D538:
     1/60, 1/50 on PAL; 98 reads of 0x8219D280), so 1/30 is a setting it
     was built for. Checked: Deadline's clock keeps real time (31 s of game
     in 31.5 s). The jobs (particles, grid, stars, debris) step a fixed
     amount per call: run twice per tick, the first step building no
     vertices. Adaptive drawing at 30 ticks: a picture every tick, or every
     second (15 a second) when a frame does not fit (with no undrawn frames
     to measure, the missing kind is taken as the drawn one's cost).
     Results (TEST_CHEATS, sound on the APU):
     - crowd test (invincible, nobody firing): median 29.8 of 30 ticks,
       lowest 12.8 (43% of full speed; at 60 ticks the lowest was 20%); in
       6 minutes 305 s of game time (60 ticks: 206 s);
     - playing test, invincible: median 29.0 of 30, lowest 5.2 once the
       invincibility pile formed;
     - playing test, lives never run out: 25-30 ticks throughout; after
       the grid fix below, 24.4-30 (mostly 27-30), 17-30 pictures.
     - Fixed on the way: the grid's second step found its row counter used
       up (the game resets it, and flips the grid's position slots, once a
       tick in guest 0x8205C8F8): now reset and flipped between the steps.
     Not yet checked: whether fast shots now pass through small enemies
     between ticks (twice the distance a tick), and how the controls feel.
   - Sound on the APU, investigated (2026-11-13/14), still off:
     - The garble was the GP running no frames at all (frame counter at 0,
       its output cursor at 0); each sound pass then waited 40 ms for output
       that never came (about 12 passes a second of 187) while the AC97
       replayed stale buffers. With the GP running, the output is clean
       (lined up with the software engine: correlation 0.998, no repeats,
       no block-edge jumps) and effects play (PlayerMixer and the effects
       mixers carry signal; the "music only" heard was the dead GP: only the
       two sounds that stay in software got through).
     - When: after a debug-monitor warm restart of a session that used the
       APU (killed mid-frame). It stays dead through further warm restarts;
       a cold boot clears it; starting from the dashboard after a clean
       session is fine (the dashboard's own sound start-up resets it).
       Leaving for the dashboard while it is dead hung the console (black
       screen; power cycle).
     - Tried at start-up, none brings frames back: the GP reset and loaded
       again (the program memory cleared first: it does reload), the frame
       counter free running instead of AC97-synced, every voice record's
       link set to the list end before the table is given to the VP. (A DMA
       abort through GPRST bit 3 was tried once; the console hung right
       after: removed.) Likely the VP halted mid-frame (it stops when a
       listed voice ends, see src/apu.cpp) and nothing we do acknowledges it.
     - In place: gw_apu_init falls back to software sound when the GP runs
       no frames (checked); the APU left quiet on the return to the
       dashboard (gw_apu_shutdown) and before the test scripts' restarts
       (tools/xbdm_restart.sh pokes gw_apu_quiet_request). The hand-back and
       per-bin levels are logged every 5 s.
     - SOLVED (2026-11-13): the dead state is the setup engine waiting for a
       frame that never completes (its frame sample count, BAR0 0x2008,
       frozen; the GP's clock count at 0x3FF18 frozen; the GP runs no code
       at all, even with its wait patched out). Found by comparing the
       registers healthy and dead live on the console (make SOUND_APU=y
       APU_PROBE=y, tools/xbdm_apu.py). Nothing in BAR0 restarts it, but
       the APU's PCI power management does: D3hot then D0 (PMCSR, config
       0x48; capability at 0x44) resets the whole function like a power
       cycle; BAR0, the command register and the interrupt line are put
       back. gw_apu_init now always starts with that reset (sample counter
       at about 960 after it, as after a cold boot), and gw_apu_shutdown
       ends with it so the dashboard finds a fresh chip. Checked: three
       debug restarts in a row with voices playing, each came up at the full
       1,500 frames a second. (Also fixed: gw_audio_stop's APU shutdown was
       never built in, the SOUND_APU define reached only sound_native.)
     - A hazard that came with it: a healthy APU keeps running through a
       debug-monitor restart, its GP writing the output ring and its VP
       reading voice tables at physical addresses the next program now owns.
       Twice that crashed the next start (a kernel exception at start-up;
       once the console stopped with the LED blinking red/orange and needed
       two power cycles). Both times the test script's quiet request had gone
       to the wrong address (it used the new build's link map, not the
       running one's). Now: every build resets the APU first thing in main
       (gw_apu_reset_at_boot); tools/xbdm_deploy.sh quiets the running game
       at its own address (build/deployed.map, which the xbdm tools now read)
       before copying and restarting. A player never restarts this way: the
       return to the dashboard quiets and resets the chip.
     - The songs play about 58 layers at once, nearly all muted. On the APU
       every layer played (muted) took a voice and a disc stream: the 64
       voices filled and the streams fell behind. Now a looping voice moves
       to the APU only once it is heard in software, and back to software
       after a quarter second of silence on the APU (where a silent layer
       is skipped and its stream rests). Effects (one-shots) start on the
       APU as before. (The 1,100 us measured then was with the bins read
       back silent, see the next point: the real figures are below.)
     - After the reset the bins read back silent, then wrong: two faults.
       - The reset clears setup-engine settings a cold-booted console has
         (BAR0 0x2010-0x2028, 0x2078-0x20AC; the bytes at 0x208C look like a
         headroom per mix bin): without them every bin is silent. Restored
         from a cold-boot snapshot (kSeColdBoot in src/apu.cpp); the
         registers that held memory addresses are left cleared. (The bins
         now come back at half level; the start-up calibration scales it.)
       - With those settings the engine works 2,048 samples ahead of the
         sample clock and makes that up in a burst of 64 frames at start:
         the GP starts its next DMA before the last ends, and the output
         ring is left half a block out of step (bins 8-15 read as bins 0-7:
         heard as "music and the ship's explosion only, no firing"). Now
         put in step after the burst (gw_apu_realign: frames off, cursor to
         0, frames on), and watched: 8 passes out of step in a row put it
         back again (logged). The reader keeps a cushion of 3 passes.
       - Checked by recording (make AUDIO_CAPTURE=y): three matched 8 s
         windows of the playing benchmark, APU against software, agree
         within 1-2 dB in every octave band, the firing's 30 ms bursts
         present in both. Not yet heard by the user with these fixes.
     - Cost, playing benchmark (heavy play, same 150 s): the sound thread
       takes 1,360-1,540 us a 5.3 ms pass on the APU against 1,690-1,920 in
       software (about 8% of the processor back, 2.8 ms a 33 ms frame);
       game speed median 29.3 against 28.2 updates/s, lowest 24.0 against
       20.6. What is left on the APU's side: control 150, the graph's
       combining 240, effects 125, reverb 65, reading the bins 100, ring
       fills and positions 190 (120 of it copying blocks one at a time),
       routes 65, the ship's thrust in software 96. Since trimmed (not yet
       measured on the console, the code runs in xemu): ring fills copy
       runs of blocks, routes are written only when they change, the bins
       are prefetched, and the thrust (45 KiB, held in memory whole) plays
       on the APU: nothing is left in software in play.
     - Memory (64 MB): the APU build had 1.2 MiB free in heavy play against
       3.0 in software (a second copy of the 65 effects as the APU wants
       them, 820 KiB; stream rings 590; the GP's ring 256). Now the
       effects are held once, in contiguous memory with their silent tails,
       and played where they are (Bank::held); 40 stream rings; a 128 KiB GP
       ring: 2.4 MiB free in the same play. (The recorder's 1.5 MiB with all
       of that did not fit: the two "freezes" in capture runs were the game
       out of memory.)
     - OPEN, found at the end of the night (2026-11-14, about 03:40): the
       AC97 stopped taking frames (buffer index 0, control "run", status
       0, nothing left to play; its PCI command 0007, power state D0; codec
       registers read 0), in every build, software sound too, through warm
       restarts. Cause not known. Suspected: the APU's power-management
       reset done under a running AC97 (the quiet request did it from the
       sound thread; about a dozen times that night without harm). Changed
       since, untested on the console: the reset is only ever done with the
       AC97's bus masters stopped (gw_audio_stop: sound thread parked, AC97
       halted, APU quiet, then reset; at boot in APU builds: AC97 halted,
       then reset; the quiet request no longer resets). The boot reset is
       now only in builds that play on the APU. The sound thread reports
       "the AC97 takes no frames" with its registers.
       - NEVER put the AC97 (PCI device 6) in D3hot: tried to revive it
         that way from the probe and the console hung at once (no network);
         power cycle.
       - After the power cycle (2026-11-14, morning): the AC97 runs again,
         in the software build and in the APU build, through three quiet
         restarts and through a return to the dashboard and a start from
         there (the APU reset, calibrated and at the menu's level each
         time). The stuck state was the hardware's, cleared by power; with
         the reset never done under a running AC97 it has not come back.
         Its cause is still not proven.
     - The out-of-step ring fixed at its cause (2026-11-14): the GP's
       program now waits while its DMA runs (jset #4,x:$FFFFD6) before it
       reports the frame complete, so frames run back to back cannot cut a
       transfer short. On the console frames run at the full rate with it
       (the bit works as xemu has it) and the ring is in step from the
       start every time; before, the start-up put-in-step sometimes failed
       (a few frames run back to back when frames resume) and the level
       calibration then read 0 (half level until restarted). The
       calibration also checks the ring first and tries up to three times.
     - Cost with the trims, playing benchmark: the sound thread takes
       1,200-1,350 us a 5.3 ms pass on the APU (software: 1,690-1,920):
       about 500 us a pass, 9% of the processor, 3 ms of a 33 ms frame.
       Nothing is decoded in software in play (the thrust plays on the
       APU). On the APU's side now: reading the bins 119 (the APU wrote
       that memory, every line is a cache miss: 8 KiB a pass), positions
       and rings 88 (was 187: runs of blocks, non-temporal stores), routes
       43 (was 60: table logarithm, only changed words written). (MMX
       intrinsics do not build for the Pentium III with this clang: they
       are lowered to SSE2.) Free memory in the same play: 2.3 MiB.
     - DONE (2026-11-14): heard on the TV by the user ("sounds right",
       firing included); sound on the APU is now the default (make
       SOUND_APU=n for software mixing). Builds kept: build/variants/
       normal-apu (on the console), normal-sw, test-apu, test-sw.
     - Test tools: tools/xbdm_apu.py (the probe: BAR0 and PCI reads and
       writes, make APU_PROBE=y), tools/xbdm_apu_voice.py (voice records),
       tools/apu_bins.sh, tools/xbdm_quiet.py (asks the running game to
       leave the APU quiet, only after checking it is the build last
       deployed), tools/xbdm_deploy.sh and tools/keep_build.sh (builds kept
       under build/variants with their link maps), make MEMORY_WATCH=y.
     Earlier notes:
   - Sound on the APU (make SOUND_APU=y): BROKEN on the console. Heard on
     the TV as garbled, modem-like sound coming in only now and then: the
     AC97 output starves (about 700 frames sent in 4 minutes, against 187 a
     second in software: the audio thread waits on the APU's output and
     rarely gets it). The recordings below and the CPU saving were measured
     on that starved stream and mean nothing. Off; the software engine is
     the sound. Earlier notes, for the record: 16
     voices on the APU, none refused; the menu's output recorded both ways
     (tools/xbdm_audio.py, AUDIO_CAPTURE=y) matches the software mix within
     0.1 dB in every band, same level and stereo. In heavy play the sound
     thread's cost went from about 1.6 to 1.2 ms a 5.3 ms pass. Not yet the
     default: needs a listen on a TV.
   - Third round (the user: every hot function native; no console runs
     until the writing is done, so none of this is measured on the console
     yet):
     - Outline corners built in single precision with SSE, only the corners
       each stream draws (`gw_outline_vertices` in src/shapes_native.cpp;
       the vertices only feed the GPU). In the pile-up full speed now holds
       to update 7,500 (was 7,000), 54 updates/s at 8,000 (was 43). (This
       one was measured before the no-testing rule.)
     - The particle store in host order in the game's own buckets
       (src/particle_native.cpp): the add 0x82063100 and bucket clear
       0x82063018 native, the job and update without byte swaps; the
       checker's whole-job and add/clear cases identical.
     - The random number generator native (src/random_native.cpp: MT19937
       0x82030058, 0x82030220, 0x82030430, 0x8203B8B0), its own check
       (`toolsandom_check.cmd`, 400,000 calls identical).
     - Sound: the combining writes each contribution in place (no copy);
       sound check identical.
     - 15 more game functions native (src/enemy_native.cpp): line debris
       0x82074B28 (3.9%), atan2 0x8212FE70, entity draw dispatch 0x82045740
       (fused with its draws), rainbow colour 0x82030678, scene draw
       0x82065E38, two-colour and second-style lines 0x8205F6B8 0x8205EF18,
       explosion burst 0x82030C58, camera 0x82067A20, blinking enemy
       0x8204D8E0, model draws 0x82056FA0 0x82046D88 0x82047DF8, world
       matrix 0x82045CF0. The world update 0x820366A0 and score display
       0x8207CA48 were transcribed (registers in locals) but run no faster
       and 13-19% of their paths are untested: off unless make BIG_NATIVE=y.
     - Measured after all of it (one console pass at the end): the pile-up
       holds full speed to about update 7,500 (the original lost it by
       3,000); past that the GPU drawing hundreds of piled-up outlines is
       the limit. The playing benchmark: median 47.1 updates/s (was 37.4 at
       the start of this work, 45.4 last round), lowest 25.6, update 16,988
       after 6 minutes. Per drawn frame: particles 3.5 ms (was 6-11), grid
       2.6, stars 1.3. Sound is now the largest cost by far: about 1.6 ms of
       every 5.3 ms pass in heavy play (16 voices; decoding 330 us, effects
       180, control 190): about 30% of the processor. Next: the voices on
       the APU (the shelved path, git 52c1b75, make SOUND_APU=y, never run
       on a console) or the sound graph's data in host order.
     - Left as recompiled: the 360 Direct3D layer's dispatchers 0x8206A1C8,
       0x820A1370, 0x820A2B78 (little time of their own); the grid is native
       and near its floor (memory-bound).
     - The checker: each case in its own function (main's stack frame had
       grown enough to crash at start-up); `OUT` may be given to run two
       checks at once.
   - Playing benchmark (`tools/stress_play.sh`, 6 minutes, the ship moving
     and firing; repeats closely but not exactly): the previous best got
     to update 14,971 (median 37.4 updates/s); with this round's work
     16,483 (median 45.4, lowest 26.1), scores past 11 million. A second
     run hit the white shape at 200 s and fell to about 10 updates/s.
     In play the sound engine is now the largest single cost (about a
     fifth of the processor: 12-14 voices, decoding about 230 us of a 5.3 ms
     pass after the decoded-effects cache), then particles (6-11 ms a drawn
     frame).
2. **720p artifacts on a v1.1 console** (reported with a photo we have not
   seen): "unplayable in 720p, beautiful artifacts". On the test console
   (revision not known) 720p runs well, by the user's eye on a TV
   (2026-11-13), as do 480p, 480i at 4:3 (the game lays out its own 4:3
   picture; the port's letterbox code logs itself active but changes
   nothing: to be removed or explained) and 1080i (the console scales a
   720p or 480-line frame). The test console's
   revision is not known yet; v1.1 has the Conexant video encoder. Get the
   photo (or a description of the artifacts) and the console's revision;
   check the 720p mode and the CRTC line-length fix (`gw_crtc_pitch`) on a
   Conexant console.
