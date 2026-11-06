// Guest threads, dispatcher objects, handles, TLS and time.
//
// Each guest thread is a host thread with its own PPCContext and a guest-side
// KTHREAD, PCR (r13), TLS block and stack. Kernel dispatcher objects (events,
// semaphores, mutants, threads) are host objects; each also has a guest
// dispatcher header so that handles and guest-embedded objects (KEVENTs the
// game initialises itself) resolve to the same host object.
#include "runtime.h"

#include <new>

#include <windows.h>

#ifndef WAIT_ABANDONED_0
#define WAIT_ABANDONED_0 0x80
#endif

// --- objects and handles --------------------------------------------------------

namespace {

enum class ObjType : u8 { Event, Semaphore, Mutant, Thread, File, Other };

struct GwObject {
  ObjType type;
  u32 guest;      // guest dispatcher header (or 0)
  HANDLE host;    // event/semaphore/mutex/thread handle
  void* extra;    // GwThread* or file state
  u32 refs;
};

constexpr int kMaxHandles = 1024;
constexpr u32 kHandleBase = 0xF8000000;
GwObject* g_handles[kMaxHandles];
CRITICAL_SECTION g_object_lock;

// guest address -> object (open addressing)
constexpr int kMapSize = 4096;
struct MapEntry {
  u32 guest;
  GwObject* obj;
};
MapEntry g_map[kMapSize];

GwObject* map_find(u32 guest) {
  u32 h = (guest >> 3) * 2654435761u;
  for (int i = 0; i < kMapSize; ++i) {
    MapEntry& e = g_map[(h + i) & (kMapSize - 1)];
    if (e.guest == guest) return e.obj;
    if (!e.guest) return nullptr;
  }
  return nullptr;
}

void map_insert(u32 guest, GwObject* obj) {
  u32 h = (guest >> 3) * 2654435761u;
  for (int i = 0; i < kMapSize; ++i) {
    MapEntry& e = g_map[(h + i) & (kMapSize - 1)];
    if (!e.guest || e.guest == guest) {
      e.guest = guest;
      e.obj = obj;
      return;
    }
  }
  gw_fatal("object map full");
}

u32 handle_add(GwObject* obj) {
  EnterCriticalSection(&g_object_lock);
  for (int i = 1; i < kMaxHandles; ++i) {
    if (!g_handles[i]) {
      g_handles[i] = obj;
      ++obj->refs;
      LeaveCriticalSection(&g_object_lock);
      return kHandleBase + i * 4;
    }
  }
  LeaveCriticalSection(&g_object_lock);
  gw_fatal("handle table full");
}

// Dispatcher header types
constexpr u8 kTypeNotificationEvent = 0;
constexpr u8 kTypeSynchronizationEvent = 1;
constexpr u8 kTypeMutant = 2;
constexpr u8 kTypeSemaphore = 5;
constexpr u8 kTypeThread = 6;

void write_header(u32 guest, u8 type, u32 signal_state) {
  gw_store8(guest + 0, type);
  gw_store8(guest + 1, 0);
  gw_store8(guest + 2, 0x10 / 4);
  gw_store8(guest + 3, 0);
  gw_store32(guest + 4, signal_state);
  gw_store32(guest + 8, guest + 8);  // empty wait list
  gw_store32(guest + 12, guest + 8);
}

GwObject* new_object(ObjType type, HANDLE host, u32 guest) {
  GwObject* obj = new GwObject{type, guest, host, nullptr, 0};
  if (guest) {
    EnterCriticalSection(&g_object_lock);
    map_insert(guest, obj);
    LeaveCriticalSection(&g_object_lock);
  }
  return obj;
}

GwObject* make_event(bool manual, bool initial, u32 guest) {
  if (!guest) {
    guest = gw_system_alloc(0x10);
    write_header(guest, manual ? kTypeNotificationEvent : kTypeSynchronizationEvent, initial);
  }
  return new_object(ObjType::Event, CreateEventA(nullptr, manual, initial, nullptr), guest);
}

GwObject* make_semaphore(i32 count, i32 limit, u32 guest) {
  if (!guest) {
    guest = gw_system_alloc(0x14);
    write_header(guest, kTypeSemaphore, count);
    gw_store32(guest + 0x10, limit);
  }
  return new_object(ObjType::Semaphore, CreateSemaphore(nullptr, count, limit, nullptr), guest);
}

GwObject* make_mutant(bool owned, u32 guest) {
  if (!guest) {
    guest = gw_system_alloc(0x20);
    write_header(guest, kTypeMutant, owned ? 0 : 1);
  }
  return new_object(ObjType::Mutant, CreateMutexA(nullptr, owned, nullptr), guest);
}

}  // namespace

