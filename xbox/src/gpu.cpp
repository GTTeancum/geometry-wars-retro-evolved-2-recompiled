// Xenos GPU stand-in.
//
// The game's statically linked Direct3D writes PM4 command packets into a ring
// buffer and moves the write pointer through the CP_RB_WPTR register. This
// command processor walks those packets on its own thread, keeps the register
// file, and performs every packet that writes guest memory or raises an
// interrupt (fences, scratch writebacks, swaps), so the game sees its frames
// complete. Draw packets are counted but not rendered yet.
//
// A vblank thread raises the 60 Hz graphics interrupt.
#include "runtime.h"
#include "xenos_vs.h"

#include <windows.h>

void gw_capture_swap(u32 swap_number, u32 frontbuffer, u32 width, u32 height, const u32* regs);
void gw_capture_shader(u32 type, u32 gpu_address, u32 guest_words, u32 dwords);
void gw_capture_draw(u32 opcode, const u32* packet, u32 packet_dwords, const u32* regs);
void gw_note_shader(u32 type, u32 hash, u32 guest_words, u32 dwords);
void gw_nv2a_init();
void gw_nv2a_draw(u32 opcode, const u32* words, u32 n, const u32* regs, u32 vs_hash, const XvsProgram* vs,
                  u32 ps_hash);
void gw_nv2a_swap();

