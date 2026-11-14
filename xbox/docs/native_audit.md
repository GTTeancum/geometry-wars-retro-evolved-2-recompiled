# What still runs as recompiled code (audit, 2026-11-13)

Read-only audit: no builds, no console runs. Every share below comes from
profiles already on disk (`xbox/test-artifacts/profile-*.txt`, taken with
`xbox/tools/xbdm_profile.py`) matched against the current source.

## Summary

- The game's hot path is now almost entirely native. In the most recent
  main-thread profile of real play, the functions that are still recompiled
  add up to roughly **8-12% of the main thread** (about 6.5% visible in the
  top 60, the rest spread over a long tail of functions under 0.25% each).
  No single recompiled function is above about 1.3%.
- Rewriting every remaining one would recover, at a typical 2-4x speed-up,
  something like **4-8% of the main thread**: perhaps 1-2 ticks a second at
  the worst moments. Worth doing in a few cheap places, not a big win.
- The large costs left are **already native**: the sound engine (about 30%
  of the single CPU, on its own thread), the grid (about 12% of the main
  thread, memory-bound), object outlines (about 14%), particles (about 8%),
  stars (about 5%), and host `memset`/`memcpy` (3.5% and 5-6%). The
  remaining speed has to come from those (doing less work, or moving the
  sound voices to the APU), and from the GPU, which is the limit in the
  heaviest pile-ups.

## Data used, and its limits

| Profile | When | Threads | Notes |
|---|---|---|---|
| `profile-playprof.txt` | 2026-11-13 08:38 | main (92) only | playing benchmark; **the baseline for the third native round** (its numbers are quoted in `TODO.md`, e.g. line debris 3.9%) |
| `profile-pileup-now.txt` | 2026-11-13 06:43 | all | pile-up; has an inclusive (with callees) table |
| `profile-6min-main.txt` | 2026-11-12 21:06 | main only | crowd at 6 min; inclusive table; before rounds 2-3 |
| `profile-play.txt`, `profile-evolved.txt` | 2026-11-12 | all | older |

- **No function-level profile exists after the third round** (commit
  3c6c56e). The third round was measured only by the speed benchmark. The
  shares below are therefore the pre-round-3 shares of functions that are
  *still* recompiled today; since round 3 removed a good part of the main
  thread's work, their share of today's (smaller) total is somewhat higher,
  roughly x1.1-1.2.
- The profiler names a function by symbol, and a native override takes the
  same `sub_XXXXXXXX` name as the recompiled one. Every `_sub_` entry was
  therefore checked against the list of native overrides below; only those
  not overridden are counted as recompiled.
- Inclusive figures include stale return addresses on the stack and time a
  thread was preempted, so they overstate; they are used only to find
  subtrees, not as costs.
- **One console measurement would settle the open points**: a 60-second
  `xbdm_profile.py <console> 60 --top 80` during `stress_play.sh` on the
  current build, once with `--thread 92` (main) and once without (to see
  which thread runs the 0x820B-0x8216 library functions, below). Not done
  here: the console is in use by another session.

## Native overrides in the build (105)

Strong definitions of `sub_XXXXXXXX` in `xbox/src/*.cpp` (the recompiled
versions in `xbox/build/gen/*_recomp.*.cpp` are weak; overrides may call
`__imp__sub_X` for the original):

- `crt_hooks.cpp`: memcpy 0x8212EDC0, XMemCpy 0x82095188, memset 0x82130E10,
  vertex-buffer pool 0x8206A518
- `memory_native.cpp`: large-block copy 0x82096F30
- `math_native.cpp`: 0x821308B8, 0x8212EA80, 0x8212E9B0 (sine/cosine),
  0x82030338, 0x82041600, 0x82041CA0 (vector/matrix products)
- `random_native.cpp`: MT19937 0x82030058, 0x82030220, 0x82030430, 0x8203B8B0
- `entity_native.cpp`: separation 0x82046248
- `enemy_native.cpp` (39 on by default): 0x82052860, 0x82046BD8, 0x8205A2B0,
  0x820631E8 (fused with 0x82059FC0, 0x8205A100), 0x8205C5D8, 0x8205C6D8,
  0x820453E0, 0x82047180, 0x8204F458, 0x82040F28, 0x82056CD8, 0x82059838,
  0x82049E60, 0x82056B48, 0x82045AB8, 0x82059CD0, 0x82060CC0, 0x8205FA98,
  0x82045B18, 0x82044330, 0x82054E30, 0x82044168, 0x82074B28, 0x82045628,
  0x82045740, 0x82030678, 0x8212FE70, 0x82065E38, 0x8205F6B8, 0x8204D8E0,
  0x82056FA0, 0x82046D88, 0x82047DF8, 0x82045CF0, 0x82030C58, 0x82067A20,
  0x8205EF18
