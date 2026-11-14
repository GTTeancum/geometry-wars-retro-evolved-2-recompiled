// The Xbox's audio processing unit (the MCPX APU) driving the game's sounds
// in hardware: its voice processor (VP) decodes Xbox ADPCM, changes pitch,
// applies volumes and mixes each voice into mix bins; a program of ours on
// its global processor (GP, a DSP56300) copies the front left/right bins
// into a ring in memory every APU frame (32 samples at 48 kHz); the audio
// thread (src/audio.cpp) passes that ring on to the AC97, adding what is still
// mixed in software.
//
// Our own code, written from what open-source work has documented of the
// chip (the register layout and the bring-up order as nxdk-ex's OpenAL on the
// APU found them on a console, and xemu's emulation of it). Only the paths a
// console runs are used: voice records and list heads written from the CPU
// (the front end's methods do not run on a console), the GP's output read
// from its ring (xemu plays it this way too when the front end is not used).
//
// A console's VP stops every frame when a voice that is in a list reaches its
// end, so a voice never ends by itself here: one-shots loop over a silent
// tail and are taken off the list by gw_apu_voice_stop.
#include "apu.h"

#include <string.h>
#include <windows.h>
#include <xmmintrin.h>
#include <xboxkrnl/xboxkrnl.h>

