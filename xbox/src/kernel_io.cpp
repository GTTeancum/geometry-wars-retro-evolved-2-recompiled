// File exports. Guest paths ("game:\...", "\Device\Cdrom0\...") map to the
// Xbox DVD (D:\), where the extracted package files are staged next to
// default.xbe. Everything is read-only; writes go nowhere.
#include "runtime.h"

#include <stdio.h>
#include <windows.h>


namespace {

struct GwFile {
  HANDLE host;
  bool directory;
  char path[260];      // host path
  HANDLE find;         // directory enumeration
};

bool starts_with_nocase(const char* s, const char* prefix) {
  for (; *prefix; ++s, ++prefix) {
    char a = *s, b = *prefix;
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    if (a != b) return false;
  }
  return true;
}

bool map_path(const char* guest, char* out, size_t out_size) {
  const char* rest = nullptr;
  if (starts_with_nocase(guest, "\\Device\\Cdrom0")) {
    rest = guest + 14;
  } else if (const char* colon = strchr(guest, ':')) {
    // "game:\x", "GAME:\x", "d:\x", "\??\game:\x"
    rest = colon + 1;
  } else {
    rest = guest;
  }
  while (*rest == '\\' || *rest == '/') ++rest;
  int n = snprintf(out, out_size, "D:\\%s", rest);
  if (n <= 0 || (size_t)n >= out_size) return false;
  for (char* p = out; *p; ++p) {
    if (*p == '/') *p = '\\';
  }
  // Drop a trailing separator (directory opens).
  size_t len = strlen(out);
  if (len > 3 && out[len - 1] == '\\') out[len - 1] = 0;
  return true;
}

void read_object_name(u32 object_attributes, char* out, size_t out_size) {
  out[0] = 0;
  u32 name = gw_load32(object_attributes + 4);
  if (!name) return;
  u16 length = gw_load16(name);
  u32 buffer = gw_load32(name + 4);
  size_t n = length < out_size - 1 ? length : out_size - 1;
  memcpy(out, gw_host(buffer), n);
  out[n] = 0;
}

void set_io_status(u32 io_status, u32 status, u32 information) {
  if (!io_status) return;
  gw_store32(io_status, status);
  gw_store32(io_status + 4, information);
}

u32 open_file(u32 handle_out, u32 object_attributes, u32 io_status, u32 disposition) {
  char guest[260];
  read_object_name(object_attributes, guest, sizeof(guest));
  // The root is either one of our directory handles or a system pseudo
  // handle (such as the \?? object directory), which needs no joining.
  auto* dir = (GwFile*)gw_file_from_handle(gw_load32(object_attributes));
  char joined[300];
  if (dir) {
    snprintf(joined, sizeof(joined), "%s\\%s", dir->path, guest);
  } else {
    snprintf(joined, sizeof(joined), "%s", guest);
  }
  auto* file = new GwFile{};
  if (dir) {
    snprintf(file->path, sizeof(file->path), "%s", joined);
  } else if (!map_path(joined, file->path, sizeof(file->path))) {
    delete file;
    set_io_status(io_status, X_STATUS_OBJECT_PATH_NOT_FOUND, 0);
    return X_STATUS_OBJECT_PATH_NOT_FOUND;
  }
  DWORD attributes = GetFileAttributesA(file->path);
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    gw_log("open %s -> not found (%s)\n", guest, file->path);
    delete file;
    set_io_status(io_status, X_STATUS_OBJECT_NAME_NOT_FOUND, 0);
    return X_STATUS_OBJECT_NAME_NOT_FOUND;
  }
  if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
    file->directory = true;
    file->host = INVALID_HANDLE_VALUE;
  } else {
    file->host = CreateFileA(file->path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file->host == INVALID_HANDLE_VALUE) {
      gw_log("open %s -> CreateFile failed\n", file->path);
      delete file;
      set_io_status(io_status, X_STATUS_ACCESS_DENIED, 0);
      return X_STATUS_ACCESS_DENIED;
    }
  }
  file->find = INVALID_HANDLE_VALUE;
  gw_store32(handle_out, gw_handle_for_file(file));
  set_io_status(io_status, X_STATUS_SUCCESS, 1 /* FILE_OPENED */);
  ++gw_stats.files_opened;
  gw_log("open %s (disposition %u)\n", guest, disposition);
  return X_STATUS_SUCCESS;
}

u64 file_size(GwFile* f) {
  if (f->directory) return 0;
  LARGE_INTEGER size;
  GetFileSizeEx(f->host, &size);
  return (u64)size.QuadPart;
}

}  // namespace

