// Kernel variables, module/XEX header queries, config, strings, CRT printf,
// and the error/exit paths.
#include "runtime.h"

#include <stdio.h>
#include <windows.h>

u32 gw_time_stamp_bundle();

namespace {

u32 g_xex_header;      // guest copy of default.xex's header
u32 g_module_handle;   // X_LDR_DATA_TABLE_ENTRY

}  // namespace

// --- boot-time setup -------------------------------------------------------------

// Points each variable import slot in the image at a guest variable.
// Slots are from tools/dump_image (rexglue's xex loader).
void gw_kernel_variables_init(const u8* xex_header, u32 xex_header_size) {
  g_xex_header = gw_system_alloc(xex_header_size, 16);
  memcpy(gw_host(g_xex_header), xex_header, xex_header_size);

  g_module_handle = gw_system_alloc(0x64, 16);
  gw_store32(g_module_handle + 0x1C, kGuestImageBase);
  gw_store32(g_module_handle + 0x38, kGuestImageSize);
  gw_store32(g_module_handle + 0x3C, kEntryPoint);
  gw_store32(g_module_handle + 0x58, g_xex_header);

  u32 thread_object_type = gw_system_alloc(0x40, 16);
  u32 debug_monitor_data = gw_system_alloc(0x40, 16);
  u32 cert_monitor_data = gw_system_alloc(0x40, 16);
  u32 module_handle_ptr = gw_system_alloc(4, 16);
  gw_store32(module_handle_ptr, g_module_handle);
  u32 command_line = gw_system_alloc(1024, 16);
  memcpy(gw_host(command_line), "\"default.xex\"", 14);
  u32 kernel_version = gw_system_alloc(8, 16);
  gw_store16(kernel_version + 0, 2);
  gw_store16(kernel_version + 2, 0xFFFF);
  gw_store16(kernel_version + 4, 0xFFFF);
  gw_store8(kernel_version + 6, 0x80);
  u32 global_device = gw_system_alloc(4, 16);
  u32 gpu_clock = gw_system_alloc(4, 16);
  gw_store32(gpu_clock, 500);
  u32 hsio_lock = gw_system_alloc(28, 16);
  gw_store8(hsio_lock, 1);
  gw_store32(hsio_lock + 0x10, 0xFFFFFFFF);

  struct Slot {
    u32 slot;
    u32 value;
  } slots[] = {
      {0x820005E8, thread_object_type},      // 0x01B ExThreadObjectType
      {0x820005DC, debug_monitor_data},      // 0x059 KeDebugMonitorData
      {0x820005D8, gw_time_stamp_bundle()},  // 0x0AD KeTimeStampBundle
      {0x820005D4, kernel_version},          // 0x158 XboxKrnlVersion
      {0x82000568, module_handle_ptr},       // 0x193 XexExecutableModuleHandle
      {0x82000570, command_line},            // 0x1AE ExLoadedCommandLine
      {0x82000540, global_device},           // 0x1BE VdGlobalDevice
      {0x820004D0, gpu_clock},               // 0x1C0 VdGpuClockInMHz
      {0x820004E4, hsio_lock},               // 0x1C1 VdHSIOCalibrationLock
      {0x8200053C, cert_monitor_data},       // 0x266 KeCertMonitorData
  };
  for (const Slot& s : slots) gw_store32(s.slot, s.value);
}

// --- modules and XEX headers -------------------------------------------------------

GW_EXPORT(XexGetModuleHandle) {
  u32 name = GW_ARG(3);
  if (name) {
    gw_log("XexGetModuleHandle(%s)\n", (const char*)gw_host(name));
    gw_store32(GW_ARG(4), 0);
    GW_RETURN(X_STATUS_NO_SUCH_FILE);
  }
  gw_store32(GW_ARG(4), g_module_handle);
  GW_RETURN(X_STATUS_SUCCESS);
}