namespace {

constexpr u32 kRegisterCount = 0x6000;
u32 g_regs[kRegisterCount];

constexpr u32 kRegScratchUmsk = 0x01DC;
constexpr u32 kRegScratchAddr = 0x01DD;
constexpr u32 kRegScratchReg0 = 0x0578;
constexpr u32 kRegCoherStatusHost = 0x0A31;
constexpr u32 kRegVgtEventInitiator = 0x21F9;
constexpr u32 kRegCpRbWptr = 0x01C5;
constexpr u32 kRegShaderConstantFetch00 = 0x4800;

u32 g_ring_base;       // GPU address
u32 g_ring_size;       // bytes
u32 g_read_index;      // dwords
volatile u32 g_write_index;
u32 g_rptr_writeback;  // GPU address
u32 g_swap_counter;
HANDLE g_work_event;

u32 g_interrupt_callback;
u32 g_interrupt_data;
CRITICAL_SECTION g_interrupt_lock;

// FNV-1a of the current vertex shader's microcode: identifies the draw for
// the NV2A renderer (inline shaders move around the ring every frame).
u32 g_vs_hash;
u32 g_ps_hash;
const XvsProgram* g_vs_program;

u32 shader_hash(u32 guest, u32 dwords) {
  u32 h = 0x811C9DC5;
  for (u32 i = 0; i < dwords; ++i) h = (h ^ gw_load32(guest + i * 4)) * 0x01000193;
  return h;
}

u32 g_bin_mask_lo = 0xFFFFFFFF, g_bin_mask_hi = 0xFFFFFFFF;
u32 g_bin_select_lo = 0xFFFFFFFF, g_bin_select_hi = 0xFFFFFFFF;

constexpr u32 kSwapSignature = 0x53574150;  // 'SWAP'

u32 gpu_swap(u32 value, u32 endian) {
  switch (endian & 3) {
    case 1:  // 8 in 16
      return ((value << 8) & 0xFF00FF00) | ((value >> 8) & 0x00FF00FF);
    case 2:  // 8 in 32
      return __builtin_bswap32(value);
    case 3:  // 16 in 32
      return (value << 16) | (value >> 16);
    default:
      return value;
  }
}

// GPU-endian store: the value is converted, then written in host order.
void gpu_store(u32 gpu_address, u32 value) {
  value = gpu_swap(value, gpu_address & 3);
  memcpy(gw_host(gw_gpu_to_guest(gpu_address & ~3u)), &value, 4);
}

u32 gpu_load(u32 gpu_address) {
  u32 value;
  memcpy(&value, gw_host(gw_gpu_to_guest(gpu_address & ~3u)), 4);
  return gpu_swap(value, gpu_address & 3);
}

void dispatch_interrupt(PPCContext& ctx, u32 source) {
  if (!g_interrupt_callback) return;
  EnterCriticalSection(&g_interrupt_lock);
  ++gw_stats.interrupts;
  gw_call_guest(ctx, g_interrupt_callback, source, g_interrupt_data);
  LeaveCriticalSection(&g_interrupt_lock);
}

void write_register(u32 index, u32 value) {
  if (index >= kRegisterCount) return;
  g_regs[index] = value;
  if (index >= kRegScratchReg0 && index <= kRegScratchReg0 + 7) {
    u32 n = index - kRegScratchReg0;
    if (g_regs[kRegScratchUmsk] & (1u << n)) gw_store32(gw_gpu_to_guest(g_regs[kRegScratchAddr] + n * 4), value);
  } else if (index == kRegCoherStatusHost) {
    g_regs[index] = 0;  // caches are always coherent here
  }
}

bool compare(u32 function, u32 value, u32 ref) {
  switch (function & 7) {
    case 1: return value < ref;
    case 2: return value <= ref;
    case 3: return value == ref;
    case 4: return value != ref;
    case 5: return value >= ref;
    case 6: return value > ref;
    case 7: return true;
    default: return false;
  }
}

struct Reader {
  u32 base;   // guest address of the buffer
  u32 size;   // dwords (power of two for the ring, exact for indirect buffers)
  u32 pos;    // dwords
  bool ring;
  u32 next() {
    u32 v = gw_load32(base + (pos % size) * 4);
    ++pos;
    return v;
  }
  void skip(u32 n) { pos += n; }
};

void execute(Reader& r, u32 end, PPCContext& ctx);

void execute_type3(Reader& r, u32 packet, PPCContext& ctx) {
  u32 opcode = (packet >> 8) & 0x7F;
  u32 count = ((packet >> 16) & 0x3FFF) + 1;
  u32 data_end = r.pos + count;
  if (packet & 1) {  // predicated on the bin select
    bool pass = ((g_bin_select_lo & g_bin_mask_lo) | (g_bin_select_hi & g_bin_mask_hi)) != 0;
    if (!pass || opcode == 0x64) {
      r.pos = data_end;
      return;
    }
  }
  switch (opcode) {
    case 0x54: {  // INTERRUPT
      u32 cpu_mask = r.next();
      if (cpu_mask) dispatch_interrupt(ctx, 1);
      break;
    }
    case 0x64: {  // XE_SWAP (written by VdSwap)
      u32 magic = r.next();
      if (magic == kSwapSignature) {
        u32 frontbuffer = r.next(), width = r.next(), height = r.next();
        ++g_swap_counter;
        gw_capture_swap(g_swap_counter, frontbuffer, width, height, g_regs);
        gw_nv2a_swap();
        gw_status_frame();
      }
      break;
    }
    case 0x27: {  // IM_LOAD: shader microcode from memory
      u32 address_type = r.next(), start_size = r.next();
      u32 address = address_type & ~3u;
      gw_capture_shader(address_type & 3, address, gw_gpu_to_guest(address), start_size & 0xFFFF);
      u32 hash = shader_hash(gw_gpu_to_guest(address), start_size & 0xFFFF);
      gw_note_shader(address_type & 3, hash, gw_gpu_to_guest(address), start_size & 0xFFFF);
      if ((address_type & 3) == 0) {
        g_vs_hash = hash;
        g_vs_program = xvs_load(hash, gw_gpu_to_guest(address), start_size & 0xFFFF);
      } else {
        g_ps_hash = hash;
        xps_analyse(hash, gw_gpu_to_guest(address), start_size & 0xFFFF);
      }
      break;
    }
    case 0x2B: {  // IM_LOAD_IMMEDIATE: shader microcode inline
      u32 type = r.next(), start_size = r.next();
      u32 dwords = start_size & 0xFFFF;
      // Inline shaders are keyed by their ring position.
      u32 guest = r.base + (r.pos % r.size) * 4;
      if (!r.ring || (r.pos % r.size) + dwords <= r.size) {
        gw_capture_shader(type & 3, 0x80000000u | guest, guest, dwords);
        u32 hash = shader_hash(guest, dwords);
        gw_note_shader(type & 3, hash, guest, dwords);
        if ((type & 3) == 0) {
          g_vs_hash = hash;
          g_vs_program = xvs_load(hash, guest, dwords);
        } else {
          g_ps_hash = hash;
          xps_analyse(hash, guest, dwords);
        }
      }
      break;
    }
    case 0x2F: {  // LOAD_ALU_CONSTANT: constants from memory
      u32 address = r.next() & 0x3FFFFFFF, offset_type = r.next(), dwords = r.next() & 0xFFF;
      u32 index = offset_type & 0x7FF, type = (offset_type >> 16) & 0xFF;
      static const u32 kBases[5] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
      if (type < 5) {
        u32 guest = gw_gpu_to_guest(address);
        for (u32 i = 0; i < dwords; ++i) write_register(kBases[type] + index + i, gw_load32(guest + i * 4));
      }
      break;
    }
    case 0x56: {  // SET_SHADER_CONSTANTS
      u32 index = r.next() & 0xFFFF;
      for (u32 i = 1; i < count; ++i) write_register(index + i - 1, r.next());
      break;
    }
    case 0x3F:
    case 0x37: {  // INDIRECT_BUFFER(_PFD)
      u32 address = r.next();
      u32 length = r.next() & 0xFFFFF;
      Reader ib{gw_gpu_to_guest(address), length ? length : 1, 0, false};
      execute(ib, length, ctx);
      break;
    }
    case 0x3C: {  // WAIT_REG_MEM
      u32 info = r.next(), poll = r.next(), ref = r.next(), mask = r.next();
      r.next();
      for (int spins = 0;; ++spins) {
        u32 value = (info & 0x10) ? gpu_load(poll) : g_regs[poll & 0x5FFF];
        if (compare(info, value & mask, ref)) break;
        // Nothing renders yet, so there is nothing for this wait to order:
        // give the CPU a moment to release it, then move on. (One wait,
        // memory 0x20B000 == 0, is never released and stalled every frame.)
        if (spins >= 20) {
          static u32 skipped = 0;
          if (skipped++ < 4) {
            gw_log("gpu: WAIT_REG_MEM %08X skipped (value %08X ref %08X function %u)\n", poll, value, ref, info & 7);
          }
          break;
        }
        Sleep(0);
      }
      break;
    }
    case 0x21: {  // REG_RMW
      u32 info = r.next(), and_mask = r.next(), or_mask = r.next();
      u32 value = g_regs[info & 0x1FFF];
      value &= (info >> 31) ? g_regs[and_mask & 0x1FFF] : and_mask;
      value |= ((info >> 30) & 1) ? g_regs[or_mask & 0x1FFF] : or_mask;
      write_register(info & 0x1FFF, value);
      break;
    }
    case 0x3E: {  // REG_TO_MEM
      u32 reg = r.next(), address = r.next();
      gpu_store(address, g_regs[reg & 0x5FFF]);
      break;
    }
    case 0x3D: {  // MEM_WRITE
      u32 address = r.next();
      for (u32 i = 1; i < count; ++i, address += 4) gpu_store(address, r.next());
      break;
    }
    case 0x45: {  // COND_WRITE
      u32 info = r.next(), poll = r.next(), ref = r.next(), mask = r.next();
      u32 write_address = r.next(), data = r.next();
      u32 value = (info & 0x10) ? gpu_load(poll) : g_regs[poll & 0x5FFF];
      if (compare(info, value & mask, ref)) {
        if (info & 0x100) gpu_store(write_address, data);
        else write_register(write_address, data);
      }
      break;
    }
    case 0x46:  // EVENT_WRITE
      write_register(kRegVgtEventInitiator, r.next() & 0x3F);
      break;
    case 0x58: {  // EVENT_WRITE_SHD
      u32 initiator = r.next(), address = r.next(), value = r.next();
      write_register(kRegVgtEventInitiator, initiator & 0x3F);
      gpu_store(address, (initiator >> 31) ? g_swap_counter : value);
      break;
    }
    case 0x5A: {  // EVENT_WRITE_EXT: screen extents
      u32 initiator = r.next(), address = r.next();
      write_register(kRegVgtEventInitiator, initiator & 0x3F);
      static const u16 extents[6] = {0, 8192 >> 3, 0, 8192 >> 3, 0, 1};
      u32 guest = gw_gpu_to_guest(address & ~3u);
      for (int i = 0; i < 6; ++i) gw_store16(guest + i * 2, extents[i]);
      break;
    }
    case 0x5B: {  // EVENT_WRITE_ZPD: occlusion query result
      u32 initiator = r.next();
      write_register(kRegVgtEventInitiator, initiator & 0x3F);
      break;
    }
    case 0x2D: {  // SET_CONSTANT
      u32 offset_type = r.next();
      u32 index = offset_type & 0x7FF;
      u32 type = (offset_type >> 16) & 0xFF;
      static const u32 kBases[5] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
      if (type < 5) {
        for (u32 i = 1; i < count; ++i) write_register(kBases[type] + index + i - 1, r.next());
      }
      break;
    }
    case 0x55: {  // SET_CONSTANT2
      u32 index = r.next() & 0xFFFF;
      for (u32 i = 1; i < count; ++i) write_register(index + i - 1, r.next());
      break;
    }
    case 0x22:
    case 0x36: {  // DRAW_INDX(_2)
      ++gw_stats.gpu_draws;
      u32 words[16];
      u32 n = count < 16 ? count : 16;
      for (u32 i = 0; i < n; ++i) words[i] = r.next();
      gw_capture_draw(opcode, words, n, g_regs);
      gw_nv2a_draw(opcode, words, n, g_regs, g_vs_hash, g_vs_program, g_ps_hash);
      break;
    }
    case 0x50: {  // SET_BIN_MASK
      u64 v = ((u64)r.next() << 32) | r.next();
      g_bin_mask_hi = (u32)(v >> 32);
      g_bin_mask_lo = (u32)v;
      break;
    }
    case 0x51: {  // SET_BIN_SELECT
      u64 v = ((u64)r.next() << 32) | r.next();
      g_bin_select_hi = (u32)(v >> 32);
      g_bin_select_lo = (u32)v;
      break;
    }
    case 0x60: g_bin_mask_lo = r.next(); break;
    case 0x61: g_bin_mask_hi = r.next(); break;
    case 0x62: g_bin_select_lo = r.next(); break;
    case 0x63: g_bin_select_hi = r.next(); break;
    default:
      // ME_INIT, NOP, WAIT_FOR_IDLE, IM_LOAD, LOAD_ALU_CONSTANT, shader
      // constants, VIZ_QUERY, INVALIDATE_STATE, CONTEXT_UPDATE...: state only.
      break;
  }
  r.pos = data_end;
}

void execute(Reader& r, u32 end, PPCContext& ctx) {
  while (r.pos < end) {
    u32 packet = r.next();
    ++gw_stats.gpu_packets;
    if (packet == 0) continue;
    switch (packet >> 30) {
      case 0: {  // register writes
        u32 count = ((packet >> 16) & 0x3FFF) + 1;
        u32 base = packet & 0x7FFF;
        bool one = (packet >> 15) & 1;
        for (u32 i = 0; i < count; ++i) write_register(one ? base : base + i, r.next());
        break;
      }
      case 1: {
        u32 a = r.next(), b = r.next();
        write_register(packet & 0x7FF, a);
        write_register((packet >> 11) & 0x7FF, b);
        break;
      }
      case 2:
        break;
      case 3:
        execute_type3(r, packet, ctx);
        break;
    }
  }
}

DWORD WINAPI command_thread(LPVOID) {
  gw_guest_fp_setup();  // runs guest interrupt callbacks
  gw_log("command processor: host thread %u, kernel thread %08X\n", (u32)GetCurrentThreadId(), gw_kthread());
  PPCContext& ctx = *gw_make_callback_context();
  gw_nv2a_init();  // pbkit is driven from this thread only
  for (;;) {
    const u64 idle = gw_wait_begin();
    WaitForSingleObject(g_work_event, 16);
    gw_wait_end(idle, 0, "command processor: waiting for commands");
    if (!g_ring_size) continue;
    u32 dwords = g_ring_size / 4;
    u32 write = g_write_index % dwords;
    if (write == g_read_index) continue;
    Reader r{gw_gpu_to_guest(g_ring_base), dwords, g_read_index, true};
    u32 end = write >= g_read_index ? write : write + dwords;
    GwPartTimer timer(kPartCommands);
    __try {
      execute(r, end, ctx);
    } __except (gw_fault_filter(GetExceptionInformation())) {
      gw_fatal("GPU command processor crashed (see log)");
    }
    g_read_index = write;
    // The CPU reads the read pointer as a big-endian word (as on the 360,
    // where the CP swaps it); a host-order store looked like garbage and
    // eventually told the game the ring was full forever (frame 453).
    if (g_rptr_writeback) gw_store32(gw_gpu_to_guest(g_rptr_writeback), g_read_index);
  }
  return 0;
}

DWORD WINAPI vblank_thread(LPVOID) {
  gw_guest_fp_setup();  // runs guest interrupt callbacks
  gw_log("vblank: host thread %u, kernel thread %08X\n", (u32)GetCurrentThreadId(), gw_kthread());
  PPCContext& ctx = *gw_make_callback_context();
  LARGE_INTEGER frequency, next, now;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&next);
  const i64 period = frequency.QuadPart / 60;
  for (;;) {
    next.QuadPart += period;
    // Sleep whole milliseconds and fire up to 1 ms early rather than spin
    // (Sleep(0) at this priority would keep the game's threads off the CPU);
    // 'next' advances by exact periods, so the rate stays 60 Hz.
    for (;;) {
      QueryPerformanceCounter(&now);
      const i64 left_ms = (next.QuadPart - now.QuadPart) * 1000 / frequency.QuadPart;
      if (left_ms <= 0) break;
      Sleep((DWORD)left_ms);
    }
    gw_wait_report();
    __try {
      dispatch_interrupt(ctx, 0);
    } __except (gw_fault_filter(GetExceptionInformation())) {
      gw_fatal("vblank interrupt crashed (see log)");
    }
  }
  return 0;
}

}  // namespace

