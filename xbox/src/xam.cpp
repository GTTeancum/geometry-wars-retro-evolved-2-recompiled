// XAM: one local signed-in player, an in-memory profile, offline Live,
// controller input, notifications, and the audio/XMA entry points.
#include "runtime.h"

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

// Title-specific profile settings written by the game, kept for the session.
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
  for (u32 i = 0; i < count; ++i) {
    u32 s = settings + i * 40;
    u32 id = gw_load32(s + 0x10);
    u32 type = gw_load8(s + 0x18);
    if (type != 6 && type != 4) continue;
    u32 size = gw_load32(s + 0x18 + 8), ptr = gw_load32(s + 0x18 + 12);
    StoredSetting* stored = find_setting(id);
    if (!stored) stored = find_setting(0);
    if (!stored) continue;
    free(stored->data);
    stored->id = id;
    stored->size = size;
    stored->data = (u8*)malloc(size ? size : 1);
    memcpy(stored->data, gw_host(ptr), size);
    gw_log("profile setting %08X saved (%u bytes, this session only)\n", id, size);
  }
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
GW_EXPORT(XamLoaderLaunchTitle) { gw_fatal("XamLoaderLaunchTitle"); }
GW_EXPORT(XamLoaderTerminateTitle) { gw_fatal("game exited (XamLoaderTerminateTitle)"); }

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
  gw_log("XMsgStartIORequest(app %08X, message %08X)\n", GW_ARG(3), GW_ARG(4));
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
