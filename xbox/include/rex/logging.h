// Xbox stand-in for rex/logging.h. Generated code only logs from ppc_trap.
#pragma once

void gw_log(const char* fmt, ...);

#define REXCPU_DEBUG(fmt, ...) ((void)0)
#define REXCPU_WARN(fmt, ...) gw_log("[cpu] " fmt "\n")
