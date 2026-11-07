// Xbox (nxdk) runtime for the recompiled Geometry Wars: Retro Evolved 2.
//
// The recompiled code addresses guest memory as base + guest_address, with
// 32-bit wraparound. GW_MEMBASE is chosen so that every guest window used
// below lands in free Xbox virtual memory:
//
//   guest 0x60000000-0x6EFFFFFF  virtual heap (NtAllocateVirtualMemory, XamAlloc)
//   guest 0x6F000000-0x6FFFFFFF  system heap (PCRs, TLS, kernel objects/vars)
//   guest 0x70000000-0x70FFFFFF  thread stacks
//   guest 0x82000000-0x8221FFFF  game image (game/analysis/default.image.bin)
//   guest 0xC0000000-0xDCFFFFFF  physical memory (MmAllocatePhysicalMemoryEx);
//                                GPU address = guest & 0x1FFFFFFF
//
// Physical memory lives at the 360's 0xC0000000 view because the game's
// Direct3D turns GPU addresses back into CPU ones with "0xC0000000 + physical"
// (the views at 0xA0000000/0xE0000000 cannot be mirrored here: one base maps
// every guest address).
//
// host = guest - 0x5E000000, so the windows sit at host 0x02000000 (above the
// XBE), 0x11000000, 0x12000000, 0x24000000 and 0x62000000-0x7EFFFFFF. The
// physical window gets most of the space: the game reserves ~250 MB of GPU
// buffers sized for the Xbox 360, of which it touches a few MB.
#pragma once

#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "gen/pch.h"

// nxdk compiles freestanding, so even 4-byte memcpy/memset become calls (a
// third of the frame went to them). The builtins inline small fixed sizes
// and still call the library for the rest.
#define memcpy(d, s, n) __builtin_memcpy((d), (s), (n))
#define memset(d, v, n) __builtin_memset((d), (v), (n))

#define GW_MEMBASE ((u8*)0xA2000000u)

constexpr u32 kGuestVirtualBase = 0x60000000;
constexpr u32 kGuestVirtualSize = 0x0F000000;
constexpr u32 kGuestSystemBase = 0x6F000000;
constexpr u32 kGuestSystemSize = 0x01000000;
constexpr u32 kGuestStackBase = 0x70000000;
constexpr u32 kGuestStackSize = 0x01000000;
constexpr u32 kGuestImageBase = 0x82000000;
constexpr u32 kGuestImageSize = 0x00220000;
constexpr u32 kGuestPhysicalBase = 0xC0000000;
constexpr u32 kGuestPhysicalSize = 0x1D000000;

constexpr u32 kEntryPoint = 0x82095FD8;
constexpr u32 kDefaultStackSize = 0x40000;

// --- guest memory -----------------------------------------------------------

// The sum wraps in 32 bits, so it is done on integers: as pointer arithmetic
// the overflow is undefined, and clang did fold such an address to 0 once it
// could prove the overflow (a table read in src/stars_native.cpp).
inline u8* gw_host(u32 guest) { return (u8*)(uintptr_t)(0xA2000000u + guest); }
inline u32 gw_guest(const void* host) { return (u32)((const u8*)host - GW_MEMBASE); }

inline u8 gw_load8(u32 a) { return *gw_host(a); }
inline u16 gw_load16(u32 a) { u16 v; memcpy(&v, gw_host(a), 2); return __builtin_bswap16(v); }
inline u32 gw_load32(u32 a) { u32 v; memcpy(&v, gw_host(a), 4); return __builtin_bswap32(v); }
inline u64 gw_load64(u32 a) { u64 v; memcpy(&v, gw_host(a), 8); return __builtin_bswap64(v); }
inline void gw_store8(u32 a, u8 v) { *gw_host(a) = v; }
inline void gw_store16(u32 a, u16 v) { v = __builtin_bswap16(v); memcpy(gw_host(a), &v, 2); }
inline void gw_store32(u32 a, u32 v) { v = __builtin_bswap32(v); memcpy(gw_host(a), &v, 4); }
inline void gw_store64(u32 a, u64 v) { v = __builtin_bswap64(v); memcpy(gw_host(a), &v, 8); }

