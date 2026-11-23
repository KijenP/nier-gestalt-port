// nier - pointing at the game's list menus with the mouse (PC).
//
// The rows of the list menus (title and pause Options, Game Settings, the PC pages) are 2D layout nodes the game
// looks up by name: list A (6 rows, "=_txt_01_sys".."06") and list B (9 rows, "=_txt_A01_sys_exp".."A09"). A node
// holds its position relative to its parent (+0x0C x, +0x10 y, +0x08 parent), in the game's 1280x720 space; the
// sum along the parents is where the row is drawn (measured 2026-11-12: A row 0 at 243,273, rows 39 apart; B rows 38
// apart). video_options_menu.cpp looks the nodes up (CacheMenuNodes) when the game draws or builds a list; the input
// gate (input_recorder.h) calls MenuMouse::Apply each poll.
//
// Pointing moves the game's own cursor there by pressing up/down for it (so the game plays its sounds, scrolls and
// draws its cursor), one step every other poll. A left click on a row that is not the selected one waits for the
// cursor to get there before the game sees A. On a value row (a choice or a bar) the click is the arrow on that
// side of the value instead, and on its label it only selects the row (2026-11-13: A cycled the value one way).

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <rex/logging.h>

#include <rex/input/input_driver.h>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace nier::menu_mouse {
constexpr int kRowsA = 6, kRowsB = 9;

struct Nodes {
  std::atomic<uint32_t> obj{0};  // guest address of the list menu object the nodes belong to
  std::atomic<uint64_t> drawn_ms{0};  // when the game last drew that list (sub_82419518)
  std::atomic<int> scroll_request{0};  // wheel steps for the settings list (+ down), taken by the game's tick
  std::atomic<uint32_t> a[kRowsA] = {};
  std::atomic<uint32_t> b[kRowsB] = {};
};

inline Nodes& nodes() {
  static Nodes n;
  return n;
}

