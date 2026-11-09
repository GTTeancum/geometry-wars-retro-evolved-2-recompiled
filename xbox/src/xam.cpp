// XAM: one local signed-in player, a profile saved on the hard disk, offline Live,
// controller input, notifications, and the audio/XMA entry points.
#include "runtime.h"

#include <nxdk/mount.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>


namespace {

constexpr u64 kXuid = 0xB13EBABEBABEBABEull;
constexpr u32 kTitleId = 0x584108FF;

// XAM_OVERLAPPED: result +0, length +4, context +8, event +0xC,
// completion routine +0x10, extended error +0x18.
u32 complete(u32 overlapped, u32 result, u32 length = 0) {
  if (!overlapped) return result;
  gw_store32(overlapped + 0x00, result);
  gw_store32(overlapped + 0x04, length);
  gw_store32(overlapped + 0x18, result);
  if (u32 event = gw_load32(overlapped + 0x0C)) gw_signal_guest_event_handle(event);
  return X_ERROR_IO_PENDING;
}

// Title-specific profile settings written by the game (saved on the hard
// disk, below).
struct StoredSetting {
  u32 id;
  u32 size;
  u8* data;
};
StoredSetting g_settings[8];

StoredSetting* find_setting(u32 id) {
  for (auto& s : g_settings) {
    if (s.id == id) return &s;
  }
  return nullptr;
}

// --- saves on the hard disk ----------------------------------------------------------
// The settings are the game's whole save (its progress and options). They
// live in the title's folder on the hard disk, named by the 360 title ID
// (also the XBE's, tools/xbe_meta.py), in one save the dashboard lists:
//   E:\UDATA\584108FF\              TitleMeta.xbx, TitleImage.xbx, SaveImage.xbx
//   E:\UDATA\584108FF\000000000000\ SaveMeta.xbx, SaveImage.xbx, profile.dat
// profile.dat: 'GWPS', version 1, count, then per setting its id, size and
// bytes, then an FNV-1a hash of everything before it (little-endian). It is
// written whole to profile.new and then moved over profile.dat; a missing or
// damaged profile.dat falls back to profile.new.
constexpr char kTitleDir[] = "E:\\UDATA\\584108FF";
constexpr char kSaveDir[] = "E:\\UDATA\\584108FF\\000000000000";
constexpr char kSaveFile[] = "E:\\UDATA\\584108FF\\000000000000\\profile.dat";
constexpr char kSaveNew[] = "E:\\UDATA\\584108FF\\000000000000\\profile.new";
constexpr u32 kSaveMagic = 0x53505747;  // "GWPS"
bool g_saves_ready;

u32 fnv1a(const u8* p, u32 n) {
  u32 h = 2166136261u;
  for (u32 i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
  return h;
}

// A UTF-16 (with byte order mark) text file, as the dashboard reads.
void write_utf16(const char* path, const char* text) {
  u16 buf[128];
  u32 n = 0;
  buf[n++] = 0xFEFF;
  for (const char* c = text; *c && n < 128; ++c) buf[n++] = (u8)*c;
  HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  DWORD done = 0;
  WriteFile(f, buf, n * 2, &done, nullptr);
  CloseHandle(f);
}

// The settings from a save file; false when it is missing or damaged.
bool load_file(const char* path) {
  HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  static u8 buf[0x8000];
  DWORD size = 0;
  const BOOL ok = ReadFile(f, buf, sizeof(buf), &size, nullptr);
  CloseHandle(f);
  if (!ok || size < 16 || size == sizeof(buf)) return false;
  auto word = [&](u32 at) {
    u32 v;
    memcpy(&v, buf + at, 4);
    return v;
  };
  if (word(0) != kSaveMagic || word(4) != 1 || word(size - 4) != fnv1a(buf, size - 4)) return false;
  const u32 count = word(8);
  // Checked whole before any of it is taken.
  u32 at = 12;
  for (u32 i = 0; i < count; ++i) {
    if (at + 8 > size - 4) return false;
    const u32 id = word(at), n = word(at + 4);
    if (i >= 8 || id == 0 || n > ((id >> 16) & 0xFFF) || at + 8 + n > size - 4) return false;
    at += 8 + n;
  }
  if (at != size - 4) return false;
  at = 12;
  for (u32 i = 0; i < count; ++i) {
    const u32 id = word(at), n = word(at + 4);
    StoredSetting& s = g_settings[i];
    s.id = id;
    s.size = n;
    s.data = (u8*)malloc(n ? n : 1);
    memcpy(s.data, buf + at + 8, n);
    at += 8 + n;
  }
  return true;
}

void save_settings() {
  if (!g_saves_ready) return;
  static u8 buf[0x8000];
  u32 at = 12, count = 0;
  for (const StoredSetting& s : g_settings) {
    if (!s.id) continue;
    if (at + 8 + s.size + 4 > sizeof(buf)) break;
    memcpy(buf + at, &s.id, 4);
    memcpy(buf + at + 4, &s.size, 4);
    memcpy(buf + at + 8, s.data, s.size);
    at += 8 + s.size;
    ++count;
  }
  const u32 head[3] = {kSaveMagic, 1, count};
  memcpy(buf, head, 12);
  const u32 hash = fnv1a(buf, at);
  memcpy(buf + at, &hash, 4);
  at += 4;
  HANDLE f = CreateFileA(kSaveNew, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    gw_log("save: %s could not be written (error %u)\n", kSaveNew, (u32)GetLastError());
    return;
  }
  DWORD done = 0;
  const BOOL ok = WriteFile(f, buf, at, &done, nullptr) && done == at;
  CloseHandle(f);
  if (!ok) {
    gw_log("save: writing %s failed\n", kSaveNew);
    return;
  }
  DeleteFileA(kSaveFile);
  if (!MoveFileA(kSaveNew, kSaveFile)) gw_log("save: %s not moved into place\n", kSaveNew);
  gw_log("save: %u settings written to %s (%u bytes)\n", count, kSaveFile, at);
}

// (Once, before the first setting is read or written.) The hard disk's
// title folder made and given the dashboard's files (made from the game's
// icon when the disc was staged, next to the XBE on D:), and the save read.
void saves_init() {
  static bool done = false;
  if (done) return;
  done = true;
  const bool mounted = nxIsDriveMounted('E');
  if (!mounted && !nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\")) {
    gw_log("save: the hard disk (E:) could not be mounted; settings kept for this session only\n");
    return;
  }
  static const char* const kDirs[] = {"E:\\UDATA", kTitleDir, kSaveDir};
  for (const char* dir : kDirs) {
    if (CreateDirectoryA(dir, nullptr)) gw_log("save: %s made\n", dir);
    else if (GetLastError() != ERROR_ALREADY_EXISTS)
      gw_log("save: %s not made (error %u; E: %s)\n", dir, (u32)GetLastError(), mounted ? "was mounted" : "mounted now");
  }
  if (GetFileAttributesA(kSaveDir) == INVALID_FILE_ATTRIBUTES) {
    gw_log("save: %s could not be made (error %u); settings kept for this session only\n", kSaveDir,
           (u32)GetLastError());
    return;
  }
  static const char* const kFiles[] = {"TitleMeta.xbx", "TitleImage.xbx", "SaveImage.xbx"};
  for (const char* name : kFiles) {
    char from[64], to[96];
    snprintf(from, sizeof(from), "D:\\%s", name);
    snprintf(to, sizeof(to), "%s\\%s", kTitleDir, name);
    if (!CopyFileA(from, to, FALSE)) gw_log("save: %s not copied (error %u)\n", from, (u32)GetLastError());
  }
  {
    char to[96];
    snprintf(to, sizeof(to), "%s\\SaveImage.xbx", kSaveDir);
    CopyFileA("D:\\SaveImage.xbx", to, FALSE);
    snprintf(to, sizeof(to), "%s\\SaveMeta.xbx", kSaveDir);
    write_utf16(to, "[default]\r\nName=Game progress\r\n");
  }
  g_saves_ready = true;
  if (load_file(kSaveFile)) gw_log("save: settings read from %s\n", kSaveFile);
  else if (load_file(kSaveNew)) gw_log("save: settings read from %s (the last write was not finished)\n", kSaveNew);
  else gw_log("save: no save in %s yet\n", kSaveDir);
}

// Notification queue (XNotifyGetNext).
struct Notification {
  u32 id;
  u32 param;
};
Notification g_notifications[16];
volatile LONG g_notification_count;

}  // namespace

// --- users -------------------------------------------------------------------------

GW_EXPORT(XamUserGetSigninState) { GW_RETURN(GW_ARG(3) == 0 ? 1 : 0); }

GW_EXPORT(XamUserGetXUID) {
  if (GW_ARG(3) != 0) GW_RETURN(X_ERROR_NO_SUCH_USER);
  gw_store64(GW_ARG(5), kXuid);
  GW_RETURN(X_ERROR_SUCCESS);
}

GW_EXPORT(XamUserGetName) {
  if (GW_ARG(3) != 0) GW_RETURN(X_ERROR_NO_SUCH_USER);
  u32 length = GW_ARG(5);
  const char name[] = "Player";
  for (u32 i = 0; i < length; ++i) gw_store8(GW_ARG(4) + i, i < sizeof(name) ? name[i] : 0);
  GW_RETURN(X_ERROR_SUCCESS);
}

GW_EXPORT(XamShowSigninUI) { GW_RETURN(X_ERROR_SUCCESS); }

// XamUserReadProfileSettings(title_id, user_index, xuid_count, xuids, setting_count,
//                            setting_ids, buffer_size_ptr, buffer, overlapped)
GW_EXPORT(XamUserReadProfileSettings) {
  u32 user_index = GW_ARG(4), xuids = GW_ARG(6), count = GW_ARG(7), ids = GW_ARG(8);
  u32 size_ptr = GW_ARG(9), buffer = GW_ARG(10), overlapped = GW_STACK_ARG(8);
  saves_init();
  u32 header_size = 8 + count * 40, data_size = 0;
  for (u32 i = 0; i < count; ++i) {
    u32 id = gw_load32(ids + i * 4);
    u32 type = id >> 28;
    if (type == 4 || type == 6) data_size += (id >> 16) & 0xFFF;  // wstring / binary
  }
  u32 needed = header_size + data_size;
  u32 size = gw_load32(size_ptr);
  if (!buffer || size < needed) {
    if (!size) gw_store32(size_ptr, needed);
    GW_RETURN(X_ERROR_INSUFFICIENT_BUFFER);
  }
  if (!xuids && user_index) GW_RETURN(complete(overlapped, X_ERROR_NO_SUCH_USER));
  gw_store32(buffer, count);
  gw_store32(buffer + 4, buffer + 8);
  u32 data = buffer + header_size;
  for (u32 i = 0; i < count; ++i) {
    u32 id = gw_load32(ids + i * 4);
    u32 out = buffer + 8 + i * 40;
    memset(gw_host(out), 0, 40);
    if (xuids) gw_store64(out + 8, kXuid);
    else gw_store32(out + 8, user_index);
    gw_store32(out + 0x10, id);
    StoredSetting* stored = find_setting(id);
    // Source 1 (default) for anything never written, as a console answers;
    // the game then creates a fresh profile. 2 = title setting with data.
    gw_store32(out + 0, stored ? 2 : 1);
    u32 type = id >> 28;
    gw_store8(out + 0x18, (u8)type);
    if (stored && (type == 4 || type == 6)) {
      u32 n = stored->size < ((id >> 16) & 0xFFF) ? stored->size : ((id >> 16) & 0xFFF);
      memcpy(gw_host(data), stored->data, n);
      gw_store32(out + 0x18 + 8, n);
      gw_store32(out + 0x18 + 12, data);
      data += n;
    }
  }
  gw_log("XamUserReadProfileSettings(%u settings)\n", count);
  GW_RETURN(complete(overlapped, X_ERROR_SUCCESS));
}

// XamUserWriteProfileSettings(title_id, user_index, count, settings, overlapped)
GW_EXPORT(XamUserWriteProfileSettings) {
  u32 count = GW_ARG(5), settings = GW_ARG(6), overlapped = GW_ARG(7);
  saves_init();
  u32 written = 0;
  for (u32 i = 0; i < count; ++i) {
    u32 s = settings + i * 40;
    u32 id = gw_load32(s + 0x10);
    u32 type = gw_load8(s + 0x18);
    if (type != 6 && type != 4) continue;
    u32 size = gw_load32(s + 0x18 + 8), ptr = gw_load32(s + 0x18 + 12);
    if (size > ((id >> 16) & 0xFFF)) {  // larger than the setting's id allows
      gw_log("profile setting %08X: %u bytes refused\n", id, size);
      continue;
    }
    StoredSetting* stored = find_setting(id);
    if (!stored) stored = find_setting(0);
    if (!stored) continue;
    free(stored->data);
    stored->id = id;
    stored->size = size;
    stored->data = (u8*)malloc(size ? size : 1);
    memcpy(stored->data, gw_host(ptr), size);
    ++written;
  }
  if (written) save_settings();
  GW_RETURN(complete(overlapped, X_ERROR_SUCCESS));
}

GW_EXPORT(XamUserCreateAchievementEnumerator) {
  if (GW_ARG(9)) gw_store32(GW_ARG(9), 0);
  gw_store32(GW_ARG(10), gw_handle_for_file(nullptr));
  GW_RETURN(X_ERROR_SUCCESS);
}

GW_EXPORT(XamUserCreateStatsEnumerator) {
  gw_log("XamUserCreateStatsEnumerator\n");
  if (GW_ARG(9)) gw_store32(GW_ARG(9), 0);
  if (GW_ARG(10)) gw_store32(GW_ARG(10), gw_handle_for_file(nullptr));
  GW_RETURN(X_ERROR_SUCCESS);
}

// XamEnumerate(handle, flags, buffer, size, items_returned_ptr, overlapped)
GW_EXPORT(XamEnumerate) {
  if (GW_ARG(7)) gw_store32(GW_ARG(7), 0);
  GW_RETURN(complete(GW_ARG(8), X_ERROR_NO_MORE_FILES));
}

// --- content, execution, loader ------------------------------------------------------------

// Full license, or the game neither loads nor creates a profile.
GW_EXPORT(XamContentGetLicenseMask) {
  gw_store32(GW_ARG(3), 1);
  GW_RETURN(complete(GW_ARG(4), X_ERROR_SUCCESS));
}

GW_EXPORT(XamGetExecutionId) {
  static u32 info = 0;
  if (!info) {
    info = gw_system_alloc(0x18, 16);
    gw_store32(info + 0x00, 0x191FF297);  // media id
    gw_store32(info + 0x04, 0x00000102);  // version 0.0.1.2
    gw_store32(info + 0x08, 0x00000102);
    gw_store32(info + 0x0C, kTitleId);
  }
  gw_store32(GW_ARG(3), info);
  GW_RETURN(X_STATUS_SUCCESS);
}

GW_EXPORT(XamGetSystemVersion) { GW_RETURN(0x20000000); }
GW_EXPORT(XamLoaderLaunchTitle) { gw_return_to_dash("the game launched another title (XamLoaderLaunchTitle)"); }
GW_EXPORT(XamLoaderTerminateTitle) { gw_return_to_dash("the game exited (XamLoaderTerminateTitle)"); }

// --- UI ------------------------------------------------------------------------------------

// XamShowMessageBoxUIEx(user, title, text, button_count, buttons, focus, flags, result_ptr, overlapped)
GW_EXPORT(XamShowMessageBoxUIEx) {
  gw_log("XamShowMessageBoxUIEx: answered with the first button\n");
  if (GW_ARG(10)) gw_store32(GW_ARG(10), 0);
  GW_RETURN(complete(GW_STACK_ARG(8), X_ERROR_SUCCESS));
}

GW_EXPORT(XamShowDirtyDiscErrorUI) { gw_fatal("dirty disc error (a game file failed to read)"); }
GW_EXPORT(XamShowAchievementsUI) { GW_RETURN(X_ERROR_SUCCESS); }
GW_EXPORT(XamShowMarketplaceUI) { GW_RETURN(X_ERROR_SUCCESS); }
GW_EXPORT(XamShowGamerCardUIForXUID) { GW_RETURN(X_ERROR_SUCCESS); }

// --- sessions and messages (offline) ---------------------------------------------------------

GW_EXPORT(XamSessionCreateHandle) {
  gw_store32(GW_ARG(3), gw_handle_for_file(nullptr));
  GW_RETURN(X_ERROR_SUCCESS);
}

GW_EXPORT(XamSessionRefObjByHandle) {
  static u32 object = 0;
  if (!object) object = gw_system_alloc(0x100, 16);
  gw_store32(GW_ARG(4), object);
  GW_RETURN(X_ERROR_SUCCESS);
}

// XMsgInProcessCall(app, message, buffer, buffer_length)
GW_EXPORT(XMsgInProcessCall) {
  u32 app = GW_ARG(3), message = GW_ARG(4), buffer = GW_ARG(5);
  if (app == 0xFA && message == 0x0007001B) {
    // XMPGetPlaybackController: the title owns playback, nothing locked.
    // A game thread polls this in a loop; the sleep keeps it from spinning.
    gw_store32(gw_load32(buffer + 4), 0);
    gw_store32(gw_load32(buffer + 8), 0);
    Sleep(10);
    GW_RETURN(X_ERROR_SUCCESS);
  }
  static u32 logged[16];
  bool seen = false;
  for (u32 m : logged) seen |= m == message;
  if (!seen) {
    for (u32& m : logged) {
      if (!m) {
        m = message;
        break;
      }
    }
    gw_log("XMsgInProcessCall(app %08X, message %08X) answered with success\n", app, message);
  }
  GW_RETURN(X_ERROR_SUCCESS);
}

// XMsgStartIORequest(app, message, overlapped, buffer, buffer_length)
GW_EXPORT(XMsgStartIORequest) {
  // Logged once per message kind: the menus send these continuously, and a
  // line each filled the log.
  static u32 seen[16];
  const u32 kind = GW_ARG(3) ^ GW_ARG(4);
  bool known = false;
  for (u32 s : seen) known |= s == kind;
  if (!known) {
    for (u32& s : seen)
      if (!s) {
        s = kind;
        break;
      }
    gw_log("XMsgStartIORequest(app %08X, message %08X) (first of its kind)\n", GW_ARG(3), GW_ARG(4));
  }
  GW_RETURN(complete(GW_ARG(5), X_ERROR_SUCCESS));
}

// --- notifications -------------------------------------------------------------------------

GW_EXPORT(XamNotifyCreateListener) {
  // Report the signed-in player once, as the system does after boot.
  if (g_notification_count == 0) {
    g_notifications[0] = {0x0000000A, 1};  // XN_SYS_SIGNINCHANGED
    g_notification_count = 1;
  }
  GW_RETURN(gw_handle_for_file(nullptr));
}

// XNotifyGetNext(handle, match_id, id_ptr, param_ptr)
GW_EXPORT(XNotifyGetNext) {
  u32 match = GW_ARG(4);
  for (LONG i = 0; i < g_notification_count; ++i) {
    if (match && g_notifications[i].id != match) continue;
    gw_store32(GW_ARG(5), g_notifications[i].id);
    if (GW_ARG(6)) gw_store32(GW_ARG(6), g_notifications[i].param);
    memmove(&g_notifications[i], &g_notifications[i + 1], (g_notification_count - i - 1) * sizeof(Notification));
    --g_notification_count;
    GW_RETURN(1);
  }
  GW_RETURN(0);
}

// --- input -----------------------------------------------------------------------------------

GW_EXPORT(XamInputGetCapabilities) {
  if (GW_ARG(3) != 0) GW_RETURN(X_ERROR_DEVICE_NOT_CONNECTED);
  u32 caps = GW_ARG(5);
  memset(gw_host(caps), 0, 20);
  gw_store8(caps + 0, 1);  // gamepad
  gw_store8(caps + 1, 1);
  gw_store16(caps + 4, 0xF3FF);
  gw_store8(caps + 6, 0xFF);
  gw_store8(caps + 7, 0xFF);
  gw_store16(caps + 8, 0xFFC0);
  gw_store16(caps + 10, 0xFFC0);
  gw_store16(caps + 12, 0xFFC0);
  gw_store16(caps + 14, 0xFFC0);
  gw_store16(caps + 16, 0xFFFF);
  gw_store16(caps + 18, 0xFFFF);
  GW_RETURN(X_ERROR_SUCCESS);
}

// XamInputGetState(user_index, flags, state_ptr): pads from src/input.cpp; pad 0
// always reads as connected (idle when nothing is plugged in).
GW_EXPORT(XamInputGetState) {
  u32 user = GW_ARG(3), state = GW_ARG(5);
  static u32 packet = 0;
  memset(gw_host(state), 0, 16);
  if (!gw_input_read(user, state + 4) && user != 0) GW_RETURN(X_ERROR_DEVICE_NOT_CONNECTED);
  gw_store32(state, ++packet);
  GW_RETURN(X_ERROR_SUCCESS);
}

GW_EXPORT(XamInputSetState) { GW_RETURN(GW_ARG(3) == 0 ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED); }

// --- audio -----------------------------------------------------------------------------------
// The render driver (src/audio.cpp): r3 points at {callback, argument}, the
// driver handle goes to *r4; each frame is submitted with r4 the samples.

GW_EXPORT(XAudioRegisterRenderDriverClient) {
  u32 callback = GW_ARG(3);
  gw_log("XAudioRegisterRenderDriverClient(callback %08X, arg %08X)\n", gw_load32(callback),
         gw_load32(callback + 4));
  gw_audio_register(gw_load32(callback), gw_load32(callback + 4));
  gw_store32(GW_ARG(4), 0x41550000);
  GW_RETURN(X_STATUS_SUCCESS);
}

GW_EXPORT(XAudioUnregisterRenderDriverClient) {
  gw_audio_unregister();
  GW_RETURN(X_STATUS_SUCCESS);
}
GW_EXPORT(XAudioSubmitRenderDriverFrame) {
  gw_audio_frame(GW_ARG(4));
  GW_RETURN(X_STATUS_SUCCESS);
}

GW_EXPORT(XAudioGetVoiceCategoryVolume) {
  float one = 1.0f;
  u32 bits;
  memcpy(&bits, &one, 4);
  gw_store32(GW_ARG(4), bits);
  GW_RETURN(X_STATUS_SUCCESS);
}

GW_EXPORT(XAudioGetVoiceCategoryVolumeChangeMask) {
  gw_store32(GW_ARG(4), 0);
  GW_RETURN(X_STATUS_SUCCESS);
}

GW_EXPORT(XMACreateContext) {
  u32 context = gw_physical_alloc(64, 0x1000);
  gw_store32(GW_ARG(3), context);
  GW_RETURN(context ? X_STATUS_SUCCESS : X_STATUS_NO_MEMORY);
}

GW_EXPORT(XMAReleaseContext) { gw_physical_free(GW_ARG(3)); }
