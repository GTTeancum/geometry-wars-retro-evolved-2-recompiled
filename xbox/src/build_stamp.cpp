// The build's stamp: 'GWBS' and the build time (the Makefile's
// GW_BUILD_STAMP, new on every build). The console tools that take addresses
// from the link map (tools/xbdm_shot.py check_build: the pad tool, the
// screenshot tool) first read it from the running game and compare it with
// the same symbol in the built main.exe; a map from another build names
// other addresses, and writing through them (the pad tool) hit whatever the
// running build keeps there.
#include "runtime.h"

#ifndef GW_BUILD_STAMP
#define GW_BUILD_STAMP 0
#endif

extern "C" volatile const u32 gw_build_stamp[2] = {0x53425747u, GW_BUILD_STAMP};
