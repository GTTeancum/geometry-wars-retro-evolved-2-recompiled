// The game's per-frame job barrier, run on the calling thread.
//
// The game starts six job threads (start routine 0x82063C08, one context
// each: +4 the job index, +8 the job object) for the 360's six hardware
// threads. Each frame the main thread's sub_82063F08 waits for all six at a
// barrier, raises the "go" flag (object +672), and waits for all six again;
// each job thread meanwhile runs its share of the particles (sub_82063A38),
// the grid job (sub_8205CA88) and, for job 0, the starfield and two more
// updates, then waits for the next frame.
//
// On the Xbox's one core the six threads only take turns, and every round of
// the barrier is a dozen thread switches. Here sub_82063F08 runs the six
// shares itself, one after the other (an order the original allows: the
// shares split their work by index or by shared counters), and the job
// threads, never released, sleep at the barrier from their first frame on.
#include "runtime.h"

#include <windows.h>

extern "C" {
void __imp__sub_82063F08(PPCContext& ctx, uint8_t* base);
void sub_820A5420(PPCContext& ctx, uint8_t* base);
void sub_820631E8(PPCContext& ctx, uint8_t* base);
void sub_820632F0(PPCContext& ctx, uint8_t* base);
void sub_82063A38(PPCContext& ctx, uint8_t* base);  // a job's particle share
void sub_8205CA88(PPCContext& ctx, uint8_t* base);  // the grid job
void sub_82074F48(PPCContext& ctx, uint8_t* base);
void sub_8205A7F0(PPCContext& ctx, uint8_t* base);
void sub_82075548(PPCContext& ctx, uint8_t* base);  // the starfield
void sub_82064110(PPCContext& ctx, uint8_t* base);
void sub_82062F28(PPCContext& ctx, uint8_t* base);
}

namespace {
constexpr u32 kJobStart = 0x82063C08;
constexpr int kJobs = 6;
u32 g_job_context[kJobs];
volatile long g_job_count = 0;
}  // namespace

// Called for every guest thread created (src/kernel_threads.cpp).
void gw_note_thread(u32 start_address, u32 start_context) {
  if (start_address != kJobStart) return;
  const long n = InterlockedIncrement(&g_job_count) - 1;
  if (n < kJobs) g_job_context[n] = start_context;
}

// r3 the job object.
extern "C" void sub_82063F08(PPCContext& ctx, uint8_t* base) {
  if (g_job_count != kJobs) {
    __imp__sub_82063F08(ctx, base);
    return;
  }
  const u32 object = ctx.r3.u32;
  ctx.r1.u64 = ctx.r1.u32 - 144;  // the original's frame, below which the calls put theirs

  // Before the barrier, as the original.
  ctx.r3.u64 = gw_load32(0x821A0000 - 11632);
  sub_820A5420(ctx, base);
  ctx.r3.u64 = object;
  sub_820631E8(ctx, base);
  ctx.r3.u64 = object + 40;
  sub_820632F0(ctx, base);
  gw_store32(object + 496, 0);

  // The six job threads' frame, one after the other.
  for (int i = 0; i < kJobs; ++i) {
    const u32 job = g_job_context[i];
    ctx.r3.u64 = gw_load32(job + 8);
    ctx.r4.u64 = job;
    sub_82063A38(ctx, base);
    sub_8205CA88(ctx, base);
    if (gw_load32(job + 4) == 0) {
      sub_82074F48(ctx, base);
      sub_8205A7F0(ctx, base);
      sub_82075548(ctx, base);
    }
  }

  // After the barrier, as the original.
  ctx.r3.u64 = object + 8;
  ctx.r4.u64 = gw_load32(object);
  ctx.r5.u64 = 0;
  sub_82064110(ctx, base);
  ctx.r3.u64 = object;
  sub_82062F28(ctx, base);
  gw_store32(object + 12, ctx.r3.u32);
  ctx.r1.u64 = ctx.r1.u32 + 144;
}