namespace {

// --- registers (BAR0 offsets) -----------------------------------------------------------
volatile u8* const kBar0 = (volatile u8*)0xFE800000;
constexpr u32 kISTS = 0x1000, kIEN = 0x1004, kFECTL = 0x1100, kFETFORCE1 = 0x1504;
constexpr u32 kSECTL = 0x2000, kSECTL_RUN = 0x08;  // XCNTMODE 1: frames run, synced to the AC97 link
constexpr u32 kSECTL_FREE = 0x18;  // XCNTMODE 3: frames free running
constexpr u32 kXGSCNT = 0x200C;
constexpr u32 kVPVADDR = 0x202C, kVPSGEADDR = 0x2030, kVPSSLADDR = 0x2034;
constexpr u32 kGPSADDR = 0x2040, kGPFADDR = 0x2044, kGPSMAXSGE = 0x20D4, kGPFMAXSGE = 0x20D8;
constexpr u32 kTVL2D = 0x2054, kTVL3D = 0x2060, kTVLMP = 0x206C;
constexpr u32 kGPOFBASE0 = 0x3024, kGPOFEND0 = 0x3028, kGPOFCUR0 = 0x302C;
constexpr u32 kGP_XMEM = 0x30000, kGP_PMEM = 0x3A000, kGPRST = 0x3FFFC, kEPRST = 0x5FFFC;
constexpr u32 kListEnd = 0xFFFF;

// --- voice record (0x80 bytes each) ---------------------------------------------------------
constexpr u32 kVoiceSize = 0x80;
constexpr u32 kV_VBIN = 0x00, kV_FMT = 0x04, kV_HRTF = 0x1C, kV_BA = 0x20, kV_LBO = 0x24;
constexpr u32 kV_CUR_VOLA = 0x28, kV_CUR_VOLB = 0x2C, kV_CUR_VOLC = 0x30;
constexpr u32 kV_STATE = 0x54, kV_CBO = 0x58, kV_EBO = 0x5C;
constexpr u32 kV_TAR_VOLA = 0x60, kV_TAR_VOLB = 0x64, kV_TAR_VOLC = 0x68, kV_PITCH_LINK = 0x7C;
constexpr u32 kState_Active = 1u << 21;
constexpr u32 kFmt_Loop = 1u << 25, kFmt_Stereo = 1u << 27, kFmt_S16 = 1u << 28, kFmt_ADPCM = 2u << 30;
constexpr u32 kFmt_SpbShift = 16;
constexpr u32 kVolMute = 0xFFF;

// --- sizes ---------------------------------------------------------------------------------
constexpr u32 kSgeEntries = 4096;          // 16 MiB of sample space (4 KiB pages)
constexpr u32 kFifoBytes = 128 * 1024;     // the GP's output ring: 128 APU frames of 16 bins (4096 samples)
constexpr u32 kScratchBytes = 16 * 1024;   // the GP's scratch: its bootstrap image (P:0..$7FF)
constexpr u32 kGpFrame = 32;               // samples per APU frame
constexpr u32 kMixbinX = 0x1400;           // bin n at X:$1400 + 32n

// The GP program (DSP56300 words): each frame, report it complete, wait for
// the next frame's start, acknowledge it, start the DMA chain at X:0 (all 32
// bins to output FIFO 0) and count the frame at X:$10.
//   P:0  movep #1,x:$FFFFC4     frame complete
//   P:2  jclr  #1,x:$FFFFC5,P:2 wait for START_FRAME
//   P:4  movep #2,x:$FFFFC5     acknowledge it
//   P:6  movep #0,x:$FFFFD4     DMA: next block = the descriptor at X:0
//   P:8  movep #1,x:$FFFFD6     DMA: start
//   P:A  inc   a
//   P:B  move  a0,x:$10         frame counter
//   P:C  jset  #4,x:$FFFFD6,P:C wait while the DMA runs
//   P:E  jmp   P:0
// The wait: the setup engine runs frames back to back when it is behind (64
// at start, a few after any pause); without it the next frame's DMA start cut
// the last transfer short and the output ring was left half a block out of
// step with the reader (bins 8-15 read as bins 0-7). Bit 4 of the DMA control
// word as "running" is xemu's reading of it; if a console never cleared it,
// no frame would complete and gw_apu_init would find no frames (the sound
// then stays in software).
const u32 kGpProgram[] = {0x08F484, 0x000001, 0x0A8581, 0x000002, 0x08F485, 0x000002, 0x08F494, 0x000000,
                          0x08F496, 0x000001, 0x000008, 0x501000, 0x0A96A4, 0x00000C, 0x0C0000};
constexpr u32 kFrameCounterX = 0x10;
// The DMA chain: two descriptors, at X:0 and X:7, each interleaved, to
// memory, FIFO 0, 16-bit, the channels 32 DSP words apart, 32 frames of 8
// bins: bins 0-7, then bins 8-15. (In xemu a single transfer of 16 channels
// came out garbled while 2 and 8 were right, and xemu checks a transfer's X
// range with its raw count word, so bins 16 and up cannot be reached.) Each
// APU frame puts 32 frames of bins 0-7, then 32 of bins 8-15, in the ring
// (16-bit, little-endian); the ring holds whole APU frames.
constexpr u32 kDescChannels = 8;
constexpr u32 kDescControl = 0x000001 | 0x000002 | (1u << 10) | (kGpFrame << 14);
const u32 kGpDescriptors[14] = {
    7,        kDescControl, (kGpFrame << 4) | (kDescChannels - 1), kMixbinX,                                0, 0, 0,
    0x004000, kDescControl, (kGpFrame << 4) | (kDescChannels - 1), kMixbinX + kDescChannels * kGpFrame, 0, 0, 0};
static_assert(kBinsCopied == 2 * kDescChannels, "two descriptors of kDescChannels bins");
constexpr u32 kGpBlockBytes = kBinsCopied * kGpFrame * 2;              // one APU frame in the ring
constexpr u32 kRingBytes = kFifoBytes / kGpBlockBytes * kGpBlockBytes;  // whole APU frames

bool g_ready;
u8* g_voices;      // kMaxVoices records (uncached)
u32* g_sge;        // the sample space's page table: entry n = 8 bytes, dword 0 the page
u8* g_fifo;        // the GP's output ring
u32 g_sge_next;    // the next free sample-space page
u32 g_fifo_read;   // bytes read from the ring (mod kFifoBytes)
bool g_listed[kMaxVoices];
u16 g_pending[kMaxVoices];  // started, not listed yet (gw_apu_voices_go)
u32 g_pending_n;
// A voice taken off the list may still be the one the VP walks to next (it
// reads a voice's link before mixing it), so its record (state, link) stays
// as it was until two APU frames have passed: the VP never meets an idle
// voice or a stale link (XEMU stops on an idle voice in a list).
bool g_settling[kMaxVoices];
u32 g_unlinked_at[kMaxVoices];  // the GP's frame count when it came off

// A voice's routes as last written (gw_apu_voice_mix): the record's words
// (the record is uncached; only what changed is written), and the format
// word's other bits as gw_apu_voice_start set them.
struct LastMix {
  bool valid;
  u32 fmt_rest, vbin, fmt_bins, a, b, c;
  i16 pitch;
};
LastMix g_last_mix[kMaxVoices];

inline void wr(u32 off, u32 v) { *(volatile u32*)(kBar0 + off) = v; }
inline u32 rd(u32 off) { return *(volatile u32*)(kBar0 + off); }
inline volatile u32* voice(u32 h, u32 off) { return (volatile u32*)(g_voices + h * kVoiceSize + off); }

void* alloc_table(u32 bytes, u32* phys, u32 protect = PAGE_READWRITE | PAGE_NOCACHE) {
  void* p = MmAllocateContiguousMemoryEx(bytes, 0, 0x03FFAFFF, 0x4000, protect);
  if (!p) return nullptr;
  memset(p, 0, bytes);
  *phys = MmGetPhysicalAddress(p);
  return p;
}

// 12-bit attenuation (0 = unity, 0xFFF = muted) for a linear gain.
// (-1280 log10 g = -385.3184 log2 g; the logarithm from the float's exponent
// and a table over its mantissa, interpolated: a library logarithm per gain
// was most of the cost of the routes, 8 gains a voice a pass.)
float g_log2[257];  // log2(1 + i / 256)
u32 atten(float gain) {
  if (!(gain > 6.3e-4f)) return kVolMute;
  if (gain >= 1.0f) return 0;
  if (g_log2[256] == 0)
    for (u32 i = 0; i <= 256; ++i) g_log2[i] = logf(1.0f + (float)i * (1.0f / 256)) * 1.44269504f;
  union {
    float f;
    u32 u;
  } bits = {gain};
  const int exponent = (int)((bits.u >> 23) & 0xFF) - 127;
  const u32 m = (bits.u >> 15) & 0xFF;
  const float frac = (float)(bits.u & 0x7FFF) * (1.0f / 32768);
  const float l2 = (float)exponent + g_log2[m] + (g_log2[m + 1] - g_log2[m]) * frac;
  const float a = -385.3184f * l2 + 0.5f;
  return a > (float)0xFFE ? 0xFFE : a < 0 ? 0 : (u32)a;
}

// Outputs 0 and 1 (front left, right) at their attenuations, the other six muted.
void volumes(float left, float right, u32* a, u32* b, u32* c) {
  const u32 l = atten(left), r = atten(right), m = kVolMute;
  // VOLA: out0 15:4, out1 31:20, out6 bits 3:0, out7 bits 19:16 (their low nibbles);
  // VOLB, VOLC: outs 2,3 and 4,5 with the next nibbles of outs 6 and 7.
  *a = (l << 4) | (r << 20) | (m & 0xF) | ((m & 0xF) << 16);
  *b = (m << 4) | (m << 20) | ((m >> 4) & 0xF) | (((m >> 4) & 0xF) << 16);
  *c = (m << 4) | (m << 20) | ((m >> 8) & 0xF) | (((m >> 8) & 0xF) << 16);
}

void link(u32 h) {
  const u32 head = rd(kTVL2D) & 0xFFFF;
  *(volatile u16*)(g_voices + h * kVoiceSize + kV_PITCH_LINK) = (u16)head;
  wr(kTVL2D, h);
  g_listed[h] = true;
}

u32 frame_count();

void unlink(u32 h) {
  if (!g_listed[h]) return;
  g_listed[h] = false;
  g_settling[h] = true;
  g_unlinked_at[h] = frame_count();
  const u32 next = *voice(h, kV_PITCH_LINK) & 0xFFFF;
  u32 cur = rd(kTVL2D) & 0xFFFF;
  if (cur == h) {
    wr(kTVL2D, next);
    return;
  }
  for (u32 guard = 0; cur != kListEnd && cur < kMaxVoices && guard < kMaxVoices; ++guard) {
    const u32 n = *voice(cur, kV_PITCH_LINK) & 0xFFFF;
    if (n == h) {
      *(volatile u16*)(g_voices + cur * kVoiceSize + kV_PITCH_LINK) = (u16)next;
      return;
    }
    cur = n;
  }
}

u32 frame_count();

// Off the list for two whole APU frames (or never listed).
bool settled(u32 h) {
  if (!g_settling[h]) return true;
  if (((frame_count() - g_unlinked_at[h]) & 0xFFFFFF) < 2) return false;
  g_settling[h] = false;
  return true;
}

// The APU reset as a cold boot leaves it: through its PCI power management
// (capability at config 0x44, PMCSR at 0x48), put in D3hot and back in D0,
// which resets the whole function, its configuration included; the command
// register, BAR0 and the interrupt line are put back as they were. A session
// killed mid-frame (a debug-monitor warm restart) leaves the setup engine
// waiting for a frame that never completes (its frame sample count 0x2008
// frozen, the GP's clock stopped) and nothing in BAR0 restarts it; a power
// cycle did, and so does this (found on the console with tools/xbdm_apu.py:
// frames at the full rate afterwards). Nothing else uses the APU meanwhile.
constexpr u32 kPciBus = 0, kPciSlot = 5;  // device 5, function 0
constexpr u32 kPciCommand = 0x04, kPciBar0 = 0x10, kPciIntLine = 0x3C, kPciCapPtr = 0x34, kPciPmcsr = 0x48;

u32 pci_read(u32 off) {
  ULONG v = 0;
  HalReadWritePCISpace(kPciBus, kPciSlot, off, &v, 4, FALSE);
  return v;
}
void pci_write(u32 off, u32 v) {
  ULONG w = v;
  HalReadWritePCISpace(kPciBus, kPciSlot, off, &w, 4, TRUE);
}

bool pci_reset_quiet();
bool pci_reset() {
  const bool ok = pci_reset_quiet();
  gw_log("apu: reset through power management (D3hot, D0): %s\n", ok ? "configuration restored" : "NOT DONE OR NOT RESTORED");
  return ok;
}
bool pci_reset_quiet() {
  // Only on the chip this was found on (nVidia MCPX APU, 10DE:01B0) with its
  // power management capability where it was.
  if (pci_read(0x00) != 0x01B010DE || (pci_read(kPciCapPtr) & 0xFF) != 0x44 || (pci_read(0x44) & 0xFF) != 0x01) {
    return false;
  }
  const u32 command = pci_read(kPciCommand) & 0xFFFF, bar0 = pci_read(kPciBar0), int_line = pci_read(kPciIntLine);
  pci_write(kPciPmcsr, 3);  // D3hot
  Sleep(10);
  pci_write(kPciPmcsr, 0);  // D0: the function reset
  Sleep(10);
  pci_write(kPciBar0, bar0);
  pci_write(kPciIntLine, int_line);
  pci_write(kPciCommand, command);
  return pci_read(kPciBar0) == bar0 && (pci_read(kPciCommand) & 0xFFFF) == command;
}

// The setup engine's settings as a console has them after a cold boot (read
// there with tools/xbdm_apu.py), which the reset clears: without them the VP
// mixes into bins that stay silent (every bin read back 0). Their meaning is
// not documented; 0x208C-0x20AC look like a byte per mix bin. Only plain
// values: the cold-boot registers holding memory addresses (0x2038, 0x203C,
// 0x2048, 0x204C, 0x115C) are left cleared, since that memory is not ours.
struct RegValue {
  u32 off, value;
};
const RegValue kSeColdBoot[] = {
    {0x2010, 0x800},      {0x2014, 0x400},      {0x2018, 0x7FF},      {0x2020, 0xE21600},   {0x2024, 0x100},
    {0x2028, 0x100},      {0x2078, 0xFFF},      {0x207C, 0xFFF},      {0x2080, 0xFFF},      {0x2084, 0xFFF},
    {0x2088, 0xFFF},      {0x208C, 0x01010101}, {0x2090, 0x01010101}, {0x2094, 0x01010101}, {0x2098, 0x01010101},
    {0x209C, 0x01010101}, {0x20A0, 0x01010101}, {0x20A4, 0x01010101}, {0x20A8, 0x00010101}, {0x20AC, 0x09070806},
};

}  // namespace

