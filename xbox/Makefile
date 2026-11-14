# Original Xbox build (nxdk). Run through xbox/tools/build.sh, which prepares
# the recompiled sources first. Output: xbox/build/release/default.xbe
XBE_TITLE = Geometry\ Wars\ Retro\ Evolved\ 2
OUTPUT_DIR = build/release
NXDK_DIR ?= /c/nxdk
NXDK_CXX = y
NXDK_SDL = y

REXGLUE_INCLUDE = ../tools/rexglue/win-amd64/include

RUNTIME_SRCS = $(wildcard src/*.cpp)
GEN_SRCS = $(wildcard build/gen/*_recomp.*.cpp) $(wildcard build/gen/*_init.cpp)
SRCS = $(RUNTIME_SRCS) $(GEN_SRCS) third_party/pbkit/pbkit.c

# pbkit's core with the frame and depth pitches set to the tile pitches (480p:
# third_party/pbkit/pbkit.c); its own headers stay nxdk's.
third_party/pbkit/pbkit.obj: NXDK_CFLAGS += -I$(NXDK_DIR)/lib/pbkit

PROJECT_OBJS = $(addsuffix .obj,$(basename $(SRCS)))
GEN_OBJS = $(addsuffix .obj,$(basename $(GEN_SRCS)))
RUNTIME_OBJS = $(addsuffix .obj,$(basename $(RUNTIME_SRCS)))

# SIMDe comes from the pinned ReXGlue SDK; -isystem keeps the SDK's own rex/
# headers behind the Xbox stand-ins in include/.
$(PROJECT_OBJS): NXDK_CXXFLAGS += -std=c++20 -O2 -Iinclude -Isrc -isystem $(REXGLUE_INCLUDE) \
	-ffunction-sections -fdata-sections -Wno-unused-label -Wno-unused-variable \
	-Wno-pass-failed -Wno-unused-but-set-variable -Wno-deprecated-volatile
# With registers as C++ locals (xbox/codegen.toml) -O2 code is no bigger than -Os.
$(GEN_OBJS): NXDK_CXXFLAGS += -Ibuild/gen -O2

# nxdk passes -fno-builtin, which turns every small memcpy (gw_load32, the
# shader interpreter) into a call; the runtime gets the builtins back.
$(RUNTIME_OBJS): NXDK_CXXFLAGS += -fbuiltin -mfpmath=sse
# Square roots as the SSE instruction: with errno kept, __builtin_sqrtf was a
# call to the library's x87 routine (two per particle). Nothing reads errno;
# the results are the same (a correctly rounded square root either way).
$(RUNTIME_OBJS): NXDK_CXXFLAGS += -fno-math-errno

# No SLP vectorising where big-endian guest values feed the arithmetic: the
# Pentium III has no SSE2, so each byte-swapped value goes through the stack,
# and clang gathered four 32-bit stores into one 16-byte load, which cannot
# be forwarded from the stores and waits for them to reach the cache (the
# particle update's hottest instructions on the console).
src/particle_native.obj: NXDK_CXXFLAGS += -fno-slp-vectorize

# A new stamp on every build (src/build_stamp.cpp): the console tools check
# that the running game is the build the link map describes.
BUILD_STAMP := $(shell date +%s)
src/build_stamp.obj: NXDK_CXXFLAGS += -DGW_BUILD_STAMP=$(BUILD_STAMP)u
# (its rebuild-every-time rule is after nxdk's include: a rule here would
# become the default target)

# Our own glow over each frame (see src/nv2a.cpp); make GLOW=n leaves it out.
ifneq ($(GLOW),n)
src/nv2a.obj: NXDK_CXXFLAGS += -DGW_GLOW
endif

# A picture every DRAW_EVERY frames always, not fewer under load
# (src/render_native.cpp): make ADAPTIVE=n
ifeq ($(ADAPTIVE),n)
src/render_native.obj: NXDK_CXXFLAGS += -DGW_DRAW_FIXED
endif

# Lines every 8 pixels in from each edge, to see where the frame lands on a
# TV: make TEST_PATTERN=y
ifeq ($(TEST_PATTERN),y)
src/nv2a.obj: NXDK_CXXFLAGS += -DGW_TEST_PATTERN
endif

# Test cheats (src/test_cheats.cpp): invincible, every mode shown, the save
# unchanged: make TEST_CHEATS=y
ifeq ($(TEST_CHEATS),y)
src/test_cheats.obj: NXDK_CXXFLAGS += -DGW_TEST_CHEATS
endif

# The world update (0x820366A0) and score display (0x8207CA48) as the
# generated code with registers in locals (src/enemy_native.cpp): no faster
# on the PC and part of them untested, so off: make BIG_NATIVE=y
ifeq ($(BIG_NATIVE),y)
src/enemy_native.obj: NXDK_CXXFLAGS += -DGW_BIG_NATIVE
endif

# Draws left out by shader, chosen from the PC while the game runs
# (src/nv2a.cpp, tools/xbdm_poke.py): make DRAW_SKIP=y
ifeq ($(DRAW_SKIP),y)
src/nv2a.obj: NXDK_CXXFLAGS += -DGW_DRAW_SKIP
endif

# Outline segments far outside the arena logged with who drew them
# (src/shapes_native.cpp): make OUTLINE_CHECK=y
ifeq ($(OUTLINE_CHECK),y)
src/shapes_native.obj: NXDK_CXXFLAGS += -DGW_OUTLINE_CHECK
endif

# The game at 30 ticks a second, a picture each (src/render_native.cpp,
# src/jobs_native.cpp); make TICK30=n for 60 ticks with a picture every
# second one. Every source sees the tick rate (runtime.h): rebuild them all
# when changing it.
ifneq ($(TICK30),n)
NXDK_CXXFLAGS += -DGW_TICK30
endif

# Vertices with positions out of all reason counted per kind of draw
# (src/nv2a.cpp, in the census): make GEOMETRY_CHECK=y
ifeq ($(GEOMETRY_CHECK),y)
src/nv2a.obj: NXDK_CXXFLAGS += -DGW_GEOMETRY_CHECK
endif

# Which game code draws text, and what (src/text_trace.cpp): make TEXT_TRACE=y
ifeq ($(TEXT_TRACE),y)
src/text_trace.obj: NXDK_CXXFLAGS += -DGW_TEXT_TRACE
endif

# The frame counter over the top of the screen (it covers the game's score):
# make OVERLAY=y
ifeq ($(OVERLAY),y)
src/nv2a.obj: NXDK_CXXFLAGS += -DGW_OVERLAY
endif

# Output: 1280x720 when the console allows it (src/status.cpp); make
# VIDEO=480 for 640x480 only.
ifeq ($(VIDEO),480)
src/status.obj: NXDK_CXXFLAGS += -DGW_VIDEO_480
endif

# Render every Nth game frame (the game updates every frame): make DRAW_EVERY=1
# to render all of them.
ifneq ($(DRAW_EVERY),)
src/render_native.obj: NXDK_CXXFLAGS += -DGW_DRAW_EVERY=$(DRAW_EVERY)
endif

# Record small copies of the frames around the ship's spawn (src/nv2a.cpp,
# tools/xbdm_frames.py): make SPAWN_CAPTURE=y
ifeq ($(SPAWN_CAPTURE),y)
src/nv2a.obj: NXDK_CXXFLAGS += -DGW_SPAWN_CAPTURE
endif

# Test only: particles never fade out, so the game's particle pool fills (the
# worst case for memory); free memory logged every 15 s: make PARTICLE_STRESS=y
ifeq ($(PARTICLE_STRESS),y)
src/particle_native.obj src/status.obj src/guest_memory.obj: NXDK_CXXFLAGS += -DGW_PARTICLE_STRESS
endif

# Record the last 8 s of sound out and log the game's sound actions and
# parameters (tools/xbdm_audio.py): make AUDIO_CAPTURE=y
ifeq ($(AUDIO_CAPTURE),y)
src/audio.obj src/sound_native.obj: NXDK_CXXFLAGS += -DGW_AUDIO_CAPTURE
endif

# The game's sounds on the APU where they can play there (src/sound_native.cpp,
# src/apu.cpp): make SOUND_APU=y
ifeq ($(SOUND_APU),y)
src/sound_native.obj: NXDK_CXXFLAGS += -DGW_SOUND_APU
endif

# The APU test (src/audio.cpp): one effect on a hardware voice, the GP's
# output recorded: make APU_TEST=y AUDIO_CAPTURE=y
ifeq ($(APU_TEST),y)
src/audio.obj: NXDK_CXXFLAGS += -DGW_APU_TEST
endif
ifeq ($(APU_TEST),pcm)
src/audio.obj: NXDK_CXXFLAGS += -DGW_APU_TEST -DGW_APU_TEST_PCM
endif

# Compare the native grid update with the original now and then: make GRID_CHECK=y
ifeq ($(GRID_CHECK),y)
src/grid_native.obj: NXDK_CXXFLAGS += -DGW_GRID_CHECK -DGW_GRID_GUEST_SYNC
endif

# One-frame GPU capture buffer (xbox/tools/gpu_capture.py): make CAPTURE=y
ifeq ($(CAPTURE),y)
src/gpu_capture.obj: NXDK_CXXFLAGS += -DGW_CAPTURE -DGW_CAPTURE_SWAP=$(or $(CAPTURE_SWAP),120)
endif

NXDK_LDFLAGS += /opt:ref /map:build/gw2.map

include $(NXDK_DIR)/Makefile

# After the include, so the XBE stays the default target.
# The build stamp (src/build_stamp.cpp) compiled on every build.
src/build_stamp.obj: FORCE
.PHONY: FORCE
FORCE:

# NV2A shaders (nxdk's Cg rules): pass-through vertex program, combiners.
src/nv2a.obj: src/nv2a_vs.inl src/nv2a_ps.inl src/nv2a_sum_ps.inl src/nv2a_glow_vs.inl