- `grid_native.cpp`: 0x8205D118, 0x8205B000, 0x8205B880
- `jobs_native.cpp`: 0x82063F08
- `particle_native.cpp`, `particle_buffers.cpp`: 0x82063448, 0x82063100,
  0x82063018, 0x82063A38, 0x82064110, 0x820673C8
- `shapes_native.cpp`: 0x8205FBA8, 0x8205EB58, 0x82061DA0
- `stars_native.cpp`: 0x82075548
- `render_native.cpp`: 0x820653C8, 0x8205D538, 0x82064930, 0x82064F88,
  0x82064C90, 0x8205E8A8
- `d3d_native.cpp`, `gpu.cpp`: 0x820A91A0, 0x820A6438, 0x820A6518,
  0x8209D288, 0x8209D8A0, 0x820A5320, 0x820A08B8, 0x820A0CA0, 0x820A08A8
- `sound_game.cpp`: the game's sound calls (0x8207A048, 0x820787F8,
  0x820783A8, 0x82078520, 0x820782E0, 0x82079BC0; 0x82078018, 0x82078360,
  0x820786E0 emptied). The sound engine itself (`sound_native.cpp`) is a
  native interpreter; none of the game's audio code runs.
- `live_scores.cpp` (leaderboards, 9), `menu_native.cpp` (0x8208EFC0):
  not hot.

**Off by default:**

- `BIG_NATIVE` (`make BIG_NATIVE=y`, `xbox/Makefile`): the world update
  0x820366A0 and the score display 0x8207CA48 in `enemy_native.cpp`. Both
  are the generated code copied out with the PowerPC registers held in
  local variables. They were left off because they measured **no faster on
  the PC** and **13-19% of their code paths are not covered by the checker**
  (`TODO.md`, third round). That is the expected result: the time in these
  two is spread over 1,200-1,400 instructions of glue and float work, and
  moving registers into locals does not change what the code does. A real
  gain needs a rewrite by hand, not a transcription; see items 1 and 2.
- `TEST_CHEATS`: 0x8204EEA8, 0x8207F5B8, 0x82030010, 0x820E8A20 (test only).

## Ranked: hot functions still recompiled

Share = self time in `profile-playprof.txt` (main thread, playing) unless
noted; "pile-up" = `profile-pileup-now.txt`, all threads. Size = PowerPC
instructions in the generated function. Difficulty: easy (under ~150
instructions, no virtual calls, little state), medium, hard (large, many
callees or branches, or GPU packet writing that must stay byte-identical).