GW_EXPORT(XexGetProcedureAddress) {
  gw_log("XexGetProcedureAddress(%08X, %u)\n", GW_ARG(3), GW_ARG(4));
  ++gw_stats.unimplemented;
  gw_store32(GW_ARG(5), 0);
  GW_RETURN(X_STATUS_UNSUCCESSFUL);
}

GW_EXPORT(XexCheckExecutablePrivilege) { GW_RETURN(0); }

// Optional header lookup: key low byte 0 = the value itself, 1 = address of
// the value field, otherwise the value is an offset of the data in the header.
GW_EXPORT(RtlImageXexHeaderField) {
  u32 header = GW_ARG(3), key = GW_ARG(4);
  u32 count = gw_load32(header + 0x14);
  for (u32 i = 0; i < count; ++i) {
    u32 entry = header + 0x18 + i * 8;
    if (gw_load32(entry) != key) continue;
    u32 value = gw_load32(entry + 4);
    switch (key & 0xFF) {
      case 0x00: GW_RETURN(value);
      case 0x01: GW_RETURN(entry + 4);
      default: GW_RETURN(header + value);
    }
  }
  GW_RETURN(0);
}

// --- config ----------------------------------------------------------------------------

// ExGetXConfigSetting(category, setting, buffer, buffer_size, required_size_ptr)
GW_EXPORT(ExGetXConfigSetting) {
  u32 category = GW_ARG(3), setting = GW_ARG(4), buffer = GW_ARG(5), size = GW_ARG(6);
  u32 required = GW_ARG(7);
  u32 value = 0, length = 4;
  if (category == 3) {  // user category
    switch (setting) {
      case 0x0009: value = 1; break;           // language: English
      case 0x000A: value = 0x00040000; break;  // video flags: widescreen
      case 0x000B: value = 0x00010001; break;  // audio flags
      case 0x000E: value = 0; break;           // retail flags
      case 0x0011: value = 103; length = 1; break;  // country: US
      default: break;
    }
  } else if (category == 2) {  // secured category
    switch (setting) {
      case 0x0002: value = 0x00001000; break;  // AV region NTSC-M
      case 0x0003: value = 0x000000FF; break;  // game region
      default: break;
    }
  }
  if (buffer && size) {
    memset(gw_host(buffer), 0, size);
    if (length == 1) gw_store8(buffer, (u8)value);
    else if (size >= 4) gw_store32(buffer, value);
    else if (size >= 2) gw_store16(buffer, (u16)value);
  }
  if (required) gw_store16(required, (u16)length);
  GW_RETURN(X_STATUS_SUCCESS);
}

GW_EXPORT(XGetAVPack) { GW_RETURN(6); }
GW_EXPORT(XGetGameRegion) { GW_RETURN(0xFFFF); }
GW_EXPORT(XGetLanguage) { GW_RETURN(1); }

GW_EXPORT(XGetVideoMode) {
  gw_write_video_mode(GW_ARG(3));
}

// --- strings ---------------------------------------------------------------------------

GW_EXPORT(RtlInitAnsiString) {
  u32 dest = GW_ARG(3), src = GW_ARG(4);
  u16 length = src ? (u16)strlen((const char*)gw_host(src)) : 0;
  gw_store16(dest + 0, length);
  gw_store16(dest + 2, src ? length + 1 : 0);
  gw_store32(dest + 4, src);
}

// RtlMultiByteToUnicodeN(unicode, max_bytes, result_bytes_ptr, multibyte, multibyte_bytes)
GW_EXPORT(RtlMultiByteToUnicodeN) {
  u32 out = GW_ARG(3), max_bytes = GW_ARG(4), result_ptr = GW_ARG(5), in = GW_ARG(6), in_bytes = GW_ARG(7);
  u32 n = in_bytes < max_bytes / 2 ? in_bytes : max_bytes / 2;
  for (u32 i = 0; i < n; ++i) gw_store16(out + i * 2, gw_load8(in + i));
  if (result_ptr) gw_store32(result_ptr, n * 2);
  GW_RETURN(X_STATUS_SUCCESS);
}

