// nier - the Jukebox page (jukebox.h): drawn over the game like Grimoire Weiss's book (its own art, read from the
// player's copy), but free of the game's fixed lists: tabs, a song list that scrolls, the record that turns while
// a song plays, the layers' mixer, playlists with times (a timeline), albums, presets, the Music folder.
//
// F6 (or the Jukebox icon on Android) opens and closes it. Mouse / touch for everything; the pad: LB / RB tabs,
// up / down and A in the list, X pause, B close. The game gets no input while it is open.
#pragma once

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/window.h>

#if defined(_WIN32)
#include <shellapi.h>
#endif

#include "../game_text.h"
#include "../menu_mouse.h"
#include "../pc_options.h"
#include "../title_carousel.h"
#include "jukebox.h"

namespace nier::jukebox {

class JukeboxDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit JukeboxDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    auto& state = pc_options_state();
    const bool open = state.jukebox_open;
    const float dt = std::clamp(io.DeltaTime, 0.0f, 0.1f);
    appear_ = std::clamp(appear_ + (open ? dt * 5.0f : -dt * 6.0f), 0.0f, 1.0f);
    now_ = Now();
    game_ = Game();
    if (now_.playing && !now_.paused) spin_ += dt * 33.3f / 60.0f * 6.2831853f;
    arm_ = arm_ + ((now_.playing && !now_.paused ? 1.0f : 0.0f) - arm_) * std::min(1.0f, dt * 4.0f);
    if (open && !was_open_) {
      selected_ = std::max(selected_, 0);
      if (rex::ui::Window* w = imgui_drawer()->window()) w->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    }
    was_open_ = open;
    // Tests: NIER_JUKEBOX_UI="<tab>[:<playlist>[:<item>]]" opens the page there, after the given seconds
    // (NIER_JUKEBOX_UI_AT, default 30).
    if (static bool test_done = false; !test_done && std::getenv("NIER_JUKEBOX_UI")) {
      static float t = 0;
      t += dt;
      const char* at = std::getenv("NIER_JUKEBOX_UI_AT");
      if (t > (at ? float(std::atof(at)) : 30.0f)) {
        test_done = true;
        int tab = 0, pl = -1, item = -1;
        std::sscanf(std::getenv("NIER_JUKEBOX_UI"), "%d:%d:%d", &tab, &pl, &item);
        SetTab(tab);
        open_playlist_ = pl;
        edit_item_ = item;
        state.jukebox_open = true;
      }
    }
    DrawPauseButton(io, dt, open);
    if (appear_ <= 0.0f) return;
    if (io.MouseWheel != 0) wheel_time_ = ImGui::GetTime();
    if (open) {
      for (uint16_t b : state.TakeJukebox()) Pad(b);
      if (rex::ui::Window* w = imgui_drawer()->window();
          w && w->GetCursorVisibility() != rex::ui::Window::CursorVisibility::kVisible)
        w->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    }
    EnsureTextures();
    DrawPage(io, dt, open);
  }

 private:
  enum Tab { kSongs, kMine, kAlbums, kPlaylists, kPresets, kSettings, kTabCount };

#if defined(__ANDROID__)
  static constexpr bool kTouch = true;  // a finger dragged over a list scrolls it (the mouse has its wheel)
#else
  static constexpr bool kTouch = false;
