// High scores, kept on the console as an arcade table with initials.
//
// The game keeps no scores in its profile: each finished single-player
// game's score goes to its Xbox Live leaderboard, and every board it shows
// comes back from Live (the mode select panel and the game-over screen's
// friends board through XUserReadStats, the Leaderboards screen through the
// stats enumerators). As in the PC port (src/live_fix.cpp, docs/offline-live.md)
// the local player is reported signed in to Live and those calls are
// answered here, from a top-10 table per leaderboard view (the game's mode
// index) with three initials per entry.
//
// A finished game's score is read from the game's score task (sub_8207F180)
// before the game decides whether to submit it (it skips scores below the
// session's best), so every top-10 score gets in. The new entry shows at
// once; its initials are entered on the game-over screen, whose button
// prompt line then reads "Enter your initials: ABC" (the prompt bar's own
// label, its colour, size and centring, with that text): up and down change
// the blinking letter, A or right moves on (B or left back), A on the last
// letter (or Start) keeps them. Meanwhile the game sees a neutral
// controller. The last initials entered come pre-filled. The game-over
// board shows one row, the best (the game reads it as a game starts); the
// Leaderboards screen lists the whole top 10.
//
// Saved on every entry, next to the profile:
//   E:\UDATA\584108FF\000000000000\scores.dat
// 'GWSC', version 2, the last initials (4 bytes), then 16 views x 10 entries
// of {signed 64-bit score, 4 bytes initials, 4 zero}, then an FNV-1a hash of
// everything before it (little-endian); written whole to scores.new, flushed
// and moved over scores.dat (gw_write_save_file). Version 1 (one best per
// view, no initials) is read as that score under "---".
//
// Every write into the game's memory stays inside a buffer and size the game
// passed, or (the initials being entered, rewritten as they change) inside a
// row this file wrote and only while that row still holds what it wrote.
#include "runtime.h"

#include <string.h>
#include <windows.h>