GwObject* gw_object_from_handle(u32 handle);
GwObject* gw_object_from_guest(u32 guest);

// --- threads ----------------------------------------------------------------------

struct GwThread {
  PPCContext* ctx;
  u32 handle;
  u32 thread_id;
  u32 kthread;
  u32 pcr;
  u32 tls;
  u32 stack_low;
  u32 stack_high;
  u32 start_address;
  u32 start_context;
  u32 xapi_startup;
  u8 irql;
  HANDLE host;
  GwObject* object;
  const char* last_call;  // most recent system call (gw_note_call)
  u32 last_r3;
  u32 last_lr;
  u32 calls;
};

namespace {

DWORD g_tls_index;
volatile LONG g_next_thread_id = 1;
constexpr u32 kTlsSlots = 64;
constexpr u32 kTlsBytes = 0x400;
u32 g_tls_used;  // bitmap of allocated KeTlsAlloc slots
u32 g_kernel_vars;

GwThread* g_threads[64];
volatile LONG g_thread_count;

GwThread* setup_thread(u32 stack_size) {
  GwThread* t = new GwThread{};
  LONG slot = InterlockedIncrement(&g_thread_count) - 1;
  if (slot < 64) g_threads[slot] = t;
  u8* raw = (u8*)malloc(sizeof(PPCContext) + 64);
  if (!raw) gw_fatal("out of host memory for a thread context");
  void* ctx_mem = (void*)(((uintptr_t)raw + 63) & ~(uintptr_t)63);
  t->ctx = new (ctx_mem) PPCContext();
  t->thread_id = (u32)InterlockedIncrement(&g_next_thread_id);

  stack_size = (stack_size ? stack_size : kDefaultStackSize) + 0xFFFF;
  stack_size &= ~0xFFFFu;
  t->stack_low = gw_stack_alloc(stack_size + 0x10000);
  if (!t->stack_low) gw_fatal("out of guest stack space");
  t->stack_high = t->stack_low + stack_size;

  t->kthread = gw_system_alloc(0xAB0, 16);
  t->pcr = gw_system_alloc(0x2D8, 16);
  t->tls = gw_system_alloc(kTlsBytes, 16);

  write_header(t->kthread, kTypeThread, 0);
  gw_store32(t->kthread + 0x5C, t->stack_high);
  gw_store32(t->kthread + 0x60, t->stack_low);
  gw_store32(t->kthread + 0x68, t->tls);
  gw_store8(t->kthread + 0x72, 1);
  gw_store8(t->kthread + 0x73, 1);  // process type: title
  gw_store32(t->kthread + 0x14C, t->thread_id);

  gw_store32(t->pcr + 0x00, t->tls);
  gw_store64(t->pcr + 0x30, t->pcr);
  gw_store32(t->pcr + 0x70, t->stack_high);
  gw_store32(t->pcr + 0x74, t->stack_low);
  gw_store32(t->pcr + 0x100, t->kthread);  // prcb.current_thread
  gw_store8(t->pcr + 0x10C, 0);             // prcb.current_cpu
  gw_store32(t->pcr + 0x2A8, t->pcr + 0x100);

  PPCContext& ctx = *t->ctx;
  ctx.r1.u64 = t->stack_high;
  ctx.r13.u64 = t->pcr;
  ctx.fpscr.InitHost();

  t->object = new_object(ObjType::Thread, nullptr, t->kthread);
  t->object->extra = t;
  ++gw_stats.threads;
  return t;
}

void run_guest(GwThread* t) {
  TlsSetValue(g_tls_index, t);
  PPCContext& ctx = *t->ctx;
  u32 address = t->start_address;
  if (t->xapi_startup) {
    address = t->xapi_startup;
    ctx.r3.u64 = t->start_address;
    ctx.r4.u64 = t->start_context;
  } else {
    ctx.r3.u64 = t->start_context;
  }
  PPCFunc* func = gw_lookup(address);
  if (!func) gw_fatal("thread start %08X is not a known function", address);
  gw_log("thread %u starts at %08X\n", t->thread_id, address);
  __try {
    func(ctx, GW_MEMBASE);
  } __except (gw_fault_filter(GetExceptionInformation())) {
    gw_fatal("guest thread %u crashed (see log)", t->thread_id);
  }
  gw_log("thread %u returned %08X\n", t->thread_id, ctx.r3.u32);
}

DWORD WINAPI thread_proc(LPVOID param) {
  run_guest((GwThread*)param);
  return 0;
}

}  // namespace