// A build that plays on the APU, first thing in main (gw_audio_boot_reset,
// the AC97's bus masters stopped first): whatever an earlier session left
// running on the APU (the GP's DMA writing its output ring, the VP reading
// voice tables, at physical addresses that now belong to this program, after
// a debug-monitor restart) is stopped by the reset. (The log is not up yet.)
void gw_apu_reset_at_boot() { pci_reset_quiet(); }

bool gw_apu_init() {
  if (g_ready) return true;
  // A fresh chip whatever the last session left (see pci_reset).
  if (!pci_reset() && pci_read(kPciBar0) != 0xFE800000) return false;
  for (const RegValue& r : kSeColdBoot) wr(r.off, r.value);
  u32 voices_phys, sge_phys, ssl_phys, scratch_phys, scratch_sge_phys, fifo_phys, fifo_sge_phys;
  g_voices = (u8*)alloc_table(kMaxVoices * kVoiceSize, &voices_phys);
  g_sge = (u32*)alloc_table(kSgeEntries * 8, &sge_phys);
  void* ssl = alloc_table(4096, &ssl_phys);
  u32* scratch = (u32*)alloc_table(kScratchBytes, &scratch_phys);
  u32* scratch_sge = (u32*)alloc_table(4096, &scratch_sge_phys);
  g_fifo = (u8*)alloc_table(kFifoBytes, &fifo_phys);
  // Read through the CPU's cache: the APU's writes to memory are seen by it
  // (nxdk-ex measured no stale data on a console); zeroed while uncached, so
  // no dirty line is ever written back over the GP's output.
  if (g_fifo) MmSetAddressProtect(g_fifo, kFifoBytes, PAGE_READWRITE);
  u32* fifo_sge = (u32*)alloc_table(4096, &fifo_sge_phys);
  if (!g_voices || !g_sge || !ssl || !scratch || !scratch_sge || !g_fifo || !fifo_sge) {
    gw_log("apu: no memory for its tables\n");
    return false;
  }
  // Every voice record's link at the list's end before the VP is given the
  // table: a zeroed record links to voice 0, itself. After a warm restart
  // from a session that used the APU, the VP could still be walking that
  // session's list when its table address changed, and it looped on voice 0
  // for good: no frame ever finished, the GP got no frames, the sound
  // starved (heard as a garbled buzz now and then).
  for (u32 h = 0; h < kMaxVoices; ++h) *(volatile u16*)(g_voices + h * kVoiceSize + kV_PITCH_LINK) = (u16)kListEnd;
  // Quiet: no frames, front end free running and untouched, both DSPs held
  // (and a moment for a frame in progress to finish).
  wr(kSECTL, 0);
  Sleep(2);
  wr(kFECTL, 0);
  wr(kGPRST, 0);
  wr(kEPRST, 0);
  wr(kIEN, 0);
  wr(kISTS, 0xFFFFFFFF);
  wr(kFETFORCE1, 0);
  // The VP's tables and empty lists.
  wr(kVPVADDR, voices_phys);
  wr(kVPSGEADDR, sge_phys);
  wr(kVPSSLADDR, ssl_phys);
  wr(kTVL2D, kListEnd);
  wr(kTVL3D, kListEnd);
  wr(kTVLMP, kListEnd);
  // The GP: its bootstrap loads P:0.. from scratch offset 0; its output
  // FIFO 0 is the ring, one page table entry per 4 KiB.
  for (u32 i = 0; i < sizeof(kGpProgram) / 4; ++i) scratch[i] = kGpProgram[i];
  for (u32 i = 0; i < kScratchBytes / 4096; ++i) scratch_sge[2 * i] = scratch_phys + i * 4096;
  for (u32 i = 0; i < kFifoBytes / 4096; ++i) fifo_sge[2 * i] = fifo_phys + i * 4096;
  wr(kGPSADDR, scratch_sge_phys);
  wr(kGPSMAXSGE, kScratchBytes / 4096 - 1);
  wr(kGPFADDR, fifo_sge_phys);
  wr(kGPFMAXSGE, kFifoBytes / 4096 - 1);
  wr(kGPOFBASE0, 0);
  wr(kGPOFEND0, kRingBytes);
  wr(kGPOFCUR0, 0);
  // Release the GP (its bootstrap copies the program), check the program
  // arrived, then the descriptor (X writes are ignored while it is held).
  bool loaded = false;
  for (u32 attempt = 0; attempt < 3 && !loaded; ++attempt) {
    wr(kGPRST, 0);
    wr(kGPRST, 3);
    Sleep(5);
    loaded = true;
    for (u32 i = 0; i < sizeof(kGpProgram) / 4; ++i)
      if ((rd(kGP_PMEM + 4 * i) & 0xFFFFFF) != kGpProgram[i]) loaded = false;
  }
  if (!loaded) gw_log("apu: the GP program did not load (P:0 = %06X)\n", rd(kGP_PMEM) & 0xFFFFFF);
  for (u32 i = 0; i < 14; ++i) wr(kGP_XMEM + 4 * i, kGpDescriptors[i]);
  wr(kGP_XMEM + 4 * kFrameCounterX, 0);
  // Frames on.
  wr(kSECTL, kSECTL_RUN);
  Sleep(20);
  u32 frames = gw_apu_frames();
  if (frames == 0) {
    const u32 count0 = rd(kXGSCNT);
    Sleep(2);
    gw_log("apu: no GP frames: SECTL %08X ISTS %08X FECTL %08X, frame count %08X, sample counter +%u in 2 ms\n",
           rd(kSECTL), rd(kISTS), rd(kFECTL), rd(0x2008), rd(kXGSCNT) - count0);
  }
  gw_log("apu: ready, GP program %s, %u frames in 20 ms (XGSCNT %u)\n", loaded ? "loaded" : "NOT loaded", frames,
         rd(kXGSCNT));
  g_ready = loaded && frames > 0;
  // The frames just started ran in a burst (the setup engine works ahead of
  // the sample clock by 0x2010's count, 2048 samples, and makes it up at
  // once). The GP's program waits for each DMA, so the ring should be in
  // step with the reader's blocks; checked, and put in step if not.
  if (g_ready && !gw_apu_settle()) gw_log("apu: its output ring was out of step after the start: put in step\n");
  // No frames even after the reset: not used, the sound in software.
  if (frames == 0) gw_log("apu: its GP runs no frames: not used, the sound is mixed in software\n");
  g_ready = loaded && frames > 0;
  return g_ready;
}