namespace {

constexpr u32 kErrorSuccess = 0;
constexpr u32 kErrorInvalidParameter = 0x57;
constexpr u32 kErrorInsufficientBuffer = 0x7A;
constexpr u32 kErrorIoPending = 0x3E5;
constexpr u32 kSignedInLocally = 1;
constexpr u32 kSignedInToLive = 2;

// Guest layouts: XUSER_STATS_READ_RESULTS (8), XUSER_STATS_VIEW (16),
// XUSER_STATS_ROW (48: XUID, rank, rating at 16, gamertag at 24),
// XUSER_STATS_SPEC (136), XSESSION_VIEW_PROPERTIES (12), XUSER_PROPERTY (24).
constexpr u32 kResultsHeaderSize = 8, kViewSize = 16, kRowSize = 48, kSpecSize = 136;
constexpr u32 kRowGamertag = 24, kGamertagSize = 16;
constexpr u32 kPropertySize = 24;
constexpr u32 kScorePropertyId = 0x20000001;  // X_PROPERTY_SCORE, 64-bit integer
constexpr u8 kTypeInt64 = 2;
constexpr u32 kViews = 16;     // views kept (the game uses 0..5)
constexpr u32 kTop = 10;       // entries per view
constexpr u32 kMaxSpecs = 16;  // views asked for in one read

// The game's score task (sub_8207F180, 'this' in r3): its state at +36 (0
// before the submit decision), the score at +88, XSESSION_VIEW_PROPERTIES
// at +96.
constexpr u32 kTaskState = 36, kTaskScore = 88, kTaskView = 96;

// The prompt bar (sub_820924A0) draws its button prompt lines with
// sub_820770E8: r3 a text line, its segments (40 bytes each: text pointer,
// colour, size, ... kind at +32, 3 a line break) at +512, their count at
// +1476.
constexpr u32 kPromptLineReturns[3] = {0x820925C0, 0x82092900, 0x82092970};
constexpr u32 kLineSegments = 512, kLineCount = 1476, kLineSize = 1480, kSegmentSize = 40;
constexpr u32 kMaxSegments = (kLineCount - kLineSegments) / kSegmentSize;
constexpr char kPrompt[] = "Enter your initials: ";
constexpr u32 kPromptSize = sizeof(kPrompt) + 3;

constexpr char kScoresFile[] = "E:\\UDATA\\584108FF\\000000000000\\scores.dat";
constexpr char kScoresNew[] = "E:\\UDATA\\584108FF\\000000000000\\scores.new";
constexpr u32 kScoresMagic = 0x43535747;  // "GWSC"
constexpr u32 kEntryBytes = 16;
constexpr u32 kFileSize = 12 + kViews * kTop * kEntryBytes + 4;
constexpr u32 kFileSizeV1 = 8 + kViews * 8 + 4;

struct Entry {
  i64 score;  // 0: empty
  char name[4];
};

CRITICAL_SECTION g_lock;
volatile LONG g_lock_state;  // 0 none, 1 being made, 2 ready
bool g_loaded;
Entry g_table[kViews][kTop];
char g_last[4] = "AAA";

// The game-over screen (sub_82088AE0): its Previous Scores names are drawn
// by the text call returning to kPreviousScoreNameReturn; its friends
// board's names and scores by the calls returning to kFriendsNameReturn
// and kFriendsScoreReturn (the text trace, make TEXT_TRACE=y).
constexpr u32 kPreviousScoreNameReturn = 0x82088D24;
constexpr u32 kFriendsNameReturn = 0x82088FF0, kFriendsScoreReturn = 0x82089030;

// Initials being entered.
struct Edit {
  bool active;
  u32 view, index;
  char name[4];
  u32 cursor;
  DWORD started;      // the blink's phase
  DWORD opened;       // the entry began (the game's end)
  DWORD next_repeat;  // held up/down repeats after this
  int held;           // -1 down, 1 up, 0 none
  u16 previous[4];    // each controller's buttons last frame
  bool any_previous[4];
  // Rows of results buffers that show the entry (a board read while it is
  // being entered), with the text last written into each.
  u32 shown[4];
  char shown_text[4][kGamertagSize];
  u32 shown_count;
  u32 prompt_frame;  // the frame the prompt text was last replaced in
};
Edit g_edit;
u32 g_prompt_text;  // kPrompt and the initials, in the runtime's own guest memory
u32 g_prompt_line;  // a copy of a prompt line there, one segment: kPrompt


// This session's finished games (the game-over screen's Previous Scores
// list): each score with the initials it was entered under, or the last
// initials when it made no top 10.
struct Recent {
  i64 score;
  char name[4];
};
Recent g_recent[16];
u32 g_recent_count;

// Score tasks already taken (a task may run its first step more than once).
u32 g_taken_task;
i64 g_taken_score;
DWORD g_taken_at;

void lock() {
  if (InterlockedCompareExchange(&g_lock_state, 1, 0) == 0) {
    InitializeCriticalSection(&g_lock);
    InterlockedExchange(&g_lock_state, 2);
  }
  while (g_lock_state != 2) Sleep(0);
  EnterCriticalSection(&g_lock);
}
void unlock() { LeaveCriticalSection(&g_lock); }

u32 fnv1a(const u8* p, u32 n) {
  u32 h = 2166136261u;
  for (u32 i = 0; i < n; ++i) h = (h ^ p[i]) * 16777619u;
  return h;
}

bool valid_letter(char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' '; }

void clean_name(char* name) {
  for (u32 i = 0; i < 3; ++i)
    if (!valid_letter(name[i])) name[i] = '-';
  name[3] = 0;
}

bool read_scores(const char* path) {
  HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  static u8 buf[kFileSize + 1];
  DWORD size = 0;
  const BOOL ok = ReadFile(f, buf, sizeof(buf), &size, nullptr);
  CloseHandle(f);
  if (!ok || size < 12) return false;
  u32 magic, version, hash;
  memcpy(&magic, buf, 4);
  memcpy(&version, buf + 4, 4);
  memcpy(&hash, buf + size - 4, 4);
  if (magic != kScoresMagic || hash != fnv1a(buf, size - 4)) return false;
  if (version == 1 && size == kFileSizeV1) {
    for (u32 v = 0; v < kViews; ++v) {
      i64 best;
      memcpy(&best, buf + 8 + v * 8, 8);
      if (best > 0) {
        g_table[v][0].score = best;
        memcpy(g_table[v][0].name, "---", 4);
      }
    }
    return true;
  }
  if (version != 2 || size != kFileSize) return false;
  memcpy(g_last, buf + 8, 4);
  clean_name(g_last);
  for (u32 v = 0; v < kViews; ++v) {
    for (u32 i = 0; i < kTop; ++i) {
      const u8* e = buf + 12 + (v * kTop + i) * kEntryBytes;
      memcpy(&g_table[v][i].score, e, 8);
      memcpy(g_table[v][i].name, e + 8, 4);
      if (g_table[v][i].score < 0) g_table[v][i].score = 0;
      clean_name(g_table[v][i].name);
    }
  }
  return true;
}

// (Under the lock.) Once, on the first score call: the save folder is ready
// by then (the game reads its profile first).
void load() {
  if (g_loaded) return;
  g_loaded = true;
  if (!gw_saves_ready()) {
    gw_log("scores: no save folder; high scores kept for this session only\n");
    return;
  }
  if (read_scores(kScoresFile)) gw_log("scores: read from %s\n", kScoresFile);
  else if (read_scores(kScoresNew)) gw_log("scores: read from %s (the last write was not finished)\n", kScoresNew);
  else gw_log("scores: none saved yet\n");
}

// (Under the lock.)
void save() {
  static u8 buf[kFileSize];
  memset(buf, 0, sizeof(buf));
  const u32 head[2] = {kScoresMagic, 2};
  memcpy(buf, head, 8);
  memcpy(buf + 8, g_last, 4);
  for (u32 v = 0; v < kViews; ++v) {
    for (u32 i = 0; i < kTop; ++i) {
      u8* e = buf + 12 + (v * kTop + i) * kEntryBytes;
      memcpy(e, &g_table[v][i].score, 8);
      memcpy(e + 8, g_table[v][i].name, 4);
    }
  }
  const u32 hash = fnv1a(buf, kFileSize - 4);
  memcpy(buf + kFileSize - 4, &hash, 4);
  if (!gw_write_save_file(kScoresFile, kScoresNew, buf, kFileSize))
    gw_log("scores: not saved; the table is kept for this session only\n");
}

// (Under the lock.) The initials as a row shows them: the letter being
// entered blinks.
void edit_text(char* out) {
  memset(out, 0, kGamertagSize);
  memcpy(out, g_edit.name, 3);
  if (((GetTickCount() - g_edit.started) / 250) % 2) out[g_edit.cursor] = ' ';
}

// (Under the lock.) A finished game's score: into the view's table when it
// makes the top 10, its initials then entered.
void take_score(u32 view, i64 score) {
  if (view >= kViews || score <= 0) return;
  load();
  {
    Recent& r = g_recent[g_recent_count++ % 16];
    r.score = score;
    memcpy(r.name, g_last, 4);
  }
  if (g_edit.active) {
    gw_log("scores: view %u score %lld while initials are entered; ignored\n", view, (long long)score);
    return;
  }
  u32 at = 0;
  while (at < kTop && g_table[view][at].score >= score) ++at;
  if (at == kTop) {
    gw_log("scores: view %u score %lld not in the top %u\n", view, (long long)score, kTop);
    return;
  }
  memmove(&g_table[view][at + 1], &g_table[view][at], (kTop - 1 - at) * sizeof(Entry));
  g_table[view][at].score = score;
  memcpy(g_table[view][at].name, g_last, 4);
  memset(&g_edit, 0, sizeof(g_edit));
  g_edit.active = true;
  g_edit.view = view;
  g_edit.index = at;
  memcpy(g_edit.name, g_last, 4);
  g_edit.started = g_edit.opened = GetTickCount();
  gw_log("scores: view %u score %lld is number %u; initials being entered\n", view, (long long)score, at + 1);
}

// (Under the lock.)
void finish_edit() {
  clean_name(g_edit.name);
  memcpy(g_table[g_edit.view][g_edit.index].name, g_edit.name, 4);
  memcpy(g_last, g_edit.name, 4);
  // This game's Previous Scores row under the initials just entered.
  if (g_recent_count) {
    Recent& r = g_recent[(g_recent_count - 1) % 16];
    if (r.score == g_table[g_edit.view][g_edit.index].score) memcpy(r.name, g_edit.name, 4);
  }
  // The rows showing the entry get the plain initials.
  for (u32 k = 0; k < g_edit.shown_count; ++k) {
    const u32 at = g_edit.shown[k];
    if (memcmp(gw_host(at), g_edit.shown_text[k], kGamertagSize) != 0) continue;
    memset(gw_host(at), 0, kGamertagSize);
    memcpy(gw_host(at), g_edit.name, 3);
  }
  gw_log("scores: view %u number %u entered as \"%s\"\n", g_edit.view, g_edit.index + 1, g_edit.name);
  g_edit.active = false;
  save();
}

char next_letter(char c, int step) {
  static const char kLetters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ";
  constexpr int n = sizeof(kLetters) - 1;
  int i = 0;
  while (i < n && kLetters[i] != c) ++i;
  if (i == n) i = 0;
  return kLetters[((i + step) % n + n) % n];
}

// One view of a results buffer, with up to kTop rows.
struct View {
  u32 id;
  u32 rows;   // rows written
  u32 total;  // total_rows
  u32 first;  // the table index of the first row
};

u32 results_size(const View* views, u32 n) {
  u32 size = kResultsHeaderSize + kViewSize * n;
  for (u32 i = 0; i < n; ++i) size += kRowSize * views[i].rows;
  return size;
}

// (Under the lock.) XUSER_STATS_READ_RESULTS at 'buffer', at most
// 'capacity' bytes.
// Rows after a view's first get XUIDs of their own (the mode select panel
// lists the players other than the first by XUID; all of them are local).
bool g_distinct_rows;

bool write_results(u32 buffer, u32 capacity, const View* views, u32 n) {
  const u32 needed = results_size(views, n);
  if (!buffer || capacity < needed) return false;
  memset(gw_host(buffer), 0, needed);
  gw_store32(buffer, n);
  gw_store32(buffer + 4, buffer + kResultsHeaderSize);
  u32 view_at = buffer + kResultsHeaderSize;
  u32 row_at = view_at + kViewSize * n;
  for (u32 i = 0; i < n; ++i) {
    const View& v = views[i];
    gw_store32(view_at, v.id);
    gw_store32(view_at + 4, v.total);
    gw_store32(view_at + 8, v.rows);
    gw_store32(view_at + 12, v.rows ? row_at : 0);
    for (u32 r = 0; r < v.rows; ++r) {
      const u32 index = v.first + r;
      const Entry& e = g_table[v.id][index];
      gw_store64(row_at, g_distinct_rows && r ? kGwLocalXuid + r : kGwLocalXuid);
      gw_store32(row_at + 8, index + 1);  // rank
      gw_store64(row_at + 16, (u64)e.score);
      const u32 tag = row_at + kRowGamertag;
      if (g_edit.active && g_edit.view == v.id && g_edit.index == index) {
        char text[kGamertagSize];
        edit_text(text);
        memcpy(gw_host(tag), text, kGamertagSize);
        if (g_edit.shown_count < 4) {
          g_edit.shown[g_edit.shown_count] = tag;
          memcpy(g_edit.shown_text[g_edit.shown_count], text, kGamertagSize);
          ++g_edit.shown_count;
        }
      } else {
        memcpy(gw_host(tag), e.name, 3);
      }
      row_at += kRowSize;
    }
    view_at += kViewSize;
  }
  return true;
}

// (Under the lock.) Rows in a view's table.
u32 filled(u32 view) {
  u32 n = 0;
  while (n < kTop && g_table[view][n].score > 0) ++n;
  return n;
}

// As a console completes an XOVERLAPPED: result, items, extended result,
// then its event.
void complete(u32 overlapped, u32 result, u32 items) {
  gw_store32(overlapped + 0x18, result);
  gw_store32(overlapped + 4, items);
  gw_store32(overlapped, result);
  if (u32 event = gw_load32(overlapped + 0x0C)) gw_signal_guest_event_handle(event);
}

// Enumerations the port answers (handles 0xEC000000 + n, away from the
// runtime's own): each is served once and dropped.
struct Query {
  u32 handle;
  bool ranked;  // by rank (pivot = first rank), else around a player (pivot = XUID)
  u64 pivot;
  u32 max_rows;
  u32 views;
  u32 view[kMaxSpecs];
};
constexpr u32 kMaxQueries = 64;
Query g_queries[kMaxQueries];
u32 g_next_handle = 1, g_next_slot;

}  // namespace