// Commit-on-touch for large guest allocations; anything else is logged and
// handled as a crash of that thread.
int gw_fault_filter(void* p) {
  auto* info = (EXCEPTION_POINTERS*)p;
  EXCEPTION_RECORD* rec = info->ExceptionRecord;
  if (rec->ExceptionCode == 0xC0000005 && rec->NumberParameters >= 2 &&
      gw_handle_fault((u32)rec->ExceptionInformation[1])) {
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  u32 address = rec->NumberParameters >= 2 ? (u32)rec->ExceptionInformation[1] : 0;
  gw_log("exception %08X at host %08X, address %08X (guest %08X)\n", (u32)rec->ExceptionCode,
         (u32)rec->ExceptionAddress, address, address - (u32)GW_MEMBASE);
  return EXCEPTION_EXECUTE_HANDLER;
}

void gw_note_call(const char* name, PPCContext& ctx) {
  GwThread* t = (GwThread*)TlsGetValue(g_tls_index);
  if (!t) return;
  t->last_call = name;
  t->last_r3 = ctx.r3.u32;
  t->last_lr = (u32)ctx.lr;
  ++t->calls;
}

u32 gw_current_caller() {
  GwThread* t = (GwThread*)TlsGetValue(g_tls_index);
  return t ? t->last_lr : 0;
}

void gw_dump_threads() {
  LONG count = g_thread_count < 64 ? g_thread_count : 64;
  for (LONG i = 0; i < count; ++i) {
    GwThread* t = g_threads[i];
    if (!t) continue;
    gw_log("  thread %u start %08X: %u calls, last %s(r3 %08X) from %08X\n", t->thread_id,
           t->start_address, t->calls, t->last_call ? t->last_call : "-", t->last_r3, t->last_lr);
  }
}

GwThread* gw_current_thread() { return (GwThread*)TlsGetValue(g_tls_index); }
PPCContext& gw_current_context() { return *gw_current_thread()->ctx; }

u32 gw_thread_create(u32 stack_size, u32 xapi_startup, u32 start_address, u32 start_context,
                     u32 creation_flags, u32* out_thread_id) {
  GwThread* t = setup_thread(stack_size);
  t->start_address = start_address;
  t->start_context = start_context;
  t->xapi_startup = xapi_startup;
  bool suspended = creation_flags & 1;
  // Host stack: recompiled code keeps its frames on the host stack.
  t->host = CreateThread(nullptr, 256 * 1024, thread_proc, t, CREATE_SUSPENDED, nullptr);
  if (!t->host) gw_fatal("CreateThread failed");
  t->object->host = t->host;
  t->handle = handle_add(t->object);
  if (out_thread_id) *out_thread_id = t->thread_id;
  gw_log("ExCreateThread %08X(%08X) startup %08X flags %X -> handle %08X\n", start_address,
         start_context, xapi_startup, creation_flags, t->handle);
  if (!suspended) ResumeThread(t->host);
  return t->handle;
}

void gw_run_main_thread() {
  GwThread* t = setup_thread(kDefaultStackSize);
  t->start_address = kEntryPoint;
  t->host = CreateThread(nullptr, 512 * 1024, thread_proc, t, CREATE_SUSPENDED, nullptr);
  t->object->host = t->host;
  t->handle = handle_add(t->object);
  ResumeThread(t->host);
}

PPCContext* gw_make_callback_context() {
  GwThread* t = setup_thread(0x10000);
  t->host = GetCurrentThread();
  TlsSetValue(g_tls_index, t);
  return t->ctx;
}

void gw_call_guest(PPCContext& ctx, u32 address, u32 r3, u32 r4) {
  PPCFunc* func = gw_lookup(address);
  if (!func) gw_fatal("callback %08X is not a known function", address);
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  func(ctx, GW_MEMBASE);
}

GwObject* gw_object_from_handle(u32 handle) {
  if (handle == 0xFFFFFFFE) return gw_current_thread()->object;  // current thread
  u32 index = (handle - kHandleBase) / 4;
  if (index == 0 || index >= kMaxHandles) return nullptr;
  return g_handles[index];
}

GwObject* gw_object_from_guest(u32 guest) {
  EnterCriticalSection(&g_object_lock);
  GwObject* obj = map_find(guest);
  LeaveCriticalSection(&g_object_lock);
  if (obj) return obj;
  // A dispatcher object the game initialised itself.
  u8 type = gw_load8(guest);
  u32 state = gw_load32(guest + 4);
  switch (type) {
    case kTypeNotificationEvent:
    case kTypeSynchronizationEvent:
      return make_event(type == kTypeNotificationEvent, state != 0, guest);
    case kTypeSemaphore:
      return make_semaphore((i32)state, (i32)gw_load32(guest + 0x10), guest);
    case kTypeMutant:
      return make_mutant(false, guest);
    default:
      gw_log("wait on unknown dispatcher type %u at %08X\n", type, guest);
      return make_event(true, true, guest);
  }
}

u32 gw_handle_for_file(void* file) {
  GwObject* obj = new_object(ObjType::File, nullptr, 0);
  obj->extra = file;
  return handle_add(obj);
}

void* gw_file_from_handle(u32 handle) {
  GwObject* obj = gw_object_from_handle(handle);
  return obj && obj->type == ObjType::File ? obj->extra : nullptr;
}

// --- waits ------------------------------------------------------------------------

namespace {

// Guest timeouts are 100 ns units: negative = relative, positive = absolute.
DWORD timeout_ms(u32 timeout_ptr) {
  if (!timeout_ptr) return INFINITE;
  i64 t = (i64)gw_load64(timeout_ptr);
  if (t == 0) return 0;
  if (t > 0) return 0;  // absolute: treat as already reached
  i64 ms = (-t + 9999) / 10000;
  return ms > 0x7FFFFFFF ? INFINITE : (DWORD)ms;
}

u32 wait_result(DWORD r) {
  if (r == WAIT_TIMEOUT) return X_STATUS_TIMEOUT;
  if (r >= WAIT_ABANDONED_0 && r < WAIT_ABANDONED_0 + 64) return X_STATUS_ABANDONED_WAIT_0 + (r - WAIT_ABANDONED_0);
  if (r < 64) return r;
  return X_STATUS_UNSUCCESSFUL;
}

void sync_guest_after_wait(GwObject* obj) {
  if (obj->guest && obj->type == ObjType::Event && gw_load8(obj->guest) == kTypeSynchronizationEvent) {
    gw_store32(obj->guest + 4, 0);
  }
}

u32 wait_one(GwObject* obj, DWORD ms) {
  if (!obj || !obj->host) return X_STATUS_INVALID_HANDLE;
  u32 r = wait_result(WaitForSingleObject(obj->host, ms));
  if (r == 0) sync_guest_after_wait(obj);
  return r;
}

}  // namespace

GW_EXPORT(KeWaitForSingleObject) {
  GW_RETURN(wait_one(gw_object_from_guest(GW_ARG(3)), timeout_ms(GW_ARG(7))));
}

GW_EXPORT(NtWaitForSingleObjectEx) {
  GW_RETURN(wait_one(gw_object_from_handle(GW_ARG(3)), timeout_ms(GW_ARG(6))));
}

GW_EXPORT(KeWaitForMultipleObjects) {
  u32 count = GW_ARG(3), objects = GW_ARG(4), wait_all = GW_ARG(5) == 0;
  HANDLE hosts[64];
  if (count > 64) GW_RETURN(X_STATUS_INVALID_PARAMETER);
  for (u32 i = 0; i < count; ++i) {
    GwObject* obj = gw_object_from_guest(gw_load32(objects + i * 4));
    hosts[i] = obj ? obj->host : nullptr;
  }
  GW_RETURN(wait_result(WaitForMultipleObjects(count, hosts, wait_all, timeout_ms(GW_ARG(9)))));
}

GW_EXPORT(NtSignalAndWaitForSingleObjectEx) {
  GwObject* signal = gw_object_from_handle(GW_ARG(3));
  GwObject* wait = gw_object_from_handle(GW_ARG(4));
  if (signal) {
    if (signal->type == ObjType::Event) SetEvent(signal->host);
    else if (signal->type == ObjType::Semaphore) ReleaseSemaphore(signal->host, 1, nullptr);
    else if (signal->type == ObjType::Mutant) ReleaseMutex(signal->host);
  }
  GW_RETURN(wait_one(wait, timeout_ms(GW_ARG(7))));
}

GW_EXPORT(KeDelayExecutionThread) {
  DWORD ms = timeout_ms(GW_ARG(5));
  Sleep(ms == INFINITE ? 0xFFFFFFFE : ms);
  GW_RETURN(X_STATUS_SUCCESS);
}

// --- events, semaphores, mutants -------------------------------------------------------

GW_EXPORT(KeSetEvent) {
  GwObject* obj = gw_object_from_guest(GW_ARG(3));
  u32 previous = gw_load32(GW_ARG(3) + 4);
  gw_store32(GW_ARG(3) + 4, 1);
  SetEvent(obj->host);
  GW_RETURN(previous);
}

GW_EXPORT(KeResetEvent) {
  GwObject* obj = gw_object_from_guest(GW_ARG(3));
  u32 previous = gw_load32(GW_ARG(3) + 4);
  gw_store32(GW_ARG(3) + 4, 0);
  ResetEvent(obj->host);
  GW_RETURN(previous);
}

GW_EXPORT(KeInitializeSemaphore) {
  u32 sem = GW_ARG(3);
  write_header(sem, kTypeSemaphore, GW_ARG(4));
  gw_store32(sem + 0x10, GW_ARG(5));
  make_semaphore((i32)GW_ARG(4), (i32)GW_ARG(5), sem);
}

GW_EXPORT(KeReleaseSemaphore) {
  GwObject* obj = gw_object_from_guest(GW_ARG(3));
  LONG previous = 0;
  ReleaseSemaphore(obj->host, (LONG)GW_ARG(5), &previous);
  GW_RETURN(previous);
}

GW_EXPORT(NtCreateEvent) {
  // event type 0 = notification (manual reset), 1 = synchronization
  GwObject* obj = make_event(GW_ARG(5) == 0, GW_ARG(6) != 0, 0);
  gw_store32(GW_ARG(3), handle_add(obj));
  GW_RETURN(X_STATUS_SUCCESS);
}

GW_EXPORT(NtSetEvent) {
  GwObject* obj = gw_object_from_handle(GW_ARG(3));
  if (!obj) GW_RETURN(X_STATUS_INVALID_HANDLE);
  if (GW_ARG(4)) gw_store32(GW_ARG(4), gw_load32(obj->guest + 4));
  gw_store32(obj->guest + 4, 1);
  SetEvent(obj->host);
  GW_RETURN(X_STATUS_SUCCESS);
}

void gw_signal_guest_event_handle(u32 handle) {
  GwObject* obj = gw_object_from_handle(handle);
  if (!obj || obj->type != ObjType::Event) return;
  gw_store32(obj->guest + 4, 1);
  SetEvent(obj->host);
}

GW_EXPORT(NtClearEvent) {
  GwObject* obj = gw_object_from_handle(GW_ARG(3));
  if (!obj) GW_RETURN(X_STATUS_INVALID_HANDLE);
  gw_store32(obj->guest + 4, 0);
  ResetEvent(obj->host);
  GW_RETURN(X_STATUS_SUCCESS);
}

GW_EXPORT(NtCreateSemaphore) {
  GwObject* obj = make_semaphore((i32)GW_ARG(5), (i32)GW_ARG(6), 0);
  gw_store32(GW_ARG(3), handle_add(obj));
  GW_RETURN(X_STATUS_SUCCESS);
}

GW_EXPORT(NtReleaseSemaphore) {
  GwObject* obj = gw_object_from_handle(GW_ARG(3));
  if (!obj) GW_RETURN(X_STATUS_INVALID_HANDLE);
  LONG previous = 0;
  if (!ReleaseSemaphore(obj->host, (LONG)GW_ARG(4), &previous)) GW_RETURN(X_STATUS_SEMAPHORE_LIMIT_EXCEEDED);
  if (GW_ARG(5)) gw_store32(GW_ARG(5), previous);
  GW_RETURN(X_STATUS_SUCCESS);
}

GW_EXPORT(NtCreateMutant) {
  GwObject* obj = make_mutant(GW_ARG(5) != 0, 0);
  gw_store32(GW_ARG(3), handle_add(obj));
  GW_RETURN(X_STATUS_SUCCESS);
}

GW_EXPORT(NtReleaseMutant) {
  GwObject* obj = gw_object_from_handle(GW_ARG(3));
  if (!obj) GW_RETURN(X_STATUS_INVALID_HANDLE);
  if (!ReleaseMutex(obj->host)) GW_RETURN(X_STATUS_MUTANT_NOT_OWNED);
  GW_RETURN(X_STATUS_SUCCESS);
}

GW_EXPORT(NtClose) {
  u32 index = (GW_ARG(3) - kHandleBase) / 4;
  if (index == 0 || index >= kMaxHandles || !g_handles[index]) GW_RETURN(X_STATUS_INVALID_HANDLE);
  EnterCriticalSection(&g_object_lock);
  GwObject* obj = g_handles[index];
  g_handles[index] = nullptr;
  bool last = --obj->refs == 0;
  LeaveCriticalSection(&g_object_lock);
  if (last && obj->type == ObjType::File) {
    gw_file_close(obj->extra);
    delete obj;
  }
  GW_RETURN(X_STATUS_SUCCESS);
}

GW_EXPORT(NtDuplicateObject) {
  GwObject* obj = gw_object_from_handle(GW_ARG(3));
  if (!obj) GW_RETURN(X_STATUS_INVALID_HANDLE);
  u32 handle = handle_add(obj);
  if (GW_ARG(4)) gw_store32(GW_ARG(4), handle);
  if (GW_ARG(5) & 1) {  // DUPLICATE_CLOSE_SOURCE
    u32 index = (GW_ARG(3) - kHandleBase) / 4;
    if (index && index < kMaxHandles) {
      g_handles[index] = nullptr;
      --obj->refs;
    }
  }
  GW_RETURN(X_STATUS_SUCCESS);
}

GW_EXPORT(ObReferenceObjectByHandle) {
  GwObject* obj = gw_object_from_handle(GW_ARG(3));
  if (!obj) GW_RETURN(X_STATUS_INVALID_HANDLE);
  ++obj->refs;
  if (GW_ARG(5)) gw_store32(GW_ARG(5), obj->guest);
  GW_RETURN(X_STATUS_SUCCESS);
}

GW_EXPORT(ObDereferenceObject) {}

// --- thread control ------------------------------------------------------------------

GW_EXPORT(ExCreateThread) {
  u32 thread_id = 0;
  u32 handle = gw_thread_create(GW_ARG(4), GW_ARG(6), GW_ARG(7), GW_ARG(8), GW_ARG(9), &thread_id);
  if (GW_ARG(3)) gw_store32(GW_ARG(3), handle);
  if (GW_ARG(5)) gw_store32(GW_ARG(5), thread_id);
  GW_RETURN(X_STATUS_SUCCESS);
}

GW_EXPORT(ExTerminateThread) {
  gw_log("thread %u terminates (%08X)\n", gw_current_thread()->thread_id, GW_ARG(3));
  ExitThread(GW_ARG(3));
}

GW_EXPORT(NtResumeThread) {
  GwObject* obj = gw_object_from_handle(GW_ARG(3));
  if (!obj || obj->type != ObjType::Thread) GW_RETURN(X_STATUS_INVALID_HANDLE);
  DWORD previous = ResumeThread(obj->host);
  if (GW_ARG(4)) gw_store32(GW_ARG(4), previous);
  GW_RETURN(X_STATUS_SUCCESS);
}

GW_EXPORT(KeResumeThread) {
  GwObject* obj = gw_object_from_guest(GW_ARG(3));
  GW_RETURN(obj && obj->host ? ResumeThread(obj->host) : 0);
}

GW_EXPORT(NtSuspendThread) {
  GwObject* obj = gw_object_from_handle(GW_ARG(3));
  if (!obj || obj->type != ObjType::Thread) GW_RETURN(X_STATUS_INVALID_HANDLE);
  DWORD previous = SuspendThread(obj->host);
  if (GW_ARG(4)) gw_store32(GW_ARG(4), previous);
  GW_RETURN(X_STATUS_SUCCESS);
}

GW_EXPORT(KeSetAffinityThread) {
  if (GW_ARG(5)) gw_store32(GW_ARG(5), 1);
  GW_RETURN(X_STATUS_SUCCESS);
}

GW_EXPORT(KeSetBasePriorityThread) {
  GwObject* obj = gw_object_from_guest(GW_ARG(3));
  i32 priority = (i32)GW_ARG(4);
  if (obj && obj->host) {
    int p = priority > 0 ? THREAD_PRIORITY_ABOVE_NORMAL : priority < 0 ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL;
    if (priority >= 15) p = THREAD_PRIORITY_HIGHEST;
    SetThreadPriority(obj->host, p);
  }
  GW_RETURN(0);
}

GW_EXPORT(KeGetCurrentProcessType) { GW_RETURN(1); }
GW_EXPORT(KeEnterCriticalRegion) {}
GW_EXPORT(KeLeaveCriticalRegion) {}
GW_EXPORT(KeLockL2) { GW_RETURN(0); }
GW_EXPORT(KeUnlockL2) {}
GW_EXPORT(KeEnableFpuExceptions) {}
GW_EXPORT(KiApcNormalRoutineNop) {}

// --- IRQL and spin locks -----------------------------------------------------------------

GW_EXPORT(KeRaiseIrqlToDpcLevel) {
  GwThread* t = gw_current_thread();
  u8 old = t->irql;
  t->irql = 2;
  gw_store8(t->pcr + 0x18, 2);
  GW_RETURN(old);
}

GW_EXPORT(KfLowerIrql) {
  GwThread* t = gw_current_thread();
  t->irql = (u8)GW_ARG(3);
  gw_store8(t->pcr + 0x18, t->irql);
}

namespace {

void spin_acquire(u32 lock) {
  volatile LONG* p = (volatile LONG*)gw_host(lock);
  LONG me = (LONG)__builtin_bswap32(gw_current_thread()->pcr);
  while (InterlockedCompareExchange(p, me, 0) != 0) SwitchToThread();
}

void spin_release(u32 lock) {
  volatile LONG* p = (volatile LONG*)gw_host(lock);
  InterlockedExchange(p, 0);
}

}  // namespace

GW_EXPORT(KfAcquireSpinLock) {
  GwThread* t = gw_current_thread();
  u8 old = t->irql;
  t->irql = 2;
  spin_acquire(GW_ARG(3));
  GW_RETURN(old);
}

GW_EXPORT(KfReleaseSpinLock) {
  spin_release(GW_ARG(3));
  gw_current_thread()->irql = (u8)GW_ARG(4);
}

GW_EXPORT(KeAcquireSpinLockAtRaisedIrql) { spin_acquire(GW_ARG(3)); }
GW_EXPORT(KeReleaseSpinLockFromRaisedIrql) { spin_release(GW_ARG(3)); }

GW_EXPORT(KeTryToAcquireSpinLockAtRaisedIrql) {
  volatile LONG* p = (volatile LONG*)gw_host(GW_ARG(3));
  LONG me = (LONG)__builtin_bswap32(gw_current_thread()->pcr);
  GW_RETURN(InterlockedCompareExchange(p, me, 0) == 0 ? 1 : 0);
}

// --- critical sections -------------------------------------------------------------------
// X_RTL_CRITICAL_SECTION: header (0x10), lock_count +0x10, recursion_count +0x14,
// owning_thread +0x18. The host lock does the work; the guest fields are kept
// plausible for code that inspects them.

namespace {

CRITICAL_SECTION* cs_for(u32 guest) {
  EnterCriticalSection(&g_object_lock);
  GwObject* obj = map_find(guest);
  if (!obj) {
    obj = new GwObject{ObjType::Other, guest, nullptr, nullptr, 1};
    auto* cs = new CRITICAL_SECTION;
    InitializeCriticalSection(cs);
    obj->extra = cs;
    map_insert(guest, obj);
  }
  LeaveCriticalSection(&g_object_lock);
  return (CRITICAL_SECTION*)obj->extra;
}

}  // namespace

GW_EXPORT(RtlInitializeCriticalSection) {
  u32 cs = GW_ARG(3);
  write_header(cs, 1, 0);
  gw_store32(cs + 0x10, 0xFFFFFFFF);
  gw_store32(cs + 0x14, 0);
  gw_store32(cs + 0x18, 0);
  cs_for(cs);
}

GW_EXPORT(RtlEnterCriticalSection) {
  u32 cs = GW_ARG(3);
  EnterCriticalSection(cs_for(cs));
  gw_store32(cs + 0x14, gw_load32(cs + 0x14) + 1);
  gw_store32(cs + 0x18, gw_current_thread()->kthread);
}

GW_EXPORT(RtlLeaveCriticalSection) {
  u32 cs = GW_ARG(3);
  u32 recursion = gw_load32(cs + 0x14) - 1;
  gw_store32(cs + 0x14, recursion);
  if (!recursion) gw_store32(cs + 0x18, 0);
  LeaveCriticalSection(cs_for(cs));
}

GW_EXPORT(RtlTryEnterCriticalSection) {
  u32 cs = GW_ARG(3);
  if (!TryEnterCriticalSection(cs_for(cs))) GW_RETURN(0);
  gw_store32(cs + 0x14, gw_load32(cs + 0x14) + 1);
  gw_store32(cs + 0x18, gw_current_thread()->kthread);
  GW_RETURN(1);
}

// --- TLS ---------------------------------------------------------------------------------

GW_EXPORT(KeTlsAlloc) {
  EnterCriticalSection(&g_object_lock);
  u32 slot = 0xFFFFFFFF;
  for (u32 i = 0; i < kTlsSlots && i < 32; ++i) {
    if (!(g_tls_used & (1u << i))) {
      g_tls_used |= 1u << i;
      slot = i;
      break;
    }
  }
  LeaveCriticalSection(&g_object_lock);
  GW_RETURN(slot);
}

GW_EXPORT(KeTlsFree) {
  if (GW_ARG(3) < 32) g_tls_used &= ~(1u << GW_ARG(3));
  GW_RETURN(1);
}

GW_EXPORT(KeTlsGetValue) { GW_RETURN(gw_load32(gw_current_thread()->tls + GW_ARG(3) * 4)); }

GW_EXPORT(KeTlsSetValue) {
  gw_store32(gw_current_thread()->tls + GW_ARG(3) * 4, GW_ARG(4));
  GW_RETURN(1);
}

// --- time ----------------------------------------------------------------------------------

namespace {

LARGE_INTEGER g_host_frequency;
LARGE_INTEGER g_host_start;
u32 g_time_stamp_bundle;

u64 host_ticks_to(u64 ticks, u64 rate) {
  u64 f = (u64)g_host_frequency.QuadPart;
  return (ticks / f) * rate + (ticks % f) * rate / f;
}

u64 elapsed_ticks() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return (u64)(now.QuadPart - g_host_start.QuadPart);
}

// KeTimeStampBundle: interrupt time and system time (100 ns), tick count (ms).
DWORD WINAPI clock_thread(LPVOID) {
  constexpr u64 kSystemTimeAt2008 = 0x01C8C3A4A1C40000ull;
  for (;;) {
    u64 t100 = host_ticks_to(elapsed_ticks(), 10000000);
    gw_store64(g_time_stamp_bundle + 0, t100);
    gw_store64(g_time_stamp_bundle + 8, kSystemTimeAt2008 + t100);
    gw_store32(g_time_stamp_bundle + 16, (u32)(t100 / 10000));
    Sleep(1);
  }
  return 0;
}

}  // namespace