bool gw_apu_ready() { return g_ready; }

// The GP's output ring back in step with the blocks the reader takes: frames
// off, the DMA let finish, the ring's cursor (and the reader's) at its start,
// frames on. (Frames stopped and started this way do not run in a burst: the
// engine's lead is kept. Checked on the console.)
void gw_apu_realign() {
  if (!g_ready) return;  // (shut down meanwhile: nothing of the APU is touched)
  wr(kSECTL, 0);
  Sleep(2);
  wr(kGPOFCUR0, 0);
  g_fifo_read = 0;
  wr(kSECTL, kSECTL_RUN);
  Sleep(2);
}
bool gw_apu_in_step() { return !g_ready || rd(kGPOFCUR0) % kGpBlockBytes == 0; }

// In step, looked at over a few milliseconds (a single look may fall between
// two of the GP's transfers); put in step if not. True if it was.
bool gw_apu_settle() {
  if (!g_ready) return true;
  for (u32 t = 0; t < 6; ++t) {
    if (rd(kGPOFCUR0) % kGpBlockBytes == 0) return true;
    Sleep(1);
  }
  gw_apu_realign();
  return false;
}

// Left quiet for whatever runs next (the game's return to the dashboard): the
// voice lists emptied, a frame in progress let finish, frames off, the GP
// held. Killed mid-frame instead (a warm restart by the debug monitor), the
// next start found the GP running no frames (gw_apu_init falls back to
// software then).
void gw_apu_shutdown() {
  if (!g_ready) return;
  wr(kTVL2D, kListEnd);
  wr(kTVL3D, kListEnd);
  wr(kTVLMP, kListEnd);
  Sleep(2);
  wr(kSECTL, 0);
  Sleep(2);
  wr(kGPRST, 0);
  g_ready = false;
}