#endif

  // The pause menu's cross (Items up, Options left, Orders right, Weapons down): a "Jukebox" plate of the same
  // kind under Weapons, which a click or a touch opens the page with. The game's pointer code (menu_mouse.h)
  // takes a click there as off the arms (nothing), so only the page opens.
  void DrawPauseButton(ImGuiIO& io, float dt, bool page_open) {
    auto& st = pc_options_state();
    const uint64_t now = nier::menu_mouse::NowMs();
    const uint8_t* gb = st.guest_base.load();
    // The pause's cross, not the title's: gameplay seen since the title's lists last ran (pc_options' own
    // gameplay_seen stays off when no one asks it in gameplay).
    if (gb) {
      auto byte = [gb](uint32_t a) { return *reinterpret_cast<const volatile uint8_t*>(gb + a); };
      if (now - nier::menu_mouse::title().ms.load() < 500 || st.BeforeTitleMenu()) played_ = false;
      // Out of the menus with Nier's object there (touch_controls.h's check: its vtable at +3472); the game's
      // gameplay marks alone stayed off in some areas (Weiss's cave, 2026-11-17: no plate).
      else if (byte(0x82757AB1) == 0 &&
               (byte(0x827EB0D8) == 0 || byte(0x827E2B54) == 4 ||
                (uint32_t(byte(0x848BE4B0u + 3472)) << 24 | uint32_t(byte(0x848BE4B0u + 3473)) << 16 |
                 uint32_t(byte(0x848BE4B0u + 3474)) << 8 | byte(0x848BE4B0u + 3475)) == 0x82740048u))
        played_ = true;
    }
    const bool show = !page_open && gb && *reinterpret_cast<const volatile uint8_t*>(gb + 0x82757AB1) != 0 &&
                      played_ && !st.open && !st.mod_panel_open && !st.BeforeTitleMenu() &&
                      !st.BookOpen() && now - nier::menu_mouse::nodes().drawn_ms.load() > 150 &&
                      nier::menu_mouse::tracker().CrossOnTop(now);
    if (nier::menu_mouse::tracker().CrossOnTop(now)) st.cross_seen_ms = now;  // the RB hint's test (video_options_menu.cpp)
    // Shown once the cross has stood a moment (its own plates come in with a fade).
    pause_seen_ = show ? pause_seen_ + dt : 0.0f;
    pause_fade_ = std::clamp(pause_fade_ + (show && pause_seen_ > 0.12f ? dt * 6.0f : -dt * 12.0f), 0.0f, 1.0f);
    if (pause_fade_ <= 0.0f) {
      pause_hot_ = false;
      return;
    }
    const ImVec2 screen = io.DisplaySize;
    if (screen.x < 1 || screen.y < 1) return;
    const float aspect = screen.x / screen.y;
    auto at = [&](float gx, float gy) {
      float fx, fy;
      title_carousel::GameToScreen(gx, gy, aspect, fx, fy);
      return ImVec2(fx * screen.x, fy * screen.y);
    };
    // The game's plates: about 250 x 40 of its 1280x720, 57 apart (Weapons at y 441); this one the next step down.
    const ImVec2 a = at(515, 477), b = at(765, 519);
    const float k = (b.y - a.y) / 42.0f;  // screen pixels per game unit
    const ImVec2 m = io.MousePos;
    const bool over = show && m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y;
    if (show && ImGui::IsMouseClicked(0)) pause_hot_ = over;
    if (pause_hot_ && ImGui::IsMouseReleased(0)) {
      pause_hot_ = false;
      if (over) {
        st.jukebox_open = true;
        st.TakeJukebox();
      }
    }
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const int alpha = int(255 * pause_fade_);
    const float midx = (a.x + b.x) * 0.5f;
    const ImU32 dark = IM_COL32(12, 14, 12, int(alpha * (over ? 0.78f : 0.62f)));
    const ImU32 clear = IM_COL32(12, 14, 12, 0);
    dl->AddRectFilledMultiColor({a.x - 40 * k, a.y}, {midx, b.y}, clear, dark, dark, clear);
    dl->AddRectFilledMultiColor({midx, a.y}, {b.x + 40 * k, b.y}, dark, clear, clear, dark);
    if (over) {
      const ImU32 lit = IM_COL32(236, 232, 214, int(alpha * 0.16f)), none = IM_COL32(236, 232, 214, 0);
      dl->AddRectFilledMultiColor({a.x, a.y}, {midx, b.y}, none, lit, lit, none);
      dl->AddRectFilledMultiColor({midx, a.y}, {b.x, b.y}, lit, none, none, lit);
    }
    ImFont* f = pc_options_title_font() ? pc_options_title_font() : (pc_options_font() ? pc_options_font() : ImGui::GetFont());
    const char* label = "Jukebox";
    const float size = 25.0f * k;
    const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, label);
    const ImVec2 tp(midx - ts.x * 0.5f, (a.y + b.y - ts.y) * 0.5f);
    dl->AddText(f, size, {tp.x + 1.5f * k, tp.y + 1.5f * k}, IM_COL32(0, 0, 0, int(alpha * 0.7f)), label);
    dl->AddText(f, size, tp, IM_COL32(240, 236, 226, alpha), label);
    dl->AddText(f, size, {tp.x + 0.7f * k, tp.y}, IM_COL32(240, 236, 226, alpha), label);  // the game's heavier type
  }

  static bool Pt() { return text::CurrentName() == "Portugues"; }
  static const char* L(const char* pt, const char* en) { return Pt() ? pt : en; }
  static std::string Time(double s) {
    if (s <= 0 || !std::isfinite(s)) return "--:--";
    const int t = int(s + 0.5);
    return fmt::format("{}:{:02}", t / 60, t % 60);
  }

  void Close() {
    pc_options_state().jukebox_open = false;
    pc_options_state().TakeJukebox();
    bool keep;
    {
      std::lock_guard lock(LibraryMutex());
      keep = GetLibrary().keep_playing;
    }
    if (!keep) Stop();
  }

  // ---- Art ----
  void EnsureTextures() {
    rex::ui::ImmediateDrawer* drawer = imgui_drawer()->immediate_drawer();
    if (drawer != texture_owner_) {
      textures_.clear();
      texture_owner_ = drawer;
    }
    auto& art = menu_art();
    if (drawer && textures_.empty() && art.ready) {
      std::lock_guard lock(art.mutex);
      for (const auto& [name, image] : art.images)
        textures_[name] = drawer->CreateTexture(uint32_t(image.width), uint32_t(image.height),
                                                rex::ui::ImmediateTextureFilter::kLinear, false, image.rgba.data());
    }
  }
  ImTextureID Art(const char* name) const {
    auto it = textures_.find(name);
    return it == textures_.end() || !it->second ? ImTextureID{} : reinterpret_cast<ImTextureID>(it->second.get());
  }

  // ---- The rows the current tab shows ----
  struct Row {
    Item item;
    std::string title, detail;
    double seconds = 0;
    int index = -1;  // playlist / album / preset index (their tabs)
  };

  std::vector<Row> Rows() {
    std::vector<Row> rows;
    const Catalog& cat = GetCatalog();
    if (tab_ == kSongs) {
      for (const Song& s : cat.songs) {
        if (Hidden(s.key)) continue;
        Row r;
        r.item.key = s.key;
        r.title = Title(s.key, false);
        std::string parts;
        int layers = 0;
        for (const Stem& st : s.stems) {
          if (st.layer + 1 > layers) layers = st.layer + 1;
          const std::string p = PartName(st.part);
          if (parts.find(p) == std::string::npos) parts += (parts.empty() ? "" : "  ·  ") + p;
        }
        r.detail = s.stems.size() > 1 ? parts : std::string(L("Faixa única", "Single track"));
        r.seconds = s.seconds;
        rows.push_back(std::move(r));
      }
    } else if (tab_ == kMine) {
      for (const UserSong& u : UserSongs()) {
        if (!album_folder_.empty() && u.folder != album_folder_) continue;
        Row r;
        r.item.file = true;
        r.item.key = u.path;
        r.title = Title(u.path, true);
        r.detail = u.folder.empty() ? std::string(L("Pasta Music", "Music folder")) : u.folder;
        rows.push_back(std::move(r));
      }
    } else if (tab_ == kAlbums && open_album_ >= 0) {
      Album al;
      {
        std::lock_guard lock(LibraryMutex());
        if (open_album_ < int(GetLibrary().albums.size())) al = GetLibrary().albums[size_t(open_album_)];
      }
      for (size_t i = 0; i < al.keys.size(); ++i) {
        Row r;
        r.item.file = cat.Find(al.keys[i]) == nullptr;
        r.item.key = al.keys[i];
        r.title = Title(al.keys[i], r.item.file);
        if (const Song* s = cat.Find(al.keys[i])) r.seconds = s->seconds;
        r.index = int(i);
        rows.push_back(std::move(r));
      }
    } else if (tab_ == kPlaylists && open_playlist_ >= 0) {
      Playlist pl;
      {
        std::lock_guard lock(LibraryMutex());
        if (open_playlist_ < int(GetLibrary().playlists.size())) pl = GetLibrary().playlists[size_t(open_playlist_)];
      }
      for (size_t i = 0; i < pl.items.size(); ++i) {
        Row r;
        r.item = pl.items[i];
        r.title = Title(r.item.key, r.item.file);
        r.seconds = r.item.seconds;
        r.index = int(i);
        rows.push_back(std::move(r));
      }
    }
    return rows;
  }

  // ---- Pad ----
  void Pad(uint16_t b) {
    // The mouse's own presses (the menu mouse: left = A, right = B, wheel = up / down) are the page's through
    // ImGui already: not taken twice (a click on a slider also played the selected row).
    if (ImGui::IsMouseDown(0) || ImGui::IsMouseReleased(0)) b &= uint16_t(~kPadA);
    if (ImGui::IsMouseDown(1) || ImGui::IsMouseReleased(1)) b &= uint16_t(~kPadB);
    if (ImGui::GetTime() - wheel_time_ < 0.4) b &= uint16_t(~(kPadUp | kPadDown));
    // Typing a name: the keys are the field's (B / Back would close the page under it).
    if (renaming_) return;
    if (!b) return;
    if (b & (kPadB | kPadBack | kPadStart)) {
      if (tab_ == kPlaylists && edit_item_ >= 0) edit_item_ = -1;
      else if (tab_ == kAlbums && open_album_ >= 0) open_album_ = -1;
      else if (tab_ == kPlaylists && open_playlist_ >= 0) open_playlist_ = -1;
      else Close();
      return;
    }
    if (b & kPadLB) SetTab((tab_ + kTabCount - 1) % kTabCount);
    if (b & kPadRB) SetTab((tab_ + 1) % kTabCount);
    if (b & kPadUp) pad_move_ -= 1;
    if (b & kPadDown) pad_move_ += 1;
    if (b & kPadA) pad_activate_ = true;
    if (b & 0x4000) Pause(!now_.paused);  // X
  }
  void SetTab(int t) {
    tab_ = t;
    selected_ = 0;
    scroll_ = 0;
    open_album_ = open_playlist_ = -1;
    edit_item_ = -1;
    album_folder_.clear();
    renaming_ = false;
  }

  // ---- Drawing ----
  void DrawPage(ImGuiIO& io, float dt, bool live) {
    const ImVec2 screen = io.DisplaySize;
    const float a = appear_ * appear_ * (3 - 2 * appear_);
    // The page: 1920x1080 units fitted to the window.
    s_ = std::min(screen.x / 1920.0f, screen.y / 1080.0f);
    ox_ = (screen.x - 1920.0f * s_) * 0.5f;
    oy_ = (screen.y - 1080.0f * s_) * 0.5f + (1.0f - a) * 40.0f * s_;
    alpha_ = a;
    font_ = pc_options_font() ? pc_options_font() : ImGui::GetFont();
    title_font_ = pc_options_title_font() ? pc_options_title_font() : font_;

    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(screen);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::Begin("##jukebox", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     (live ? 0 : ImGuiWindowFlags_NoInputs));
    dl_ = ImGui::GetWindowDrawList();
    live_ = live;

    // The book.
    dl_->AddRectFilled({0, 0}, screen, Col(30, 30, 30, 235));
    Image(Art("back_back_D1"), {0, 0}, screen, {0, 0}, {1, 1}, 1.0f, false);
    if (!Image(Art("menu_base_$book_a"), P(0, 0), P(1730, 1080))) dl_->AddRectFilled(P(0, 0), P(1730, 1080), Col(214, 212, 204));
    Image(Art("menu_base_$book_b"), P(1722, 0), P(1790, 1080));
    Image(Art("menu_base_$book_d"), P(0, 1000), P(1760, 1064));
    Image(Art("back_moyo"), P(1180, 120), P(1700, 640), {0, 0}, {1, 1}, 0.18f);

    // Title and the tabs.
    Image(Art("menu_com_title_icon"), P(170, 92), P(216, 146));
    Text(title_font_, 50, {228, 90}, Ink(), "Jukebox");
    // Close (the touch screen's way out; the mouse's too).
    if (SmallButton("close", {1600, 96}, 56, "X")) Close();
    // The switch (2026-11-19): off at every start, and off the jukebox leaves the game's music alone.
    const bool on = Enabled();
    if (SmallButton("power", {1390, 96}, 190, on ? L("Desligar", "Switch off") : L("Ligar", "Switch on"))) SetEnabled(!on);
    if (!on) {
      DrawHints();
      Text(title_font_, 40, {230, 300}, Ink(), L("A jukebox está desligada", "The jukebox is off"));
      Text(font_, 26, {230, 370}, Faded(),
           L("Desligada, ela não toca nada e não mexe na música do jogo (nem nas trocas de música dos lugares). "
             "Ela começa desligada sempre que o jogo abre.",
             "Off, it plays nothing and leaves the game's music alone (the places' replaced music too). "
             "It starts off every time the game opens."),
           1300);
      if (SmallButton("power2", {230, 480}, 300, L("Ligar a jukebox", "Switch the jukebox on")) || pad_activate_) SetEnabled(true);
      pad_activate_ = false;
      pad_move_ = 0;
      ImGui::End();
      ImGui::PopStyleVar();
      return;
    }
    DrawHints();
    DrawTabs(dt);

    const bool wide = tab_ == kSettings;
    if (wide) DrawSettings();
    else DrawList(dt);
    if (!wide) DrawNowPlaying(dt);

    ImGui::End();
    ImGui::PopStyleVar();
  }

  void DrawHints() {
    Image(Art("explanation_explanation_plate_D5"), P(0, 1010), P(1000, 1056), {0, 0}, {1, 1}, 0.9f);
#if defined(__ANDROID__)
    // The phone (2026-11-19): no keyboard there (F6 on the bar made no sense); the fingers' ways.
    const char* t = L("Toque  tocar     Arraste  rolar     X  fechar     LB/RB  abas",
                      "Tap  play     Drag  scroll     X  close     LB/RB  tabs");
#else
    const char* t = L("LB/RB  abas     A  tocar     X  pausar     B  voltar     F6  fechar",
                      "LB/RB  tabs     A  play     X  pause     B  back     F6  close");
#endif
    dl_->AddText(font_, 24 * s_, P(60, 1020), Col(236, 234, 228), t);
  }

  void DrawTabs(float dt) {
    static const char* const kPt[] = {"Músicas do jogo", "Minhas músicas", "Álbuns", "Playlists", "Presets", "Ajustes"};
    static const char* const kEn[] = {"Game music", "My music", "Albums", "Playlists", "Presets", "Settings"};
    float x = 230;
    float sel_x = 0, sel_w = 0;
    for (int i = 0; i < kTabCount; ++i) {
      const char* name = Pt() ? kPt[i] : kEn[i];
      const float w = font_->CalcTextSizeA(30 * s_, FLT_MAX, 0, name).x / s_;
      if (Button(fmt::format("tab{}", i).c_str(), {x - 10, 170}, {x + w + 10, 214})) SetTab(i);
      Text(font_, 30, {x, 174}, i == tab_ ? Ink() : Faded(), name);
      if (i == tab_) sel_x = x, sel_w = w;
      x += w + 46;
    }
    tab_x_ += (sel_x - tab_x_) * std::min(1.0f, dt * 12.0f);
    tab_w_ += (sel_w - tab_w_) * std::min(1.0f, dt * 12.0f);
    dl_->AddRectFilled(P(tab_x_, 214), P(tab_x_ + tab_w_, 218), Ink());
    dl_->AddLine(P(200, 228), P(1660, 228), Faded(), 1.5f * s_);
  }

  // The list (left): songs, the folder, an album's / playlist's items, or the albums / playlists / presets.
  void DrawList(float dt) {
    const float left = 200, right = 1010, top = 250, row_h = 74;
    const int visible = 9;
    // Lists of collections first (albums, playlists, presets) when none is open.
    if (tab_ == kAlbums && open_album_ < 0) return DrawAlbums();
    if (tab_ == kPlaylists && open_playlist_ < 0) return DrawPlaylists();
    if (tab_ == kPresets) return DrawPresets();
    if (tab_ == kPlaylists && edit_item_ >= 0) return DrawTimeline();

    // A header for an open album / playlist (back, its options).
    float y0 = top;
    if (tab_ == kAlbums || tab_ == kPlaylists) y0 = DrawCollectionHeader(left, right, top);
    if (tab_ == kMine) y0 = DrawMineHeader(left, right, top);

    std::vector<Row> rows = Rows();
    const int n = int(rows.size());
    const Catalog& cat = GetCatalog();
    if (tab_ == kSongs && !cat.ready) {
      Text(font_, 30, {left, y0 + 20}, Faded(), L("Lendo as músicas do jogo...", "Reading the game's music..."));
      return;
    }
    if (n == 0) {
      const char* msg = tab_ == kMine ? L("Ponha arquivos MP3 ou WAV na pasta Music (Ajustes mostra onde).",
                                          "Put MP3 or WAV files in the Music folder (Settings shows where).")
                                      : L("Vazio. Use + nas músicas para adicionar.", "Empty. Use + on songs to add.");
      Text(font_, 26, {left, y0 + 20}, Faded(), msg, right - left);
      return;
    }
    const int rows_fit = std::max(1, int((1000 - y0) / row_h));
    const int vis = std::min(visible, rows_fit);
    // The list follows the selection only when the pad moved it (2026-11-19, Android: a finger dragged over the rows
    // selected the row under it, and following that undid every step of the drag: the list never scrolled).
    const bool pad_moved = pad_move_ != 0;
    if (pad_move_) {
      selected_ = std::clamp(selected_ + pad_move_, 0, n - 1);
      pad_move_ = 0;
    }
    selected_ = std::clamp(selected_, 0, n - 1);
    if (live_ && ImGui::IsMouseHoveringRect(P(left, y0), P(right, y0 + vis * row_h)) && ImGui::GetIO().MouseWheel != 0)
      scroll_ = std::clamp(scroll_ - int(ImGui::GetIO().MouseWheel), 0, std::max(0, n - vis));
    if (pad_moved && selected_ < scroll_) scroll_ = selected_;
    if (pad_moved && selected_ >= scroll_ + vis) scroll_ = selected_ - vis + 1;
    scroll_ = std::clamp(scroll_, 0, std::max(0, n - vis));
    // Touch: a finger dragged over the list (from a row, not one of its buttons) scrolls it. The mouse scrolls with
    // its wheel and the arrows only (dragging it moved the list, 2026-11-17).
    DragScroll(left, y0, right, y0 + vis * row_h, row_h, scroll_, std::max(0, n - vis));
    // The arrows above and below: a page each.
    if (scroll_ > 0 && Button("lup", {570, y0 - 20}, {650, y0})) scroll_ = std::max(0, scroll_ - vis);
    if (scroll_ + vis < n && Button("ldown", {570, y0 + vis * row_h - 4}, {650, y0 + vis * row_h + 22}))
      scroll_ = std::min(std::max(0, n - vis), scroll_ + vis);

    for (int v = 0; v < vis && scroll_ + v < n; ++v) {
      const int i = scroll_ + v;
      Row& r = rows[size_t(i)];
      const float y = y0 + v * row_h;
      const bool playing = now_.playing && now_.item.file == r.item.file && now_.item.key == r.item.key;
      const std::string id = fmt::format("row{}_{}", tab_, i);
      const bool hovered = live_ && ImGui::IsMouseHoveringRect(P(left, y), P(right - 120, y + row_h - 6));
      // Hovering selects (the mouse moving with no button down; a finger pressed on the list is a drag, no hover).
      if (hovered && !ImGui::IsMouseDown(0) && ImGui::GetIO().MouseDelta.x + ImGui::GetIO().MouseDelta.y != 0) selected_ = i;
      const bool sel = i == selected_;
      float& glow = glow_[id];
      glow += ((sel ? 1.0f : 0.0f) - glow) * std::min(1.0f, dt * 10.0f);
      if (glow > 0.02f && !Image(Art("decision_line_line"), P(left - 10, y + 4), P(left - 10 + (right - left) * glow, y + row_h - 10),
                                 {0, 0}, {1, 1}, glow))
        dl_->AddRectFilled(P(left - 10, y + 4), P(left - 10 + (right - left) * glow, y + row_h - 10), Col(250, 250, 250, int(160 * glow)));
      dl_->AddLine(P(left, y + row_h - 6), P(right, y + row_h - 6), Col(26, 30, 44, 60), 1.5f * s_);
      // A small turning record when it plays.
      if (playing) Disc({left + 22, y + 30}, 18, spin_, r.item.key, 1.0f);
      else dl_->AddCircle(P(left + 22, y + 30), 6 * s_, Faded(), 0, 2 * s_);
      const bool in_playlist = tab_ == kPlaylists && open_playlist_ >= 0;
      const bool official_button = !r.item.file && (tab_ == kSongs || tab_ == kAlbums);
      const float text_right = in_playlist ? right - 470 : official_button ? right - 340 : right - 200;
      if (in_playlist && !r.item.sections.empty())
        r.detail = fmt::format("{} {}", r.item.sections.size(), r.item.sections.size() == 1 ? L("trecho", "section") : L("trechos", "sections"));
      Clipped({left + 40, y}, {text_right, y + row_h}, [&] { Text(font_, 30, {left + 54, y + 4}, Ink(), r.title); });
      if (!r.detail.empty()) Clipped({left + 40, y}, {text_right, y + row_h - 8}, [&] { Text(font_, 20, {left + 54, y + 42}, Faded(), r.detail); });
      if (r.seconds > 0 && !in_playlist) Text(font_, 24, {right - 190, y + 8}, Faded(), Time(r.seconds));
      // Click: play (in a playlist, from that item on).
      if (hovered && Tapped(P(left, y), P(right - 120, y + row_h - 6))) Activate(r);
      if (sel && pad_activate_) Activate(r);
      // The row's own buttons: + (to a playlist / album), and in a playlist its time and order.
      DrawRowButtons(r, i, y, right, n);
    }
    pad_activate_ = false;
    if (scroll_ > 0) Image(Art("arrow_varrow"), P(590, y0 - 18), P(630, y0 - 2), {0, 1}, {1, 0});
    if (scroll_ + vis < n) Image(Art("arrow_varrow"), P(590, y0 + vis * row_h), P(630, y0 + vis * row_h + 16));
  }

  // A finger dragged over a list (from the list itself, not one of its buttons) scrolls it a row at a time; the
  // lift that ends a drag is no tap (dragged_ stays until the next press: the lift's frame cleared it before and
  // the row under the finger played).
  void DragScroll(float x0, float y0, float x1, float y1, float step, int& scroll, int max_scroll) {
    if (ImGui::IsMouseClicked(0)) dragged_ = false, drag_acc_ = 0;
    if (!kTouch || !live_ || !ImGui::IsMouseDown(0)) return;
    const ImVec2 c = ImGui::GetIO().MouseClickedPos[0];
    const ImVec2 a = P(x0, y0), b = P(x1, y1);
    if (c.x < a.x || c.x >= b.x || c.y < a.y || c.y >= b.y) return;
    if (!dragged_ && (ItemHeld() || !ImGui::IsMouseDragging(0, 12.0f))) return;
    dragged_ = true;
    drag_acc_ += ImGui::GetIO().MouseDelta.y / (step * s_);
    while (drag_acc_ > 1) drag_acc_ -= 1, scroll = std::max(0, scroll - 1);
    while (drag_acc_ < -1) drag_acc_ += 1, scroll = std::min(max_scroll, scroll + 1);
  }

  // One of the page's buttons / sliders held. Not the page itself: a press on its background makes ImGui hold the
  // window's own move id (even with NoMove), and IsAnyItemActive() said yes to it, so a finger on the list never
  // scrolled it (2026-11-19, Android).
  static bool ItemHeld() {
    const ImGuiContext& g = *ImGui::GetCurrentContext();  // (GImGui itself is not exported to the game)
    return g.ActiveId != 0 && !(g.ActiveIdWindow && g.ActiveId == g.ActiveIdWindow->MoveId);
  }

  void Activate(const Row& r) {
    if (tab_ == kPlaylists && open_playlist_ >= 0) PlayPlaylist(open_playlist_, r.index);
    else Play(r.item);
  }

  void DrawRowButtons(Row& r, int i, float y, float right, int n) {
    if (tab_ == kPlaylists && open_playlist_ >= 0) {
      // Its timeline: the volumes by stretches of time.
      if (SmallButton(fmt::format("tl{}", i).c_str(), {right - 460, y + 6}, 110, L("Trechos", "Sections"))) {
        edit_item_ = r.index;
        edit_scroll_ = 0;
      }
      // Time: whole, or minutes (a timeline).
      const std::string t = r.item.seconds > 0 ? fmt::format("{} min", int(r.item.seconds / 60 + 0.5)) : std::string(L("inteira", "whole"));
      if (SmallButton(fmt::format("t{}", i).c_str(), {right - 340, y + 6}, 110, t.c_str())) {
        const double steps[] = {0, 60, 120, 180, 300, 600, 900, 1800};
        double next = 0;
        for (double s : steps)
          if (s > r.item.seconds + 1) { next = s; break; }
        EditPlaylist([&](Playlist& p) { p.items[size_t(r.index)].seconds = next; });
      }
      if (SmallButton(fmt::format("u{}", i).c_str(), {right - 222, y + 6}, 34, "^") && r.index > 0)
        EditPlaylist([&](Playlist& p) { std::swap(p.items[size_t(r.index)], p.items[size_t(r.index - 1)]); });
      if (SmallButton(fmt::format("d{}", i).c_str(), {right - 184, y + 6}, 34, "v") && r.index + 1 < n)
        EditPlaylist([&](Playlist& p) { std::swap(p.items[size_t(r.index)], p.items[size_t(r.index + 1)]); });
      if (SmallButton(fmt::format("x{}", i).c_str(), {right - 60, y + 6}, 40, "x"))
        EditPlaylist([&](Playlist& p) { p.items.erase(p.items.begin() + r.index); });
      return;
    }
    // The official mode: the song with its layers moved by the game, live (in its own place).
    if (!r.item.file && (tab_ == kSongs || tab_ == kAlbums)) {
      const bool on = now_.playing && now_.official && now_.item.key == r.item.key;
      if (SmallButton(fmt::format("of{}", i).c_str(), {right - 330, y + 6}, 120, L("Oficial", "Official")))
        PlayOfficial(r.item.key);
      if (on) dl_->AddCircleFilled(P(right - 222, y + 25), 5 * s_, now_.official_live ? Col(200, 70, 60) : Faded());
    }
    if (tab_ == kAlbums && open_album_ >= 0) {
      if (SmallButton(fmt::format("x{}", i).c_str(), {right - 60, y + 6}, 40, "x")) {
        {
          std::lock_guard lock(LibraryMutex());
          auto& al = GetLibrary().albums[size_t(open_album_)];
          if (r.index < int(al.keys.size())) al.keys.erase(al.keys.begin() + r.index);
        }
        Save();
      }
      return;
    }
    if (SmallButton(fmt::format("add{}", i).c_str(), {right - 60, y + 6}, 40, "+")) {
      add_item_ = r.item;
      add_open_ = true;
    }
  }

  // ---- The timeline: one playlist item's sections (from, to, the layers' volumes) ----
  // Left side, in place of the list; the right side keeps playing, so the edits are heard (SetTimeline).
  void DrawTimeline() {
    const float left = 200, right = 1010, top = 250;
    Item item;
    {
      std::lock_guard lock(LibraryMutex());
      auto& pls = GetLibrary().playlists;
      if (open_playlist_ < 0 || open_playlist_ >= int(pls.size()) || edit_item_ >= int(pls[size_t(open_playlist_)].items.size())) {
        edit_item_ = -1;
        return;
      }
      item = pls[size_t(open_playlist_)].items[size_t(edit_item_)];
    }
    const std::vector<std::string> parts = PartsOf(item);
    const size_t nl = std::min(parts.size(), size_t(kMaxLayers));
    // The song's length (for the times' limit): known for the game's, else up to an hour.
    const Song* song = item.file ? nullptr : GetCatalog().Find(item.key);
    const double limit = item.seconds > 0 ? item.seconds : song && !song->loops && song->seconds > 0 ? song->seconds : 3600.0;
    const bool this_playing = now_.playing && now_.playlist == open_playlist_ && now_.playlist_index == edit_item_;

    if (SmallButton("tlback", {left, top + 4}, 44, "<")) {
      edit_item_ = -1;
      return;
    }
    Clipped({left + 60, top}, {right, top + 46}, [&] {
      Text(title_font_, 32, {left + 60, top}, Ink(), fmt::format("{}: {}", L("Trechos", "Sections"), Title(item.key, item.file)));
    });
    float y = top + 54;
    // The item's time in the playlist (whole, or minutes and seconds).
    Text(font_, 22, {left, y + 6}, Ink(), L("Tempo na playlist", "Time in the playlist"));
    if (Repeat("tlsec-", {left + 250, y}, 44, "-"))
      EditItem([](Item& it) { it.seconds = it.seconds <= 30 ? 0.0 : it.seconds - 30; });
    Text(font_, 24, {left + 304, y + 4}, Ink(), item.seconds > 0 ? Time(item.seconds) : std::string(L("inteira", "whole")));
    if (Repeat("tlsec+", {left + 400, y}, 44, "+")) EditItem([](Item& it) { it.seconds += 30; });
    if (SmallButton("tlplay", {right - 170, y}, 160, this_playing ? L("Do começo", "From the start") : L("Ouvir", "Listen"))) {
      PlayPlaylist(open_playlist_, edit_item_);
    }
    y += 50;
    Text(font_, 20, {left, y}, Faded(),
         L("Cada trecho dá um volume a cada camada entre dois tempos. Fora dos trechos toca a mistura da música.",
           "Each section sets each layer's volume between two times. Outside them the song's own mix plays."),
         right - left);
    y += 60;

    // The sections, a block each.
    const float bottom = 936;
    const float block_h = 56 + 34 * float(nl) + 14;
    const int n = int(item.sections.size());
    const int fit = std::max(1, int((bottom - y) / block_h));
    if (live_ && ImGui::IsMouseHoveringRect(P(left, y), P(right, bottom)) && ImGui::GetIO().MouseWheel != 0)
      edit_scroll_ -= int(ImGui::GetIO().MouseWheel);
    DragScroll(left, y, right, bottom, block_h, edit_scroll_, std::max(0, n - fit));
    edit_scroll_ = std::clamp(edit_scroll_, 0, std::max(0, n - fit));
    if (n == 0)
      Text(font_, 24, {left, y + 10}, Faded(), L("Nenhum trecho ainda: use \"+ Trecho\".", "No sections yet: use \"+ Section\"."), right - left);
    for (int v = 0; v < fit && edit_scroll_ + v < n; ++v) {
      const int si = edit_scroll_ + v;
      const Section sec = item.sections[size_t(si)];
      const float by = y + float(v) * block_h;
      const bool active = this_playing && now_.section == si;
      dl_->AddRectFilled(P(left - 10, by), P(right, by + block_h - 10), active ? Col(250, 248, 236, 210) : Col(250, 250, 248, 110), 6 * s_);
      dl_->AddRect(P(left - 10, by), P(right, by + block_h - 10), active ? Ink() : Faded(), 6 * s_, 0, (active ? 2.0f : 1.0f) * s_);
      Text(font_, 24, {left + 4, by + 10}, Ink(), fmt::format("{} {}", L("Trecho", "Section"), si + 1));
      // From / to: 5 s a tap, held it runs.
      const std::string id = fmt::format("s{}", si);
      if (Repeat((id + "f-").c_str(), {left + 150, by + 8}, 40, "-"))
        EditSection(si, [](Section& s) { s.from = std::max(0.0, s.from - 5); });
      Text(font_, 24, {left + 198, by + 10}, Ink(), Time0(sec.from));
      if (Repeat((id + "f+").c_str(), {left + 268, by + 8}, 40, "+"))
        EditSection(si, [&](Section& s) { s.from = std::min(s.to - 5, s.from + 5); });
      Text(font_, 22, {left + 320, by + 12}, Faded(), L("até", "to"));
      if (Repeat((id + "t-").c_str(), {left + 362, by + 8}, 40, "-"))
        EditSection(si, [](Section& s) { s.to = std::max(s.from + 5, s.to - 5); });
      Text(font_, 24, {left + 410, by + 10}, Ink(), Time0(sec.to));
      if (Repeat((id + "t+").c_str(), {left + 480, by + 8}, 40, "+"))
        EditSection(si, [&](Section& s) { s.to = std::min(limit, s.to + 5); });
      // Listen from here; take the mix playing now; remove.
      if (SmallButton((id + "go").c_str(), {right - 270, by + 8}, 80, L("Ouvir", "Listen"))) {
        PlayPlaylist(open_playlist_, edit_item_);
        Seek(sec.from);
      }
      if (this_playing && SmallButton((id + "mix").c_str(), {right - 180, by + 8}, 110, L("Mistura atual", "Mix now"))) {
        const auto lv = now_.item.layers;
        EditSection(si, [lv](Section& s) { s.layers = lv; });
      }
      if (SmallButton((id + "x").c_str(), {right - 60, by + 8}, 44, "x")) {
        EditItem([si](Item& it) {
          if (si < int(it.sections.size())) it.sections.erase(it.sections.begin() + si);
        });
        continue;
      }
      // The layers' volumes.
      for (size_t l = 0; l < nl; ++l) {
        const float ly = by + 56 + 34 * float(l);
        Clipped({left + 10, ly - 4}, {left + 210, ly + 30}, [&] { Text(font_, 22, {left + 14, ly}, Ink(), parts[l]); });
        float value = sec.layers[l];
        if (Slider(fmt::format("{}l{}", id, l).c_str(), {left + 220, ly + 13}, 440, value)) {
          EditSection(si, [l, value](Section& s) { s.layers[l] = value; }, false);
          timeline_dirty_ = true;
        }
        Text(font_, 22, {left + 690, ly}, Faded(), fmt::format("{}%", int(value * 100 + 0.5f)));
      }
    }
    if (timeline_dirty_ && !ImGui::IsMouseDown(0)) {
      timeline_dirty_ = false;
      Save();
    }
    if (edit_scroll_ > 0 && Button("tlup", {570, y - 20}, {650, y}))
      edit_scroll_ = std::max(0, edit_scroll_ - fit);
    if (edit_scroll_ > 0) Image(Art("arrow_varrow"), P(590, y - 18), P(630, y - 2), {0, 1}, {1, 0});
    if (edit_scroll_ + fit < n) {
      Image(Art("arrow_varrow"), P(590, bottom - 16), P(630, bottom));
      if (Button("tldown", {570, bottom - 20}, {650, bottom + 4})) edit_scroll_ = std::min(std::max(0, n - fit), edit_scroll_ + fit);
    }
    // New: after the last one, a minute long, with its mix (or the song's).
    if (SmallButton("tladd", {left, 946}, 220, L("+ Trecho", "+ Section"))) {
      EditItem([&](Item& it) {
        Section s;
        if (!it.sections.empty()) {
          s.from = it.sections.back().to;
          s.layers = it.sections.back().layers;
        } else {
          s.layers = it.layers;
        }
        s.from = std::min(s.from, std::max(0.0, limit - 5));
        s.to = std::min(limit, s.from + 60);
        it.sections.push_back(s);
      });
      edit_scroll_ = std::max(0, int(item.sections.size()) + 1 - fit);
    }
    // Remove all: a second tap within 3 s confirms.
    const bool armed = ImGui::GetTime() - clear_armed_ < 3.0;
    if (n > 0 && SmallButton("tlclear", {left + 240, 946}, 220, armed ? L("Confirmar?", "Sure?") : L("Apagar todos", "Remove all"))) {
      if (armed) {
        EditItem([](Item& it) { it.sections.clear(); });
        clear_armed_ = -10;
      } else {
        clear_armed_ = ImGui::GetTime();
      }
    }
  }

  static std::string Time0(double s) {  // 0:00 included
    const int t = int(std::max(0.0, s) + 0.5);
    return fmt::format("{}:{:02}", t / 60, t % 60);
  }

  // Changes the edited item (saved; the playing one follows).
  template <typename Fn>
  void EditItem(Fn fn, bool save = true) {
    Item copy;
    bool ok = false;
    {
      std::lock_guard lock(LibraryMutex());
      auto& pls = GetLibrary().playlists;
      if (open_playlist_ >= 0 && open_playlist_ < int(pls.size()) && edit_item_ >= 0 &&
          edit_item_ < int(pls[size_t(open_playlist_)].items.size())) {
        Item& it = pls[size_t(open_playlist_)].items[size_t(edit_item_)];
        fn(it);
        for (Section& s : it.sections) {
          s.from = std::max(0.0, s.from);
          if (s.to < s.from + 1) s.to = s.from + 1;
        }
        copy = it;
        ok = true;
      }
    }
    if (!ok) return;
    if (save) Save();
    SetTimeline(open_playlist_, edit_item_, copy);
  }
  template <typename Fn>
  void EditSection(int si, Fn fn, bool save = true) {
    EditItem(
        [&](Item& it) {
          if (si >= 0 && si < int(it.sections.size())) fn(it.sections[size_t(si)]);
        },
        save);
  }

  // A small button that repeats while held (a time stepper).
  bool Repeat(const char* id, ImVec2 at, float w, const char* label) {
    ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
    const bool r = SmallButton(id, at, w, label);
    ImGui::PopItemFlag();
    return r;
  }

  template <typename Fn>
  void EditPlaylist(Fn fn) {
    {
      std::lock_guard lock(LibraryMutex());
      auto& pls = GetLibrary().playlists;
      if (open_playlist_ >= 0 && open_playlist_ < int(pls.size())) fn(pls[size_t(open_playlist_)]);
    }
    Save();
  }

  float DrawCollectionHeader(float left, float right, float top) {
    std::string name;
    bool shuffle = false;
    int repeat = 1;
    {
      std::lock_guard lock(LibraryMutex());
      auto& lib = GetLibrary();
      if (tab_ == kAlbums && open_album_ < int(lib.albums.size())) name = lib.albums[size_t(open_album_)].name;
      if (tab_ == kPlaylists && open_playlist_ < int(lib.playlists.size())) {
        name = lib.playlists[size_t(open_playlist_)].name;
        shuffle = lib.playlists[size_t(open_playlist_)].shuffle;
        repeat = lib.playlists[size_t(open_playlist_)].repeat;
      }
    }
    if (SmallButton("back", {left, top + 4}, 44, "<")) {
      open_album_ = open_playlist_ = -1;
      renaming_ = false;
      return top;
    }
    // The name; "Renomear" (or a tap on it) turns it into a field (the phone's keyboard comes up).
    if (renaming_) {
      if (const std::string done = RenameField(left + 60, top, right - left - 420); !done.empty()) {
        SetLib([&](Library& l) {
          if (tab_ == kAlbums && open_album_ < int(l.albums.size())) l.albums[size_t(open_album_)].name = done;
          if (tab_ == kPlaylists && open_playlist_ < int(l.playlists.size())) l.playlists[size_t(open_playlist_)].name = done;
        });
      }
    } else {
      Clipped({left + 60, top}, {right - 270, top + 50}, [&] { Text(title_font_, 36, {left + 60, top}, Ink(), name); });
      const bool tap = Tapped(P(left + 60, top), P(right - 270, top + 46));
      if (SmallButton("rename", {right - 250, top + 4}, 120, L("Renomear", "Rename")) || tap) StartRename(name);
    }
    if (tab_ == kPlaylists) {
      float x = left + 60;
      if (SmallButton("playall", {x, top + 52}, 150, L("Tocar tudo", "Play all"))) PlayPlaylist(open_playlist_, 0);
      x += 160;
      if (SmallButton("shuffle", {x, top + 52}, 170, shuffle ? L("Aleatório: sim", "Shuffle: on") : L("Aleatório: não", "Shuffle: off")))
        EditPlaylist([](Playlist& p) { p.shuffle = !p.shuffle; });
      x += 180;
      const char* rep = repeat == 0 ? L("Repetir: não", "Repeat: off") : repeat == 1 ? L("Repetir: tudo", "Repeat: all") : L("Repetir: uma", "Repeat: one");
      if (SmallButton("repeat", {x, top + 52}, 170, rep)) EditPlaylist([](Playlist& p) { p.repeat = (p.repeat + 1) % 3; });
      x += 180;
      if (SmallButton("delpl", {right - 120, top + 4}, 110, L("Apagar", "Delete"))) {
        {
          std::lock_guard lock(LibraryMutex());
          auto& pls = GetLibrary().playlists;
          if (open_playlist_ < int(pls.size())) pls.erase(pls.begin() + open_playlist_);
        }
        Save();
        open_playlist_ = -1;
      }
      Text(font_, 20, {left + 60, top + 96}, Faded(),
           L("Tempo de cada música: toque nele (inteira, 1, 2, 3, 5, 10, 15, 30 min). Trechos: volumes por tempo.",
             "Each song's time: tap it (whole, 1, 2, 3, 5, 10, 15, 30 min). Sections: volumes by time."),
           right - left - 60);
      return top + 130;
    }
    if (SmallButton("delal", {right - 120, top + 4}, 110, L("Apagar", "Delete"))) {
      {
        std::lock_guard lock(LibraryMutex());
        auto& als = GetLibrary().albums;
        if (open_album_ < int(als.size())) als.erase(als.begin() + open_album_);
      }
      Save();
      open_album_ = -1;
    }
    return top + 60;
  }

  float DrawMineHeader(float left, float right, float top) {
    // The folder's subfolders: shown as albums of their own.
    std::vector<std::string> folders;
    for (const UserSong& u : UserSongs())
      if (!u.folder.empty() && std::find(folders.begin(), folders.end(), u.folder) == folders.end()) folders.push_back(u.folder);
    float x = left;
    if (SmallButton("allf", {x, top + 4}, 100, L("Todas", "All"))) album_folder_.clear();
    x += 110;
    for (size_t i = 0; i < folders.size() && x < right - 160; ++i) {
      const float w = std::max(100.0f, font_->CalcTextSizeA(24 * s_, FLT_MAX, 0, folders[i].c_str()).x / s_ + 30);
      if (SmallButton(fmt::format("f{}", i).c_str(), {x, top + 4}, w, folders[i].c_str())) album_folder_ = folders[i];
      x += w + 10;
    }
    if (SmallButton("rescan", {right - 150, top + 4}, 140, L("Atualizar", "Rescan"))) RescanMusicFolder();
    return top + 60;
  }

  void DrawAlbums() {
    std::vector<Album> albums;
    {
      std::lock_guard lock(LibraryMutex());
      albums = GetLibrary().albums;
    }
    const float left = 200, top = 260;
    if (SmallButton("newal", {left, top}, 200, L("+ Novo álbum", "+ New album"))) {
      {
        std::lock_guard lock(LibraryMutex());
        GetLibrary().albums.push_back({fmt::format("{} {}", L("Álbum", "Album"), GetLibrary().albums.size() + 1), "", {}});
      }
      Save();
    }
    // Covers: a record each, three across.
    for (size_t i = 0; i < albums.size(); ++i) {
      const float cx = left + 130 + float(i % 3) * 270, cy = top + 170 + float(i / 3) * 290;
      const bool hov = live_ && ImGui::IsMouseHoveringRect(P(cx - 120, cy - 120), P(cx + 120, cy + 150));
      const std::string cover = albums[i].cover.empty() && !albums[i].keys.empty() ? albums[i].keys[0] : albums[i].cover;
      Disc({cx, cy}, hov ? 112 : 104, hov ? spin_ : 0.0f, cover.empty() ? albums[i].name : cover, 1.0f);
      Text(font_, 26, {cx - 120, cy + 118}, Ink(), albums[i].name, 240);
      Text(font_, 20, {cx - 120, cy + 148}, Faded(), fmt::format("{} {}", albums[i].keys.size(), L("músicas", "songs")), 240);
      if (hov && Tapped(P(cx - 120, cy - 120), P(cx + 120, cy + 150))) {
        open_album_ = int(i);
        selected_ = scroll_ = 0;
      }
      if (hov && ImGui::IsMouseReleased(1) && !albums[i].keys.empty()) {
        // Right click: play the album as a list (a playlist made for it).
        PlayAlbum(albums[i]);
      }
    }
    if (albums.empty())
      Text(font_, 24, {left, top + 70}, Faded(),
           L("Crie álbuns e use + nas músicas para juntar as suas favoritas.", "Make albums and use + on songs to gather your favourites."), 760);
  }

  void PlayAlbum(const Album& al) {
    int index;
    {
      std::lock_guard lock(LibraryMutex());
      auto& pls = GetLibrary().playlists;
      const std::string name = fmt::format("{} ({})", al.name, L("álbum", "album"));
      auto it = std::find_if(pls.begin(), pls.end(), [&](const Playlist& p) { return p.name == name; });
      if (it == pls.end()) it = pls.insert(pls.end(), Playlist{name, {}, false, 1});
      it->items.clear();
      const Catalog& cat = GetCatalog();
      for (const auto& k : al.keys) {
        Item item;
        item.file = cat.Find(k) == nullptr;
        item.key = k;
        it->items.push_back(item);
      }
      index = int(it - pls.begin());
    }
    Save();
    PlayPlaylist(index, 0);
  }

  void DrawPlaylists() {
    std::vector<Playlist> pls;
    {
      std::lock_guard lock(LibraryMutex());
      pls = GetLibrary().playlists;
    }
    const float left = 200, right = 1010, top = 260, row_h = 70;
    if (SmallButton("newpl", {left, top}, 230, L("+ Nova playlist", "+ New playlist"))) {
      {
        std::lock_guard lock(LibraryMutex());
        GetLibrary().playlists.push_back({fmt::format("Playlist {}", GetLibrary().playlists.size() + 1), {}, false, 1});
      }
      Save();
    }
    for (size_t i = 0; i < pls.size() && i < 10; ++i) {
      const float y = top + 60 + float(i) * row_h;
      const bool hov = live_ && ImGui::IsMouseHoveringRect(P(left, y), P(right - 140, y + row_h - 8));
      if (hov) dl_->AddRectFilled(P(left - 10, y), P(right - 140, y + row_h - 8), Col(250, 250, 250, 120));
      double total = 0;
      bool unknown = false;
      for (const Item& it : pls[i].items) {
        if (it.seconds > 0) total += it.seconds;
        else if (const Song* s = GetCatalog().Find(it.key)) total += s->seconds;
        else unknown = true;
      }
      Text(font_, 30, {left + 10, y + 4}, Ink(), pls[i].name);
      Text(font_, 20, {left + 10, y + 40}, Faded(),
           fmt::format("{} {}  ·  {}{}", pls[i].items.size(), L("itens", "items"), Time(total), unknown ? "+" : ""));
      if (hov && Tapped(P(left, y), P(right - 140, y + row_h - 8))) {
        open_playlist_ = int(i);
        edit_item_ = -1;
        selected_ = scroll_ = 0;
      }
      if (SmallButton(fmt::format("plplay{}", i).c_str(), {right - 130, y + 8}, 120, L("Tocar", "Play"))) PlayPlaylist(int(i), 0);
    }
    if (pls.empty())
      Text(font_, 24, {left, top + 70}, Faded(),
           L("Uma playlist pode ser uma linha do tempo: cada música com seu tempo (ex.: X por 5 min, depois Y por 3).",
             "A playlist can be a timeline: each song with its own time (e.g. X for 5 min, then Y for 3)."),
           right - left);
  }

  void DrawPresets() {
    std::vector<Preset> presets;
    {
      std::lock_guard lock(LibraryMutex());
      presets = GetLibrary().presets;
    }
    const float left = 200, right = 1010, top = 260, row_h = 66;
    Text(font_, 22, {left, top}, Faded(),
         L("Presets guardam a mistura das camadas. Salve um no painel à direita enquanto uma música toca.",
           "Presets keep a layer mix. Save one in the panel on the right while a song plays."),
         right - left);
    for (size_t i = 0; i < presets.size() && i < 10; ++i) {
      const float y = top + 70 + float(i) * row_h;
      const bool hov = live_ && ImGui::IsMouseHoveringRect(P(left, y), P(right - 120, y + row_h - 8));
      if (hov) dl_->AddRectFilled(P(left - 10, y), P(right - 120, y + row_h - 8), Col(250, 250, 250, 120));
      Text(font_, 28, {left + 10, y + 4}, Ink(), presets[i].name);
      std::string mix;
      for (int l = 0; l < kMaxLayers; ++l) mix += fmt::format("{}{}%", l ? "  " : "", int(presets[i].layers[size_t(l)] * 100));
      const std::string song = presets[i].key.empty() ? std::string(L("qualquer música", "any song")) : Title(presets[i].key, false);
      Text(font_, 20, {left + 10, y + 38}, Faded(), song + "  ·  " + mix);
      if (hov && Tapped(P(left, y), P(right - 120, y + row_h - 8))) {
        if (!presets[i].key.empty() && (!now_.playing || now_.item.key != presets[i].key)) {
          Item it;
          it.key = presets[i].key;
          it.layers = presets[i].layers;
          Play(it);
        } else {
          SetLayers(presets[i].layers);
        }
      }
      if (SmallButton(fmt::format("delpr{}", i).c_str(), {right - 110, y + 8}, 100, L("Apagar", "Delete"))) {
        {
          std::lock_guard lock(LibraryMutex());
          auto& p = GetLibrary().presets;
          if (i < p.size()) p.erase(p.begin() + long(i));
        }
        Save();
      }
    }
  }

  void DrawSettings() {
    Library lib;
    {
      std::lock_guard lock(LibraryMutex());
      lib = GetLibrary();
    }
    const float left = 220, top = 270;
    float y = top;
    auto toggle = [&](const char* id, const char* label, bool value, auto set) {
      if (SmallButton(id, {left, y}, 90, value ? L("Sim", "On") : L("Não", "Off"))) {
        {
          std::lock_guard lock(LibraryMutex());
          set(GetLibrary(), !value);
        }
        Save();
      }
      Text(font_, 28, {left + 110, y + 2}, Ink(), label);
      y += 64;
    };
    toggle("hold", L("Silenciar a música do jogo enquanto a jukebox toca", "Silence the game's music while the jukebox plays"),
           lib.hold_game_music, [](Library& l, bool v) { l.hold_game_music = v; });
    toggle("keep", L("Continuar tocando ao fechar a página", "Keep playing after closing the page"), lib.keep_playing,
           [](Library& l, bool v) { l.keep_playing = v; });
    Text(font_, 28, {left, y}, Ink(), fmt::format("{}: {:.1f} s", L("Transição entre músicas", "Crossfade"), lib.crossfade));
    if (SmallButton("cf-", {left + 560, y}, 50, "-")) SetLib([](Library& l) { l.crossfade = std::max(0.0f, l.crossfade - 0.5f); });
    if (SmallButton("cf+", {left + 620, y}, 50, "+")) SetLib([](Library& l) { l.crossfade = std::min(10.0f, l.crossfade + 0.5f); });
    y += 70;
    Text(font_, 28, {left, y}, Ink(), L("Pasta Music (suas músicas, MP3 e WAV; subpastas viram álbuns):",
                                        "Music folder (your songs, MP3 and WAV; subfolders become albums):"));
    y += 40;
    Text(font_, 22, {left + 20, y}, Faded(), MusicFolder().string(), 1300);
    y += 50;
#if defined(_WIN32)
    if (SmallButton("openf", {left, y}, 220, L("Abrir a pasta", "Open the folder"))) {
      std::error_code ec;
      std::filesystem::create_directories(MusicFolder(), ec);
      ShellExecuteW(nullptr, L"open", MusicFolder().wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
#endif
    if (SmallButton("rescan2", {left + 240, y}, 220, L("Atualizar lista", "Rescan"))) RescanMusicFolder();
    y += 80;
    // The places whose music is replaced.
    Text(font_, 28, {left, y}, Ink(), L("Músicas trocadas em lugares do jogo:", "Places whose music is replaced:"));
    y += 44;
    if (lib.replace.empty())
      Text(font_, 22, {left + 20, y}, Faded(),
           L("Nenhuma. No painel Tocando agora, use \"Tocar aqui sempre\" enquanto o jogo toca a música do lugar.",
             "None. In Now playing, use \"Always play here\" while the game plays that place's music."),
           1300);
    for (auto& [cue, item] : lib.replace) {
      const Song* from = GetCatalog().ByCue(cue);
      Text(font_, 22, {left + 20, y}, Ink(),
           fmt::format("{}  ->  {}", from ? Title(from->key, false) : fmt::format("cue {}", cue), Title(item.key, item.file)));
      if (SmallButton(fmt::format("rr{}", cue).c_str(), {left + 1100, y - 4}, 110, L("Remover", "Remove"))) {
        SetLib([c = cue](Library& l) { l.replace.erase(c); });
        break;
      }
      y += 40;
    }
  }

  template <typename Fn>
  void SetLib(Fn fn) {
    {
      std::lock_guard lock(LibraryMutex());
      fn(GetLibrary());
    }
    Save();
  }

  // The right side: the record, the song, the time, the controls, the layers.
  void DrawNowPlaying(float dt) {
    // Laid out top to bottom in 1040..1680, all of it above the bottom strip (y < 990).
    const float cx = 1360, cy = 335, r = 118;
    const float left = 1040, right = 1680;
    // The record player: a plate, the record, the tone arm.
    dl_->AddCircleFilled(P(cx, cy), (r + 16) * s_, Col(40, 38, 36, 220), 96);
    dl_->AddCircle(P(cx, cy), (r + 16) * s_, Col(26, 30, 44, 180), 96, 2 * s_);
    if (now_.playing) Disc({cx, cy}, r, spin_, now_.item.key, 1.0f);
    else Disc({cx, cy}, r, 0, "", 0.5f);
    {
      // The arm pivots at the top right; it swings onto the record while playing.
      const ImVec2 pivot = P(cx + r + 34, cy - r - 6);
      const float ang = -0.15f + 0.77f * arm_;
      const float len = (r + 60) * s_;
      const ImVec2 tip(pivot.x - std::sin(ang) * len * 0.35f, pivot.y + std::cos(ang) * len);
      const ImVec2 mid(pivot.x - std::sin(ang) * len * 0.8f, pivot.y + std::cos(ang) * len * 0.55f);
      dl_->AddCircleFilled(pivot, 14 * s_, Col(90, 88, 84, 240));
      dl_->AddBezierQuadratic(pivot, mid, tip, Col(200, 196, 186, 240), 5 * s_);
      dl_->AddRectFilled({tip.x - 7 * s_, tip.y - 4 * s_}, {tip.x + 7 * s_, tip.y + 14 * s_}, Col(60, 58, 54, 255), 2 * s_);
    }
    float y = cy + r + 26;
    // Title, time, the bar (click to seek).
    const std::string title = now_.playing ? now_.title : std::string(L("Nada tocando", "Nothing playing"));
    Clipped({left, y}, {right, y + 44}, [&] { Text(title_font_, 32, {left, y}, Ink(), title); });
    y += 44;
    if (now_.playing) {
      const double len = now_.length;
      const float f = len > 0 ? float(std::clamp(now_.position / len, 0.0, 1.0)) : 0.0f;
      const ImVec2 b0 = P(left, y + 10), b1 = P(right, y + 18);
      dl_->AddRectFilled(b0, b1, Col(90, 92, 100, 110), 3 * s_);
      dl_->AddRectFilled(b0, {b0.x + (b1.x - b0.x) * f, b1.y}, Ink(), 3 * s_);
      dl_->AddCircleFilled({b0.x + (b1.x - b0.x) * f, (b0.y + b1.y) / 2}, 8 * s_, Ink());
      if (live_ && len > 0 && ImGui::IsMouseHoveringRect({b0.x, b0.y - 12 * s_}, {b1.x, b1.y + 12 * s_}) && ImGui::IsMouseClicked(0)) {
        const float mx = (ImGui::GetIO().MousePos.x - b0.x) / (b1.x - b0.x);
        Seek(std::clamp(double(mx), 0.0, 1.0) * len);
      }
      Text(font_, 22, {left, y + 24}, Faded(), Time(now_.position));
      Text(font_, 22, {right - 70, y + 24}, Faded(), len > 0 ? Time(len) : std::string("--:--"));
      if (now_.playlist >= 0) {
        std::string pl;
        {
          std::lock_guard lock(LibraryMutex());
          if (now_.playlist < int(GetLibrary().playlists.size())) pl = GetLibrary().playlists[size_t(now_.playlist)].name;
        }
        if (now_.section >= 0 && now_.section < int(now_.item.sections.size())) {
          const Section& sc = now_.item.sections[size_t(now_.section)];
          pl += fmt::format("  ·  {} {} ({}–{})", L("Trecho", "Section"), now_.section + 1, Time0(sc.from), Time0(sc.to));
        }
        Clipped({left + 100, y + 20}, {right - 90, y + 52}, [&] { Text(font_, 20, {left + 110, y + 24}, Faded(), pl); });
      }
    }
    y += 56;
    // Transport and volume.
    {
      float x = left;
      if (IconButton("prev", {x, y}, 0)) Previous();
      x += 70;
      if (IconButton("play", {x, y}, now_.playing && !now_.paused ? 2 : 1)) {
        if (now_.playing) Pause(!now_.paused);
      }
      x += 70;
      if (IconButton("stop", {x, y}, 3)) Stop();
      x += 70;
      if (IconButton("next", {x, y}, 4)) Next();
      x += 84;
      float vol;
      {
        std::lock_guard lock(LibraryMutex());
        vol = GetLibrary().volume;
      }
      Text(font_, 22, {x, y + 12}, Faded(), L("Volume", "Volume"));
      if (Slider("vol", {x + 90, y + 24}, right - x - 100, vol)) {
        std::lock_guard lock(LibraryMutex());
        GetLibrary().volume = vol;
        volume_dirty_ = true;
      }
      if (volume_dirty_ && !ImGui::IsMouseDown(0)) {
        volume_dirty_ = false;
        Save();
      }
    }
    y += 64;
    // The layers, and the mixes (one row of buttons).
    if (now_.playing && !now_.item.file && now_.parts.size() > 1) {
      Text(font_, 24, {left, y}, Ink(), L("Camadas", "Layers"));
      if (now_.official)
        Clipped({left + 120, y}, {right, y + 30}, [&] {
          Text(font_, 20, {left + 120, y + 3}, now_.official_live ? Col(170, 60, 50) : Faded(),
               now_.official_live ? L("Modo oficial: seguindo o jogo ao vivo", "Official mode: following the game live")
                                  : L("Modo oficial: vá ao lugar desta música", "Official mode: go to this song's place"));
        });
      y += 32;
      std::array<float, kMaxLayers> layers = now_.item.layers;
      bool changed = false;
      for (size_t i = 0; i < now_.parts.size(); ++i) {
        Clipped({left, y - 4}, {left + 200, y + 30}, [&] { Text(font_, 22, {left, y}, Ink(), now_.parts[i]); });
        if (Slider(fmt::format("layer{}", i).c_str(), {left + 210, y + 13}, 330, layers[i])) changed = true;
        // The layer's loudness now.
        const float lv = std::min(1.0f, now_.levels[i] * layers[i] * 1.4f);
        dl_->AddRectFilled(P(left + 560, y + 8), P(left + 560 + 80 * lv, y + 18), Col(26, 30, 44, 200), 2 * s_);
        dl_->AddRect(P(left + 560, y + 8), P(left + 640, y + 18), Faded(), 2 * s_);
        y += 34;
      }
      if (changed) SetLayers(layers, 0.15f);
      y += 6;
      auto voice = [&](size_t i) {
        const std::string p = Lower(now_.parts[i]);
        return p.find("voz") != std::string::npos || p.find("vocal") != std::string::npos || p.find("coral") != std::string::npos ||
               p.find("choir") != std::string::npos;
      };
      float x = left;
      if (SmallButton("mixall", {x, y}, 104, L("Completa", "Full"))) SetLayers({1, 1, 1, 1});
      x += 112;
      if (SmallButton("mixinst", {x, y}, 146, L("Instrumental", "Instrumental"))) {
        std::array<float, kMaxLayers> l{1, 1, 1, 1};
        for (size_t i = 0; i < now_.parts.size(); ++i) l[i] = voice(i) ? 0.0f : 1.0f;
        SetLayers(l);
      }
      x += 154;
      if (SmallButton("mixvoice", {x, y}, 96, L("Só voz", "Vocals"))) {
        std::array<float, kMaxLayers> l{0, 0, 0, 0};
        for (size_t i = 0; i < now_.parts.size(); ++i) l[i] = voice(i) ? 1.0f : 0.0f;
        SetLayers(l);
      }
      x += 104;
      if (game_.layer_count > 0 && SmallButton("mixgame", {x, y}, 96, L("Do jogo", "Game's"))) SetLayers(game_.layers);
      x += 104;
      if (SmallButton("savepreset", {x, y}, right - x, L("Salvar preset", "Save preset"))) {
        {
          std::lock_guard lock(LibraryMutex());
          auto& p = GetLibrary().presets;
          Preset pr;
          pr.key = now_.item.key;
          pr.layers = now_.item.layers;
          pr.name = fmt::format("{} {}", L("Mistura", "Mix"), p.size() + 1);
          p.push_back(pr);
        }
        Save();
      }
      y += 48;
    }
    // The game's own music, and making ours play there: below the rest (never over it).
    {
      const float gy = std::max(y + 6, 880.0f);
      const Song* g = game_.cue >= 0 ? GetCatalog().ByCue(uint32_t(game_.cue)) : nullptr;
      const std::string line = fmt::format("{}: {}{}", L("Jogo", "Game"),
                                           g ? Title(g->key, false) : std::string(L("sem música", "no music")),
                                           game_.held ? std::string(L("  (silenciada)", "  (silenced)")) : std::string());
      Clipped({left, gy}, {right, gy + 28}, [&] { Text(font_, 20, {left, gy}, Faded(), line); });
      if (g && now_.playing && SmallButton("replace", {left, gy + 30}, 300, L("Tocar esta aqui sempre", "Always play this here"))) {
        Item it = now_.item;
        SetLib([c = uint32_t(game_.cue), it](Library& l) { l.replace[c] = it; });
      }
      if (g && SmallButton("playgame", {left + 310, gy + 30}, right - left - 310, L("Tocar a música do jogo", "Play the game's song"))) {
        Item it;
        it.key = g->key;
        for (int i = 0; i < kMaxLayers; ++i) it.layers[size_t(i)] = game_.layer_count ? game_.layers[size_t(i)] : 1.0f;
        Play(it);
      }
    }
    DrawAddPopup();
    (void)dt;
  }

  // "+": to a playlist or an album.
  void DrawAddPopup() {
    if (!add_open_) return;
    const float x0 = 560, y0 = 300, x1 = 1060, y1 = 860;
    dl_->AddRectFilled(P(x0, y0), P(x1, y1), Col(236, 234, 228, 250), 8 * s_);
    dl_->AddRect(P(x0, y0), P(x1, y1), Ink(), 8 * s_, 0, 2 * s_);
    Text(font_, 26, {x0 + 20, y0 + 14}, Ink(), L("Adicionar a...", "Add to..."));
    std::vector<std::string> pls, als;
    {
      std::lock_guard lock(LibraryMutex());
      for (auto& p : GetLibrary().playlists) pls.push_back(p.name);
      for (auto& a : GetLibrary().albums) als.push_back(a.name);
    }
    float y = y0 + 60;
    for (size_t i = 0; i < pls.size() && y < y1 - 140; ++i, y += 44)
      if (SmallButton(fmt::format("addpl{}", i).c_str(), {x0 + 20, y}, 460, (std::string("Playlist: ") + pls[i]).c_str())) {
        SetLib([&](Library& l) { l.playlists[i].items.push_back(add_item_); });
        add_open_ = false;
      }
    for (size_t i = 0; i < als.size() && y < y1 - 140; ++i, y += 44)
      if (SmallButton(fmt::format("addal{}", i).c_str(), {x0 + 20, y}, 460, (std::string(L("Álbum: ", "Album: ")) + als[i]).c_str())) {
        SetLib([&](Library& l) {
          auto& k = l.albums[i].keys;
          if (std::find(k.begin(), k.end(), add_item_.key) == k.end()) k.push_back(add_item_.key);
        });
        add_open_ = false;
      }
    if (SmallButton("addnewpl", {x0 + 20, y1 - 120}, 220, L("Nova playlist", "New playlist"))) {
      SetLib([&](Library& l) { l.playlists.push_back({fmt::format("Playlist {}", l.playlists.size() + 1), {add_item_}, false, 1}); });
      add_open_ = false;
    }
    if (SmallButton("addnewal", {x0 + 260, y1 - 120}, 220, L("Novo álbum", "New album"))) {
      SetLib([&](Library& l) {
        l.albums.push_back({fmt::format("{} {}", L("Álbum", "Album"), l.albums.size() + 1), add_item_.key, {add_item_.key}});
      });
      add_open_ = false;
    }
    if (SmallButton("addcancel", {x0 + 20, y1 - 64}, 460, L("Cancelar", "Cancel"))) add_open_ = false;
  }

  // ---- Pieces ----
  static std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
  }
  ImVec2 P(float x, float y) const { return {ox_ + x * s_, oy_ + y * s_}; }
  // A press and its lift both inside a box, no drag between, no button taking it: a tap / click on it.
  bool Tapped(ImVec2 a, ImVec2 b) const {
    if (!live_ || dragged_ || !ImGui::IsMouseReleased(0) || ItemHeld()) return false;
    const ImVec2 c = ImGui::GetIO().MouseClickedPos[0], m = ImGui::GetIO().MousePos;
    return c.x >= a.x && c.x < b.x && c.y >= a.y && c.y < b.y && m.x >= a.x && m.x < b.x && m.y >= a.y && m.y < b.y;
  }
  ImU32 Col(int r, int g, int b, int a = 255) const { return IM_COL32(r, g, b, int(a * alpha_)); }
  ImU32 Ink() const { return Col(26, 30, 44); }
  ImU32 Faded() const { return Col(26, 30, 44, 150); }

  bool Image(ImTextureID id, ImVec2 a, ImVec2 b, ImVec2 uv0 = {0, 0}, ImVec2 uv1 = {1, 1}, float alpha = 1.0f,
             bool fade = true) {
    if (id) dl_->AddImage(id, a, b, uv0, uv1, IM_COL32(255, 255, 255, int(255 * alpha * (fade ? alpha_ : 1.0f))));
    return id != ImTextureID{};
  }
  void Text(ImFont* f, float size, ImVec2 pos, ImU32 color, const std::string& text, float wrap = 0.0f) {
    const ImVec2 p = P(pos.x, pos.y);
    // The game's slight emboss.
    dl_->AddText(f, size * s_, {p.x + 1.5f * s_, p.y + 1.5f * s_}, IM_COL32(255, 255, 255, int(90 * alpha_)), text.c_str(),
                 nullptr, wrap * s_);
    dl_->AddText(f, size * s_, p, color, text.c_str(), nullptr, wrap * s_);
  }
  template <typename Fn>
  void Clipped(ImVec2 a, ImVec2 b, Fn fn) {
    dl_->PushClipRect(P(a.x, a.y), P(b.x, b.y), true);
    fn();
    dl_->PopClipRect();
  }
  bool Button(const char* id, ImVec2 a, ImVec2 b) {
    if (!live_) return false;
    ImGui::SetCursorScreenPos(P(a.x, a.y));
    return ImGui::InvisibleButton(id, {(b.x - a.x) * s_, (b.y - a.y) * s_});
  }
  bool SmallButton(const char* id, ImVec2 at, float w, const char* label) {
    const ImVec2 a = P(at.x, at.y), b = P(at.x + w, at.y + 38);
    const bool hov = live_ && ImGui::IsMouseHoveringRect(a, b);
    dl_->AddRectFilled(a, b, hov ? Col(26, 30, 44, 230) : Col(250, 250, 248, 170), 4 * s_);
    dl_->AddRect(a, b, Ink(), 4 * s_, 0, 1.5f * s_);
    float fs = 22 * s_;
    ImVec2 ts = font_->CalcTextSizeA(fs, FLT_MAX, 0, label);
    if (ts.x > (w - 12) * s_) {  // a long label: smaller, never past the button
      fs *= (w - 12) * s_ / ts.x;
      ts = font_->CalcTextSizeA(fs, FLT_MAX, 0, label);
    }
    dl_->AddText(font_, fs, {(a.x + b.x - ts.x) / 2, (a.y + b.y - ts.y) / 2}, hov ? Col(236, 234, 228) : Ink(), label);
    return Button(id, at, {at.x + w, at.y + 38});
  }
  void StartRename(const std::string& name) {
    renaming_ = true;
    rename_focus_ = true;
    std::snprintf(rename_buf_, sizeof(rename_buf_), "%s", name.c_str());
  }
  // A name field at `at` (w wide) with OK and Cancel after it. Returns the new name once confirmed (OK or Enter),
  // empty while editing or when cancelled.
  std::string RenameField(float x, float y, float w) {
    std::string done;
    if (!live_) {
      Text(title_font_, 36, {x, y}, Ink(), rename_buf_);
      return done;
    }
    ImGui::SetCursorScreenPos(P(x, y + 2));
    ImGui::PushFont(font_, 30 * s_);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {8 * s_, 4 * s_});
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4 * s_);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f * s_);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Col(250, 250, 248, 230));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Col(255, 255, 252, 240));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Col(255, 255, 252, 250));
    ImGui::PushStyleColor(ImGuiCol_Text, Ink());
    ImGui::PushStyleColor(ImGuiCol_Border, Ink());
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, Col(120, 132, 150, 140));
    ImGui::SetNextItemWidth(w * s_);
    if (rename_focus_) {
      ImGui::SetKeyboardFocusHere();
      rename_focus_ = false;
    }
    const bool enter = ImGui::InputText("##rename", rename_buf_, sizeof(rename_buf_),
                                        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(3);
    ImGui::PopFont();
    const bool ok = SmallButton("renok", {x + w + 10, y + 4}, 90, "OK");
    const bool cancel = SmallButton("rencancel", {x + w + 110, y + 4}, 110, L("Cancelar", "Cancel"));
    if (enter || ok) {
      std::string s = rename_buf_;
      while (!s.empty() && s.back() == ' ') s.pop_back();
      while (!s.empty() && s.front() == ' ') s.erase(0, 1);
      done = s;
      renaming_ = false;
    }
    if (cancel) renaming_ = false;
    return done;
  }

  // Transport buttons: 0 previous, 1 play, 2 pause, 3 stop, 4 next.
  bool IconButton(const char* id, ImVec2 at, int kind) {
    const ImVec2 c = P(at.x + 30, at.y + 26);
    const bool hov = live_ && ImGui::IsMouseHoveringRect(P(at.x, at.y), P(at.x + 60, at.y + 52));
    dl_->AddCircleFilled(c, 28 * s_, hov ? Col(26, 30, 44, 235) : Col(250, 250, 248, 180), 32);
    dl_->AddCircle(c, 28 * s_, Ink(), 32, 2 * s_);
    const ImU32 k = hov ? Col(236, 234, 228) : Ink();
    const float u = s_;
    switch (kind) {
      case 0:
        dl_->AddTriangleFilled({c.x + 8 * u, c.y - 10 * u}, {c.x + 8 * u, c.y + 10 * u}, {c.x - 6 * u, c.y}, k);
        dl_->AddRectFilled({c.x - 10 * u, c.y - 10 * u}, {c.x - 6 * u, c.y + 10 * u}, k);
        break;
      case 1:
        dl_->AddTriangleFilled({c.x - 7 * u, c.y - 12 * u}, {c.x - 7 * u, c.y + 12 * u}, {c.x + 12 * u, c.y}, k);
        break;
      case 2:
        dl_->AddRectFilled({c.x - 9 * u, c.y - 11 * u}, {c.x - 3 * u, c.y + 11 * u}, k);
        dl_->AddRectFilled({c.x + 3 * u, c.y - 11 * u}, {c.x + 9 * u, c.y + 11 * u}, k);
        break;
      case 3:
        dl_->AddRectFilled({c.x - 9 * u, c.y - 9 * u}, {c.x + 9 * u, c.y + 9 * u}, k);
        break;
      case 4:
        dl_->AddTriangleFilled({c.x - 8 * u, c.y - 10 * u}, {c.x - 8 * u, c.y + 10 * u}, {c.x + 6 * u, c.y}, k);
        dl_->AddRectFilled({c.x + 6 * u, c.y - 10 * u}, {c.x + 10 * u, c.y + 10 * u}, k);
        break;
    }
    return Button(id, at, {at.x + 60, at.y + 52});
  }
  bool Slider(const char* id, ImVec2 at, float w, float& v) {
    const ImVec2 a = P(at.x, at.y - 3), b = P(at.x + w, at.y + 3);
    dl_->AddRectFilled(a, b, Col(90, 92, 100, 110), 3 * s_);
    dl_->AddRectFilled(a, {a.x + (b.x - a.x) * v, b.y}, Ink(), 3 * s_);
    dl_->AddCircleFilled({a.x + (b.x - a.x) * v, (a.y + b.y) / 2}, 10 * s_, Ink());
    if (!live_) return false;
    ImGui::SetCursorScreenPos(P(at.x - 10, at.y - 18));
    ImGui::InvisibleButton(id, {(w + 20) * s_, 36 * s_});
    if (ImGui::IsItemActive()) {
      const float nv = std::clamp((ImGui::GetIO().MousePos.x - a.x) / (b.x - a.x), 0.0f, 1.0f);
      if (std::fabs(nv - v) > 0.001f) {
        v = nv;
        return true;
      }
    }
    return false;
  }
  // A record: grooves, a label coloured by the song, the game's lace on it, a light that turns.
  void Disc(ImVec2 at, float r, float angle, const std::string& key, float alpha) {
    const ImVec2 c = P(at.x, at.y);
    const float R = r * s_;
    const int a = int(255 * alpha);
    dl_->AddCircleFilled(c, R, Col(18, 18, 20, a), 96);
    for (float g = 0.42f; g < 0.98f; g += 0.045f) dl_->AddCircle(c, R * g, Col(48, 48, 52, int(a * 0.8f)), 96, 1.0f * s_);
    // The sheen: two arcs opposite, turning.
    for (int k = 0; k < 2; ++k) {
      const float base = angle + k * 3.14159f;
      dl_->PathArcTo(c, R * 0.82f, base, base + 0.7f, 24);
      dl_->PathStroke(Col(200, 200, 210, int(a * 0.22f)), 0, R * 0.22f);
    }
    uint32_t h = 2166136261u;
    for (char ch : key) h = (h ^ uint8_t(ch)) * 16777619u;
    // NieR's colours: sepia, ash, faded blue, rust.
    static const ImU32 kLabels[] = {IM_COL32(176, 156, 120, 255), IM_COL32(140, 146, 150, 255), IM_COL32(120, 132, 150, 255),
                                    IM_COL32(160, 110, 90, 255), IM_COL32(190, 184, 170, 255), IM_COL32(110, 120, 100, 255)};
    ImU32 label = kLabels[h % 6];
    label = (label & 0x00FFFFFF) | (ImU32(int(a * alpha_)) << 24);
    dl_->AddCircleFilled(c, R * 0.36f, key.empty() ? Col(120, 120, 120, a) : label, 64);
    if (ImTextureID moyo = Art("back_moyo")) {
      const float rr = R * 0.33f;
      // The lace turns with the record (its quad rotated).
      const float ca = std::cos(angle), sa = std::sin(angle);
      auto rot = [&](float x, float y) { return ImVec2(c.x + (x * ca - y * sa) * rr, c.y + (x * sa + y * ca) * rr); };
      dl_->AddImageQuad(moyo, rot(-1, -1), rot(1, -1), rot(1, 1), rot(-1, 1), {0, 0}, {1, 0}, {1, 1}, {0, 1},
                        IM_COL32(255, 255, 255, int(a * 0.5f * alpha_)));
    }
    dl_->AddCircle(c, R * 0.36f, Col(30, 30, 30, a), 64, 1.5f * s_);
    dl_->AddCircleFilled(c, R * 0.035f, Col(220, 218, 210, a), 16);
  }

  // State.
  int tab_ = kSongs;
  int selected_ = 0, scroll_ = 0, pad_move_ = 0;
  bool pad_activate_ = false;
  int open_album_ = -1, open_playlist_ = -1;
  int edit_item_ = -1, edit_scroll_ = 0;  // the timeline being edited (an item of the open playlist)
  bool timeline_dirty_ = false;
  double clear_armed_ = -10, wheel_time_ = -10;
  float pause_fade_ = 0, pause_seen_ = 0;  // the pause menu's Jukebox plate
  bool pause_hot_ = false, played_ = false;
  std::string album_folder_;
  bool add_open_ = false;
  Item add_item_;
  bool renaming_ = false, rename_focus_ = false;  // an open album's / playlist's name being typed
  char rename_buf_[128] = {};
  float appear_ = 0, spin_ = 0, arm_ = 0, tab_x_ = 230, tab_w_ = 0;
  float drag_acc_ = 0;
  bool dragged_ = false, was_open_ = false, live_ = false, volume_dirty_ = false;
  std::map<std::string, float> glow_;
  NowPlaying now_;
  GameMusic game_;
  float s_ = 1, ox_ = 0, oy_ = 0, alpha_ = 1;
  ImFont *font_ = nullptr, *title_font_ = nullptr;
  ImDrawList* dl_ = nullptr;
  std::map<std::string, std::unique_ptr<rex::ui::ImmediateTexture>> textures_;
  rex::ui::ImmediateDrawer* texture_owner_ = nullptr;
};

}  // namespace nier::jukebox