GW_EXPORT(RtlNtStatusToDosError) {
  u32 status = GW_ARG(3);
  if (!status) GW_RETURN(0);
  if ((status & 0xFFFF0000) == 0x80070000 || (status & 0xFFFF0000) == 0xC0070000) GW_RETURN(status & 0xFFFF);
  switch (status) {
    case X_STATUS_OBJECT_NAME_NOT_FOUND: GW_RETURN(2);
    case X_STATUS_OBJECT_PATH_NOT_FOUND: GW_RETURN(3);
    case X_STATUS_NO_MEMORY: GW_RETURN(8);
    case X_STATUS_INVALID_HANDLE: GW_RETURN(6);
    case X_STATUS_END_OF_FILE: GW_RETURN(38);
    case X_STATUS_PENDING: GW_RETURN(997);
    default: GW_RETURN(317);  // ERROR_MR_MID_NOT_FOUND
  }
}

// --- CRT formatting (the game's sprintf/_vsnprintf/DbgPrint) ---------------------------------
// Guest varargs are 8-byte slots: r3..r10, then the stack at r1+0x54.

namespace {

struct GuestArgs {
  PPCContext* ctx;
  u32 next;         // next slot index (0 = r3)
  u32 va_list_ptr;  // nonzero: slots come from this array instead
  u64 get() {
    u32 i = next++;
    if (va_list_ptr) return gw_load64(va_list_ptr + i * 8);
    if (i < 8) {
      const PPCRegister* regs[8] = {&ctx->r3, &ctx->r4, &ctx->r5, &ctx->r6,
                                    &ctx->r7, &ctx->r8, &ctx->r9, &ctx->r10};
      return regs[i]->u64;
    }
    return gw_load64(ctx->r1.u32 + 0x54 + (i - 8) * 8 - 4);
  }
};

// Formats into host memory; handles the conversions the game uses.
int guest_format(char* out, size_t out_size, const char* fmt, GuestArgs& args) {
  size_t n = 0;
  auto put = [&](char c) {
    if (n + 1 < out_size) out[n] = c;
    ++n;
  };
  for (const char* p = fmt; *p; ++p) {
    if (*p != '%') {
      put(*p);
      continue;
    }
    char spec[32];
    size_t s = 0;
    spec[s++] = '%';
    ++p;
    while (*p && strchr("-+ #0", *p) && s < 20) spec[s++] = *p++;
    while (*p && ((*p >= '0' && *p <= '9') || *p == '.' || *p == '*') && s < 26) {
      if (*p == '*') {
        s += snprintf(spec + s, sizeof(spec) - s, "%d", (int)(i32)args.get());
        ++p;
      } else {
        spec[s++] = *p++;
      }
    }
    bool wide = false, is64 = false;
    while (*p == 'l' || *p == 'h' || *p == 'I' || *p == 'w' || *p == 'L') {
      if (*p == 'l' && p[1] == 'l') { is64 = true; ++p; }
      else if (*p == 'I' && p[1] == '6' && p[2] == '4') { is64 = true; p += 2; }
      else if (*p == 'l' || *p == 'w') wide = true;
      ++p;
    }
    char conv = *p;
    if (!conv) break;
    char piece[256];
    switch (conv) {
      case '%': put('%'); continue;
      case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c': {
        if (is64) {
          spec[s++] = 'l';
          spec[s++] = 'l';
        }
        spec[s++] = conv;
        spec[s] = 0;
        u64 v = args.get();
        if (is64) snprintf(piece, sizeof(piece), spec, (long long)v);
        else snprintf(piece, sizeof(piece), spec, (int)(u32)v);
        break;
      }
      case 'p':
        snprintf(piece, sizeof(piece), "%08X", (u32)args.get());
        break;
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
        spec[s++] = conv;
        spec[s] = 0;
        u64 bits = args.get();
        double d;
        memcpy(&d, &bits, 8);
        snprintf(piece, sizeof(piece), spec, d);
        break;
      }
      case 's': case 'S': {
        spec[s++] = 's';
        spec[s] = 0;
        u32 str = (u32)args.get();
        char text[200];
        if (!str) {
          snprintf(text, sizeof(text), "(null)");
        } else if (wide || conv == 'S') {
          size_t i = 0;
          for (; i + 1 < sizeof(text); ++i) {
            u16 c = gw_load16(str + i * 2);
            if (!c) break;
            text[i] = c < 128 ? (char)c : '?';
          }
          text[i] = 0;
        } else {
          snprintf(text, sizeof(text), "%s", (const char*)gw_host(str));
        }
        snprintf(piece, sizeof(piece), spec, text);
        break;
      }
      default:
        piece[0] = 0;
        break;
    }
    for (const char* q = piece; *q; ++q) put(*q);
  }
  if (out_size) out[n < out_size ? n : out_size - 1] = 0;
  return (int)n;
}

}  // namespace