void gw_gpu_init() {
  InitializeCriticalSection(&g_interrupt_lock);
  g_work_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  HANDLE cp = CreateThread(nullptr, 256 * 1024, command_thread, nullptr, 0, nullptr);
  SetThreadPriority(cp, THREAD_PRIORITY_ABOVE_NORMAL);
  HANDLE vb = CreateThread(nullptr, 256 * 1024, vblank_thread, nullptr, 0, nullptr);
  SetThreadPriority(vb, THREAD_PRIORITY_HIGHEST);
}

// --- register window (0x7FC80000) ------------------------------------------------

void gw_gpu_write_register(u32 addr, u32 value) {
  u32 index = (addr & 0xFFFF) / 4;
  if (index < kRegisterCount) g_regs[index] = value;
  if (index == kRegCpRbWptr) {
    g_write_index = value;
    SetEvent(g_work_event);
  }
}

u32 gw_gpu_read_register(u32 addr) {
  u32 index = (addr & 0xFFFF) / 4;
  switch (index) {
    case 0x0F00: return 0x08100748;  // RB_EDRAM_TIMING
    case 0x0F01: return 0x0000200E;  // RB_BC_CONTROL
    case 0x194C: return 720;         // D1MODE_V_COUNTER
    case 0x1951: return 1;           // interrupt status: vblank
    case 0x1961: return (1280 << 16) | 720;
    default: return index < kRegisterCount ? g_regs[index] : 0;
  }
}

