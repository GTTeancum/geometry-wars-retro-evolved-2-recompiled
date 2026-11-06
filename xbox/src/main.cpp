// Boot: status screen, guest memory, game image and XEX header from the DVD,
// dispatch table, kernel state, GPU, then the game's entry point on its own
// thread.
#include "runtime.h"

#include <stdio.h>
#include <windows.h>

void gw_kernel_threads_init();
void gw_kernel_variables_init(const u8* xex_header, u32 xex_header_size);

namespace {

bool read_file(const char* path, u32 offset, void* out, u32 size, u32* got) {
  HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  SetFilePointer(f, (LONG)offset, nullptr, FILE_BEGIN);
  DWORD done = 0;
  BOOL ok = ReadFile(f, out, size, &done, nullptr);
  CloseHandle(f);
  if (got) *got = done;
  return ok;
}

}  // namespace

int main() {
  // Reserve the guest windows before anything else allocates (the virtual
  // heap window starts just above the XBE).
  bool reserved = gw_memory_init();
  gw_status_init();
  gw_log("boot\n");
  if (!reserved) gw_fatal("could not reserve the guest memory windows");
  gw_log_memory("after reserving guest windows");

  u32 got = 0;
  if (!read_file("D:\\gw2\\default.image.bin", 0, gw_host(kGuestImageBase), kGuestImageSize, &got) ||
      got != kGuestImageSize) {
    gw_fatal("D:\\gw2\\default.image.bin missing or short (%u bytes)", got);
  }

  u8 head[12];
  if (!read_file("D:\\default.xex", 0, head, sizeof(head), &got) || got != sizeof(head)) {
    gw_fatal("D:\\default.xex missing");
  }
  u32 header_size = ((u32)head[8] << 24) | ((u32)head[9] << 16) | ((u32)head[10] << 8) | head[11];
  if (header_size == 0 || header_size > 0x10000) gw_fatal("bad XEX header size %X", header_size);
  u8* header = (u8*)malloc(header_size);
  read_file("D:\\default.xex", 0, header, header_size, &got);

  u32 functions = 0;
  for (PPCFuncMapping* m = PPCFuncMappings; m->host; ++m, ++functions) {
    gw_dispatch_table[((u32)m->guest - REX_CODE_BASE) >> 2] = m->host;
  }

  gw_kernel_threads_init();
  gw_kernel_variables_init(header, header_size);
  free(header);
  gw_gpu_init();

  gw_log("%u functions, image loaded, starting the game at %08X\n", functions, kEntryPoint);
  gw_run_main_thread();
  for (;;) Sleep(1000);
  return 0;
}
