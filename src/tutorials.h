// nier - the port's own tutorials (2026-11-17): new things the game never taught, shown once in gameplay as a window
// in the game's manner, the game paused for input until it is closed (A, B, a click, a tap). Each one: an id kept in
// nier_tutorials_seen (nier.toml) once read, its texts (pc_text: every language), and when it is due.
//
// The first: the quick weapon switch (weapon_switch.h), due once Nier has a weapon of every kind.
#pragma once

#include <algorithm>
#include <cfloat>
#include <functional>
#include <string>
#include <vector>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include "menu_mouse.h"
#include "pc_options.h"
#include "pc_text.h"
#include "weapon_switch.h"

namespace nier::tutorials {

struct Tutorial {
  const char* id;
  const char* title;  // English (pc_text's key)
  std::function<std::string()> body;
  std::function<bool(const uint8_t* gb)> due;
};

inline const std::vector<Tutorial>& All() {
  static const std::vector<Tutorial> list = {
      {"weapon_switch", "Quick Weapon Switch",
       [] {
         return nier::pc_text::T(weapon_switch::HoldMode()
                                     ? "You now have every kind of weapon. Hold a weapon button (D-pad left, right or down; "
                                       "keys 1, 2 and 3; or the on-screen weapon button) to equip at once the weapon chosen "
                                       "for that kind. A tap opens the weapon menu to choose another. To swap tap and hold: "
                                       "Options > Control Options > Weapon Switch."
                                     : "You now have every kind of weapon. Tap a weapon button (D-pad left, right or down; "
                                       "keys 1, 2 and 3; or the on-screen weapon button) to equip at once the weapon chosen "
                                       "for that kind. Hold it to open the weapon menu and choose another. To swap tap and "
                                       "hold: Options > Control Options > Weapon Switch.");
       },
       [](const uint8_t* gb) { return weapon_switch::Unlocked(gb); }},
  };
  return list;
}

inline bool Seen(const std::string& id) {
  const std::string seen = "," + rex::cvar::GetFlagByName("nier_tutorials_seen") + ",";
  return seen.find("," + id + ",") != std::string::npos;
}
inline void MarkSeen(const std::string& id) {
  std::string seen = rex::cvar::GetFlagByName("nier_tutorials_seen");
  if (Seen(id)) return;
  seen += (seen.empty() ? "" : ",") + id;
  rex::cvar::SetFlagByName("nier_tutorials_seen", seen);
  auto& st = pc_options_state();
  if (!st.config_path.empty()) SavePcSettings(st.config_path);
}

class TutorialDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit TutorialDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    auto& st = pc_options_state();
    const float dt = std::clamp(io.DeltaTime, 0.0f, 0.1f);
    const uint8_t* gb = st.guest_base.load();
    if (current_ < 0) {
      // Due in plain gameplay for a moment (no menu, no talk, nothing of ours open).
      // Nier's object there (its vtable at +3472) and its controls free (0x827CD450: 0 in talks and cutscenes); the
      // game's gameplay marks alone stay off in some areas (touch_controls.h).
      auto byte = [gb](uint32_t a) { return *reinterpret_cast<const volatile uint8_t*>(gb + a); };
      const bool calm = gb && byte(0x82757AB1) == 0 && byte(0x827CD450) != 0 &&
                        weapon_switch::Be32(gb, 0x848BE4B0u + 3472) == 0x82740048u && !nier::menu_mouse::MenuSeenRecently() &&
                        !st.open && !st.jukebox_open && !st.mod_panel_open && !st.BeforeTitleMenu();
      calm_ = calm ? calm_ + dt : 0.0f;
      if (calm_ < 2.0f) return;
      const auto& all = All();
      for (size_t i = 0; i < all.size(); ++i)
        if (!Seen(all[i].id) && all[i].due(gb)) {
          current_ = int(i);
          appear_ = 0;
          st.tutorial_presses = 0;
          st.tutorial_open = true;
          break;
        }
      if (current_ < 0) return;
    }
    const Tutorial& t = All()[size_t(current_)];
    appear_ = std::min(1.0f, appear_ + dt * 4.0f);
    // Closed by A / B (the gates: tutorial_presses), a click or a tap, after half a second (no stray press).
    const uint16_t presses = st.tutorial_presses.exchange(0);
    if (appear_ >= 1.0f && shown_ > 0.5f && (presses & 0x3000 || ImGui::IsMouseClicked(0))) {
      MarkSeen(t.id);
      st.tutorial_open = false;
      current_ = -1;
      shown_ = 0;
      return;
    }
    shown_ += dt;
    Draw(io, t);
  }

 private:
  void Draw(ImGuiIO& io, const Tutorial& t) {
    const ImVec2 screen = io.DisplaySize;
    const float s = std::min(screen.x / 1280.0f, screen.y / 720.0f);
    const float a = appear_ * appear_ * (3 - 2 * appear_);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = pc_options_font() ? pc_options_font() : ImGui::GetFont();
    ImFont* title_font = pc_options_title_font() ? pc_options_title_font() : font;
    dl->AddRectFilled({0, 0}, screen, IM_COL32(0, 0, 0, int(110 * a)));
    const float w = 760 * s, h = 300 * s;
    const ImVec2 p0((screen.x - w) / 2, (screen.y - h) / 2 + (1 - a) * 20 * s), p1(p0.x + w, p0.y + h);
    // The game's plates: dark, soft-edged, a light rule under the title.
    dl->AddRectFilled(p0, p1, IM_COL32(16, 18, 16, int(225 * a)), 6 * s);
    dl->AddRect(p0, p1, IM_COL32(220, 214, 196, int(120 * a)), 6 * s, 0, 1.5f * s);
    const ImU32 ink = IM_COL32(240, 236, 226, int(255 * a)), dim = IM_COL32(200, 196, 186, int(220 * a));
    const std::string title = nier::pc_text::T(t.title);
    dl->AddText(title_font, 34 * s, {p0.x + 32 * s, p0.y + 22 * s}, ink, title.c_str());
    dl->AddLine({p0.x + 28 * s, p0.y + 70 * s}, {p1.x - 28 * s, p0.y + 70 * s}, IM_COL32(220, 214, 196, int(150 * a)), 1.5f * s);
    const std::string body = t.body();
    dl->AddText(font, 22 * s, {p0.x + 32 * s, p0.y + 88 * s}, ink, body.c_str(), nullptr, w - 64 * s);
    const std::string more = nier::pc_text::T("Press A or click to continue.");
    const ImVec2 ms = font->CalcTextSizeA(20 * s, FLT_MAX, 0, more.c_str());
    dl->AddText(font, 20 * s, {p1.x - 32 * s - ms.x, p1.y - 40 * s}, dim, more.c_str());
  }

  int current_ = -1;
  float calm_ = 0, appear_ = 0, shown_ = 0;
};

}  // namespace nier::tutorials