// --- the controller while initials are entered (src/xam.cpp, XamInputGetState) ---

// Called with each controller's XINPUT_GAMEPAD as the game reads it: while
// initials are entered it drives the entry and the game sees it neutral.
void gw_scores_input(u32 user, u32 gamepad) {
  if (!g_edit.active || user >= 4) return;
  lock();
  if (!g_edit.active) {
    unlock();
    return;
  }
  constexpr u16 kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kStart = 0x0010;
  constexpr u16 kA = 0x1000, kB = 0x2000;
  u16 buttons = gw_load16(gamepad);
  const i16 stick_y = (i16)gw_load16(gamepad + 6);
  const i16 stick_x = (i16)gw_load16(gamepad + 4);
  if (stick_y > 16000) buttons |= kUp;
  if (stick_y < -16000) buttons |= kDown;
  if (stick_x > 16000) buttons |= kRight;
  if (stick_x < -16000) buttons |= kLeft;
  // Nothing counts until the game-over screen is up (a stick still held as
  // the game ended stepped through the letters), and what is held then is
  // not a press.
  const DWORD now = GetTickCount();
  if (now - g_edit.opened < 1500) {
    g_edit.any_previous[user] = false;
    memset(gw_host(gamepad), 0, 12);
    unlock();
    return;
  }
  const u16 previous = g_edit.any_previous[user] ? g_edit.previous[user] : buttons;
  const u16 pressed = buttons & ~previous;
  g_edit.previous[user] = buttons;
  g_edit.any_previous[user] = true;
  const int vertical = (buttons & kUp) ? 1 : (buttons & kDown) ? -1 : 0;
  if (pressed & (kUp | kDown)) {
    g_edit.name[g_edit.cursor] = next_letter(g_edit.name[g_edit.cursor], vertical);
    g_edit.held = vertical;
    g_edit.next_repeat = now + 400;
    g_edit.started = now;  // the changed letter shows at once
  } else if (vertical && vertical == g_edit.held && (int)(now - g_edit.next_repeat) >= 0) {
    g_edit.name[g_edit.cursor] = next_letter(g_edit.name[g_edit.cursor], vertical);
    g_edit.next_repeat = now + 90;
    g_edit.started = now;
  } else if (!vertical) {
    g_edit.held = 0;
  }
  bool done = (pressed & kStart) != 0;
  if (pressed & (kA | kRight)) {
    if (g_edit.cursor < 2) {
      ++g_edit.cursor;
      g_edit.started = now;
    } else if (pressed & kA) {
      done = true;
    }
  }
  if ((pressed & (kB | kLeft)) && g_edit.cursor > 0) {
    --g_edit.cursor;
    g_edit.started = now;
  }
  if (done) {
    finish_edit();
  } else {
    // The rows showing the entry, kept up to date (the blink, the letters).
    char text[kGamertagSize];
    edit_text(text);
    for (u32 k = 0; k < g_edit.shown_count; ++k) {
      const u32 at = g_edit.shown[k];
      if (memcmp(gw_host(at), g_edit.shown_text[k], kGamertagSize) != 0) continue;  // no longer ours
      memcpy(gw_host(at), text, kGamertagSize);
      memcpy(g_edit.shown_text[k], text, kGamertagSize);
    }
  }
  // Neutral for the game, including the press that finished the entry.
  memset(gw_host(gamepad), 0, 12);
  if (done) {
    for (u32 u = 0; u < 4; ++u) g_edit.any_previous[u] = false;
  }
  unlock();
}