// For the return to the dashboard, after gw_apu_shutdown and with the AC97's
// bus masters stopped (gw_audio_stop): the APU reset, as a cold boot leaves
// it, for the dashboard's own sound start-up.
void gw_apu_reset_for_exit() { pci_reset(); }

u32 gw_apu_frames() { return g_voices ? rd(kGP_XMEM + 4 * kFrameCounterX) & 0xFFFFFF : 0; }
namespace {
u32 frame_count() { return gw_apu_frames(); }
}  // namespace

bool gw_apu_voice_free(u32 h) { return g_ready && h < kMaxVoices && !g_listed[h] && settled(h); }

u32 gw_apu_map(const void* data, u32 bytes) {
  const u32 phys = MmGetPhysicalAddress((void*)data);
  const u32 first = phys & ~0xFFFu, pages = ((phys & 0xFFF) + bytes + 0xFFF) / 4096;
  if (!g_ready || g_sge_next + pages > kSgeEntries) return 0xFFFFFFFF;
  const u32 base = g_sge_next;
  for (u32 i = 0; i < pages; ++i) {
    // Contiguous memory: the pages follow each other physically too.
    g_sge[2 * (base + i)] = MmGetPhysicalAddress((u8*)data - (phys & 0xFFF) + i * 4096);
    g_sge[2 * (base + i) + 1] = 0;
  }
  (void)first;
  g_sge_next += pages;
  return base * 4096 + (phys & 0xFFF);
}

