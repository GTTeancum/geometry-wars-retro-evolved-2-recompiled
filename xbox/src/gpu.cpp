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
HANDLE g_work_event;          // the interrupt thread's wake-up
volatile long g_pending_interrupts;  // command-processor interrupts to deliver
CRITICAL_SECTION g_command_lock;    // one thread runs the commands at a time
bool g_nv2a_started;
PPCContext* g_command_ctx;  // for the commands' callbacks (run_commands, native draws)

u32 g_interrupt_callback;
u32 g_interrupt_data;
CRITICAL_SECTION g_interrupt_lock;

// Each INTERRUPT packet's callback, as the scratch registers held it when
// the packet ran. The game's Direct3D puts the callback for an interrupt in
// scratch registers 4 and 5 (written back to the scratch area, which its
// handler reads, sub_820A4300: +16 the function, +20 its argument) and
// marks it taken (0x0BADF00D). The commands here run well ahead of the
// interrupts' delivery, so a later callback would overwrite an earlier one
// before its interrupt is delivered (the handler then found 0x0BADF00D,
// "Unanticipated CPU_INTERRUPT", and called it). Before each delivery the
// scratch write-back of those two registers is repeated with the values of
// that interrupt's own time: the same write the command processor makes, to
// the same two words of the scratch area.
constexpr u32 kCallbackFirst = 4, kCallbackCount = 2;  // scratch registers 4 and 5
struct PendingInterrupt {
  u32 scratch_addr;  // GPU address of the scratch area
  u32 mask;          // which of the two were written back (scratch umask)
  u32 value[kCallbackCount];
};
constexpr u32 kPendingMax = 64;
PendingInterrupt g_pending[kPendingMax];
u32 g_pending_head, g_pending_tail;  // under g_pending_lock
CRITICAL_SECTION g_pending_lock;

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
// Audit (temporary): writes to the game's GPU progress word (device +10896).
void audit_progress(u32 guest, u32 value, const char* how) {
  const u32 dev = gw_load32(0x8219D290);
  if (!dev || guest != gw_load32(dev + 10896)) return;
  static u32 count = 0;
  ++count;
  if (count <= 6 || count % 2000 == 0)
    gw_log("progress write %u by %s: %08X (fence now %08X)\n", count, how, value, gw_load32(dev + 10908));
}

void gpu_store(u32 gpu_address, u32 value) {
  value = gpu_swap(value, gpu_address & 3);
  memcpy(gw_host(gw_gpu_to_guest(gpu_address & ~3u)), &value, 4);
  audit_progress(gw_gpu_to_guest(gpu_address & ~3u), __builtin_bswap32(value), "memory write");
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
    if (g_regs[kRegScratchUmsk] & (1u << n)) {
      gw_store32(gw_gpu_to_guest(g_regs[kRegScratchAddr] + n * 4), value);
      audit_progress(gw_gpu_to_guest(g_regs[kRegScratchAddr] + n * 4), value, "scratch");
    }
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
  // Wraps the ring by a mask (its size is a power of two: a division per
  // word was a tenth of the command processor's time on the Xbox); an
  // indirect buffer never wraps.
  u32 next() {
    const u32 at = ring ? pos & (size - 1) : pos;
    ++pos;
    return gw_load32(base + at * 4);
  }
  void skip(u32 n) { pos += n; }
};

// Register writes from the stream: count consecutive registers from first
// (or count writes to first when 'one'). Runs that touch none of the
// registers with side effects (scratch, coherency) go in a tight loop.
void write_registers(Reader& r, u32 first, u32 count, bool one) {
  const u32 last = one ? first : first + count - 1;
  const bool plain = last < kRegisterCount && !(first <= kRegScratchReg0 + 7 && last >= kRegScratchReg0) &&
                     !(first <= kRegCoherStatusHost && last >= kRegCoherStatusHost);
  if (!plain) {
    for (u32 i = 0; i < count; ++i) write_register(one ? first : first + i, r.next());
    return;
  }
  if (one) {
    for (u32 i = 0; i < count; ++i) g_regs[first] = r.next();
    return;
  }
  for (u32 i = 0; i < count; ++i) g_regs[first + i] = r.next();
}

void execute(Reader& r, u32 end, PPCContext& ctx);

// Command buffer words the native draws (below) already ran, ahead of the
// game handing them over: [g_consumed_start, g_consumed_end), guest
// addresses. A command buffer handed over later (INDIRECT_BUFFER) skips
// them. g_buffer_end: the end of the last buffer run, where the game's next
// commands start unless it moved to a new chunk.
u32 g_consumed_start, g_consumed_end, g_buffer_end;