inline uint32_t Be32At(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

// Where the node is drawn, in the game's 1280x720 space.
inline bool NodePos(const uint8_t* base, uint32_t node, int& x, int& y) {
  if (node < 0x1000 || node >= 0x20000000) return false;
  x = y = 0;
  for (int depth = 0; depth < 8 && node >= 0x1000 && node < 0x20000000; ++depth) {
    x += int32_t(Be32At(base, node + 0x0C));
    y += int32_t(Be32At(base, node + 0x10));
    node = Be32At(base, node + 0x08);
  }
  return true;
}

// Where the cursor of the other menus is (measured 2026-11-13 with a scan of the game's calls):
// - the book's pages (Weapons, Items, Quests, Notes, Map; also the Options lists) place their cursor on the
//   selected row with sub_82405028 (r3 = the page, r4 = the cursor's sprite, r5 = the row's node name); the
//   row icons go through it too, each always on its own row, so a sprite is the cursor once it has moved.
// - the system windows (the title's and the pause's cross, New Game / Load, the save lists, dialogs) mark the
//   selected row's plate with sub_823F4518 & co. (r4 = the window, r5 = the plate's node; 0 = none, closing).
// Both are called when the cursor moves or a screen opens, not every frame: the latest one is the screen on
// top. The rows are that cursor node's siblings: the nodes of the same owner whose names differ only in the
// row number ("=_txt_03_item_a" -> "=_txt_##_item_a"), learned from the game's own lookups (sub_823BC8F0).
class Tracker {
 public:
  enum Kind { kNone = 0, kList = 1, kBook = 2, kSystem = 3 };
  struct Active {
    Kind kind = kNone;
    uint32_t owner = 0, cursor = 0;  // the rows' lookup owner, the cursor's node (0 = not known yet)
    uint64_t version = 0;            // changes with every event
    std::vector<uint32_t> rows;      // system: all the window's rows
    bool cross = false;
  };

  static bool Wanted(const std::string& name) {
    return name.find("_txt_") != std::string::npos || name.find("sys_menu_win") != std::string::npos;
  }
  void OnLookup(uint32_t owner, const std::string& name, uint32_t node) {
    if (node < 0x1000 || node >= 0x20000000) return;
    std::lock_guard lock(m_);
    if (by_node_.size() > 20000) {  // a long session: start over (the menus look their rows up again)
      by_node_.clear();
      by_name_.clear();
    }
    by_node_[node] = {owner, name};
    by_name_[{owner, name}] = node;
  }
  // sub_82405028 / sub_82404F40: something placed on a page's row. is_cursor: it is the page's cursor (its
  // +2808; the row icons go through the same call). list_obj: the Options lists' object (own code).
  void OnPlace(uint32_t page, bool is_cursor, const std::string& name, uint32_t list_obj) {
    if (!is_cursor) return;
    std::lock_guard lock(m_);
    ++seq_;
    if (page == list_obj) {
      list_seq_ = seq_;
      return;
    }
    book_page_ = page;
    book_name_ = name;
    book_seq_ = seq_;
  }
  // sub_823F4518 & co.: a system window's cursor moved or a window opened / closed.
  void OnSystemEvent() {
    std::lock_guard lock(m_);
    sys_seq_ = ++seq_;
  }
  // From the windows' own update (each frame they show, video_options_menu.cpp): the active window, its
  // cursor's row and all its rows (cross: one row each way, moved by clicks only).
  void SetSystem(uint32_t window, uint32_t cursor, std::vector<uint32_t> rows, bool cross, uint64_t now_ms) {
    std::lock_guard lock(m_);
    // A window that (re)appears is the newest screen: the pause's cross opened over the Options list's object,
    // which still says it is open (its stale rows were pointed at and the cross took the presses, 2026-11-13).
    if (window != sys_window_ || cursor != sys_cursor_ || now_ms - sys_ms_ > 200) sys_seq_ = ++seq_;
    sys_window_ = window;
    sys_cursor_ = cursor;
    sys_rows_ = std::move(rows);
    sys_cross_ = cross;
    sys_ms_ = now_ms;
  }
  // book_open: Weiss's book is showing.
  // The Options list's cursor was placed after any window's or page's: the list is the newest screen.
  bool ListOnTop() {
    std::lock_guard lock(m_);
    return list_seq_ && list_seq_ > sys_seq_ && list_seq_ > book_seq_;
  }
  Active Get(bool book_open, uint64_t now_ms) {
    std::lock_guard lock(m_);
    Active a;
    a.version = seq_;
    // The Options lists' own code (Step) runs only while that list is the newest screen: its object keeps
    // saying it is open after it closes, and its stale rows pointed at moved the pause's cross (Weiss opened
    // by itself, 2026-11-13).
    // The Options list drawn just now is on screen (the pause's cross under it keeps running its update).
    if (now_ms - nodes().drawn_ms.load() < 150 || (list_seq_ && list_seq_ > sys_seq_ && list_seq_ > book_seq_)) {
      a.kind = kList;
      return a;
    }
    if (book_open) {
      // The book's pages: not yet (pressing the way to a row ran on over rows the cursor cannot reach). To be
      // done with each page's own "select this row", as the title's lists.
      a.kind = kNone;
      return a;
    }
    // The windows are on screen while their update runs.
    if (now_ms - sys_ms_ > 200 || sys_cursor_ == 0) {
      a.kind = kNone;
      return a;
    }
    a.kind = kSystem;
    a.owner = sys_window_;
    a.cursor = sys_cursor_;
    a.rows = sys_rows_;
    a.cross = sys_cross_;
    return a;
  }
  // The pause's cross is the screen on top (its manager ran just now, newer than any Options list event).
  bool CrossOnTop(uint64_t now_ms) {
    std::lock_guard lock(m_);
    // Not while the Options list is drawn (2026-11-23, the player): a hitch of over 200 ms (V-Sync turned on or off
    // rebuilds the swapchain) made the cross's next update count as the cross reappearing, newer than the list's last
    // cursor event, and the open page of ours was closed under the list still on screen.
    return sys_cross_ && now_ms - sys_ms_ < 200 && sys_seq_ > list_seq_ && now_ms - nodes().drawn_ms.load() >= 150;
  }
  uint64_t SystemMs() {
    std::lock_guard lock(m_);
    return sys_ms_;
  }
  std::pair<uint32_t, std::string> NodeInfo(uint32_t node) {
    std::lock_guard lock(m_);
    auto it = by_node_.find(node);
    return it == by_node_.end() ? std::pair<uint32_t, std::string>{} : it->second;
  }  // The cursor's row and its siblings (cursor first), with their names.
  std::vector<uint32_t> Siblings(uint32_t cursor) {
    std::lock_guard lock(m_);
    std::vector<uint32_t> out;
    auto it = by_node_.find(cursor);
    if (it == by_node_.end()) return out;
    const uint32_t owner = it->second.first;
    const std::string key = Key(it->second.second);
    if (key.empty()) return out;
    out.push_back(cursor);
    for (auto& [node, info] : by_node_) {
      if (node != cursor && info.first == owner && Key(info.second) == key) out.push_back(node);
    }
    return out;
  }

 private:
  // The name with its row number replaced: the first digits after the last ']' (or in the whole name).
  static std::string Key(const std::string& name) {
    size_t from = name.rfind(']');
    from = from == std::string::npos ? 0 : from + 1;
    const size_t a = name.find_first_of("0123456789", from);
    if (a == std::string::npos) return {};
    size_t b = a;
    while (b < name.size() && name[b] >= '0' && name[b] <= '9') ++b;
    return name.substr(0, a) + "#" + name.substr(b);
  }

  std::mutex m_;
  std::map<uint32_t, std::pair<uint32_t, std::string>> by_node_;   // node -> owner, name
  std::map<std::pair<uint32_t, std::string>, uint32_t> by_name_;  // owner, name -> node
  uint64_t seq_ = 0, list_seq_ = 0, sys_seq_ = 0, book_seq_ = 0;
  uint32_t book_page_ = 0;
  std::string book_name_;
  uint32_t sys_window_ = 0, sys_cursor_ = 0;
  std::vector<uint32_t> sys_rows_;
  bool sys_cross_ = false;
  uint64_t sys_ms_ = 0;
};

inline uint64_t NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

inline Tracker& tracker() {
  static Tracker t;
  return t;
}

// The title menu, which the mouse drives from the game's side (video_options_menu.cpp): when it last ran and
// the row under the pointer (-1: none, a click there does nothing).
struct TitleMenu {
  std::atomic<uint64_t> ms{0};
  std::atomic<int> hovered{-1};
  std::atomic<uint16_t> press{0};  // a button the game's side asks to be pressed (a click on an arrow)
  int last_x = -1, last_y = -1;  // the game's thread only
  std::atomic<uint64_t> moved_ms{0};  // when the pointer last moved the row (the game takes no A while its
                                      // highlight starts: a touch waits it out, touch_controls.h)
};
inline TitleMenu& title() {
  static TitleMenu t;
  return t;
}

// A menu the hooks drive (the title's lists, a choice dialog, the save list) took input in the last moments:
// the game is in a menu whatever its own flags say (the save list and the dialogs left them saying gameplay:
// the mouse turned to the camera and the cursor hid, 2026-11-13). The system windows are left out: their
// manager may run in gameplay too (not checked).
// Only the menus the game's own marks miss count (the title's lists, the choice dialogs, the save list: they
// call MarkOutsideMenu). The book's pages and their popups ran from code that may also run in gameplay (the
// book's manager each frame): counted, gameplay became a menu for good (the mouse a cursor, the click A, WASD
// the arrows; 2026-11-13). The book is already a menu by the game's list-menu mark.
struct OutsideMenu {
  std::atomic<uint64_t> ms{0};
  std::atomic<int> source{0};  // 1 title lists, 2 choice dialog, 3 save list, 4 a shop's list
};
inline OutsideMenu& outside_menu() {
  static OutsideMenu o;
  return o;
}
inline void MarkOutsideMenu(int source) {
  outside_menu().ms = NowMs();
  outside_menu().source = source;
}
// A talk's column of choices took input then (touch: they go by the stick, not the pointer; touch_controls.h).
inline std::atomic<uint64_t>& choice_ms() {
  static std::atomic<uint64_t> t{0};
  return t;
}
inline bool MenuSeenRecently() {
  return NowMs() - outside_menu().ms.load() < 300;
}

// The game's frames: its present (sub_8257AB38, perf_overlay.cpp) counts them (not its pad read: that runs once
// a controller port, four times a frame). The steps the pointer presses toward a
// row (MenuMouse) go a frame each: counted in the input's polls, which are several a frame on the phone, the
// game saw a press as held or missed it, the cursor "did not move" and the steps gave up after one row.
inline std::atomic<uint32_t>& game_frame() {
  static std::atomic<uint32_t> f{0};
  return f;
}

// The performance meter on a touch screen (perf_overlay.cpp draws it, touch_controls.h moves it): where it was
// last drawn (0..1 of the screen), how far it was dragged from its place, and whether a tap shrank it to the FPS.
struct PerfPanel {
  std::atomic<float> x0{0}, y0{0}, x1{0}, y1{0};
  std::atomic<uint64_t> drawn_ms{0};
  std::atomic<float> dx{0}, dy{0};
  std::atomic<bool> mini{false};
  bool Has(float x, float y) const {
    return NowMs() - drawn_ms.load() < 300 && x >= x0.load() && x < x1.load() && y >= y0.load() && y < y1.load();
  }
};
inline PerfPanel& perf_panel() {
  static PerfPanel p;
  return p;
}

// The pointer on a touch screen (Android): the finger touching a menu (touch_controls.h sets it), in the game's
// 1280x720 space; the last place stays after it lifts (as a mouse that stopped). Down: the finger is on.
struct TouchPointer {
  std::atomic<int> x{-1}, y{-1};
  std::atomic<bool> down{false};
};
inline TouchPointer& touch_pointer() {
  static TouchPointer p;
  return p;
}

#if !defined(_WIN32)
inline bool MouseInGame(int& x, int& y) {
  x = touch_pointer().x.load();
  y = touch_pointer().y.load();
  return x >= 0 && y >= 0 && x < 1280 && y < 720;
}
inline bool PointerDown() { return touch_pointer().down.load(); }
inline bool ArrowKeysHeld() { return false; }
#else
inline bool PointerDown() { return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0; }
// Keys that give up / down in menus (the arrows, W / S): their presses are not the wheel's.
inline bool ArrowKeysHeld() {
  for (int vk : {VK_UP, VK_DOWN, int('W'), int('S')})
    if (GetAsyncKeyState(vk) & 0x8000) return true;
  return false;
}
// The mouse in the game's 1280x720 space (the picture is fitted to the window, letterboxed), when the game's
// window has the focus and the pointer is over the picture.
inline bool MouseInGame(int& x, int& y) {
  HWND w = GetForegroundWindow();
  DWORD pid = 0;
  if (!w || !GetWindowThreadProcessId(w, &pid) || pid != GetCurrentProcessId()) return false;
  POINT p;
  RECT r;
  if (!GetCursorPos(&p) || !ScreenToClient(w, &p) || !GetClientRect(w, &r)) return false;
  const double cw = r.right - r.left, ch = r.bottom - r.top;
  if (cw < 16 || ch < 16) return false;
  const double s = cw / ch > 1280.0 / 720.0 ? ch / 720.0 : cw / 1280.0;
  const double ox = (cw - 1280.0 * s) / 2, oy = (ch - 720.0 * s) / 2;
  x = int((p.x - ox) / s);
  y = int((p.y - oy) / s);
  return x >= 0 && y >= 0 && x < 1280 && y < 720;
}
#endif

// Weiss's book lists all draw their rows at one place (measured 2026-11-13): the first at y 273, 39 apart,
// x 190..910. The row under the pointer (0..rows-1, -1: none), noted for the click (title()); `act`: the
// pointer moved or the button is down (a click on a row the cursor is not on picks it first). The game's
// thread only (last_x / last_y).
inline int BookRowUnderMouse(int rows, bool& act, int y0 = 273, int x0 = 190, int x1 = 910) {
  auto& tm = title();
  int mx = 0, my = 0;
  const bool inside = MouseInGame(mx, my);
  int hovered = -1;
  if (inside && mx >= x0 && mx < x1) {
    for (int i = 0; i < rows; ++i) {
      const int y = y0 + 39 * i;
      if (my >= y - 6 && my < y + 33) hovered = i;
    }
  }
  tm.ms = NowMs();
  tm.hovered = hovered;
  const bool moved = inside && (mx != tm.last_x || my != tm.last_y);
  tm.last_x = mx;
  tm.last_y = my;
  act = moved || (inside && PointerDown());
  return hovered;
}

class MenuMouse {
 public:
  // g: the pad state the game is about to get, already in menu meaning. base: guest memory.
  // book_open: Weiss's book is showing (its pages' cursors count only then; the windows' only without it).
  void Apply(rex::input::X_INPUT_GAMEPAD& g, const uint8_t* base, bool book_open) {
    uint16_t b = uint16_t(g.buttons);
    // The title menu: the game's side moves its row under the pointer; a click off the rows is nothing.
    if (NowMs() - title().ms.load() < 200) {
      if (const uint16_t p = title().press.exchange(0)) {
        hold_ = p;
        hold_polls_ = 3;
      }
      if (hold_polls_ > 0) {
        --hold_polls_;
        b |= hold_;
      }
      int mx, my;
      const bool left = PointerDown() && MouseInGame(mx, my);
      // Off the rows while it is down (a touch lands and moves the pointer at once: the row under it is known
      // a frame later, and the A latched at the press was eaten whole).
      if (left && !left_was_) press_ms_ = NowMs();
      if (left) eat_a_ = title().hovered.load() < 0;
#if !defined(_WIN32)
      // A touch still off the rows after 120 ms is on something else: the save list's object keeps running under
      // "Operation complete" after a load, and its "off the rows" ate the notice's A (2026-11-13). (The touch's A
      // lasts long enough for the rest to go through: touch_controls.h.)
      // hovered -2: on a control whose A must never pass (the shops' quantity arrows: their A bought one,
      // 2026-11-19; a talk's choices waking).
      if (left && eat_a_ && NowMs() - press_ms_ > 120 && title().hovered.load() != -2) eat_a_ = false;
#endif
      if (left && !left_was_) {
      }
      if (!left) eat_a_ = false;
      left_was_ = left;
      if (eat_a_) b &= uint16_t(~kA);
      g.buttons = b;
      return;
    }
    const Tracker::Active act = tracker().Get(book_open, NowMs());
    if (act.kind != Tracker::kSystem) cross_wait_ = 0;  // the cross's click opened another screen
    if (base && act.kind == Tracker::kBook) {
      Generic(b, base, act, false);
    } else if (base && act.kind == Tracker::kSystem) {
      Generic(b, base, act, true);
    } else if (act.kind == Tracker::kList) {
      Step(b, base);
    } else {
      Reset();  // a screen the mouse does not drive yet: clicks stay the plain A
    }
    g.buttons = b;
  }

 private:
  // The pointer, shared by both kinds of menus: true when it is over the game. Up / down from elsewhere
  // (wheel, keys, pad) stop the pointing until the pointer really moves (see Step).
  bool Pointer(uint16_t buttons, int& mx, int& my, bool& left, bool& moved) {
    if (buttons & (kUp | kDown)) {
      Reset();
      if (MouseInGame(mx, my)) {
        quiet_x_ = mx;
        quiet_y_ = my;
        quiet_ = true;
      }
      return false;
    }
    const bool inside = MouseInGame(mx, my);
    left = PointerDown() && inside;
    moved = inside && (mx != last_x_ || my != last_y_);
    if (moved && quiet_) {
      const int dx = mx - quiet_x_, dy = my - quiet_y_;
      if (dx * dx + dy * dy < kQuietDistance * kQuietDistance) {
        moved = false;
      } else {
        quiet_ = false;
      }
    }
    if (left) quiet_ = false;
    last_x_ = mx;
    last_y_ = my;
    return true;
  }

  // The book's pages and the system windows: the rows are the cursor node's siblings; the pointer moves the
  // game's cursor to the row under it (pressing the way to it), a click there is A. The cross (the title's and
  // the pause's: one arm each way) only moves when clicked, by its arm's direction (the pause opens the arm
  // on that press), and a click on the arm the cursor is on is A.
  void Generic(uint16_t& buttons, const uint8_t* base, const Tracker::Active& act, bool system) {
    const bool own_a = hold_polls_ > 0 && hold_ == kA;
    if (hold_polls_ > 0) {
      --hold_polls_;
      buttons |= hold_;
    }
    // The click's own A (the mouse button is A in menus) while it is held: not the game's. Stripped here, before
    // any early return (it went along with the cross's up and the cross took neither, 2026-11-13).
    if (eat_a_ && !own_a) {
      if (!PointerDown()) eat_a_ = false;
      else buttons &= uint16_t(~kA);
    }
    // The windows come with all their rows; a book page's are its cursor row's siblings (all looked up when
    // the page is drawn), again when the screen or the cursor changes.
    if (system) {
      gen_rows_ = act.rows;
    } else if (act.version != gen_version_ || act.cursor != gen_cursor_ ||
               (gen_rows_.size() < 2 && ++gen_retry_ >= 15)) {
      gen_version_ = act.version;
      gen_cursor_ = act.cursor;
      gen_retry_ = 0;
      gen_rows_ = act.cursor ? tracker().Siblings(act.cursor) : std::vector<uint32_t>{};
    }
    // A cross arm clicked away from the cursor: its direction was pressed; where that only moved the cursor
    // there (the title), the click goes on to A. Where it opened the arm (the pause), the window changed.
    if (cross_wait_ > 0) {
      --cross_wait_;
      if (!system || act.owner != cross_window_) {
        cross_wait_ = 0;
      } else if (act.cursor == cross_target_ && cross_wait_ < 6) {
        cross_wait_ = 0;
        hold_ = kA;
        hold_polls_ = 3;
      }
    }
    if (!act.cursor || gen_rows_.size() < 2 || gen_rows_[0] == 0) return Reset();

    int mx = 0, my = 0;
    bool left = false, moved = false;
    if (!Pointer(buttons, mx, my, left, moved)) return;

    // Positions; the layouts keep a second screen 720 below (the game shows it by turning the page): only the
    // half the cursor is on shows.
    struct R { uint32_t node; int x, y; };
    std::vector<R> rows;
    int cx = 0, cy = 0;
    if (!NodePos(base, act.cursor, cx, cy)) return Reset();
    const bool lower = cy >= 720;
    for (uint32_t n : gen_rows_) {
      int x, y;
      if (!NodePos(base, n, x, y) || (y >= 720) != lower) continue;
      // The book's lists are one column (the Quests' "=_txt_100_quest_a" & co. at the bottom are not rows).
      if (!system && std::abs(x - cx) > 40) continue;
      rows.push_back({n, x, lower ? y - 720 : y});
    }
    if (lower) cy -= 720;
    if (rows.size() < 2) return Reset();

    // A cross: one row each way from their middle.
    double ax = 0, ay = 0;
    for (auto& r : rows) ax += r.x, ay += r.y;
    ax /= rows.size(), ay /= rows.size();
    auto way = [&](const R& r) -> uint16_t {
      const double dx = r.x - ax, dy = r.y - ay;
      return std::abs(dx) > std::abs(dy) ? (dx < 0 ? kLeft : kRight) : (dy < 0 ? kUp : kDown);
    };
    const bool cross = system && act.cross;

    // Each row's box. Book rows: text nodes, up to the next row below (420 x 39 when alone), from a sixth above.
    // Window plates: the node is the plate's top left (measured on the pause's cross: "Options" plate from
    // 290,365, about 250 x 40), up to the next plate across / below.
    int hovered = -1;
    for (size_t i = 0; i < rows.size(); ++i) {
      int w = system ? 300 : 420, h = system ? 46 : 39;
      for (size_t j = 0; j < rows.size(); ++j) {
        if (i == j) continue;
        const int dx = rows[j].x - rows[i].x, dy = std::abs(rows[j].y - rows[i].y);
        if (system) {
          if (dy > 12) h = std::min(h, dy - 4);
        } else if (dy > 12 && dy < h * 3) {
          h = std::min(h == 39 ? dy : h, dy);
        }
        if (dy <= 12 && dx > 40) w = std::min(w, dx - 10);
      }
      if (h < 16) h = 16;
      const int top = system ? rows[i].y : rows[i].y - h / 6;
      const int left = system ? rows[i].x : rows[i].x - 40;
      if (mx >= left && mx < rows[i].x + w && my >= top && my < top + h) {
        hovered = int(i);
        break;
      }
    }

    const uint32_t target_node = hovered >= 0 ? rows[size_t(hovered)].node : 0;
    if (cross) {  // no pointing, clicks only
      if (left && !left_was_) {
        // The cross opens an arm by its direction only (A does nothing there, 2026-11-13), also the arm the
        // cursor is on.
        if (hovered >= 0) {
          eat_a_ = true;
          hold_ = way(rows[size_t(hovered)]);
          hold_polls_ = 3;
        } else {
          eat_a_ = true;  // off the arms: nothing
        }
      }
      if (!left) eat_a_ = false;
      left_was_ = left;
      if (eat_a_) buttons &= uint16_t(~kA);
      return;
    }

    // A row the cursor cannot reach (an empty row under a short list, a locked area on the Map) stays out until
    // the pointer goes to another row.
    if (moved && target_node != gen_blocked_) gen_blocked_ = 0;
    const bool usable = hovered >= 0 && target_node != gen_blocked_;
    if (moved && usable) gen_target_ = target_node;
    if (left && !left_was_ && usable) {
      eat_a_ = target_node != act.cursor;
      gen_target_ = target_node;
      pending_ = target_node != act.cursor ? kA : 0;
    }
    if (left && !left_was_ && !usable) eat_a_ = true;  // off the rows: nothing
    if (!left) eat_a_ = false;
    left_was_ = left;
    if (pending_ || eat_a_) buttons &= uint16_t(~kA);

    auto pos = [&](uint32_t node, int& x, int& y) {
      x = y = 0;
      for (auto& r : rows)
        if (r.node == node) x = r.x, y = r.y;
    };
    if (gen_target_ && gen_target_ != act.cursor) {
      int tx = 0, ty = 0;
      pos(gen_target_, tx, ty);
      const int dist = std::abs(ty - cy) + std::abs(tx - cx);
      if (gen_steer_for_ != gen_target_) {  // a new target: from here
        gen_steer_for_ = gen_target_;
        gen_best_ = act.cursor;
        gen_best_dist_ = dist;
        gen_press_cursor_ = act.cursor;
        gen_same_ = 0;
      }
      const bool tick = FrameTick();  // a press a frame, a release the next
      if (tick && !pulse_) {  // this frame presses: how did the last press go?
        if (act.cursor == gen_press_cursor_) {
          ++gen_same_;
        } else {
          gen_same_ = 0;
        }
        gen_press_cursor_ = act.cursor;
        if (dist < gen_best_dist_) {
          gen_best_ = act.cursor;
          gen_best_dist_ = dist;
        } else if (dist > gen_best_dist_ || gen_same_ >= 8 || ++steps_ > 60) {
          // Went past it (the list wrapped round, a row skipped) or does not move: back to the closest row.
          gen_blocked_ = gen_target_;
          gen_target_ = gen_best_ != act.cursor ? gen_best_ : 0;
          gen_steer_for_ = gen_target_;
          pending_ = 0;
          steps_ = 0;
          gen_same_ = 0;
          if (!gen_target_) return;
          pos(gen_target_, tx, ty);
          gen_best_dist_ = std::abs(ty - cy) + std::abs(tx - cx);
        }
      }
      if (tick) pulse_ = !pulse_;
      buttons &= uint16_t(~(kUp | kDown | kLeft | kRight));
      if (pulse_) {
        if (std::abs(ty - cy) > 12) {
          buttons |= ty < cy ? kUp : kDown;
        } else if (tx != cx) {
          buttons |= tx < cx ? kLeft : kRight;
        }
      }
    } else {
      if (gen_target_ && pending_) {
        hold_ = pending_;
        hold_polls_ = 3;
      }
      gen_target_ = 0;
      gen_steer_for_ = 0;
      pending_ = 0;
      steps_ = 0;
      pulse_ = false;
    }
  }
  uint64_t gen_version_ = 0;
  uint32_t gen_target_ = 0, gen_cursor_ = 0, gen_blocked_ = 0, gen_steer_for_ = 0, gen_best_ = 0;
  uint32_t gen_press_cursor_ = 0, cross_target_ = 0, cross_window_ = 0;
  int cross_wait_ = 0;
  int gen_retry_ = 0, gen_best_dist_ = 0, gen_same_ = 0;
  std::vector<uint32_t> gen_rows_;

  void Step(uint16_t& g_buttons, const uint8_t* base) {
    struct { uint16_t& buttons; } g{g_buttons};
    const bool own_a = hold_polls_ > 0 && hold_ == kA;
    if (hold_polls_ > 0) {  // the press a click asked for, once the cursor got to its row
      --hold_polls_;
      g.buttons |= hold_;
    }
    if (eat_a_ && !own_a) {  // the click's own A, before any early return (see Generic)
      if (!PointerDown()) eat_a_ = false;
      else g.buttons &= uint16_t(~kA);
    }
    const uint32_t obj = nodes().obj.load();
    if (!base || !obj || base[0x82757AB1] == 0) return Reset();  // 0x82757AB1: a list menu is open
    // Only the screens whose list is known: 1 = Options (list A), 4 = Game Settings and the PC pages, 7 = the
    // pause's Options (list B). Difficulty and the others keep their own cursors (pressing down there never reached
    // the row: it ran on and on).
    const uint32_t state = Be32At(base, obj + 3556);
    if (state != 1 && state != 4 && state != 7) return Reset();
    const bool list_b = state != 1;
    const uint32_t off_scroll = list_b ? 3572 : 3564, off_sel = list_b ? 3576 : 3568;
    const uint32_t off_items = list_b ? 3916 : 3580, off_count = list_b ? 5264 : 5260;
    const int rows = list_b ? kRowsB : kRowsA;
    // The game's selection counts from the first row shown; the rows here count from the list's top (scroll +
    // row). Compared as they were, a scrolled list never matched: the cursor was pressed on and on and the
    // list ran (the Controls page, Return never reached, 2026-11-13).
    const uint32_t scroll = Be32At(base, obj + off_scroll), selected = scroll + Be32At(base, obj + off_sel);
    const uint32_t count = Be32At(base, obj + off_count);
    if (count == 0 || count > 24 || selected >= count) return Reset();

    // Up / down from somewhere else (the wheel, the keys, a pad) win over the pointer: the row it points at
    // stops being the target until the mouse moves again (the wheel scrolled and the pointer pulled it back).
    // A wheel's turn also nudges the pointer a few pixels on many mice, and that counted as pointing again: after
    // one, the pointer only counts once it has gone a real distance from where it was.
    // The wheel on the settings lists (list B: Game Settings, the PC pages, the pause's Options) scrolls the
    // page under the cursor, a row a notch, wherever the pointer is (user's choice, 2026-11-13): the mouse's
    // wheel comes as up / down pulses; with no arrow key held they are the wheel's.
    const uint16_t ud = g.buttons & (kUp | kDown);
    const bool arrows = ArrowKeysHeld();
    const bool drawn_now = NowMs() - nodes().drawn_ms.load() < 150;  // the list itself on screen
    if (list_b && ud && !arrows && drawn_now) {
      if (ud != wheel_last_) nodes().scroll_request.fetch_add((ud & kDown) ? 1 : -1);
      wheel_last_ = ud;
      g.buttons &= uint16_t(~(kUp | kDown));
      Reset();
      return;
    }
    wheel_last_ = ud;
    if (g.buttons & (kUp | kDown)) {
      Reset();
      int mx0, my0;
      if (MouseInGame(mx0, my0)) {
        quiet_x_ = mx0;
        quiet_y_ = my0;
        quiet_ = true;
      }
      return;
    }

    int mx, my;
    const bool inside = MouseInGame(mx, my);
    const bool left = PointerDown() && inside;
    bool moved = inside && (mx != last_x_ || my != last_y_);
    if (moved && quiet_) {
      const int dx = mx - quiet_x_, dy = my - quiet_y_;
      if (dx * dx + dy * dy < kQuietDistance * kQuietDistance) {
        moved = false;
      } else {
        quiet_ = false;
      }
    }
    if (left) quiet_ = false;  // a click points again
    last_x_ = mx;
    last_y_ = my;

    int hovered = -1;
    bool value_row = false;
    bool rows_known = false;  // some row's place is known (a notice on list A, "Operation complete", has none)
    int side = 0;  // on a value row of list B: -1 = the left arrow's half of the value column, +1 = the right's
    if (inside) {
      for (int i = 0; i < rows; ++i) {
        const uint32_t node = (list_b ? nodes().b[i] : nodes().a[i]).load();
        int x, y;
        if (!NodePos(base, node, x, y)) continue;
        rows_known = true;
        int next_x, next_y;
        const uint32_t next = i + 1 < rows ? (list_b ? nodes().b[i + 1] : nodes().a[i + 1]).load() : 0;
        const int h = NodePos(base, next, next_x, next_y) && next_y > y ? next_y - y : (list_b ? 38 : 39);
        // List B lies one screen below list A in the layout; the game turns the page by 720 to show it (measured:
        // row 0 at 898 in the layout, drawn at 178).
        if (list_b) y -= 720;
        if (mx < x - 70 || mx > x + 700 || my < y - h / 6 || my >= y - h / 6 + h) continue;
        const uint32_t item = scroll + uint32_t(i);
        if (item >= count) break;
        const uint8_t kind = base[obj + off_items + item * 56];
        if (kind != 0 && kind != 8) hovered = int(item);
        // Value rows (2 = a choice with arrows, 5 / 6 = a bar): the value column is centred at x + 502
        // (measured on the Video page: values and bars around 755 with the row at 253), its arrows on
        // each side. Its halves are the arrows; the label only selects the row.
        if (hovered >= 0 && list_b && (kind == 2 || kind == 5 || kind == 6)) {
          // Arrows ~60 either side of the centre, bars 690..818: the halves end a little past them.
          side = mx < x + 420 || mx >= x + 590 ? 0 : (mx < x + 502 ? -1 : +1);
          value_row = true;
        }
        break;
      }
    }

    if (moved && hovered != blocked_) blocked_ = -1;
    if (hovered >= 0 && hovered == blocked_) hovered = -1;  // a row the cursor would not reach
    if (moved && hovered >= 0) target_ = hovered;
    if (left && !left_was_ && hovered >= 0) {  // a fresh click on a row
      // A value row is changed by its arrows only (A there stepped the value round one way, which on a
      // touch screen left no way back); its label just selects it. Other rows: the click is A.
      const uint16_t press = value_row ? (side < 0 ? kLeft : side > 0 ? kRight : 0) : kA;
      eat_a_ = value_row || uint32_t(hovered) != selected;  // the click's own A (mnk) is not the game's
      target_ = hovered;
      pending_ = uint32_t(hovered) != selected || press != kA ? press : 0;
    }
    // A click off the rows (the page's top, a blank row) is nothing: its A changed the selected row's value.
    // Rows whose places are unknown: the click is the plain A (it confirms the notice).
    if (left && !left_was_ && hovered < 0 && rows_known) eat_a_ = true;
    if (!left) eat_a_ = false;
    left_was_ = left;
    if (pending_ || eat_a_) g.buttons &= uint16_t(~kA);

    // The list changed under the target (a page opened or closed, Return): the target was a row of the old one.
    const uint32_t list_sig = state << 16 ^ count << 8 ^ uint32_t(list_b);
    if (list_sig != list_sig_) {
      list_sig_ = list_sig;
      if (target_ >= 0) Reset();
    }
    if (target_ >= 0 && uint32_t(target_) != selected) {
      // The cursor must follow each press; when it does not (the game skips, waits or wraps) or went past
      // the row, give up until the pointer goes to another row (it ran on down the list, 2026-11-13).
      if (FrameTick()) {  // a press a frame, a release the next
        if (!pulse_) {
          if (selected == press_sel_) {
            ++same_;
          } else {
            same_ = 0;
          }
          const bool past = press_sel_ != ~0u && ((press_dir_ == kDown && selected < press_sel_) ||
                                                 (press_dir_ == kUp && selected > press_sel_));
          // The game may take a few frames to move its cursor (8 presses without a move: it will not).
          if (same_ >= 8 || past || ++steps_ > 40) {
            blocked_ = target_;
            return Reset();
          }
          press_sel_ = selected;
          press_dir_ = uint32_t(target_) < selected ? kUp : kDown;
        }
        pulse_ = !pulse_;
      }
      g.buttons &= uint16_t(~(kUp | kDown));
      if (pulse_) g.buttons |= press_dir_;
    } else {
      press_sel_ = ~0u;
      same_ = 0;
      if (target_ >= 0 && pending_) {
        hold_ = pending_;
        hold_polls_ = 3;
      }
      target_ = -1;
      pending_ = 0;
      steps_ = 0;
      pulse_ = false;
    }
  }

  static constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kA = 0x1000;

  // A new game frame since the last step (or 60 ms, should the frame count stop): the next step may go.
  bool FrameTick() {
    const uint32_t f = game_frame().load();
    const uint64_t now = NowMs();
    if (f == pulse_frame_ && now - pulse_ms_ < 60) return false;
    pulse_frame_ = f;
    pulse_ms_ = now;
    return true;
  }
  uint32_t pulse_frame_ = 0;
  uint64_t pulse_ms_ = 0;

  void Reset() {
    gen_target_ = 0;
    gen_steer_for_ = 0;
    target_ = -1;
    pending_ = 0;
    steps_ = 0;
    pulse_ = false;
    press_sel_ = ~0u;
    same_ = 0;
  }

  int target_ = -1, blocked_ = -1, same_ = 0;
  uint32_t list_sig_ = 0, press_sel_ = ~0u;
  uint16_t press_dir_ = 0, wheel_last_ = 0;
  uint16_t pending_ = 0, hold_ = 0;  // the press a click waits to give (A, left or right)
  bool pulse_ = false, left_was_ = false, eat_a_ = false;
  uint64_t press_ms_ = 0;  // when the pointer went down (the title path's off-the-rows wait)
  int steps_ = 0, hold_polls_ = 0;
  int last_x_ = -1, last_y_ = -1;
  static constexpr int kQuietDistance = 16;  // in the 1280x720 space: under half a row
  bool quiet_ = false;
  int quiet_x_ = 0, quiet_y_ = 0;
};

}  // namespace nier::menu_mouse