bool gw_apu_voice_start(u32 h, const GwApuVoice& v) {
  if (!gw_apu_voice_free(h)) return false;  // (still listed, or the VP may still reach it)
  g_last_mix[h].valid = false;  // (its format word's other bits: below)
  for (u32 i = 0; i < g_pending_n; ++i)
    if (g_pending[i] == h) g_pending[i--] = g_pending[--g_pending_n];
  *voice(h, kV_STATE) = 0;
  for (u32 i = 0; i < kVoiceSize; i += 4) *voice(h, i) = 0;
  u32 a, b, c;
  volumes(v.left, v.right, &a, &b, &c);
  // Outputs 0..5 to bins 0..5 (only 0 and 1 are heard), 6 and 7 to bins 6, 7.
  *voice(h, kV_VBIN) = 0 | (1 << 5) | (2 << 10) | (3 << 16) | (4 << 21) | (5 << 26);
  // 16-bit samples: S16 in a 2-byte container (1 << 30); Xbox ADPCM: the ADPCM container.
  u32 fmt = kFmt_S16 | (v.bits == 4 ? kFmt_ADPCM : 1u << 30) | kFmt_Loop | 6 | (7 << 5);
  if (v.channels == 2) fmt |= kFmt_Stereo | (1u << kFmt_SpbShift);
  *voice(h, kV_FMT) = fmt;
  g_last_mix[h].fmt_rest = fmt & ~0x3FFu;
  *voice(h, kV_HRTF) = 0xFFFF;
  *voice(h, kV_BA) = v.ba;
  *voice(h, kV_LBO) = v.loop_start;
  *voice(h, kV_CBO) = v.start;
  *voice(h, kV_EBO) = v.loop_end - 1;
  *voice(h, kV_TAR_VOLA) = a;
  *voice(h, kV_TAR_VOLB) = b;
  *voice(h, kV_TAR_VOLC) = c;
  *voice(h, kV_CUR_VOLA) = a;  // (current volumes start at the targets: zero is unity)
  *voice(h, kV_CUR_VOLB) = b;
  *voice(h, kV_CUR_VOLC) = c;
  *voice(h, kV_PITCH_LINK) = ((u32)(u16)v.pitch << 16) | kListEnd;
  *voice(h, kV_STATE) = kState_Active;
  // Listed at the next gw_apu_voices_go, with the others started till then,
  // so they all begin in the same APU frame.
  if (g_pending_n < kMaxVoices) g_pending[g_pending_n++] = (u16)h;
  return true;
}

void gw_apu_voice_pause(u32 h, bool paused) {
  if (!g_ready || h >= kMaxVoices) return;
  // Off the list, its record kept: the VP leaves it (and its play position)
  // alone; back on at the next gw_apu_voices_go.
  if (paused) {
    for (u32 i = 0; i < g_pending_n; ++i)
      if (g_pending[i] == h) g_pending[i--] = g_pending[--g_pending_n];
    unlink(h);
  } else if (!g_listed[h] && g_pending_n < kMaxVoices) {
    g_pending[g_pending_n++] = (u16)h;
  }
}

void gw_apu_voices_go() {
  if (!g_ready || !g_pending_n) return;
  // Chained among themselves in front of the list, then the head written
  // once: the VP takes them all in one frame.
  // (A voice the VP may still reach, paused and resumed within two frames,
  // waits for the next call: its link is not rewritten under the VP.)
  u32 head = rd(kTVL2D) & 0xFFFF, kept = 0;
  const u32 first = head;
  for (u32 i = g_pending_n; i-- > 0;) {
    const u32 h = g_pending[i];
    if (!settled(h)) {
      g_pending[kept++] = (u16)h;
      continue;
    }
    *(volatile u16*)(g_voices + h * kVoiceSize + kV_PITCH_LINK) = (u16)head;
    g_listed[h] = true;
    head = h;
  }
  if (head != first) wr(kTVL2D, head);
  g_pending_n = kept;
}