// Runs guest command words [start, end) (dwords).
void run_words(u32 start, u32 end, PPCContext& ctx) {
  if (end <= start) return;
  Reader r{start, (end - start) / 4, 0, false};
  execute(r, (end - start) / 4, ctx);
}

void run_indirect(u32 start, u32 length, PPCContext& ctx) {
  const u32 end = start + length * 4;
  if (g_consumed_end > g_consumed_start && start < g_consumed_end && end > g_consumed_start) {
    run_words(start, g_consumed_start > start ? g_consumed_start : start, ctx);
    if (g_consumed_end < end) run_words(g_consumed_end, end, ctx);
    if (end >= g_consumed_end) g_consumed_start = g_consumed_end = 0;
  } else {
    run_words(start, end, ctx);
  }
  g_buffer_end = end;
}

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
      // Delivered by the interrupt thread, as the 360 delivers it on its
      // own: the commands run on the thread that wrote them (run_commands),
      // inside the game's Direct3D, where calling back into the game could
      // take its locks out of order.
      if (cpu_mask) {
        PendingInterrupt p{g_regs[kRegScratchAddr], 0, {}};
        for (u32 k = 0; k < kCallbackCount; ++k) {
          const u32 n = kCallbackFirst + k;
          if (g_regs[kRegScratchUmsk] & (1u << n)) p.mask |= 1u << k;
          p.value[k] = g_regs[kRegScratchReg0 + n];
        }
        EnterCriticalSection(&g_pending_lock);
        if (g_pending_tail - g_pending_head < kPendingMax) {
          g_pending[g_pending_tail % kPendingMax] = p;
          ++g_pending_tail;
        } else {
          static bool logged;
          if (!logged) gw_log("gpu: more than %u interrupts waiting; one left out\n", kPendingMax);
          logged = true;
        }
        LeaveCriticalSection(&g_pending_lock);
        InterlockedIncrement(&g_pending_interrupts);
        SetEvent(g_work_event);
      }
      (void)ctx;
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
      if (count > 1) write_registers(r, index, count - 1, false);
      break;
    }
    case 0x3F:
    case 0x37: {  // INDIRECT_BUFFER(_PFD)
      u32 address = r.next();
      u32 length = r.next() & 0xFFFFF;
      run_indirect(gw_gpu_to_guest(address), length, ctx);
      break;
    }
    case 0x3C: {  // WAIT_REG_MEM
      u32 info = r.next(), poll = r.next(), ref = r.next(), mask = r.next();
      r.next();
      // The commands run on the thread that wrote them (run_commands), so
      // nothing this wait could wait for changes while it waits: checked
      // once and passed either way. (It used to yield 20 times first; one
      // wait, memory 0x20B000 == 0, is never released, and on the game's own
      // thread the yields were idle time in every frame.)
      const u32 value = (info & 0x10) ? gpu_load(poll) : g_regs[poll & 0x5FFF];
      if (!compare(info, value & mask, ref)) {
        static u32 skipped = 0;
        if (skipped++ < 4) {
          gw_log("gpu: WAIT_REG_MEM %08X skipped (value %08X ref %08X function %u)\n", poll, value, ref, info & 7);
        }
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
        if (count > 1) write_registers(r, kBases[type] + index, count - 1, false);
      }
      break;
    }
    case 0x55: {  // SET_CONSTANT2
      u32 index = r.next() & 0xFFFF;
      if (count > 1) write_registers(r, index, count - 1, false);
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
    const u32 at = r.base + (r.ring ? (r.pos & (r.size - 1)) : r.pos) * 4;
    u32 packet = r.next();
    ++gw_stats.gpu_packets;
    if (packet == 0) continue;
    // Audit: a packet running past the words given is not a packet (words
    // that are not commands, read as commands).
    {
      const u32 t = packet >> 30;
      const u32 words = t == 0 || t == 3 ? ((packet >> 16) & 0x3FFF) + 1 : t == 1 ? 2 : 0;
      if (r.pos + words > end) {
        static u32 told = 0;
        if (told++ < 16) gw_log("gpu audit: packet %08X at %08X runs %u words past its buffer\n", packet, at,
                                r.pos + words - end);
      }
    }
    switch (packet >> 30) {
      case 0: {  // register writes
        u32 count = ((packet >> 16) & 0x3FFF) + 1;
        u32 base = packet & 0x7FFF;
        bool one = (packet >> 15) & 1;
        write_registers(r, base, count, one);
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

// The commands written so far, run on the thread that moved the write
// pointer (the game's main thread), at once. They used to run on a command
// processor thread of their own, which on the Xbox's one core took turns
// with the game: each of the frame's kicks was a thread switch there and
// back, and the thread's work was the game's lost time all the same.
// pbkit is driven from here only (gw_nv2a_init on the first kick).
void run_commands() {
  EnterCriticalSection(&g_command_lock);
  if (!g_nv2a_started) {
    g_nv2a_started = true;
    gw_nv2a_init();
  }
  if (!g_command_ctx) g_command_ctx = gw_make_callback_context();
  PPCContext* const ctx = g_command_ctx;
  if (g_ring_size) {
    const u32 dwords = g_ring_size / 4;
    const u32 write = g_write_index % dwords;
    if (write != g_read_index) {
      Reader r{gw_gpu_to_guest(g_ring_base), dwords, g_read_index, true};
      const u32 end = write >= g_read_index ? write : write + dwords;
      GwPartTimer timer(kPartCommands);
      __try {
        execute(r, end, *ctx);
      } __except (gw_fault_filter(GetExceptionInformation())) {
        gw_fatal("GPU command processor crashed (see log)");
      }
      g_read_index = write;
      // The CPU reads the read pointer as a big-endian word (as on the 360,
      // where the CP swaps it); a host-order store looked like garbage and
      // eventually told the game the ring was full forever (frame 453).
      if (g_rptr_writeback) gw_store32(gw_gpu_to_guest(g_rptr_writeback), g_read_index);
    }
  }
  LeaveCriticalSection(&g_command_lock);
}


// --- Native Direct3D draws -------------------------------------------------------
//
// The game's Direct3D sends graphics state lazily: its setters write shadow
// copies of the Xenos registers in the device (with a dirty bit per register
// or group), and each draw call writes the dirty ones into the command
// buffer as packets, then the draw packet; the buffer reaches the command
// processor (here, execute) a few times a frame. The draws here do what the
// draw calls do, but write the dirty state straight into the register file
// (g_regs) and draw on the NV2A at once: no packets built for the state or
// the draw, none decoded.
//
// Order is kept by running, before each draw, the command words the game
// wrote since the last run (render targets, clears, resolves, scissors...)
// and noting them as consumed so the buffer, when handed over, skips them
// (run_indirect). What the game does through packets only (shader microcode
// loads, after it patches its shaders in memory, and their own constants)
// is left to its own draw setup (sub_820A9F38), whose words are run the
// same way at once. When the words since the last run cannot be found (the
// game moved to a new chunk of the buffer since), a draw goes the original
// way.
//
// Device layout (dev = the device, guest 0x8219D290): dirty masks at +0
// (vertex constants, shadow +1920 -> 0x4000), +8 (pixel constants, +6016 ->
// 0x4400), +16, +24, +32 (state, below); the command buffer's last written
// word at +48, its soft end at +56; the index buffer at +12436.
extern "C" {
void __imp__sub_820A08B8(PPCContext& ctx, uint8_t* base);  // DrawVertices
void __imp__sub_820A0CA0(PPCContext& ctx, uint8_t* base);  // DrawIndexedVertices
void __imp__sub_820A08A8(PPCContext& ctx, uint8_t* base);  // EndVertices
void sub_820A9F38(PPCContext& ctx, uint8_t* base);  // draw setup: shaders (writes packets)
void sub_820A8A68(PPCContext& ctx, uint8_t* base);  // hi-Z and surface info (writes packets)
void sub_820A8EC0(PPCContext& ctx, uint8_t* base);  // coherency (writes packets)
void sub_820A5320(PPCContext& ctx, uint8_t* base);  // makes room: hands the buffer over
void __imp__sub_820A5320(PPCContext& ctx, uint8_t* base);
}

namespace {

// The GPU address the game writes for a guest address (its own formula).
u32 gpu_address(u32 a) { return (a & 0x1FFFFFFF) + ((((a >> 20) & 0xFFF) + 0x200) & 0x1000); }

// The command words since the last run, run now and noted as consumed.
// False when they cannot be found.
bool catch_up(u32 dev, PPCContext& ctx) {
  const u32 end = gw_gpu_to_guest(gpu_address(gw_load32(dev + 48) + 4));
  u32 start = g_consumed_end && g_consumed_end <= end && end - g_consumed_end < 0x100000 ? g_consumed_end : g_buffer_end;
  if (!start || start > end || end - start >= 0x100000) return false;
  run_words(start, end, ctx);
  if (start != g_consumed_end) g_consumed_start = start;
  g_consumed_end = end;
  return true;
}

// Dirty ALU constants: mask bit 63 - k stands for registers base + 16k to
// base + 16k + 15 (shadow + 64k).
void apply_constants(u64 mask, u32 base, u32 shadow) {
  for (u32 k = 0; mask; ++k, mask <<= 1) {
    if (!(mask >> 63)) continue;
    for (u32 w = 0; w < 16; ++w) g_regs[base + k * 16 + w] = gw_load32(shadow + k * 64 + w * 4);
  }
}

// Dirty registers: register first + k is mask bit 'top' - k, value at
// shadow + 4k (the run writer sub_820A8E00 with the mask shifted to the top).
void apply_registers(u64 mask, u32 top, u32 first, u32 count, u32 shadow) {
  for (u32 k = 0; k < count; ++k) {
    if ((mask >> (top - k)) & 1) g_regs[first + k] = gw_load32(shadow + k * 4);
  }
}

u64 call_with_mask(void (*fn)(PPCContext&, uint8_t*), PPCContext& ctx, uint8_t* base, u32 dev, u64 mask,
                   u32 r5) {
  ctx.r1.u64 = ctx.r1.u32 - 208;  // the draw call's frame
  ctx.r3.u64 = dev;
  ctx.r4.u64 = mask;
  ctx.r5.u64 = r5;
  fn(ctx, base);
  ctx.r1.u64 = ctx.r1.u32 + 208;
  return ctx.r3.u64;
}

// The draw calls' state flush, into the register file. False: not possible
// natively (the caller goes the original way, nothing done yet).
bool flush_state(u32 dev, PPCContext& ctx, uint8_t* base) {
  if (!catch_up(dev, ctx)) return false;
  if (const u64 m = gw_load64(dev + 0)) {
    apply_constants(m, 0x4000, dev + 1920);
    gw_store64(dev + 0, 0);
  }
  if (const u64 m = gw_load64(dev + 8)) {
    apply_constants(m, 0x4400, dev + 6016);
    gw_store64(dev + 8, 0);
  }
  if (u64 m = gw_load64(dev + 16)) {
    bool wrote = false;
    if (m & 0x1E0000) {
      m = call_with_mask(sub_820A9F38, ctx, base, dev, m, 0);
      wrote = true;
    }
    if (gw_load64(dev + 40) & m) {
      m = call_with_mask(sub_820A8A68, ctx, base, dev, m, gw_load32(dev + 10560));
      wrote = true;
    }
    if (wrote && !catch_up(dev, ctx)) gw_log("native draw: shader words not found\n");
    apply_registers(m, 11, 0x2200, 12, dev + 10548);
    apply_registers(m, 16, 0x2180, 5, dev + 10528);
    apply_registers(m, 57, 0x2000, 16, dev + 10368);
    apply_registers(m, 41, 0x2100, 21, dev + 10444);
    gw_store64(dev + 16, 0);
  }
  if (const u64 f = gw_load64(dev + 24)) {
    for (u32 k = 0; k < 32; ++k) {  // fetch constants: slot k is bit 31 - k
      if (!((f >> (31 - k)) & 1)) continue;
      for (u32 w = 0; w < 6; ++w) g_regs[0x4800 + k * 6 + w] = gw_load32(dev + 1152 + k * 24 + w * 4);
    }
    apply_registers(f, 54, 0x2280, 21, dev + 10596);
    gw_store64(dev + 24, 0);
  }
  if (const u64 g = gw_load64(dev + 32)) {
    apply_registers(g, 37, 0x2300, 38, dev + 10680);
    apply_registers(g, 45, 0x2380, 8, dev + 10832);
    if (g & (63ull << 49)) {
      call_with_mask(sub_820A8EC0, ctx, base, dev, (g << 9) & 0xFC00000000000000ull, 0);
      if (!catch_up(dev, ctx)) gw_log("native draw: coherency words not found\n");
    }
    if ((g >> 56) & 1) {
      for (u32 k = 0; k < 40; ++k) g_regs[0x4900 + k] = gw_load32(dev + 10112 + k * 4);
    }
    gw_store64(dev + 32, 0);
  }
  // The make-room check before the draw packet: hands the buffer over when
  // past its soft end (what frees ring space for the game's allocations).
  if (gw_load32(dev + 48) > gw_load32(dev + 56)) {
    ctx.r1.u64 = ctx.r1.u32 - 208;
    ctx.r3.u64 = dev;
    sub_820A5320(ctx, base);
    // Its result is the write pointer the callers carry on from (they store
    // it back after their packet; there is none here).
    gw_store32(dev + 48, ctx.r3.u32);
    ctx.r1.u64 = ctx.r1.u32 + 208;
  }
  return true;
}

// The draw in pieces of at most 65535 vertices, as the game splits them
// (table at 0x820066A8: per primitive the step and the overlap), each
// through gw_nv2a_draw as a draw packet would be.
template <typename Piece>
void split_draw(u32 prim, u32 count, Piece piece) {
  const u32 step = gw_load32(0x820066A8 + (prim & 63) * 8), overlap = gw_load32(0x820066A8 + (prim & 63) * 8 + 4);
  u32 first = 0;
  for (;;) {
    u32 n = count;
    if (count > 65535 && step) n = (65535 / step & ~1u) * step;
    piece(first, n);
    if (n >= count) break;
    count = count - n + overlap;
    first += n - overlap;
  }
}

bool native_enabled(u32 dev) { return g_nv2a_started && !(gw_load8(dev + 10940) & 1); }

}  // namespace

// Make room (guest 0x820A5320, r3 the device): hands the command buffer
// over and moves the game to a new chunk of it. The game's next commands
// start after the word it returns, which catch_up then starts from
// (the end of the last buffer run would be the old chunk's).
extern "C" void sub_820A5320(PPCContext& ctx, uint8_t* base) {
  __imp__sub_820A5320(ctx, base);
  const u32 result = ctx.r3.u32;
  EnterCriticalSection(&g_command_lock);
  // r3: the new last-written word (the callers write after it).
  g_buffer_end = gw_gpu_to_guest(gpu_address(result + 4));
  LeaveCriticalSection(&g_command_lock);
  ctx.r3.u64 = result;
}

// DrawVertices (guest 0x820A08B8): r3 device, r4 primitive, r5 first
// vertex, r6 count.
extern "C" void sub_820A08B8(PPCContext& ctx, uint8_t* base) {
  const u32 dev = ctx.r3.u32, prim = ctx.r4.u32, start = ctx.r5.u32, count = ctx.r6.u32;
  EnterCriticalSection(&g_command_lock);
  if (!native_enabled(dev) || !flush_state(dev, ctx, base)) {
    LeaveCriticalSection(&g_command_lock);
    ctx.r3.u64 = dev;
    ctx.r4.u64 = prim;
    ctx.r5.u64 = start;
    ctx.r6.u64 = count;
    __imp__sub_820A08B8(ctx, base);
    return;
  }
  split_draw(prim, count, [&](u32 first, u32 n) {
    g_regs[0x2102] = start + first;  // VGT_INDX_OFFSET
    const u32 words[2] = {0, n << 16 | 0x80 | (prim & 63)};  // auto-indexed
    ++gw_stats.gpu_draws;
    gw_nv2a_draw(0x22, words, 2, g_regs, g_vs_hash, g_vs_program, g_ps_hash);
  });
  LeaveCriticalSection(&g_command_lock);
}

// DrawIndexedVertices (guest 0x820A0CA0): r3 device, r4 primitive, r5 base
// vertex, r6 first index, r7 count; the index buffer at device +12436 (+0
// bit 31: 32-bit indices; +24 its data).
extern "C" void sub_820A0CA0(PPCContext& ctx, uint8_t* base) {
  const u32 dev = ctx.r3.u32, prim = ctx.r4.u32, base_vertex = ctx.r5.u32, start = ctx.r6.u32,
            count = ctx.r7.u32;
  EnterCriticalSection(&g_command_lock);
  if (!native_enabled(dev) || !flush_state(dev, ctx, base)) {
    LeaveCriticalSection(&g_command_lock);
    ctx.r3.u64 = dev;
    ctx.r4.u64 = prim;
    ctx.r5.u64 = base_vertex;
    ctx.r6.u64 = start;
    ctx.r7.u64 = count;
    __imp__sub_820A0CA0(ctx, base);
    return;
  }
  const u32 ib = gw_load32(dev + 12436);
  const u32 ib0 = gw_load32(ib + 0), data = gw_load32(ib + 24);
  const bool wide = ib0 >> 31;
  split_draw(prim, count, [&](u32 first, u32 n) {
    g_regs[0x2102] = base_vertex;
    const u32 at = data + (start + first) * (wide ? 4 : 2);
    const u32 size = wide ? (n & 0xFFFFFF) : ((n * 2) & 0xFFFFFE);
    const u32 words[4] = {0, n << 16 | (prim & 63) | (wide ? 0x800 : 0), gpu_address(at),
                          size | ((ib0 << 1) & 0xC0000000)};
    ++gw_stats.gpu_draws;
    gw_nv2a_draw(0x22, words, 4, g_regs, g_vs_hash, g_vs_program, g_ps_hash);
  });
  LeaveCriticalSection(&g_command_lock);
}

// EndVertices (guest 0x820A08A8): commits what BeginVertices wrote (its
// state, the vertices' fetch constant and the draw) and runs it at once, in
// order with the native draws.
extern "C" void sub_820A08A8(PPCContext& ctx, uint8_t* base) {
  const u32 dev = ctx.r3.u32;
  __imp__sub_820A08A8(ctx, base);
  EnterCriticalSection(&g_command_lock);
  if (g_nv2a_started) catch_up(dev, ctx);
  LeaveCriticalSection(&g_command_lock);
}

// Delivers the command processor's interrupts (INTERRUPT packets) to the
// game's graphics callback, on its own thread as on the 360.
DWORD WINAPI interrupt_thread(LPVOID) {
  gw_guest_fp_setup();  // runs guest interrupt callbacks
  gw_log("graphics interrupts: host thread %u, kernel thread %08X\n", (u32)GetCurrentThreadId(), gw_kthread());
  PPCContext& ctx = *gw_make_callback_context();
  gw_set_processor(kInterruptProcessor);  // interrupts come on their own hardware thread (gw_set_irql)
  for (;;) {
    const u64 idle = gw_wait_begin();
    WaitForSingleObject(g_work_event, INFINITE);
    gw_wait_end(idle, 0, "graphics interrupts: waiting");
    while (g_pending_interrupts > 0) {
      InterlockedDecrement(&g_pending_interrupts);
      PendingInterrupt p;
      bool have = false;
      EnterCriticalSection(&g_pending_lock);
      if (g_pending_head != g_pending_tail) {
        p = g_pending[g_pending_head % kPendingMax];
        ++g_pending_head;
        have = true;
      }
      LeaveCriticalSection(&g_pending_lock);
      if (have) {
        for (u32 k = 0; k < kCallbackCount; ++k)
          if (p.mask & (1u << k)) gw_store32(gw_gpu_to_guest(p.scratch_addr + (kCallbackFirst + k) * 4), p.value[k]);
      }
      __try {
        dispatch_interrupt(ctx, 1);
      } __except (gw_fault_filter(GetExceptionInformation())) {
        gw_fatal("graphics interrupt crashed (see log)");
      }
    }
  }
  return 0;
}

DWORD WINAPI vblank_thread(LPVOID) {
  gw_guest_fp_setup();  // runs guest interrupt callbacks
  gw_log("vblank: host thread %u, kernel thread %08X\n", (u32)GetCurrentThreadId(), gw_kthread());
  PPCContext& ctx = *gw_make_callback_context();
  gw_set_processor(kInterruptProcessor);  // interrupts come on their own hardware thread (gw_set_irql)
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

volatile u32 gw_gpu_kicks;

void gw_gpu_init() {
  InitializeCriticalSection(&g_interrupt_lock);
  InitializeCriticalSection(&g_pending_lock);
  InitializeCriticalSection(&g_command_lock);
  g_work_event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  HANDLE it = CreateThread(nullptr, 256 * 1024, interrupt_thread, nullptr, 0, nullptr);
  SetThreadPriority(it, THREAD_PRIORITY_ABOVE_NORMAL);
  HANDLE vb = CreateThread(nullptr, 256 * 1024, vblank_thread, nullptr, 0, nullptr);
  SetThreadPriority(vb, THREAD_PRIORITY_HIGHEST);
}

// --- register window (0x7FC80000) ------------------------------------------------

void gw_gpu_write_register(u32 addr, u32 value) {
  u32 index = (addr & 0xFFFF) / 4;
  if (index < kRegisterCount) g_regs[index] = value;
  if (index == kRegCpRbWptr) {
    g_write_index = value;
    ++gw_gpu_kicks;
    run_commands();
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