void gw_file_close(void* p) {
  auto* f = (GwFile*)p;
  if (!f) return;  // handle-only objects (sessions, enumerators, listeners)
  if (f->host != INVALID_HANDLE_VALUE) CloseHandle(f->host);
  if (f->find != INVALID_HANDLE_VALUE) FindClose(f->find);
  delete f;
}

GW_EXPORT(NtCreateFile) { GW_RETURN(open_file(GW_ARG(3), GW_ARG(5), GW_ARG(6), GW_ARG(10))); }
GW_EXPORT(NtOpenFile) { GW_RETURN(open_file(GW_ARG(3), GW_ARG(5), GW_ARG(6), 1)); }

// NtReadFile(handle, event, apc_routine, apc_context, io_status, buffer, length, byte_offset_ptr)
GW_EXPORT(NtReadFile) {
  auto* f = (GwFile*)gw_file_from_handle(GW_ARG(3));
  u32 event = GW_ARG(4), io_status = GW_ARG(7), buffer = GW_ARG(8), length = GW_ARG(9);
  u32 offset_ptr = GW_ARG(10);
  if (!f || f->directory) GW_RETURN(X_STATUS_INVALID_HANDLE);
  if (offset_ptr) {
    i64 offset = (i64)gw_load64(offset_ptr);
    if (offset >= 0) {
      LARGE_INTEGER pos;
      pos.QuadPart = offset;
      SetFilePointerEx(f->host, pos, nullptr, FILE_BEGIN);
    }
  }
  // The kernel cannot fault pages in for us mid-read.
  gw_commit_range(buffer, length);
  DWORD done = 0;
  BOOL ok = ReadFile(f->host, gw_host(buffer), length, &done, nullptr);
  u32 status = !ok ? X_STATUS_UNSUCCESSFUL : (done == 0 && length) ? X_STATUS_END_OF_FILE : X_STATUS_SUCCESS;
  set_io_status(io_status, status, done);
  gw_stats.bytes_read += done;
  if (event) {
    gw_signal_guest_event_handle(event);
  }
  GW_RETURN(status);
}

GW_EXPORT(NtReadFileScatter) {
  gw_log("NtReadFileScatter not supported\n");
  ++gw_stats.unimplemented;
  GW_RETURN(X_STATUS_NOT_IMPLEMENTED);
}

GW_EXPORT(NtWriteFile) {
  set_io_status(GW_ARG(7), X_STATUS_SUCCESS, GW_ARG(9));
  GW_RETURN(X_STATUS_SUCCESS);
}

GW_EXPORT(NtFlushBuffersFile) {
  set_io_status(GW_ARG(4), X_STATUS_SUCCESS, 0);
  GW_RETURN(X_STATUS_SUCCESS);
}

// NtQueryInformationFile(handle, io_status, info, length, class)
GW_EXPORT(NtQueryInformationFile) {
  auto* f = (GwFile*)gw_file_from_handle(GW_ARG(3));
  u32 io_status = GW_ARG(4), info = GW_ARG(5), length = GW_ARG(6), cls = GW_ARG(7);
  if (!f) GW_RETURN(X_STATUS_INVALID_HANDLE);
  memset(gw_host(info), 0, length);
  u64 size = file_size(f);
  u32 written = 0;
  switch (cls) {
    case 5:  // FileStandardInformation
      gw_store64(info + 0, (size + 0xFFF) & ~0xFFFull);
      gw_store64(info + 8, size);
      gw_store32(info + 16, 1);
      gw_store8(info + 21, f->directory);
      written = 24;
      break;
    case 14: {  // FilePositionInformation
      LARGE_INTEGER zero{}, pos{};
      if (!f->directory) SetFilePointerEx(f->host, zero, &pos, FILE_CURRENT);
      gw_store64(info, (u64)pos.QuadPart);
      written = 8;
      break;
    }
    case 34:  // FileNetworkOpenInformation
      gw_store64(info + 32, (size + 0xFFF) & ~0xFFFull);
      gw_store64(info + 40, size);
      gw_store32(info + 48, f->directory ? 0x10 : 0x80);
      written = 56;
      break;
    case 4:  // FileBasicInformation
      gw_store32(info + 32, f->directory ? 0x10 : 0x80);
      written = 40;
      break;
    default:
      gw_log("NtQueryInformationFile class %u\n", cls);
      ++gw_stats.unimplemented;
      set_io_status(io_status, X_STATUS_INVALID_PARAMETER, 0);
      GW_RETURN(X_STATUS_INVALID_PARAMETER);
  }
  set_io_status(io_status, X_STATUS_SUCCESS, written);
  GW_RETURN(X_STATUS_SUCCESS);
}

