// nier - the Achievements page of Grimoire Weiss's book, and the notice when one is unlocked.
//
// Notes' sixth row, "Achievements", opens it (memo_achievements.cpp): it is drawn over the game's
// book with the book's own art (menu pages, lace, ink) and each achievement's icon from the game's
// XDBF. Up/down (or the mouse wheel / pointer) choose one; B or a right click go back to Notes. Nothing of the game is shipped: art, icons and texts are
// read from the player's copy.
#pragma once

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>

#include "achievements.h"
#include "pc_text.h"
#include "game_text.h"
#include "pc_options.h"

namespace nier {

class AchievementsDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit AchievementsDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    auto& state = pc_options_state();
    EnsureTextures();
    {  // TEMP (dev): NIER_ACH_TEST=<seconds>|<id> unlocks one by itself, to see the notice.
      static const char* test = std::getenv("NIER_ACH_TEST");
      static bool done = false;
      if (test && !done && ImGui::GetTime() > std::atof(test)) {
        done = true;
        if (const char* bar = std::strchr(test, '|')) achievements::Unlock(uint32_t(std::atoi(bar + 1)));
      }
    }
    achievements::Info fresh;
    if (achievements::TakeNotice(fresh)) {
      notice_ = fresh;
      notice_until_ = ImGui::GetTime() + 6.0;
    }
    if (state.ach_open) {
      if (!was_open_) {  // just opened: the list as it is now
        list_ = achievements::List();
        selected_ = scroll_ = 0;
        was_open_ = true;
      }
      for (uint16_t b : state.TakeAch()) {
        if (b & kPadUp) Move(-1);
        if (b & kPadDown) Move(+1);
        if (b & (kPadB | kPadBack | kPadStart)) Close();
      }
      if (state.ach_open) DrawPage(io);
    } else {
      was_open_ = false;
    }
    if (ImGui::GetTime() < notice_until_) DrawNotice(io);
  }

 private:
  static constexpr int kVisible = 6;

  static bool Portuguese() { return text::CurrentName() == "Portugues"; }
  static const char* L(const char* pt, const char* en) { return Portuguese() ? pt : en; }

  void Close() {
    pc_options_state().ach_open = false;
    pc_options_state().TakeAch();
  }
  void Move(int d) {
    if (list_.empty()) return;
    selected_ = std::clamp(selected_ + d, 0, int(list_.size()) - 1);
    if (selected_ < scroll_) scroll_ = selected_;
    if (selected_ >= scroll_ + kVisible) scroll_ = selected_ - kVisible + 1;
  }

  // The book's art (decoded at start, menu_art) and the achievements' icons, made textures once.
  void EnsureTextures() {
    rex::ui::ImmediateDrawer* drawer = imgui_drawer()->immediate_drawer();
    if (drawer != texture_owner_) {
      for (auto& [name, t] : textures_) t.release();
      for (auto& [id, t] : icons_) t.release();
      textures_.clear();
      icons_.clear();
      texture_owner_ = drawer;
    }
    auto& art = menu_art();
    if (drawer && textures_.empty() && art.ready) {
      std::lock_guard lock(art.mutex);
      for (const auto& [name, image] : art.images) {
        textures_[name] = drawer->CreateTexture(uint32_t(image.width), uint32_t(image.height),
                                                rex::ui::ImmediateTextureFilter::kLinear, false,
                                                image.rgba.data());
      }
    }
  }
  ImTextureID Art(const char* name) const {
    auto it = textures_.find(name);
    return it == textures_.end() || !it->second ? ImTextureID{} : reinterpret_cast<ImTextureID>(it->second.get());
  }
  ImTextureID IconTexture(uint32_t image_id) {
    rex::ui::ImmediateDrawer* drawer = imgui_drawer()->immediate_drawer();
    if (!drawer) return ImTextureID{};
    auto it = icons_.find(image_id);
    if (it == icons_.end()) {
      int w = 0, h = 0;
      std::vector<uint8_t> rgba;
      std::unique_ptr<rex::ui::ImmediateTexture> t;
      if (achievements::Icon(image_id, w, h, rgba)) {
        t = drawer->CreateTexture(uint32_t(w), uint32_t(h), rex::ui::ImmediateTextureFilter::kLinear, false,
                                  rgba.data());
      }
      it = icons_.emplace(image_id, std::move(t)).first;
    }
    return it->second ? reinterpret_cast<ImTextureID>(it->second.get()) : ImTextureID{};
  }

  void DrawPage(ImGuiIO& io) {
    const ImVec2 screen = io.DisplaySize;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // In 1920x1080 units fitted to the window, as the game's book.
    const float scale = std::min(screen.x / 1920.0f, screen.y / 1080.0f);
    const float ox = (screen.x - 1920.0f * scale) * 0.5f, oy = (screen.y - 1080.0f * scale) * 0.5f;
    auto P = [&](float x, float y) { return ImVec2(ox + x * scale, oy + y * scale); };
    auto image = [&](ImTextureID id, ImVec2 a, ImVec2 b, ImVec2 uv0 = {0, 0}, ImVec2 uv1 = {1, 1},
                     ImU32 tint = IM_COL32_WHITE) {
      if (id) dl->AddImage(id, a, b, uv0, uv1, tint);
      return id != ImTextureID{};
    };
    ImFont* font = pc_options_font() ? pc_options_font() : ImGui::GetFont();
    ImFont* title_font = pc_options_title_font() ? pc_options_title_font() : font;
    const ImU32 ink = IM_COL32(26, 30, 44, 255), faded = IM_COL32(26, 30, 44, 150);
    const ImU32 paper = IM_COL32(214, 212, 204, 255), light = IM_COL32(236, 234, 228, 255);
    auto ink_text = [&](ImFont* f, ImVec2 pos, float size, ImU32 color, const std::string& text,
                        float wrap = 0.0f) {
      dl->AddText(f, size, {pos.x + 1.5f * scale, pos.y + 1.5f * scale}, IM_COL32(255, 255, 255, 110),
                  text.c_str(), nullptr, wrap);  // the game's slight emboss
      dl->AddText(f, size, pos, color, text.c_str(), nullptr, wrap);
    };

    // The book, as Grimoire Weiss's other pages: backdrop, page with spine, page edges, lace.
    dl->AddRectFilled({0, 0}, screen, IM_COL32(60, 60, 60, 255));
    image(Art("back_back_D1"), {0, 0}, screen);
    if (!image(Art("menu_base_$book_a"), P(0, 0), P(1730, 1080))) dl->AddRectFilled(P(0, 0), P(1730, 1080), paper);
    image(Art("menu_base_$book_b"), P(1722, 0), P(1790, 1080));
    image(Art("menu_base_$book_d"), P(0, 1000), P(1760, 1064));
    image(Art("back_moyo"), P(1330, 300), P(1720, 690), {0, 0}, {1, 1}, IM_COL32(255, 255, 255, 60));

    // Key hints on the dark strip, then the book's pages with ours last and marked.
    const bool pad = rex::cvar::GetFlagByName("mnk_mode") != "true";
    image(Art("explanation_explanation_plate_D5"), P(0, 95), P(900, 141));
    {
      float x = 150;
      const float hy = 100, hs = 30 * scale;
      struct Hint { int icon_x, icon_y; const char* key; const char* text; };
      const Hint hints[] = {{1, 5, "W/S", L("Selecionar", "Select")},
                            {1, 0, "Esc", L("Voltar", "Back")}};
      for (const Hint& h : hints) {
        if (pad && Art("bt_icon_360_bt_icon_D5")) {
          image(Art("bt_icon_360_bt_icon_D5"), P(x, hy), P(x + 36, hy + 36), {h.icon_x / 4.0f, h.icon_y / 6.0f},
                {(h.icon_x + 1) / 4.0f, (h.icon_y + 1) / 6.0f});
          x += 42;
        } else {
          dl->AddText(font, hs, P(x, hy), light, h.key);
          x += font->CalcTextSizeA(hs, FLT_MAX, 0, h.key).x / scale + 10;
        }
        dl->AddText(font, hs, P(x, hy), light, h.text);
        x += font->CalcTextSizeA(hs, FLT_MAX, 0, h.text).x / scale + 30;
      }
    }

    // Heading and the score.
    int got = 0, points = 0, total = 0;
    for (const auto& a : list_) {
      total += int(a.points);
      if (a.unlocked) {
        ++got;
        points += int(a.points);
      }
    }
    image(Art("menu_com_title_icon"), P(200, 222), P(246, 276));
    ink_text(title_font, P(258, 222), 44 * scale, ink, L("Conquistas", "Achievements"));
    {
      const std::string score = fmt::format("{} {} / {}     {} {} / {}", L("Desbloqueadas", "Unlocked"), got,
                                            list_.size(), L("Pontos", "Points"), points, total);
      ink_text(font, P(760, 232), 32 * scale, ink, score);
      // Progress bar in the book's ink.
      const float f = list_.empty() ? 0.0f : float(got) / float(list_.size());
      dl->AddRectFilled(P(760, 274), P(1350, 282), IM_COL32(90, 92, 100, 120), 3 * scale);
      dl->AddRectFilled(P(760, 274), P(760 + 590 * f, 282), ink, 3 * scale);
    }

    if (list_.empty()) {
      ink_text(font, P(381, 400), 36 * scale, faded,
               L("As conquistas aparecem quando o jogo terminar de carregar.", "Achievements show once the game has loaded."));
      return;
    }

    // The rows: icon, name, how it is earned (or the secret's hint), points and date.
    const float left = 300, right = 1400, row_h = 104, y0 = 300;
    // The mouse only while open (the page also draws while turning away).
    const bool live = pc_options_state().ach_open;
    const bool mouse_moved = live && (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f);
    if (live && io.MouseWheel != 0.0f) {
      scroll_ = std::clamp(scroll_ - int(io.MouseWheel), 0, std::max(0, int(list_.size()) - kVisible));
      selected_ = std::clamp(selected_, scroll_, scroll_ + kVisible - 1);
    }
    // The right button is B, taken by the input gate (input_recorder.h): closing here, before the
    // game's poll, let that B reach the book and close it all (2026-11-13).
    for (int v = 0; v < kVisible && scroll_ + v < int(list_.size()); ++v) {
      const int i = scroll_ + v;
      const auto& a = list_[i];
      const float y = y0 + v * row_h;
      if (mouse_moved && ImGui::IsMouseHoveringRect(P(left, y), P(right, y + row_h - 6), false)) selected_ = i;
      const bool sel = i == selected_;
      if (sel && !image(Art("decision_line_line"), P(left + 96, y + 4), P(left + 760, y + 54))) {
        dl->AddRectFilled(P(left + 96, y + 4), P(left + 760, y + 54), IM_COL32(250, 250, 250, 200));
      }
      dl->AddLine(P(left, y + row_h - 8), P(right, y + row_h - 8), ink, 2.0f * scale);
      // The icon in a thin frame; locked ones faded to grey.
      const ImVec2 i0 = P(left + 4, y + 6), i1 = P(left + 86, y + 88);
      dl->AddRectFilled(i0, i1, IM_COL32(40, 40, 44, 255));
      if (!image(IconTexture(a.image_id), i0, i1, {0, 0}, {1, 1},
                 a.unlocked ? IM_COL32_WHITE : IM_COL32(120, 120, 120, 150))) {
        dl->AddText(font, 40 * scale, P(left + 32, y + 22), light, "?");
      }
      dl->AddRect(i0, i1, ink, 0, 0, 2.0f * scale);
      ink_text(font, P(left + 112, y + 8), 36 * scale, a.unlocked ? ink : faded, pc_text::Achievement(a.id, 0, a.name));
      ink_text(font, P(left + 112, y + 54), 24 * scale, faded, a.unlocked ? pc_text::Achievement(a.id, 1, a.description) : pc_text::Achievement(a.id, 2, a.locked));
      const std::string pts = fmt::format("{} {}", a.points, L("pts", "pts"));
      ink_text(font, P(right - 250, y + 12), 30 * scale, a.unlocked ? ink : faded, pts);
      ink_text(font, P(right - 250, y + 54), 24 * scale, faded,
               a.unlocked ? a.when : std::string(L("Bloqueada", "Locked")));
    }
    // More above / below: the game's scroll arrows.
    if (scroll_ > 0) image(Art("arrow_varrow"), P(820, 286), P(860, 304), {0, 1}, {1, 0});
    if (scroll_ + kVisible < int(list_.size())) image(Art("arrow_varrow"), P(820, 924), P(860, 942));

    // The help plate at the bottom: the selected one in full.
    {
      const auto& a = list_[selected_];
      image(Art("explanation_explanation_plate_D5"), P(560, 950), P(1500, 996), {0, 0}, {1, 1}, IM_COL32(255, 255, 255, 220));
      // In the player's language (pc_text), as the rows above.
      const std::string text = a.unlocked ? fmt::format("{}  ({} {})", pc_text::Achievement(a.id, 1, a.description),
                                                        pc_text::T("unlocked on"), a.when)
                                          : pc_text::Achievement(a.id, 2, a.locked);
      dl->AddText(font, 26 * scale, P(590, 958), light, text.c_str());
    }
  }

  // "Achievement unlocked" at the top right for a few seconds, with its icon.
  void DrawNotice(ImGuiIO& io) {
    const ImVec2 screen = io.DisplaySize;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float scale = std::min(screen.x / 1920.0f, screen.y / 1080.0f);
    ImFont* font = pc_options_font() ? pc_options_font() : ImGui::GetFont();
    const float left_time = float(notice_until_ - ImGui::GetTime());
    const float alpha = std::clamp(std::min(left_time, 6.0f - left_time) * 2.0f, 0.0f, 1.0f);
    const ImVec2 a(screen.x - 640 * scale, 60 * scale), b(screen.x - 40 * scale, 170 * scale);
    dl->AddRectFilled(a, b, IM_COL32(20, 20, 24, int(220 * alpha)), 6 * scale);
    dl->AddRect(a, b, IM_COL32(200, 196, 186, int(255 * alpha)), 6 * scale, 0, 2 * scale);
    if (ImTextureID icon = IconTexture(notice_.image_id)) {
      dl->AddImage(icon, {a.x + 14 * scale, a.y + 14 * scale}, {a.x + 96 * scale, a.y + 96 * scale}, {0, 0}, {1, 1},
                   IM_COL32(255, 255, 255, int(255 * alpha)));
    }
    dl->AddText(font, 26 * scale, {a.x + 112 * scale, a.y + 18 * scale}, IM_COL32(200, 196, 186, int(255 * alpha)),
                L("Conquista desbloqueada", "Achievement unlocked"));
    dl->AddText(font, 34 * scale, {a.x + 112 * scale, a.y + 52 * scale}, IM_COL32(240, 238, 232, int(255 * alpha)),
                fmt::format("{}  ({} pts)", pc_text::Achievement(notice_.id, 0, notice_.name), notice_.points).c_str());
  }

  std::vector<achievements::Info> list_;
  int selected_ = 0, scroll_ = 0;
  bool was_open_ = false;
  achievements::Info notice_;
  double notice_until_ = 0.0;
  std::map<std::string, std::unique_ptr<rex::ui::ImmediateTexture>> textures_;
  std::map<uint32_t, std::unique_ptr<rex::ui::ImmediateTexture>> icons_;
  rex::ui::ImmediateDrawer* texture_owner_ = nullptr;
};

}  // namespace nier