namespace rex::runtime {
MMIOHandler* MMIOHandler::global_handler() {
  static MMIOHandler handler;
  return &handler;
}
void MMIOHandler::CheckStore(u32 address, u32 value) { gw_gpu_write_register(address, value); }
void MMIOHandler::CheckLoad(u32 address, u32* value) { *value = gw_gpu_read_register(address); }
}  // namespace rex::runtime

// --- Vd exports ----------------------------------------------------------------------

void gw_write_video_mode(u32 mode) {
  memset(gw_host(mode), 0, 48);
  gw_store32(mode + 0, 1280);
  gw_store32(mode + 4, 720);
  gw_store32(mode + 8, 0);   // interlaced
  gw_store32(mode + 12, 1);  // widescreen
  gw_store32(mode + 16, 0);  // hi-def: off, as on PC (keeps the 1280x720 canvas)
  float hz = 60.0f;
  u32 bits;
  memcpy(&bits, &hz, 4);
  gw_store32(mode + 20, bits);
  gw_store32(mode + 24, 1);  // NTSC
  gw_store32(mode + 28, 0x4A);
  gw_store32(mode + 32, 0x01);
}

GW_EXPORT(VdQueryVideoMode) { gw_write_video_mode(GW_ARG(3)); }
GW_EXPORT(VdQueryVideoFlags) { GW_RETURN(1 | 2); }
GW_EXPORT(VdSetDisplayMode) { GW_RETURN(0); }
GW_EXPORT(VdInitializeEngines) { GW_RETURN(1); }
GW_EXPORT(VdShutdownEngines) {}
GW_EXPORT(VdEnableDisableClockGating) { GW_RETURN(0); }
GW_EXPORT(VdIsHSIOTrainingSucceeded) { GW_RETURN(1); }
GW_EXPORT(VdRetrainEDRAM) { GW_RETURN(0); }
GW_EXPORT(VdRetrainEDRAMWorker) { GW_RETURN(0); }
GW_EXPORT(VdCallGraphicsNotificationRoutines) { GW_RETURN(0); }
GW_EXPORT(VdSetSystemCommandBufferGpuIdentifierAddress) {}