namespace rex::chrono {
u64 Clock::QueryGuestTickCount() { return host_ticks_to(elapsed_ticks(), 50000000); }
}  // namespace rex::chrono

GW_EXPORT(KeQueryPerformanceFrequency) { ctx.r3.u64 = 50000000; }

// --- global lock (mfmsr/mtmsrd) ------------------------------------------------------------

std::atomic<i32> gw_global_lock_count{0};

namespace {
CRITICAL_SECTION g_global_lock;
}

namespace rex::thread {
void RecursiveLock::lock() { EnterCriticalSection(&g_global_lock); }
void RecursiveLock::unlock() { LeaveCriticalSection(&g_global_lock); }
RecursiveLock& global_critical_region::mutex() {
  static RecursiveLock lock;
  return lock;
}
}  // namespace rex::thread

// --- setjmp map --------------------------------------------------------------------------------

namespace {
struct JmpEntry {
  u32 guest;
  DWORD thread;
  jmp_buf buf;
};
JmpEntry g_jmp[32];
}  // namespace

jmp_buf* gw_jmp_buf_for(u32 guest_buf_addr, bool create) {
  DWORD me = GetCurrentThreadId();
  for (auto& e : g_jmp) {
    if (e.guest == guest_buf_addr && e.thread == me) return &e.buf;
  }
  if (!create) return nullptr;
  for (auto& e : g_jmp) {
    if (!e.guest) {
      e.guest = guest_buf_addr;
      e.thread = me;
      return &e.buf;
    }
  }
  gw_fatal("setjmp table full");
}

// --- kernel variables --------------------------------------------------------------------------

u32 gw_time_stamp_bundle() { return g_time_stamp_bundle; }

void gw_kernel_threads_init() {
  g_tls_index = TlsAlloc();
  InitializeCriticalSection(&g_object_lock);
  InitializeCriticalSection(&g_global_lock);
  QueryPerformanceFrequency(&g_host_frequency);
  QueryPerformanceCounter(&g_host_start);
  g_time_stamp_bundle = gw_system_alloc(0x18, 16);
  CreateThread(nullptr, 0, clock_thread, nullptr, 0, nullptr);
}