void gw_apu_voice_mix(u32 h, i16 pitch, u32 n, const u8* bins, const float* gains) {
  if (!g_ready || h >= kMaxVoices) return;
  // Outputs 0..n-1 to the given bins at their gains, the rest muted (on the
  // spare bin 15). VBIN holds outputs 0-5 (5 bits each, 4:0 9:5 14:10 20:16
  // 25:21 30:26), FMT bits 9:0 outputs 6 and 7; the volumes are 12-bit
  // attenuations packed as gw_apu_voice_start's.
  u8 b[8];
  u32 v[8];
  for (u32 k = 0; k < 8; ++k) {
    b[k] = k < n ? bins[k] & 31 : 15;
    v[k] = k < n ? atten(gains[k]) : kVolMute;
  }
  const u32 vbin = b[0] | (b[1] << 5) | (b[2] << 10) | (b[3] << 16) | (b[4] << 21) | ((u32)b[5] << 26);
  const u32 fmt_bins = b[6] | (b[7] << 5);
  const u32 va = (v[0] << 4) | (v[1] << 20) | (v[6] & 0xF) | ((v[7] & 0xF) << 16);
  const u32 vb = (v[2] << 4) | (v[3] << 20) | ((v[6] >> 4) & 0xF) | (((v[7] >> 4) & 0xF) << 16);
  const u32 vc = (v[4] << 4) | (v[5] << 20) | ((v[6] >> 8) & 0xF) | (((v[7] >> 8) & 0xF) << 16);
  // Only the words that changed since the last pass.
  LastMix& last = g_last_mix[h];
  const bool all = !last.valid;
  if (all || vbin != last.vbin) *voice(h, kV_VBIN) = vbin;
  if (all || fmt_bins != last.fmt_bins) *voice(h, kV_FMT) = last.fmt_rest | fmt_bins;
  if (all || va != last.a) *voice(h, kV_TAR_VOLA) = va;
  if (all || vb != last.b) *voice(h, kV_TAR_VOLB) = vb;
  if (all || vc != last.c) *voice(h, kV_TAR_VOLC) = vc;
  if (all || pitch != last.pitch) *(volatile u16*)(g_voices + h * kVoiceSize + kV_PITCH_LINK + 2) = (u16)pitch;
  last.valid = true;
  last.vbin = vbin;
  last.fmt_bins = fmt_bins;
  last.a = va;
  last.b = vb;
  last.c = vc;
  last.pitch = pitch;
}

void gw_apu_voice_set(u32 h, i16 pitch, float left, float right) {
  if (!g_ready || h >= kMaxVoices) return;
  g_last_mix[h].valid = false;
  u32 a, b, c;
  volumes(left, right, &a, &b, &c);
  *voice(h, kV_TAR_VOLA) = a;
  *voice(h, kV_TAR_VOLB) = b;
  *voice(h, kV_TAR_VOLC) = c;
  *(volatile u16*)(g_voices + h * kVoiceSize + kV_PITCH_LINK + 2) = (u16)pitch;
}

void gw_apu_voice_loop(u32 h, u32 loop_start, u32 loop_end) {
  if (!g_ready || h >= kMaxVoices) return;
  // EBO first when it grows, LBO first when it shrinks: the play position
  // always stays before EBO.
  *voice(h, kV_EBO) = loop_end - 1;
  *voice(h, kV_LBO) = loop_start;
}

void gw_apu_voice_stop(u32 h) {
  if (!g_ready || h >= kMaxVoices) return;
  for (u32 i = 0; i < g_pending_n; ++i)
    if (g_pending[i] == h) g_pending[i--] = g_pending[--g_pending_n];
  unlink(h);  // (its record left alone: see g_settling)
}

u32 gw_apu_voice_position(u32 h) { return g_ready && h < kMaxVoices ? *voice(h, kV_CBO) & 0xFFFFFF : 0; }

i16 gw_apu_pitch(float ratio) {
  if (!(ratio > 0)) return -32768;
  float p = 4096.0f * logf(ratio) * 1.44269504f;
  if (p > 32767) return 32767;
  if (p < -32768) return -32768;
  return (i16)(p < 0 ? p - 0.5f : p + 0.5f);
}

u32 gw_apu_out_cursor() { return g_ready ? rd(kGPOFCUR0) : 0; }

u32 gw_apu_available() {
  if (!g_ready) return 0;
  const u32 cur = rd(kGPOFCUR0) % kRingBytes / kGpBlockBytes * kGpBlockBytes;  // whole APU frames
  return (cur - g_fifo_read + kRingBytes) % kRingBytes / kGpBlockBytes * kGpFrame;
}

void gw_apu_skip(u32 frames) {
  g_fifo_read = (g_fifo_read + frames / kGpFrame * kGpBlockBytes) % kRingBytes;
}