GW_EXPORT(VdGetCurrentDisplayGamma) {
  gw_store32(GW_ARG(3), 2);
  float power = 2.22222233f;
  u32 bits;
  memcpy(&bits, &power, 4);
  gw_store32(GW_ARG(4), bits);
}

GW_EXPORT(VdGetCurrentDisplayInformation) {
  u32 info = GW_ARG(3);
  memset(gw_host(info), 0, 0x58);
  gw_store16(info + 0x00, 1280);
  gw_store16(info + 0x02, 720);
  gw_store32(info + 0x08 + 0x08, 1280);  // scaler source rect x2
  gw_store32(info + 0x08 + 0x0C, 720);   // y2
  gw_store32(info + 0x08 + 0x10, 1280);  // scaled output
  gw_store32(info + 0x08 + 0x14, 720);
  gw_store32(info + 0x08 + 0x18, 1);
  gw_store32(info + 0x08 + 0x28, 1);
  gw_store16(info + 0x40, 320);
  gw_store16(info + 0x42, 180);
  gw_store16(info + 0x44, 320);
  gw_store16(info + 0x46, 180);
  gw_store16(info + 0x48, 1280);
  gw_store16(info + 0x4A, 720);
  float hz = 60.0f;
  u32 bits;
  memcpy(&bits, &hz, 4);
  gw_store32(info + 0x4C, bits);
  gw_store16(info + 0x56, 1280);
}

