// One-frame capture of the game's GPU command stream, for working out what
// the NV2A renderer has to reproduce. Starts at swap kCaptureSwap and ends at
// the next swap. xbox/tools/gpu_capture.py saves gw_capture through the XEMU
// monitor and decodes it.
//
// Stream of u32 records:
//   'SNAP' then the context registers 0x2000-0x23FF and the shader
//          constants 0x4000-0x4927 at the start of the frame
//   'SHDR' type, gpu address, dwords, then the microcode (first sight only)
//   'DRAW' opcode, packet dwords n, the packet, vertex shader address,
//          pixel shader address, changed-register count, (index, value)...
//   'SWAP' frontbuffer, width, height
#include "runtime.h"

extern "C" {
// Read by xbox/tools/gpu_capture.py: magic, state (0 waiting, 1 capturing,
// 2 done), words used, then the stream.
u32 gw_capture[3 + 384 * 1024];
}

namespace {

constexpr u32 kCaptureSwap = 120;
constexpr u32 kCapacity = sizeof(gw_capture) / 4 - 3;

constexpr u32 kContextBase = 0x2000, kContextCount = 0x400;
constexpr u32 kConstantBase = 0x4000, kConstantCount = 0x928;

u32 g_shadow_context[kContextCount];
u32 g_shadow_constants[kConstantCount];
u32 g_seen_shaders[128];
u32 g_seen_count;
u32 g_vs_address, g_ps_address;

u32& state() { return gw_capture[1]; }
u32& used() { return gw_capture[2]; }
u32* stream() { return gw_capture + 3; }

bool room(u32 words) { return used() + words <= kCapacity; }
void put(u32 v) {
  if (used() < kCapacity) stream()[used()++] = v;
}

void snapshot(const u32* regs) {
  if (!room(1 + kContextCount + kConstantCount)) return;
  put(0x50414E53);  // 'SNAP'
  for (u32 i = 0; i < kContextCount; ++i) put(g_shadow_context[i] = regs[kContextBase + i]);
  for (u32 i = 0; i < kConstantCount; ++i) put(g_shadow_constants[i] = regs[kConstantBase + i]);
}

}  // namespace

void gw_capture_swap(u32 swap_number, u32 frontbuffer, u32 width, u32 height, const u32* regs) {
  gw_capture[0] = 0x50414347;  // 'GCAP'
  if (state() == 0 && swap_number == kCaptureSwap) {
    state() = 1;
    used() = 0;
    snapshot(regs);
    gw_log("gpu capture: frame %u started\n", swap_number);
  } else if (state() == 1) {
    put(0x50415753);  // 'SWAP'
    put(frontbuffer);
    put(width);
    put(height);
    state() = 2;
    gw_log("gpu capture: done, %u words\n", used());
  }
}

// Shader microcode as loaded by IM_LOAD / IM_LOAD_IMMEDIATE.
void gw_capture_shader(u32 type, u32 gpu_address, u32 guest_words, u32 dwords) {
  if (type == 0) g_vs_address = gpu_address;
  else g_ps_address = gpu_address;
  if (state() != 1) return;
  u32 key = gpu_address ^ (dwords << 20) ^ type;
  for (u32 i = 0; i < g_seen_count; ++i) {
    if (g_seen_shaders[i] == key) return;
  }
  if (g_seen_count < 128) g_seen_shaders[g_seen_count++] = key;
  if (!room(4 + dwords)) return;
  put(0x52444853);  // 'SHDR'
  put(type);
  put(gpu_address);
  put(dwords);
  for (u32 i = 0; i < dwords; ++i) put(gw_load32(guest_words + i * 4));
}

void gw_capture_draw(u32 opcode, const u32* packet, u32 packet_dwords, const u32* regs) {
  if (state() != 1) return;
  if (!room(7 + packet_dwords + 2 * (kContextCount + kConstantCount))) return;
  put(0x57415244);  // 'DRAW'
  put(opcode);
  put(packet_dwords);
  for (u32 i = 0; i < packet_dwords; ++i) put(packet[i]);
  put(g_vs_address);
  put(g_ps_address);
  u32 count_at = used();
  put(0);
  u32 changed = 0;
  for (u32 i = 0; i < kContextCount; ++i) {
    u32 v = regs[kContextBase + i];
    if (v != g_shadow_context[i]) {
      put(kContextBase + i);
      put(v);
      g_shadow_context[i] = v;
      ++changed;
    }
  }
  for (u32 i = 0; i < kConstantCount; ++i) {
    u32 v = regs[kConstantBase + i];
    if (v != g_shadow_constants[i]) {
      put(kConstantBase + i);
      put(v);
      g_shadow_constants[i] = v;
      ++changed;
    }
  }
  stream()[count_at] = changed;
}
