# Xbox port: to do

Status and history live in `README.md`; this is what is left, roughly in
order. Measurements are from the console (a debug kit at 64 MB) unless noted.

## 1. Fit in 64 MB

About 4.9 MB is free in a four-player level (console). In XEMU (2026-11-09,
12 minutes of four players): 5.7 MB free at the lowest; with `SOUND_APU=y`
3.7 MB (its copies of the effects, the stream rings and tables).

- [ ] Main-picture particle buffers: two sets of six jobs' buffers for the
      frames in flight (`src/particle_buffers.cpp`); one set may do now
      that the jobs run inline.
- [ ] Vertex buffer pool: the game's 256 buffers of 128 KB share 32 real
      ones (4 MB, `src/crt_hooks.cpp`); try 16.

## 2. Sound

No XMA on the Xbox: the installer converts every sound to Xbox ADPCM (the
APU's own format), and a native engine plays the game's sound graphs
(`src/sound_native.cpp`).

- [ ] **Sound on the APU (2026-11-09) — works in XEMU, the console not
      tried.** `make SOUND_APU=y`; the default build mixes in software.
      - Banks: Xbox ADPCM, 'GWX1' (36-byte blocks of 64 samples, in groups
        of 1008 bytes), from the IMA ADPCM by `tools\xadpcm_banks.cmd`
        (`src/xadpcm.cpp`) into `build\audio_xadpcm`, which
        `tools/stage_disc.py` stages; the installer does the same
        (`installer\SoundConvert.cs`, byte for byte the same banks). 46-59 dB
        below the sound (`tools\xadpcm_check.cmd`); the software engine
        plays them (57.7 dB from the old banks over 70 s of menu music). The
        console still has IMA banks in xe:\GW2\Audio: copy the new ones.
      - Driver (`src/apu.cpp`, ours, written from what nxdk-ex and xemu
        document; neither library states a license): voice records and the
        list head written from the CPU (voices started in a pass listed
        together, so they begin in one APU frame), a GP program copying bins
        0-15 to a ring (two 8-channel descriptors: in XEMU one 16-channel
        transfer came out garbled, and its range check uses the raw count
        word, so bins 16+ cannot be reached). Voices 0-63 are silent in XEMU
        (its HRTF stage): 64-127 are used. A console's VP stops every frame
        when a listed voice ends: one-shots loop over a silent tail.
      - Engine: every mixer with inserts and every graph output's mixer is a
        bin mixer; a sound whose ways out are plain mixing up to bin mixers
        (267 of 269) plays on a hardware voice (stereo as one voice) mixing
        into their bins, read back each pass and added before the mixers'
        inserts; the audio thread waits on the APU's output. In-memory
        effects from a copy on the APU; streamed sounds through a ring of
        256 blocks per voice filled ahead by copying (looping waves that are
        not a whole number of blocks long stay in software: Song 1's, and
        one each in Songs 3, 5, 6). Pause takes the songs' voices off the list.
        The bins' level is measured at start-up (XEMU: half).
      - Seen in XEMU: menu music on the APU as in the software engine (the
        layers within a sample of each other, 0.23 unexplained by the layers,
        as the PC check); a game: 718 hardware voices started, none refused,
        12 bins; pause silent and resumed. Twelve minutes of four players:
        38,000 voices started, none refused. (Before, XEMU stopped after a few
        minutes: a voice taken off the list was cleared at once, and the VP,
        which reads a voice's link before mixing it, could still reach it;
        now a voice off the list is left alone for two APU frames.)
      - Memory: 2 MB less free than the software engine in a four-player
        level. To win back: play the effects from the banks' own memory
        (contiguous) instead of a copy, a smaller GP ring, smaller rings.
      - CPU (XEMU, four players, us per 5.3 ms frame, the wait for the APU
        left out): software 700-850, APU 650-770 in the busiest stretches
        and 300-450 in lighter ones. The voices' own mixing goes (280 -> 30);
        the inserts, control and combining stay (the same in both), and the
        APU's side costs about 110 (reading its output 45, positions and
        rings 30, routes 33). Bins all silent count as nothing reaching the
        mixer (before, every bin mixer ran its inserts and the reverb ran
        all the time). To trim: write a voice's routes only when they
        change (each costs 8 log10f), skip reading silent bins' owners.
      - Next, on the console: hear it; measure the CPU time saved; the
        console's ADPCM rounding, its mix level and whether stereo voices
        put the right channel on the odd outputs. Then make it the default.
- [ ] Mixing cost: about 0.65 ms per 5.3 ms frame in a level (0.16 in the
      menu), mostly data fetched from memory (the game's threads push the
      engine's data out of the cache between frames). Next: a smaller
      working set (hot fields of components and voices together; combining
      only what changed).
- [ ] TransitionSwitch (Song 3's output the game waits on): heard only in
      the PC check so far.