void gw_apu_read_bins(float (*bins)[256], const u8* wanted, float scale) {
  // 256 frames = 8 APU frames, each 32 frames of bins 0-7, then of bins 8-15.
  // Each half wanted is turned into floats in one run over its samples
  // (they follow each other in memory), then its wanted bins are picked out
  // of that.
  float half[kGpFrame * kDescChannels];
  for (u32 f = 0; f < 256 / kGpFrame; ++f) {
    const i16* block = (const i16*)(g_fifo + g_fifo_read);
    for (u32 d = 0; d < kBinsCopied / kDescChannels; ++d) {
      bool any = false;
      for (u32 c = 0; c < kDescChannels; ++c) any |= wanted[d * kDescChannels + c] != 0;
      if (!any) continue;
      const i16* p = block + d * kGpFrame * kDescChannels;
      // (The APU wrote this memory: none of it is in the cache. Each line
      // is asked for a few lines before it is read; past the block's end
      // that is the next block, or a harmless miss at the ring's end.)
      for (u32 i = 0; i < kGpFrame * kDescChannels; i += 16) {
        __builtin_prefetch((const char*)(p + i) + 160, 0, 0);
        for (u32 n = i; n < i + 16; ++n) half[n] = (float)p[n] * scale;
      }
      for (u32 c = 0; c < kDescChannels; ++c) {
        if (!wanted[d * kDescChannels + c]) continue;
        float* out = bins[d * kDescChannels + c] + f * kGpFrame;
        for (u32 n = 0; n < kGpFrame; ++n) out[n] = half[n * kDescChannels + c];
      }
    }
    g_fifo_read = (g_fifo_read + kGpBlockBytes) % kRingBytes;
  }
}

u32 gw_apu_read(i16* out, u32 frames) {
  // Bins 0 and 1, whole APU frames.
  u32 got = 0;
  while (got + kGpFrame <= frames && gw_apu_available() >= kGpFrame) {
    const i16* p = (const i16*)(g_fifo + g_fifo_read);
    for (u32 n = 0; n < kGpFrame; ++n) {
      out[(got + n) * 2] = p[n * kDescChannels];
      out[(got + n) * 2 + 1] = p[n * kDescChannels + 1];
    }
    g_fifo_read = (g_fifo_read + kGpBlockBytes) % kRingBytes;
    got += kGpFrame;
  }
  return got;
}

// --- the probe (test only: make APU_PROBE=y) ------------------------------------
// The PC (tools/xbdm_apu.py) fills gw_apu_probe through the debug monitor, the
// sequence number last; the sound thread carries the command out on its next
// pass and stores the sequence number in done. Register offsets are checked
// against BAR0's size (512 KiB), counts against the result buffer; the results
// go only into gw_apu_probe_out (ours).
#ifdef GW_APU_PROBE
extern "C" {
// op, offset, value, count, seq, done
volatile u32 gw_apu_probe[6];
volatile u32 gw_apu_probe_out[256];
}
void gw_apu_probe_poll() {
  const u32 seq = gw_apu_probe[4];
  if (seq == gw_apu_probe[5]) return;
  const u32 op = gw_apu_probe[0], off = gw_apu_probe[1], value = gw_apu_probe[2];
  u32 count = gw_apu_probe[3];
  constexpr u32 kBar0Bytes = 0x80000, kOut = sizeof(gw_apu_probe_out) / 4;
  if (count > kOut) count = kOut;
  const bool in_range = (off & 3) == 0 && off < kBar0Bytes && count <= (kBar0Bytes - off) / 4;
  switch (op) {
    case 1:  // read count words from off
      if (in_range)
        for (u32 i = 0; i < count; ++i) gw_apu_probe_out[i] = rd(off + 4 * i);
      break;
    case 2:  // write value at off, read it back
      if (in_range) {
        wr(off, value);
        gw_apu_probe_out[0] = rd(off);
      }
      break;
    case 3:  // sleep value ms (at most 200)
      Sleep(value > 200 ? 200 : value);
      break;
    case 4:  // the GP frame count, then after value ms (at most 200) again, and the sample counter both times
      gw_apu_probe_out[0] = rd(kGP_XMEM + 4 * kFrameCounterX) & 0xFFFFFF;
      gw_apu_probe_out[1] = rd(kXGSCNT);
      Sleep(value > 200 ? 200 : value);
      gw_apu_probe_out[2] = rd(kGP_XMEM + 4 * kFrameCounterX) & 0xFFFFFF;
      gw_apu_probe_out[3] = rd(kXGSCNT);
      break;
    case 6:  // the APU's PCI configuration (bus 0, device 5, function 0): 256 bytes
      HalReadWritePCISpace(0, value ? value : 5, 0, (PVOID)gw_apu_probe_out, 256, FALSE);  // (value: another slot)
      break;
    case 7:  // a dword of it written (off < 256), read back
      if ((off & 3) == 0 && off < 256) {
        ULONG v = value;
        const u32 slot = count > 1 && count < 32 ? count : 5;  // (count: another slot)
        HalReadWritePCISpace(0, slot, off, &v, 4, TRUE);
        HalReadWritePCISpace(0, slot, off, &v, 4, FALSE);
        gw_apu_probe_out[0] = v;
      }
      break;
    case 5:  // the whole bring-up again (its tables allocated afresh)
      g_ready = false;
      g_sge_next = 0;
      gw_apu_probe_out[0] = gw_apu_init();
      gw_apu_probe_out[1] = gw_apu_frames();
      break;
  }
  gw_log("apu probe %u: op %u off %05X value %08X count %u -> %08X\n", seq, op, off, value, count, gw_apu_probe_out[0]);
  gw_apu_probe[5] = seq;
}
#else
void gw_apu_probe_poll() {}
#endif