GW_EXPORT(VdSetGraphicsInterruptCallback) {
  g_interrupt_callback = GW_ARG(3);
  g_interrupt_data = GW_ARG(4);
  gw_log("VdSetGraphicsInterruptCallback(%08X, %08X)\n", GW_ARG(3), GW_ARG(4));
}

GW_EXPORT(VdInitializeRingBuffer) {
  g_ring_base = GW_ARG(3) & 0x1FFFFFFF;
  g_ring_size = 1u << (GW_ARG(4) + 3);
  g_read_index = 0;
  gw_log("VdInitializeRingBuffer(%08X, %u bytes)\n", g_ring_base, g_ring_size);
}

GW_EXPORT(VdEnableRingBufferRPtrWriteBack) {
  g_rptr_writeback = GW_ARG(3) & 0x1FFFFFFF;
  gw_log("VdEnableRingBufferRPtrWriteBack(%08X)\n", g_rptr_writeback);
}

GW_EXPORT(VdGetSystemCommandBuffer) {
  memset(gw_host(GW_ARG(3)), 0, 0x94);
  gw_store32(GW_ARG(3), 0xBEEF0000);
  gw_store32(GW_ARG(4), 0xBEEF0001);
}

GW_EXPORT(VdInitializeScalerCommandBuffer) {
  u32 dest = GW_STACK_ARG(10), count = GW_STACK_ARG(11);
  for (u32 i = 0; i < count; ++i) gw_store32(dest + i * 4, 0x80000000);
  GW_RETURN(count);
}

GW_EXPORT(VdPersistDisplay) {
  if (GW_ARG(4)) gw_store32(GW_ARG(4), gw_physical_alloc(64, 0x1000));
  GW_RETURN(1);
}

// VdSwap(buffer, fetch, unk2, unk3, unk4, frontbuffer_ptr, format_ptr, color_space_ptr,
//        width_ptr, height_ptr): writes the swap packets into the ring buffer.
GW_EXPORT(VdSwap) {
  u32 buffer = GW_ARG(3), fetch = GW_ARG(4), frontbuffer_ptr = GW_ARG(8);
  u32 width_ptr = GW_STACK_ARG(8), height_ptr = GW_STACK_ARG(9);
  u32 frontbuffer = gw_load32(frontbuffer_ptr) & 0x1FFFFFFF;
  u32 width = width_ptr ? gw_load32(width_ptr) : 1280;
  u32 height = height_ptr ? gw_load32(height_ptr) : 720;
  u32 words[64];
  u32 n = 0;
  words[n++] = (5u << 16) | kRegShaderConstantFetch00;  // type 0, 6 registers
  for (int i = 0; i < 6; ++i) words[n++] = gw_load32(fetch + i * 4);
  words[n++] = (3u << 30) | (3u << 16) | (0x64u << 8);  // type 3 XE_SWAP, 4 dwords
  words[n++] = kSwapSignature;
  words[n++] = frontbuffer;
  words[n++] = width;
  words[n++] = height;
  while (n < 64) words[n++] = 0x80000000;  // type 2 padding
  for (u32 i = 0; i < 64; ++i) gw_store32(buffer + i * 4, words[i]);
}
