# Xbox port: to do

Status and history live in `README.md`; this is what is left, roughly in
order. Measurements are from the console (a debug kit at 64 MB) unless noted.

## 1. Fit in 64 MB

Done for the public test. Four players on the console (2026-11-09, 12
minutes, the particle cap in): 4.7 MB free, flat through the run. In XEMU
5.7 MB (XEMU shows about 1 MB more free than the console); with
`SOUND_APU=y` 2 MB less (shelved, see Sound).

Particles: with none fading out (`make PARTICLE_STRESS=y`, a test build)
four players in XEMU ran out of memory at about 25,400 live particles, and
the game froze; the busiest real play seen had about 5,200. They are now
capped at 16,000 (`kMaxLive` in `src/particle_native.cpp`; past it a
particle is dropped as a dead one is). The stress build at the cap: 6
minutes, 4.1 MB free at the lowest in XEMU, no fault (on the console about
3 MB, by the difference above; not run there).

Spare memory, if it is ever needed:

- Main-picture particle buffers: two sets of six jobs' buffers for the
  frames in flight (`src/particle_buffers.cpp`); one set may do now that
  the jobs run inline.
- Vertex buffer pool: the game's 256 buffers of 128 KB share 32 real ones
  (4 MB, `src/crt_hooks.cpp`); 16 would give back 2 MB.

## 2. Sound

Done for the public test. No XMA on the Xbox: the installer converts every
sound to Xbox ADPCM, and a native engine plays the game's sound graphs in
software (`src/sound_native.cpp`).

Listen for in the public test:

- Song 3's TransitionSwitch (an output the game reads to move the music on)
  has only been heard in the PC check.

Shelved (2026-11-09): sound on the APU, `make SOUND_APU=y`. Works in XEMU
(effects and music on the chip, 12 minutes of four players with 38,000
voices and no fault); never tried on the console. Not worth it as it
stands: 2 MB less free memory for about 10-15% of the sound's CPU time in
busy play (the inserts, control and combining stay on the CPU). If it is
taken up again: play the effects from the banks' own memory instead of a
copy (about 1 MB back), write a voice's routes only when they change, and
on the console check its ADPCM rounding, mix level and stereo outputs. The
driver is `src/apu.cpp` (ours, from what nxdk-ex and xemu document); the
engine's side is under `GW_SOUND_APU` in `src/sound_native.cpp`.