GW_EXPORT(sprintf) {
  u32 buffer = GW_ARG(3), fmt = GW_ARG(4);
  GuestArgs args{&ctx, 2, 0};
  char text[2048];
  int n = guest_format(text, sizeof(text), (const char*)gw_host(fmt), args);
  memcpy(gw_host(buffer), text, strlen(text) + 1);
  GW_RETURN(n);
}

// _vsnprintf(buffer, count, format, va_list)
GW_EXPORT(_vsnprintf) {
  u32 buffer = GW_ARG(3), count = GW_ARG(4), fmt = GW_ARG(5), va = GW_ARG(6);
  GuestArgs args{&ctx, 0, va};
  char text[2048];
  int n = guest_format(text, sizeof(text), (const char*)gw_host(fmt), args);
  if (count) {
    u32 copy = (u32)n < count ? (u32)n + 1 : count;
    memcpy(gw_host(buffer), text, copy);
    if ((u32)n >= count) GW_RETURN(-1);
  }
  GW_RETURN(n);
}

GW_EXPORT(DbgPrint) {
  GuestArgs args{&ctx, 1, 0};
  char text[512];
  guest_format(text, sizeof(text), (const char*)gw_host(GW_ARG(3)), args);
  gw_log("[guest] %s", text);
  GW_RETURN(X_STATUS_SUCCESS);
}

// --- error and exit paths ------------------------------------------------------------------

GW_EXPORT(DbgBreakPoint) { gw_log("DbgBreakPoint\n"); }
GW_EXPORT(KeBugCheck) { gw_fatal("KeBugCheck(%08X)", GW_ARG(3)); }
GW_EXPORT(KeBugCheckEx) { gw_fatal("KeBugCheckEx(%08X, %08X, %08X)", GW_ARG(3), GW_ARG(4), GW_ARG(5)); }
GW_EXPORT(RtlRaiseException) { gw_fatal("RtlRaiseException(code %08X)", gw_load32(GW_ARG(3))); }
GW_EXPORT(RtlUnwind) { gw_fatal("RtlUnwind"); }
GW_EXPORT(__C_specific_handler) { gw_fatal("__C_specific_handler"); }
GW_EXPORT(HalReturnToFirmware) { gw_fatal("game asked to return to the dashboard (%u)", GW_ARG(3)); }
GW_EXPORT(ExRegisterTitleTerminateNotification) {}

// --- networking (offline) ------------------------------------------------------------------

GW_EXPORT(NetDll_WSAStartup) {
  if (GW_ARG(5)) {
    memset(gw_host(GW_ARG(5)), 0, 0x190);
    gw_store16(GW_ARG(5), 0x0202);
    gw_store16(GW_ARG(5) + 2, 0x0202);
  }
  GW_RETURN(0);
}

GW_EXPORT(NetDll_XNetStartup) { GW_RETURN(0); }