| # | Address | What it does | Share | Size | Difficulty | Expected gain |
|---|---|---|---|---|---|---|
| 1 | 0x820366A0 | World update, once a tick (calls 33 functions: spawning, players, entities, grid flips 0x8205C8F8) | 1.3% (pile-up 0.5%, 6 min 0.7%) | 1,367 (498 float) | hard; transcription exists (BIG_NATIVE) but no faster | 0.5-0.8% by hand, mostly by inlining its small float callees |
| 2 | 0x8207CA48 | Score display / HUD (scores, multiplier, times), via 0x8208F430 | 0.9% | 1,209 (371 float) | hard; transcription exists (BIG_NATIVE) | 0.4-0.6% |
| 3 | 0x8206A1C8 + 0x82069FB8 | D3D-side matrix helpers: the projection and a 4x4 vector routine (AltiVec), called from the camera (native 0x82067A20) and 0x82067E40, 0x82068408 | 0.7% + 0.4% | 77 + 132 (111 vector ops) | **easy**: pure math, vector ops are the most expensive kind to emulate | ~0.8%; best value per hour on this list |
| 4 | 0x820A1370 | 360 Direct3D state dispatcher (writes command packets via 0x820A5320, native) | 0.6% (6 min 0.4%) | 141 | medium (byte-identical packets) | ~0.4% |
| 5 | 0x820A2B78 | Direct3D end-of-frame / resolve step, from the swap 0x8205E8A8 and 0x8205E1F0 | 0.4% | 76 (vector) | medium | ~0.3% |
| 6 | 0x820A9F38 | Direct3D draw setup: shaders, packet words (called from native 0x820A08B8, 0x820A0CA0) | 0.3% (6 min 0.4%, play 0.4%) | 517 | hard | ~0.2% |
| 7 | 0x820A3788 | Direct3D state setup (via 0x820A3E18) | 0.3% | 420 | hard | ~0.2% |
| 8 | 0x8205A3E0 | Counting sort of the grid pushers by cell, called by native 0x820631E8 | 0.3% | 161 | **easy**, sits next to native code that already knows its data | ~0.2-0.3% |
| 9 | 0x820473D0, 0x82057B60, 0x82052418, 0x820599B0, 0x82049FA0, 0x82054D08, 0x820587C0 | The enemy kinds not yet rewritten (virtual updates; they call native random, separation, sine/cosine, atan2, look-at) | 0.2-0.3% each, ~1.5% together | 74-274 each | easy to medium each; the same pattern as the 15 enemy updates already done | ~1% together |
| 10 | 0x8204FF00, 0x82051B18 | Player::Update (ship movement, firing, sounds) and a Player method | 0.3% + 0.3% | 896 + 167 | medium (input, sound calls, many branches) | ~0.3% |
| 11 | 0x820669F0 | Scene draw driver (stars, grid lines, ships; calls 18 functions) | 0.3% | 517 | medium; mostly calls | ~0.1% |
| 12 | 0x82090690 (via 0x8208F718) | HUD element layout (float work) | 0.2% (pile-up 0.2%, Evolved 0.7%) | 257 | medium | ~0.2% |
| 13 | 0x82068CF8 | Per-frame update after the world update (controller / camera-shake style float work; calls 0x82037EA0, 0x82044798, 0x820688E8) | small self; large inclusive (21-23%) is mostly waits | 500 (425 float) | medium | unknown; needs the console profile |
| 14 | 0x8203FFB8, 0x8203ECC0, 0x8203BEE8 | The wave spawner and "spawn one enemy" (constructors of every enemy kind, heap allocation 0x820E8130) | small self; 0x8203ECC0 12% inclusive in the pile-up | 531, 797, 609 | hard (many object kinds, allocation) | unknown; only matters if the inclusive figure is real |
| 15 | 0x820B2558, 0x8215E578, 0x82100FD0, 0x82154CE8, 0x8213A7D8 | Library code (0x820B..0x8216 range, vector-heavy) seen only in all-thread profiles, so most likely **on another thread** (thread 4 runs 5-10% of rounds) | 0.5-1.8% each (Evolved: 0x820B2558 1.8%, 0x8213A7D8 1.5%) | 16-811 | unknown until identified | up to 2-3% of the CPU if they turn out to be steady per-frame work |

Not counted as recompiled (they were in older profiles but are native now
or fused): 0x8205A100 and 0x82059FC0 (fused into native 0x820631E8),
0x82074B28, 0x8212FE70, 0x82045740 and the other third-round functions.

## The large costs that are already native

From the same main-thread profile (round-3 changes noted):

- Grid 0x8205B880 11.6% + row update 3.8%: native, memory-bound, near its
  floor (`TODO.md`).
- Object outlines: `outline_build` 9.3% + `gw_outline_vertices` 4.5% +
  0x82061DA0 3.5%: native; round 3 sped up the corner building.
- Particles `particle_update4` 8.0%: native, four at a time; round 3 removed
  the byte swaps.
- Stars 0x82075548 4.7%, homing enemies 0x82056CD8 3.0%: native.
- Host `memset` 3.5% (main thread) and `memcpy` 5-6% (all threads,
  20% inclusive in the pile-up): native library calls. Worth one look at who
  calls `memset` that often on the main thread (likely clearing particle
  buckets or a vertex area every tick); a clear that can be skipped is free
  time.
- Sound: about 1.6 ms of every 5.3 ms pass in heavy play, about 30% of the
  CPU (`TODO.md`), all native (graph `compute` 13.5%, `gw_sound_mix` 6.9%,
  `source_ready` 5.5%, `collect` 4.9% of all samples in the pile-up). The
  original Xbox has one CPU core, so this comes straight out of the game's
  time. The options already identified: voices on the APU (shelved; the APU
  can come up dead after a warm restart, `TODO.md`), or the sound graph's
  data in host order.
- GPU: in the heaviest pile-ups the GPU is the limit, not the CPU.

## Recommendation

1. **Biggest win: the sound engine's cost** (about 30% of the CPU), not
   recompiled code. Either finish the APU path or cut the graph's per-pass
   work (host-order data, fewer passes over silent layers).
2. **Cheap recompiled wins, about 2-3% together**: the D3D matrix helpers
   0x8206A1C8/0x82069FB8 (vector-heavy, small), the pusher sort 0x8205A3E0,
   and the seven remaining enemy updates (item 9).
3. **Skip** a transcription-style BIG_NATIVE; the world update and score
   display are worth about 1-1.5% together even rewritten by hand.
4. **Measure first** (one console profile, above) for items 13-15: the
   spawner subtree and the library functions on the second thread are the
   only places where a sizeable recompiled cost could still be hiding.