bool gw_memory_init();
u32 gw_virtual_alloc(u32 size, u32 align);   // committed, zeroed; 0 on failure
u32 gw_virtual_reserve(u32 size, u32 align); // address range only
bool gw_virtual_commit(u32 guest, u32 size);   // large ranges are left to gw_handle_fault
bool gw_commit_range(u32 guest, u32 size);     // always commits now
bool gw_handle_fault(u32 host_address);        // commit-on-touch; false if not ours
// Records a file read into uncommitted guest memory; pages are read on touch.
bool gw_map_from_disc(u32 guest, u32 size, const char* host_path, u64 offset);
int gw_fault_filter(void* exception_pointers);  // SEH filter for guest threads
void gw_virtual_decommit(u32 guest, u32 size);
u32 gw_allocation_base(u32 guest);           // 0 if not allocated
void gw_virtual_free(u32 guest);
u32 gw_virtual_size(u32 guest);              // size of the allocation containing guest
u32 gw_physical_alloc(u32 size, u32 align);  // committed, zeroed; 0 on failure
// The host mirror of the grid's line vertex buffer for a guest address in it
// (src/kernel_memory.cpp), or null: each 4-vertex segment of the guest buffer
// is one NV2A line there (2 vertices of x, y, z, brightness, edge), which the
// GPU reads in place. The address is rounded down to its segment.
u8* gw_grid_line_host(u32 guest);
// Host mirrors of guest vertex buffers in NV2A order (src/vertex_mirror.cpp):
// add one for the first 'size' bytes of a guest buffer; find the host address
// of [guest, guest + bytes) when it lies wholly in a mirror, else null.
u8* gw_vertex_mirror_add(u32 guest, u32 size);
u8* gw_vertex_mirror(u32 guest, u32 bytes);
void gw_physical_free(u32 guest);
u32 gw_system_alloc(u32 size, u32 align = 16);
u32 gw_stack_alloc(u32 size);                // returns the low end
inline u32 gw_gpu_to_guest(u32 gpu) { return kGuestPhysicalBase + (gpu & 0x1FFFFFFF); }
inline u32 gw_guest_to_gpu(u32 guest) { return guest & 0x1FFFFFFF; }
u32 gw_memory_committed();                   // bytes committed by the guest
void gw_log_memory(const char* when);        // Xbox kernel memory breakdown
void gw_dump_allocations();                   // largest guest allocations
bool gw_census_due();                         // committed memory passed another 4 MB step

// --- logging / status -------------------------------------------------------

void gw_log(const char* fmt, ...);
[[noreturn]] void gw_fatal(const char* fmt, ...);
void gw_status_init();
void gw_status_frame();  // called on each guest swap
void gw_log_backtrace(PPCContext& ctx);

struct GwStats {
  volatile u32 swaps;  // the game's frames (updates)
  volatile u32 drawn;  // frames rendered and shown
  volatile u32 interrupts;
  volatile u32 files_opened;
  volatile u32 bytes_read;
  volatile u32 threads;
  volatile u32 unimplemented;
  volatile u32 gpu_packets;
  volatile u32 gpu_draws;
};
extern GwStats gw_stats;

// CPU time of the largest parts (timestamp-counter cycles), logged in ms
// per frame every 5 s by src/status.cpp. A part's time includes any time
// its thread was switched out inside it.
enum GwPart { kPartGrid, kPartParticles, kPartStars, kPartCommands, kPartCount };
extern volatile u64 gw_part_cycles[kPartCount];
extern volatile u32 gw_gpu_kicks;  // moves of the command ring's write pointer (src/gpu.cpp)
struct GwPartTimer {
  GwPart part;
  u64 start;
  explicit GwPartTimer(GwPart p) : part(p), start(__builtin_ia32_rdtsc()) {}
  ~GwPartTimer() { __atomic_fetch_add(&gw_part_cycles[part], __builtin_ia32_rdtsc() - start, __ATOMIC_RELAXED); }
};

// The Xbox kernel's current thread (KPCR Prcb.CurrentThread, fs:[0x28]), to
// match profiler samples to threads.
inline u32 gw_kthread() {
  u32 thread;
  __asm__ volatile("movl %%fs:0x28, %0" : "=r"(thread));
  return thread;
}

// Flush-to-zero for SSE on a thread that runs guest code (see FPSCRRegister
// in include/rex/ppc/context.h: the generated code no longer switches it).
inline void gw_guest_fp_setup() {
  unsigned csr;
  __asm__ volatile("stmxcsr %0" : "=m"(csr));
  csr |= 0x8000;  // FTZ
  __asm__ volatile("ldmxcsr %0" : : "m"(csr));
}

// Wait timing by call site (src/wait_stats.cpp).
u64 gw_wait_begin();
void gw_wait_end(u64 start, u32 caller, const char* what);
void gw_wait_report();
void gw_fault_report();  // commit-on-touch faults by allocation (src/guest_memory.cpp)

// Controllers (src/input.cpp).
void gw_input_init();
bool gw_input_read(u32 user, u32 gamepad);

// --- threads ----------------------------------------------------------------

struct GwThread;
GwThread* gw_current_thread();
PPCContext& gw_current_context();
// src/jobs_native.cpp: records the game's job threads.
void gw_note_thread(u32 start_address, u32 start_context);
// src/render_native.cpp: whether the current game frame will be rendered
// (it updates every frame, renders every second one).
bool gw_frame_drawn();
u32 gw_thread_create(u32 stack_size, u32 xapi_startup, u32 start_address, u32 start_context,
                     u32 creation_flags, u32* out_thread_id);  // returns a handle
