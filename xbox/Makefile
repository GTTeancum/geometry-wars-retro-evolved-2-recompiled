# Original Xbox build (nxdk). Run through xbox/tools/build.sh, which prepares
# the recompiled sources first. Output: xbox/build/release/default.xbe
XBE_TITLE = Geometry\ Wars\ Retro\ Evolved\ 2
OUTPUT_DIR = build/release
NXDK_DIR ?= /c/nxdk
NXDK_CXX = y

REXGLUE_INCLUDE = ../tools/rexglue/win-amd64/include

RUNTIME_SRCS = $(wildcard src/*.cpp)
GEN_SRCS = $(wildcard build/gen/*_recomp.*.cpp) $(wildcard build/gen/*_init.cpp)
SRCS = $(RUNTIME_SRCS) $(GEN_SRCS)

PROJECT_OBJS = $(addsuffix .obj,$(basename $(SRCS)))
GEN_OBJS = $(addsuffix .obj,$(basename $(GEN_SRCS)))

# SIMDe comes from the pinned ReXGlue SDK; -isystem keeps the SDK's own rex/
# headers behind the Xbox stand-ins in include/.
$(PROJECT_OBJS): NXDK_CXXFLAGS += -std=c++20 -O2 -Iinclude -Isrc -isystem $(REXGLUE_INCLUDE) \
	-ffunction-sections -fdata-sections -Wno-unused-label -Wno-unused-variable \
	-Wno-pass-failed -Wno-unused-but-set-variable -Wno-deprecated-volatile
# Size over speed for the generated code: memory is the tighter limit.
$(GEN_OBJS): NXDK_CXXFLAGS += -Ibuild/gen -Os

# One-frame GPU capture buffer (xbox/tools/gpu_capture.py): make CAPTURE=y
ifeq ($(CAPTURE),y)
src/gpu_capture.obj: NXDK_CXXFLAGS += -DGW_CAPTURE
endif

NXDK_LDFLAGS += /opt:ref /map:build/gw2.map

include $(NXDK_DIR)/Makefile

# After the include, so the XBE stays the default target.
# NV2A shaders (nxdk's Cg rules): pass-through vertex program, combiners.
src/nv2a.obj: src/nv2a_vs.inl src/nv2a_ps.inl