// NtSetInformationFile(handle, io_status, info, length, class)
GW_EXPORT(NtSetInformationFile) {
  auto* f = (GwFile*)gw_file_from_handle(GW_ARG(3));
  u32 io_status = GW_ARG(4), info = GW_ARG(5), cls = GW_ARG(7);
  if (!f) GW_RETURN(X_STATUS_INVALID_HANDLE);
  if (cls == 14 && !f->directory) {
    LARGE_INTEGER pos;
    pos.QuadPart = (i64)gw_load64(info);
    SetFilePointerEx(f->host, pos, nullptr, FILE_BEGIN);
  }
  set_io_status(io_status, X_STATUS_SUCCESS, 0);
  GW_RETURN(X_STATUS_SUCCESS);
}

// NtQueryVolumeInformationFile(handle, io_status, info, length, class)
GW_EXPORT(NtQueryVolumeInformationFile) {
  u32 io_status = GW_ARG(4), info = GW_ARG(5), length = GW_ARG(6), cls = GW_ARG(7);
  memset(gw_host(info), 0, length);
  switch (cls) {
    case 1:  // FileFsVolumeInformation
      gw_store32(info + 8, 0x12345678);
      break;
    case 3:  // FileFsSizeInformation
      gw_store64(info + 0, 0x100000);
      gw_store64(info + 8, 0);
      gw_store32(info + 16, 1);
      gw_store32(info + 20, 0x800);
      break;
    case 5:  // FileFsAttributeInformation
      gw_store32(info + 0, 0);
      gw_store32(info + 4, 255);
      gw_store32(info + 8, 4);
      memcpy(gw_host(info + 12), "GDFX", 4);
      break;
    default:
      gw_log("NtQueryVolumeInformationFile class %u\n", cls);
      break;
  }
  set_io_status(io_status, X_STATUS_SUCCESS, length);
  GW_RETURN(X_STATUS_SUCCESS);
}

// NtQueryDirectoryFile(handle, event, apc, apc_ctx, io_status, info, length, name, restart)
GW_EXPORT(NtQueryDirectoryFile) {
  auto* f = (GwFile*)gw_file_from_handle(GW_ARG(3));
  u32 io_status = GW_ARG(7), info = GW_ARG(8), length = GW_ARG(9), pattern = GW_ARG(10);
  if (!f || !f->directory) GW_RETURN(X_STATUS_INVALID_HANDLE);
  WIN32_FIND_DATAA data;
  BOOL found;
  if (f->find == INVALID_HANDLE_VALUE) {
    char mask[64] = "*";
    if (pattern) {
      u16 n = gw_load16(pattern);
      if (n && n < sizeof(mask)) {
        memcpy(mask, gw_host(gw_load32(pattern + 4)), n);
        mask[n] = 0;
      }
    }
    char search[300];
    snprintf(search, sizeof(search), "%s\\%s", f->path, mask);
    f->find = FindFirstFileA(search, &data);
    found = f->find != INVALID_HANDLE_VALUE;
  } else {
    found = FindNextFileA(f->find, &data);
  }
  if (!found) {
    set_io_status(io_status, X_STATUS_NO_MORE_FILES, 0);
    GW_RETURN(X_STATUS_NO_MORE_FILES);
  }
  // FILE_DIRECTORY_INFORMATION
  u32 name_len = (u32)strlen(data.cFileName);
  if (0x40 + name_len > length) GW_RETURN(X_STATUS_INVALID_PARAMETER);
  memset(gw_host(info), 0, 0x40 + name_len);
  u64 size = ((u64)data.nFileSizeHigh << 32) | data.nFileSizeLow;
  gw_store64(info + 0x28, size);
  gw_store64(info + 0x30, (size + 0xFFF) & ~0xFFFull);
  gw_store32(info + 0x38, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0x10 : 0x80);
  gw_store32(info + 0x3C, name_len);
  memcpy(gw_host(info + 0x40), data.cFileName, name_len);
  set_io_status(io_status, X_STATUS_SUCCESS, 0x40 + name_len);
  GW_RETURN(X_STATUS_SUCCESS);
}

GW_EXPORT(ObCreateSymbolicLink) { GW_RETURN(X_STATUS_SUCCESS); }
GW_EXPORT(ObDeleteSymbolicLink) { GW_RETURN(X_STATUS_SUCCESS); }