void gw_run_main_thread();
// Runs a guest function on a dedicated guest context owned by the calling host
// thread (interrupt, timer and audio callbacks).
void gw_call_guest(PPCContext& ctx, u32 address, u32 r3, u32 r4);
PPCContext* gw_make_callback_context();
void gw_kernel_init();
u32 gw_handle_for_file(void* file);   // also used for handle-only objects (file = nullptr)
void* gw_file_from_handle(u32 handle);
void gw_file_close(void* file);
void gw_signal_guest_event_handle(u32 handle);
void gw_write_video_mode(u32 mode);

// --- GPU --------------------------------------------------------------------

void gw_gpu_init();
extern volatile bool gw_nv2a_active;  // pbkit owns the screen once set
extern volatile u32 gw_nv2a_stage;
void gw_gpu_write_register(u32 addr, u32 value);
u32 gw_gpu_read_register(u32 addr);

// --- dispatch ---------------------------------------------------------------

#include "dispatch.h"
PPCFunc* gw_lookup(u32 guest_address);

// --- export helpers ---------------------------------------------------------

// Each export records its name on the calling guest thread, so a stalled
// thread can be reported with the system call it is sitting in.
void gw_note_call(const char* name, PPCContext& ctx);
#define GW_EXPORT(name)                                                                   \
  static void gw_body_##name(PPCContext& ctx, uint8_t* base);                             \
  extern "C" void __imp__##name(PPCContext& ctx, uint8_t* base) {                         \
    gw_note_call(#name, ctx);                                                             \
    gw_body_##name(ctx, base);                                                            \
  }                                                                                       \
  static void gw_body_##name([[maybe_unused]] PPCContext& ctx, [[maybe_unused]] uint8_t* base)
void gw_dump_threads();
#define GW_ARG(n) ((u32)ctx.r##n.u32)
// Arguments past r10 (0-based index 8 and up) are on the guest stack.
#define GW_STACK_ARG(i) gw_load32(ctx.r1.u32 + 0x54 + ((i) - 8) * 8)
#define GW_RETURN(v) do { ctx.r3.u64 = (u64)(u32)(v); return; } while (0)

// NTSTATUS values
constexpr u32 X_STATUS_SUCCESS = 0x00000000;
constexpr u32 X_STATUS_ABANDONED_WAIT_0 = 0x00000080;
constexpr u32 X_STATUS_USER_APC = 0x000000C0;
constexpr u32 X_STATUS_ALERTED = 0x00000101;
constexpr u32 X_STATUS_TIMEOUT = 0x00000102;
constexpr u32 X_STATUS_PENDING = 0x00000103;
constexpr u32 X_STATUS_NO_MORE_FILES = 0x80000006;
constexpr u32 X_STATUS_UNSUCCESSFUL = 0xC0000001;
constexpr u32 X_STATUS_NOT_IMPLEMENTED = 0xC0000002;
constexpr u32 X_STATUS_INVALID_HANDLE = 0xC0000008;
constexpr u32 X_STATUS_INVALID_PARAMETER = 0xC000000D;
constexpr u32 X_STATUS_NO_SUCH_FILE = 0xC000000F;
constexpr u32 X_STATUS_END_OF_FILE = 0xC0000011;
constexpr u32 X_STATUS_NO_MEMORY = 0xC0000017;
constexpr u32 X_STATUS_ACCESS_DENIED = 0xC0000022;
constexpr u32 X_STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034;
constexpr u32 X_STATUS_OBJECT_NAME_COLLISION = 0xC0000035;
constexpr u32 X_STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
constexpr u32 X_STATUS_MUTANT_NOT_OWNED = 0xC0000046;
constexpr u32 X_STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047;

// Win32 error codes returned by XAM
constexpr u32 X_ERROR_SUCCESS = 0;
constexpr u32 X_ERROR_FILE_NOT_FOUND = 2;
constexpr u32 X_ERROR_ACCESS_DENIED = 5;
constexpr u32 X_ERROR_INVALID_PARAMETER = 87;
constexpr u32 X_ERROR_INSUFFICIENT_BUFFER = 122;
constexpr u32 X_ERROR_IO_PENDING = 997;
constexpr u32 X_ERROR_DEVICE_NOT_CONNECTED = 1167;
constexpr u32 X_ERROR_NOT_FOUND = 1168;
constexpr u32 X_ERROR_NO_MORE_FILES = 18;
constexpr u32 X_ERROR_FUNCTION_FAILED = 0x80004005;
constexpr u32 X_ERROR_NO_SUCH_USER = 1008;