// --- the game's Live calls (the PC port hooks the same functions) -------------------------

extern "C" void __imp__sub_820955C8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8207F180(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_820956B8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82095718(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_820958D0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_820770E8(PPCContext& ctx, uint8_t* base);

// XUserGetSigninState(user): the game only scores and reads leaderboards for
// a player signed in to Live; the local player is reported so.
extern "C" void sub_820955C8(PPCContext& ctx, uint8_t* base) {
  const u32 user = ctx.r3.u32;
  __imp__sub_820955C8(ctx, base);
  if (user == 0 && ctx.r3.u32 == kSignedInLocally) ctx.r3.u64 = kSignedInToLive;
}

// The game's score task: the finished game's score taken before the game
// decides whether to submit it.
extern "C" void sub_8207F180(PPCContext& ctx, uint8_t* base) {
  const u32 task = ctx.r3.u32;
  if (task && gw_load32(task + kTaskState) == 0) {
    const u32 view = gw_load32(task + kTaskView), count = gw_load32(task + kTaskView + 4);
    const u32 properties = gw_load32(task + kTaskView + 8);
    const i64 score = (i64)gw_load64(task + kTaskScore);
    bool is_score = false;
    if (count >= 1 && properties) {
      is_score = gw_load32(properties) == kScorePropertyId && gw_load8(properties + 8) == kTypeInt64 &&
                 (i64)gw_load64(properties + 16) == score;
    }
    const DWORD now = GetTickCount();
    if (is_score && !(task == g_taken_task && score == g_taken_score && now - g_taken_at < 10000)) {
      g_taken_task = task;
      g_taken_score = score;
      g_taken_at = now;
      lock();
      take_score(view, score);
      unlock();
    }
  }
  __imp__sub_8207F180(ctx, base);
}

// A text line (sub_820770E8): the prompt bar's first button line of a frame
// reads "Enter your initials: ABC" while initials are entered (a copy of the line
// with one segment, the line's first text segment with that text), its
// other lines left out.
extern "C" void sub_820770E8(PPCContext& ctx, uint8_t* base) {
  const u32 lr = (u32)ctx.lr;
  const bool prompt = lr == kPromptLineReturns[0] || lr == kPromptLineReturns[1] || lr == kPromptLineReturns[2];
  if (!g_edit.active || !prompt) return __imp__sub_820770E8(ctx, base);
  if (g_edit.prompt_frame == gw_stats.swaps) return;
  g_edit.prompt_frame = gw_stats.swaps;
  if (!g_prompt_text) {
    g_prompt_text = gw_system_alloc(kPromptSize, 16);
    g_prompt_line = gw_system_alloc(kLineSize, 16);
  }
  {
    char text[kGamertagSize];
    lock();
    edit_text(text);
    unlock();
    char* out = (char*)gw_host(g_prompt_text);
    memcpy(out, kPrompt, sizeof(kPrompt) - 1);
    memcpy(out + sizeof(kPrompt) - 1, text, 3);
    out[kPromptSize - 1] = 0;
  }
  const u32 line = ctx.r3.u32;
  const u32 count = gw_load32(line + kLineCount);
  u32 pick = kMaxSegments;
  for (u32 i = 0; i < count && i < kMaxSegments && pick == kMaxSegments; ++i) {
    const u32 segment = line + kLineSegments + i * kSegmentSize;
    const u32 text = gw_load32(segment);
    if (!text || gw_load32(segment + 32) == 3) continue;
    u32 n = 0;
    while (n < 4 && gw_load8(text + n)) ++n;
    if (n >= 2) pick = i;  // text, not a one-character button glyph
  }
  if (pick == kMaxSegments) return __imp__sub_820770E8(ctx, base);
  memcpy(gw_host(g_prompt_line), gw_host(line), kLineSize);
  memcpy(gw_host(g_prompt_line + kLineSegments), gw_host(line + kLineSegments + pick * kSegmentSize), kSegmentSize);
  gw_store32(g_prompt_line + kLineSegments, g_prompt_text);
  gw_store32(g_prompt_line + kLineCount, 1);
  ctx.r3.u64 = g_prompt_line;
  __imp__sub_820770E8(ctx, base);
}

// The game-over screen's Previous Scores rows (sub_82088AE0): each row is the
// game's record of a finished game (score at +0, the player's name at +12),
// its name drawn with sub_82076200 (text in r10) returning to 0x82088D24.
// The row's initials are drawn instead, from this session's list (the
// game's record is left as it is).
// The friends board means nothing offline (the port has no friends): its
// rows are not drawn, so the panel is empty under its heading.
extern "C" void __imp__sub_82076200(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_820762F0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_820762F0(PPCContext& ctx, uint8_t* base) {
  if ((u32)ctx.lr == kFriendsScoreReturn) return;
  __imp__sub_820762F0(ctx, base);
}
extern "C" void sub_82076200(PPCContext& ctx, uint8_t* base) {
  if ((u32)ctx.lr == kFriendsNameReturn) return;
  if ((u32)ctx.lr == kPreviousScoreNameReturn && ctx.r10.u32) {
    static u32 texts;  // 8 rows x 8 bytes, the runtime's own guest memory
    static u32 next;
    if (!texts) texts = gw_system_alloc(64, 16);
    const i64 score = (i64)gw_load64(ctx.r10.u32 - 12);
    char name[4];
    lock();
    memcpy(name, g_last, 4);
    for (u32 k = 0; k < g_recent_count && k < 16; ++k)
      if (g_recent[k].score == score) memcpy(name, g_recent[k].name, 4);
    unlock();
    const u32 at = texts + (next++ % 8) * 8;
    memset(gw_host(at), 0, 8);
    memcpy(gw_host(at), name, 3);
    ctx.r10.u64 = at;
  }
  __imp__sub_82076200(ctx, base);
}

// XUserCreateStatsEnumeratorByRank / ByXuid(title, pivot, max rows, spec
// count, specs, size ptr, handle ptr).
static void create_enumerator(PPCContext& ctx, bool ranked) {
  const u32 max_rows = ctx.r5.u32, spec_count = ctx.r6.u32, specs = ctx.r7.u32;
  const u32 size_ptr = ctx.r8.u32, handle_ptr = ctx.r9.u32;
  if (!spec_count || spec_count > kMaxSpecs || !specs || !size_ptr || !handle_ptr) {
    ctx.r3.u64 = kErrorInvalidParameter;
    return;
  }
  lock();
  Query& q = g_queries[g_next_slot];
  g_next_slot = (g_next_slot + 1) % kMaxQueries;  // the oldest unserved one is dropped
  q.handle = 0xEC000000u | (g_next_handle++ & 0x00FFFFFF);
  q.ranked = ranked;
  q.pivot = ranked ? ctx.r4.u32 : ctx.r4.u64;
  q.max_rows = max_rows;
  q.views = spec_count;
  for (u32 i = 0; i < spec_count; ++i) q.view[i] = gw_load32(specs + i * kSpecSize);
  const u32 handle = q.handle;
  unlock();
  const u32 rows = max_rows < kTop ? max_rows : kTop;
  gw_store32(size_ptr, kResultsHeaderSize + (kViewSize + kRowSize * rows) * spec_count);
  gw_store32(handle_ptr, handle);
  ctx.r3.u64 = kErrorSuccess;
}

extern "C" void sub_820956B8(PPCContext& ctx, uint8_t* base) {
  if (ctx.r3.u32 != 0) return __imp__sub_820956B8(ctx, base);
  create_enumerator(ctx, true);
}

extern "C" void sub_82095718(PPCContext& ctx, uint8_t* base) {
  if (ctx.r3.u32 != 0) return __imp__sub_82095718(ctx, base);
  create_enumerator(ctx, false);
}

// XEnumerate(handle, buffer, size, items ptr, overlapped): the port's own
// handles answered here (the table's rows from the asked rank, or from the
// top around the local player), others passed on.
extern "C" void sub_820958D0(PPCContext& ctx, uint8_t* base) {
  const u32 handle = ctx.r3.u32;
  Query q = {};
  bool found = false;
  if ((handle & 0xFF000000u) == 0xEC000000u) {
    lock();
    for (Query& slot : g_queries) {
      if (slot.handle == handle) {
        q = slot;
        slot.handle = 0;
        found = true;
        break;
      }
    }
    unlock();
  }
  if (!found) return __imp__sub_820958D0(ctx, base);
  const u32 buffer = ctx.r4.u32, size = ctx.r5.u32, items_ptr = ctx.r6.u32, overlapped = ctx.r7.u32;
  lock();
  load();
  View views[kMaxSpecs];
  for (u32 i = 0; i < q.views; ++i) {
    const u32 id = q.view[i] < kViews ? q.view[i] : 0;
    const u32 total = q.view[i] < kViews ? filled(id) : 0;
    u32 first = 0;
    if (q.ranked) first = q.pivot > 1 ? (u32)(q.pivot - 1) : 0;
    else if (q.pivot != kGwLocalXuid) first = total;  // nobody else exists
    if (first > total) first = total;
    u32 rows = total - first;
    if (rows > q.max_rows) rows = q.max_rows;
    views[i] = {id, rows, total, first};
  }
  const u32 result = write_results(buffer, size, views, q.views) ? kErrorSuccess : kErrorInsufficientBuffer;
  unlock();
  const u32 items = result == kErrorSuccess ? 1 : 0;
  if (items_ptr) gw_store32(items_ptr, items);
  if (!overlapped) {
    ctx.r3.u64 = result;
    return;
  }
  complete(overlapped, result, items);
  ctx.r3.u64 = kErrorIoPending;
}

// XUserReadStats(title, xuid count, xuids, spec count, specs, size ptr,
// results, overlapped): the mode select panel's board and the game-over
// screen's friends board, one row per player asked for: the local player's
// best.
extern "C" void sub_820955D0(PPCContext& ctx, uint8_t* base) {
  const u32 spec_count = ctx.r6.u32, specs = ctx.r7.u32;
  const u32 size_ptr = ctx.r8.u32, results = ctx.r9.u32, overlapped = ctx.r10.u32;
  if (!specs || !spec_count || spec_count > kMaxSpecs || !size_ptr) {
    ctx.r3.u64 = kErrorInvalidParameter;
    return;
  }
  lock();
  load();
  View views[kMaxSpecs];
  for (u32 i = 0; i < spec_count; ++i) {
    const u32 id = gw_load32(specs + i * kSpecSize);
    // The top three (the mode select panel's list; the game-over screen's
    // friends board, read the same way, is not drawn: sub_820762F0 below).
    u32 rows = id < kViews ? filled(id) : 0;
    if (rows > 3) rows = 3;
    views[i] = {id < kViews ? id : 0, rows, rows, 0};
  }
  const u32 needed = results_size(views, spec_count);
  if (!results || gw_load32(size_ptr) < needed) {
    unlock();
    gw_store32(size_ptr, needed);
    ctx.r3.u64 = kErrorInsufficientBuffer;
    return;
  }
  g_distinct_rows = true;
  write_results(results, gw_load32(size_ptr), views, spec_count);
  g_distinct_rows = false;
  unlock();
  if (!overlapped) {
    ctx.r3.u64 = kErrorSuccess;
    return;
  }
  complete(overlapped, kErrorSuccess, 0);
  ctx.r3.u64 = kErrorIoPending;
}
