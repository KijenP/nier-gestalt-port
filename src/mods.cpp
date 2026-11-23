// nier - Nier Plus code mods: the loader, the API table (mods/sdk/nier_mod.h), the game events and the panels.
//
// The events are the game's own setters of the party's stats (found 2026-11-15, NOTAS.md "Nier Plus"):
//   sub_8237D938(character, new HP)      sets HP, clamped by the game to 0..max (the max from the vtable +60)
//   sub_8237DC10(character, f1 = delta) adds MP, clamped to 0..max (character +20)
//   sub_8237F220(character, amount, r5)  adds experience (r5: check for a level up); nothing at level 99
//   sub_82345CB0(save block, delta)      adds gold, clamped to 0..9,999,999
// The character's party slot is at +12 of its object. With no code mod loaded every hook is a straight call.

#include "mods.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include <fmt/format.h>
#include <sstream>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#if defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#endif

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>

#include "../mods/sdk/nier_mod.h"
#include "game_text.h"
#include "pc_options.h"
#include "pc_text.h"

REXCVAR_DECLARE(std::string, nier_language);

extern "C" void __imp__sub_8237D938(PPCContext& ctx, uint8_t* base);  // set HP
extern "C" void __imp__sub_8237DC10(PPCContext& ctx, uint8_t* base);  // add MP
extern "C" void __imp__sub_8237F220(PPCContext& ctx, uint8_t* base);  // add experience
extern "C" void __imp__sub_82345CB0(PPCContext& ctx, uint8_t* base);  // add gold
// sub_822105B8(status r3, damage r4, flags r5, no damage r6, result r7): a hit on a character's status object (the
// party's and, as its two callers suggest, the enemies'). Status +4 bit 0x10 set: the game's own invulnerability (it
// writes 8194 to *r7 and returns 0, nothing else). Else the damage less the defence (vtable +28/+32), at least 1,
// times the float at +8, is taken off with the HP setter (vtable +72). The party's status vtable: 0x8203C830.
extern "C" void __imp__sub_822105B8(PPCContext& ctx, uint8_t* base);
constexpr uint32_t kPartyStatusVtable = 0x8203C830;
// Its two callers, where a character receives a hit (r3 = the character, its status at +3472, r4 = the attack):
// sub_821B95B8 (r5 = result flags; an attack with no damage sets 514 there and returns 0: no hit) and sub_821C92B8
// (void; leaves at once while the character is in state 15). The flinch and knock-back happen inside them.
extern "C" void __imp__sub_821B95B8(PPCContext& ctx, uint8_t* base);
// sub_821A7D00(camera manager r3, f1 = the frame's time): moves the game's camera each frame (found 2026-11-15,
// NIER_WATCH_WRITE). The camera at 0x826B40A0 holds its world matrix twice, at +0xE0 and +0x120 (16 big-endian
// floats each, rows right / up / forward / position); the view is made from them later in the frame.
extern "C" void __imp__sub_821A7D00(PPCContext& ctx, uint8_t* base);
constexpr uint32_t kCameraWorld = 0x826B4180, kCameraWorldCopy = 0x826B41C0;
extern "C" void __imp__sub_821C92B8(PPCContext& ctx, uint8_t* base);

namespace fs = std::filesystem;

namespace nier::mods {
namespace {

struct Control {
  enum Kind { kToggle, kInt, kButton, kText, kChoice } kind;
  std::string id, label;
  std::vector<std::string> names;  // kChoice
  int32_t* value = nullptr;
  int32_t min = 0, max = 1;
  void (*on_click)(void*) = nullptr;
  void* user = nullptr;
};

struct Mod;

}  // namespace
}  // namespace nier::mods

struct NierModPanel {
  nier::mods::Mod* mod = nullptr;
  int ordinal = 0;   // which of its mod's panels (the saved hotkey's key: _hotkey<ordinal>)
  std::string bind;  // its hotkey's keybind name
  std::string title, hotkey, icon_png;
  int32_t menus = 0;
  std::vector<nier::mods::Control> controls;
  bool open = false;
};

namespace nier::mods {
namespace {

struct Mod {
  std::string name;
  fs::path dir;
  std::string dir_utf8;
  std::map<std::string, int32_t> saved;     // settings.txt: the controls' values
  std::map<std::string, std::string> text;  // settings.txt: the game's own entries (_hotkey<n>)
  bool enabled = true;                      // _enabled: off, its events, panels and icons do nothing
  int panels = 0;
  NierModApi api{};
};

template <typename Fn>
struct Handler {
  Fn fn;
  void* user;
  Mod* mod;
};

struct State {
  std::vector<std::unique_ptr<Mod>> mods;
  std::vector<std::unique_ptr<NierModPanel>> panels;
  std::vector<Handler<void (*)(void*, uint32_t, int32_t, int32_t*)>> set_hp;
  std::vector<Handler<void (*)(void*, uint32_t, float*)>> add_mp;
  std::vector<Handler<void (*)(void*, uint32_t, int32_t*)>> add_exp;
  std::vector<Handler<void (*)(void*, int32_t*)>> add_gold;
  std::vector<Handler<void (*)(void*, uint32_t, int32_t, int32_t*, int32_t*)>> damage;
  std::vector<Handler<void (*)(void*, uint32_t, int32_t, int32_t*)>> hit;
  std::vector<Handler<void (*)(void*, float*)>> camera;
  std::vector<Handler<void (*)(void*, NierModPad*)>> pad;
  std::vector<Handler<void (*)(void*, int32_t*)>> warp;
  std::string language = "en";
  Mod* initializing = nullptr;  // the mod whose nier_mod_init runs (the API's calls belong to it)
  bool loaded = false;
  rex::ui::ImGuiDrawer* drawer = nullptr;
  std::function<void(std::function<void()>)> run_on_ui;
  bool hotkeys_done = false;
  std::mutex mutex;
};
State& S() {
  static State s;
  return s;
}

uint32_t LoadBe32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = base + address;
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

void SaveSettings(Mod& mod) {
  std::ofstream out(mod.dir / "settings.txt", std::ios::trunc);
  if (!out) return;
  out << "# Nier Plus: this mod's settings (written by the game)\n";
  out << "_enabled=" << (mod.enabled ? 1 : 0) << "\n";
  for (const auto& [id, value] : mod.text) out << id << "=" << value << "\n";
  for (const auto& [id, value] : mod.saved) out << id << "=" << value << "\n";
}

void LoadSettings(Mod& mod) {
  std::ifstream in(mod.dir / "settings.txt");
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string id = line.substr(0, eq), value = line.substr(eq + 1);
    if (id == "_enabled") mod.enabled = std::atoi(value.c_str()) != 0;
    else if (id.rfind("_hotkey", 0) == 0) mod.text[id] = value;
    else mod.saved[id] = std::atoi(value.c_str());
  }
}

// ---- The API table's functions ----

void ApiLog(const char* text) {
  Mod* mod = S().initializing;
  REXLOG_INFO("[mod {}] {}", mod ? mod->name : std::string("?"), text ? text : "");
}

NierModPanel* ApiCreatePanel(const char* title, const char* hotkey, const char* icon_png) {
  State& s = S();
  if (!s.initializing) return nullptr;
  auto panel = std::make_unique<NierModPanel>();
  panel->mod = s.initializing;
  panel->ordinal = s.initializing->panels++;
  panel->title = title ? title : s.initializing->name;
  panel->hotkey = hotkey ? hotkey : "";
  // The player's own key for it (the mod's page in the menu), over the mod's.
  if (auto it = s.initializing->text.find("_hotkey" + std::to_string(panel->ordinal));
      it != s.initializing->text.end())
    panel->hotkey = it->second;
  if (icon_png && *icon_png) panel->icon_png = (s.initializing->dir / icon_png).string();
  s.panels.push_back(std::move(panel));
  return s.panels.back().get();
}

void AddValue(NierModPanel* panel, Control::Kind kind, const char* id, const char* label, int32_t* value,
              int32_t min, int32_t max) {
  if (!panel || !value || !id) return;
  Control c{kind};
  c.id = id;
  c.label = label ? label : id;
  c.value = value;
  c.min = min;
  c.max = max;
  if (auto it = panel->mod->saved.find(c.id); it != panel->mod->saved.end())
    *value = std::clamp(it->second, min, max);
  panel->controls.push_back(std::move(c));
}

void ApiAddToggle(NierModPanel* panel, const char* id, const char* label, int32_t* value) {
  AddValue(panel, Control::kToggle, id, label, value, 0, 1);
}

void ApiAddInt(NierModPanel* panel, const char* id, const char* label, int32_t* value, int32_t min, int32_t max) {
  AddValue(panel, Control::kInt, id, label, value, min, std::max(min, max));
}

void ApiAddChoice(NierModPanel* panel, const char* id, const char* label, int32_t* value, const char* const* names,
                  int32_t count) {
  if (!panel || !names || count <= 0) return;
  AddValue(panel, Control::kChoice, id, label, value, 0, count - 1);
  if (panel->controls.empty() || panel->controls.back().value != value) return;
  for (int32_t i = 0; i < count; ++i) panel->controls.back().names.emplace_back(names[i] ? names[i] : "?");
}

// The moveset mod's options only (a mod setting any of the game's would be too much).
int32_t ApiSetOption(const char* name, const char* value) {
  if (!name || !value || std::string_view(name).rfind("nier_moveset_", 0) != 0) return 0;
  rex::cvar::SetFlagByName(name, value);
  return 1;
}

void ApiAddButton(NierModPanel* panel, const char* label, void (*on_click)(void*), void* user) {
  if (!panel || !on_click) return;
  Control c{Control::kButton};
  c.label = label ? label : "?";
  c.on_click = on_click;
  c.user = user;
  panel->controls.push_back(std::move(c));
}

void ApiAddText(NierModPanel* panel, const char* text) {
  if (!panel) return;
  Control c{Control::kText};
  c.label = text ? text : "";
  panel->controls.push_back(std::move(c));
}

void ApiShowInMenus(NierModPanel* panel, int32_t where) {
  if (panel) panel->menus = where;
}

void ApiOnSetHp(void (*fn)(void*, uint32_t, int32_t, int32_t*), void* user) {
  if (fn && S().initializing) S().set_hp.push_back({fn, user, S().initializing});
}
void ApiOnAddMp(void (*fn)(void*, uint32_t, float*), void* user) {
  if (fn && S().initializing) S().add_mp.push_back({fn, user, S().initializing});
}
void ApiOnAddExp(void (*fn)(void*, uint32_t, int32_t*), void* user) {
  if (fn && S().initializing) S().add_exp.push_back({fn, user, S().initializing});
}
void ApiOnDamage(void (*fn)(void*, uint32_t, int32_t, int32_t*, int32_t*), void* user) {
  if (fn && S().initializing) S().damage.push_back({fn, user, S().initializing});
}
void ApiOnHit(void (*fn)(void*, uint32_t, int32_t, int32_t*), void* user) {
  if (fn && S().initializing) S().hit.push_back({fn, user, S().initializing});
}
void ApiOnCamera(void (*fn)(void*, float*), void* user) {
  if (fn && S().initializing) S().camera.push_back({fn, user, S().initializing});
}
void ApiGetPad(NierModPad* out) {
  if (!out) return;
  const auto& st = pc_options_state();
  const uint32_t b = st.pad_buttons.load();
  const uint64_t s = st.pad_sticks.load();
  out->buttons = uint16_t(b);
  out->left_trigger = uint8_t(b >> 16);
  out->right_trigger = uint8_t(b >> 24);
  out->lx = int16_t(uint16_t(s));
  out->ly = int16_t(uint16_t(s >> 16));
  out->rx = int16_t(uint16_t(s >> 32));
  out->ry = int16_t(uint16_t(s >> 48));
}
void ApiOnPlayerWarp(void (*fn)(void*, int32_t*), void* user) {
  if (fn && S().initializing) S().warp.push_back({fn, user, S().initializing});
}
void ApiOnPad(void (*fn)(void*, NierModPad*), void* user) {
  if (fn && S().initializing) S().pad.push_back({fn, user, S().initializing});
}
void ApiBlockGameInput(int32_t block) { pc_options_state().mod_input_blocked = block != 0; }
void ApiOnAddGold(void (*fn)(void*, int32_t*), void* user) {
  if (fn && S().initializing) S().add_gold.push_back({fn, user, S().initializing});
}

// ---- Loading ----

void* OpenLibrary(const fs::path& path, std::string& error) {
#if defined(_WIN32)
  HMODULE module = LoadLibraryW(path.wstring().c_str());
  if (!module) error = "LoadLibrary error " + std::to_string(GetLastError());
  return module;
#else
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) error = dlerror() ? dlerror() : "dlopen failed";
  return handle;
#endif
}

void* FindSymbol(void* library, const char* name) {
#if defined(_WIN32)
  return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
  return dlsym(library, name);
#endif
}

#if defined(__ANDROID__)
// Shared storage is mounted without execute permission: the library is copied into the app's own files first.
fs::path LoadableCopy(const fs::path& source, const std::string& mod_name) {
  const char* internal = SDL_GetAndroidInternalStoragePath();
  if (!internal) return {};
  const fs::path dir = fs::path(internal) / "mods" / mod_name;
  std::error_code ec;
  fs::create_directories(dir, ec);
  const fs::path dest = dir / source.filename();
  fs::copy_file(source, dest, fs::copy_options::overwrite_existing, ec);
  return ec ? fs::path{} : dest;
}
#endif

bool IsModLibrary(const fs::path& file) {
#if defined(_WIN32)
  return file.extension() == ".dll";
#else
  const std::string name = file.filename().string();
  return file.extension() == ".so" && name.rfind("lib", 0) == 0;
#endif
}

std::string LanguageCode() {
  const std::string name = REXCVAR_GET(nier_language);
  if (name.find("Portu") != std::string::npos) return "pt";
  if (name.find("Espa") != std::string::npos) return "es";
  return "en";
}

void RegisterHotkeysLocked(State& s) {
  if (s.hotkeys_done || !s.loaded || !s.run_on_ui) return;
  s.hotkeys_done = true;
  for (size_t i = 0; i < s.panels.size(); ++i) {
    NierModPanel& p = *s.panels[i];
    p.bind = "bind_mod_" + p.mod->name + "_" + std::to_string(p.ordinal);
    for (char& c : p.bind) {
      if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
    }
    // Every panel gets its bind (a key can be given later, in its mod's page); settings.txt has the say over a
    // value nier.toml may have kept.
    rex::ui::RegisterBind(p.bind, p.hotkey, p.title, [i] { TogglePanel(int(i)); });
    rex::cvar::SetFlagByName(p.bind, p.hotkey);
    REXLOG_INFO("[mods] {}: \"{}\" opens with {}", p.mod->name, p.title, p.hotkey.empty() ? "(no key)" : p.hotkey);
  }
}

// ---- The panels' window ----

class PanelsDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit PanelsDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    State& s = S();
    std::lock_guard lock(s.mutex);
    if (rex::ui::Window* window = imgui_drawer()->window();
        window && window->GetCursorVisibility() != rex::ui::Window::CursorVisibility::kVisible) {
      window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    }
    const float scale = std::max(1.0f, io.DisplaySize.y / 720.0f);
    ImFont* font = pc_options_font();
    int shown = 0;
    for (auto& panel_ptr : s.panels) {
      NierModPanel& panel = *panel_ptr;
      if (!panel.open) continue;
      if (font) ImGui::PushFont(font);
      ImGui::SetNextWindowPos(ImVec2(40.0f * scale + shown * 30.0f * scale, 80.0f * scale), ImGuiCond_Appearing);
      ImGui::SetNextWindowSize(ImVec2(520.0f * scale, 0.0f), ImGuiCond_Appearing);
      ImGui::SetNextWindowBgAlpha(0.92f);
      bool keep = true;
      const std::string id = panel.title + "##mod" + std::to_string(reinterpret_cast<uintptr_t>(&panel));
      if (ImGui::Begin(id.c_str(), &keep, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        // The menu font is 40 px: about 24 px tall at 1080p. Without it (Android: no Georgia) ImGui's own 13 px font,
        // made big enough for a finger.
        if (font) ImGui::SetWindowFontScale(0.6f * io.DisplaySize.y / 1080.0f);
        else ImGui::SetWindowFontScale(std::max(1.0f, io.DisplaySize.y / 400.0f));
        for (Control& c : panel.controls) {
          switch (c.kind) {
            case Control::kToggle: {
              bool on = *c.value != 0;
              if (ImGui::Checkbox(c.label.c_str(), &on)) Changed(panel, c, on ? 1 : 0);
              break;
            }
            case Control::kInt: {
              int v = *c.value;
              ImGui::SetNextItemWidth(220.0f * scale);
              if (ImGui::SliderInt(c.label.c_str(), &v, c.min, c.max)) Changed(panel, c, v);
              break;
            }
            case Control::kButton:
              if (ImGui::Button(c.label.c_str())) c.on_click(c.user);
              break;
            case Control::kText:
              ImGui::TextWrapped("%s", c.label.c_str());
              break;
            case Control::kChoice: {
              // Arrows either side (a finger hits them; a combo's list is long), the name between.
              int v = *c.value;
              ImGui::PushID(&c);
              if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) Changed(panel, c, v <= c.min ? c.max : v - 1);
              ImGui::SameLine();
              ImGui::SetNextItemWidth(260.0f * scale);
              if (ImGui::BeginCombo("##list", size_t(v) < c.names.size() ? c.names[size_t(v)].c_str() : "?")) {
                for (int i = 0; i < int(c.names.size()); ++i)
                  if (ImGui::Selectable(c.names[size_t(i)].c_str(), i == v)) Changed(panel, c, i);
                ImGui::EndCombo();
              }
              ImGui::SameLine();
              if (ImGui::ArrowButton("##next", ImGuiDir_Right)) Changed(panel, c, v >= c.max ? c.min : v + 1);
              ImGui::SameLine();
              ImGui::TextUnformatted(c.label.c_str());
              ImGui::PopID();
              break;
            }
          }
        }
        if (!panel.hotkey.empty()) {
          ImGui::Separator();
          ImGui::TextDisabled("%s: %s", s.language == "pt" ? "Fechar" : s.language == "es" ? "Cerrar" : "Close",
                              panel.hotkey.c_str());
        }
      }
      ImGui::End();
      if (font) ImGui::PopFont();
      if (!keep || ImGui::IsKeyPressed(ImGuiKey_Escape)) panel.open = false;
      ++shown;
    }
    if (!shown) UpdateOpenLocked(s);
  }

 public:
  // A value changed (by the panel or the game's menu; under the mutex): kept and saved.
  static void Set(NierModPanel& panel, Control& c, int32_t v) {
    *c.value = std::clamp(v, c.min, c.max);
    panel.mod->saved[c.id] = *c.value;
    SaveSettings(*panel.mod);
  }

 private:
  static void Changed(NierModPanel& panel, Control& c, int32_t v) { Set(panel, c, v); }

 public:
  static void UpdateOpenLocked(State& s);
};

std::unique_ptr<PanelsDialog> g_dialog;

void PanelsDialog::UpdateOpenLocked(State& s) {
  bool any = false;
  for (auto& p : s.panels) any |= p->open;
  auto& pc = pc_options_state();
  if (pc.mod_panel_open.exchange(any) != any) {
    rex::input::mnk::SetMouseLookActive(!any);
    if (!any && s.run_on_ui) {
      // Not from inside the dialog's own drawing.
      s.run_on_ui([] {
        bool still = false;
        {
          std::lock_guard lock(S().mutex);
          for (auto& p : S().panels) still |= p->open;
        }
        if (!still) g_dialog.reset();
      });
    }
  }
}

}  // namespace

void Load(uint8_t* guest_base) {
  State& s = S();
  std::lock_guard lock(s.mutex);
  if (s.loaded) return;
  s.loaded = true;
  s.language = LanguageCode();
  std::vector<fs::path> dirs = text::ModDirs();
  std::sort(dirs.begin(), dirs.end());  // load in name order
  for (const fs::path& dir : dirs) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      if (!entry.is_regular_file(ec) || !IsModLibrary(entry.path())) continue;
      auto mod = std::make_unique<Mod>();
      mod->name = dir.filename().string();
      mod->dir = dir;
      mod->dir_utf8 = dir.string();
      LoadSettings(*mod);
      fs::path path = entry.path();
#if defined(__ANDROID__)
      path = LoadableCopy(path, mod->name);
      if (path.empty()) {
        REXLOG_ERROR("[mods] {}: could not copy {} to the app's files", mod->name, entry.path().string());
        continue;
      }
#endif
      std::string error;
      void* library = OpenLibrary(path, error);
      if (!library) {
        REXLOG_ERROR("[mods] {}: {} did not load ({})", mod->name, entry.path().filename().string(), error);
        continue;
      }
      auto init = reinterpret_cast<NierModInitFn>(FindSymbol(library, "nier_mod_init"));
      if (!init) {
        REXLOG_ERROR("[mods] {}: {} has no nier_mod_init", mod->name, entry.path().filename().string());
        continue;
      }
      NierModApi& api = mod->api;
      api.version = NIER_MOD_API_VERSION;
      api.guest_base = guest_base;
      api.language = s.language.c_str();
      api.mod_dir = mod->dir_utf8.c_str();
#if defined(_WIN32)
      api.platform = "windows";
#else
      api.platform = "android";
#endif
      api.log = &ApiLog;
      api.create_panel = &ApiCreatePanel;
      api.add_toggle = &ApiAddToggle;
      api.add_int = &ApiAddInt;
      api.add_button = &ApiAddButton;
      api.add_text = &ApiAddText;
      api.show_in_menus = &ApiShowInMenus;
      api.on_set_hp = &ApiOnSetHp;
      api.on_add_mp = &ApiOnAddMp;
      api.on_add_exp = &ApiOnAddExp;
      api.on_add_gold = &ApiOnAddGold;
      api.on_damage = &ApiOnDamage;
      api.on_hit = &ApiOnHit;
      api.on_camera = &ApiOnCamera;
      api.get_pad = &ApiGetPad;
      api.block_game_input = &ApiBlockGameInput;
      api.on_pad = &ApiOnPad;
      api.add_choice = &ApiAddChoice;
      api.set_option = &ApiSetOption;
      api.on_player_warp = &ApiOnPlayerWarp;
      s.initializing = mod.get();
      const int32_t result = init(&api);
      s.initializing = nullptr;
      REXLOG_INFO("[mods] {}: {} loaded (init {})", mod->name, entry.path().filename().string(), result);
      s.mods.push_back(std::move(mod));
    }
  }
  RegisterHotkeysLocked(s);
}

void InitUi(rex::ui::ImGuiDrawer* drawer, std::function<void(std::function<void()>)> run_on_ui_thread) {
  State& s = S();
  std::lock_guard lock(s.mutex);
  s.drawer = drawer;
  s.run_on_ui = std::move(run_on_ui_thread);
  RegisterHotkeysLocked(s);
}

std::vector<PanelInfo> Panels() {
  State& s = S();
  std::lock_guard lock(s.mutex);
  std::vector<PanelInfo> out;
  for (size_t i = 0; i < s.panels.size(); ++i) {
    if (s.panels[i]->mod->enabled)
      out.push_back({int(i), s.panels[i]->title, s.panels[i]->icon_png, s.panels[i]->menus});
  }
  return out;
}

std::vector<ModInfo> Mods() {
  State& s = S();
  std::lock_guard lock(s.mutex);
  std::vector<ModInfo> out;
  for (size_t i = 0; i < s.mods.size(); ++i) {
    const Mod& m = *s.mods[i];
    std::string title = m.name;
    for (auto& p : s.panels) {
      if (p->mod == &m && p->ordinal == 0) title = p->title;
    }
    out.push_back({int(i), m.name, title, m.enabled});
  }
  return out;
}

void SetModEnabled(int mod, bool enabled) {
  State& s = S();
  std::lock_guard lock(s.mutex);
  if (mod < 0 || size_t(mod) >= s.mods.size()) return;
  Mod& m = *s.mods[size_t(mod)];
  m.enabled = enabled;
  if (!enabled) {
    for (auto& p : s.panels) {
      if (p->mod == &m) p->open = false;
    }
    pc_options_state().mod_input_blocked = false;  // a mod turned off gives the controller back
  }
  SaveSettings(m);
  REXLOG_INFO("[mods] {} {}", m.name, enabled ? "ativado" : "desativado");
}

std::vector<HotkeyInfo> Hotkeys(int mod) {
  State& s = S();
  std::lock_guard lock(s.mutex);
  std::vector<HotkeyInfo> out;
  if (mod < 0 || size_t(mod) >= s.mods.size()) return out;
  for (size_t i = 0; i < s.panels.size(); ++i) {
    if (s.panels[i]->mod == s.mods[size_t(mod)].get())
      out.push_back({int(i), s.panels[i]->title, s.panels[i]->hotkey});
  }
  return out;
}

void SetHotkey(int panel, const std::string& key) {
  State& s = S();
  std::lock_guard lock(s.mutex);
  if (panel < 0 || size_t(panel) >= s.panels.size()) return;
  NierModPanel& p = *s.panels[size_t(panel)];
  p.hotkey = key;
  p.mod->text["_hotkey" + std::to_string(p.ordinal)] = key;
  if (!p.bind.empty()) rex::cvar::SetFlagByName(p.bind, key);
  SaveSettings(*p.mod);
}

std::vector<MenuRow> ModRows(int mod) {
  State& s = S();
  std::lock_guard lock(s.mutex);
  std::vector<MenuRow> rows;
  if (mod < 0 || size_t(mod) >= s.mods.size()) return rows;
  for (auto& panel_ptr : s.panels) {
    NierModPanel* panel = panel_ptr.get();
    if (panel->mod != s.mods[size_t(mod)].get()) continue;
    for (size_t i = 0; i < panel->controls.size(); ++i) {
      Control* c = &panel->controls[i];
      MenuRow row;
      row.label = c->label;
      switch (c->kind) {
        case Control::kToggle:
          row.value = [c] { return pc_text::T(*c->value ? "On" : "Off"); };
          row.act = [panel, c](int) {
            std::lock_guard lock(S().mutex);
            PanelsDialog::Set(*panel, *c, *c->value ? 0 : 1);
          };
          break;
        case Control::kInt:
          row.value = [c] { return std::to_string(*c->value); };
          row.act = [panel, c](int dir) {
            std::lock_guard lock(S().mutex);
            int v = *c->value;
            if (dir == 0) v = v >= c->max ? c->min : v + 1;
            else v = std::clamp(v + dir, c->min, c->max);
            PanelsDialog::Set(*panel, *c, v);
          };
          break;
        case Control::kButton:
          row.act = [c](int dir) {
            if (dir == 0) c->on_click(c->user);
          };
          break;
        case Control::kText:
          continue;
        case Control::kChoice:
          row.value = [c] { return size_t(*c->value) < c->names.size() ? c->names[size_t(*c->value)] : std::string("?"); };
          row.act = [panel, c](int dir) {
            std::lock_guard lock(S().mutex);
            int v = *c->value + (dir < 0 ? -1 : 1);
            if (v > c->max) v = c->min;
            if (v < c->min) v = c->max;
            PanelsDialog::Set(*panel, *c, v);
          };
          break;
      }
      rows.push_back(std::move(row));
    }
  }
  return rows;
}

void TogglePanel(int index) {
  State& s = S();
  std::function<void(std::function<void()>)> run;
  {
    std::lock_guard lock(s.mutex);
    if (index < 0 || size_t(index) >= s.panels.size() || !s.panels[index]->mod->enabled) return;
    s.panels[index]->open = !s.panels[index]->open;
    if (s.panels[index]->open) {
      pc_options_state().mod_panel_open = true;
      rex::input::mnk::SetMouseLookActive(false);
    }
    run = s.run_on_ui;
  }
  if (run) {
    run([] {
      std::lock_guard lock(S().mutex);
      bool any = false;
      for (auto& p : S().panels) any |= p->open;
      if (any && !g_dialog && S().drawer) g_dialog = std::make_unique<PanelsDialog>(S().drawer);
    });
  }
}

bool AnyPanelOpen() { return pc_options_state().mod_panel_open; }

void OnPad(uint16_t& buttons, uint8_t& left_trigger, uint8_t& right_trigger, int16_t& lx, int16_t& ly, int16_t& rx,
           int16_t& ry) {
  auto& handlers = S().pad;
  if (handlers.empty()) return;
  NierModPad p{buttons, left_trigger, right_trigger, lx, ly, rx, ry};
  for (auto& h : handlers) if (h.mod->enabled) h.fn(h.user, &p);
  buttons = p.buttons;
  left_trigger = p.left_trigger;
  right_trigger = p.right_trigger;
  lx = p.lx;
  ly = p.ly;
  rx = p.rx;
  ry = p.ry;
}

}  // namespace nier::mods

// ---- The game events ----

REX_HOOK_RAW(sub_8237D938) {
  {  // TEMP (2026-11-15, the trainer's search): NIER_HPCALLER logs each new caller of the HP setter.
    static const bool log = std::getenv("NIER_HPCALLER") != nullptr;
    static std::mutex m;
    static std::map<uint32_t, int> seen;
    if (log) {
      std::lock_guard lock(m);
      const uint32_t lr = uint32_t(ctx.lr);
      if (seen[lr]++ < 3)
        REXLOG_INFO("HPCALLER lr={:08X} obj={:08X} vt={:08X} new={} slot={}", lr, ctx.r3.u32,
                    nier::mods::LoadBe32(base, ctx.r3.u32), ctx.r4.s32, nier::mods::LoadBe32(base, ctx.r3.u32 + 12));
    }
  }
  auto& handlers = nier::mods::S().set_hp;
  if (!handlers.empty()) {
    const uint32_t slot = nier::mods::LoadBe32(base, ctx.r3.u32 + 12);
    if (slot < 16) {
      const int32_t current = int32_t(nier::mods::LoadBe32(base, NIER_SAVE_HP(slot)));
      int32_t value = ctx.r4.s32;
      for (auto& h : handlers) if (h.mod->enabled) h.fn(h.user, slot, current, &value);
      ctx.r4.s64 = value;
    }
  }
  __imp__sub_8237D938(ctx, base);
}

namespace nier::moveset {
void TestSpawnFromCast(const PPCContext& ctx, uint8_t* base);  // bullet_trace.cpp
}
REX_HOOK_RAW(sub_8237DC10) {
  // TEMP (the moveset mod's proof): Dark Lance's cost (sub_822396D8+0x4A4) also fires nier_moveset_test's pattern.
  if (uint32_t(ctx.lr) == 0x82239B7Cu && ctx.f1.f64 < 0.0) nier::moveset::TestSpawnFromCast(ctx, base);
  {  // TEMP (2026-11-19, the moveset mod): who spends MP (a magic cast), with the callers above, each chain once.
    if (ctx.f1.f64 < 0.0) {
      static std::mutex m;
      static std::set<std::string> seen;
      // The chain: this call's LR, then each frame's saved LR (back chain at 0(r1); a frame's LR at -8 of the
      // caller's frame, as __savegprlr leaves it).
      std::string chain = fmt::format("{:08X}", uint32_t(ctx.lr));
      uint32_t sp = ctx.r1.u32;
      for (int i = 0; i < 5; ++i) {
        const uint32_t up = nier::mods::LoadBe32(base, sp);
        if (up <= sp || up - sp > 0x10000u || (up & 7)) break;  // the back chain only goes up the stack
        sp = up;
        chain += fmt::format(" < {:08X}", nier::mods::LoadBe32(base, sp - 8));
      }
      std::lock_guard lock(m);
      if (seen.size() < 200 && seen.insert(chain).second)
        REXLOG_INFO("[magia] MP {:.1f} por {}", ctx.f1.f64, chain);
    }
  }
  auto& handlers = nier::mods::S().add_mp;
  if (!handlers.empty()) {
    const uint32_t slot = nier::mods::LoadBe32(base, ctx.r3.u32 + 12);
    float delta = float(ctx.f1.f64);
    for (auto& h : handlers) if (h.mod->enabled) h.fn(h.user, slot, &delta);
    ctx.f1.f64 = delta;
  }
  __imp__sub_8237DC10(ctx, base);
}

REX_HOOK_RAW(sub_8237F220) {
  auto& handlers = nier::mods::S().add_exp;
  if (!handlers.empty()) {
    const uint32_t slot = nier::mods::LoadBe32(base, ctx.r3.u32 + 12);
    int32_t amount = ctx.r4.s32;
    for (auto& h : handlers) if (h.mod->enabled) h.fn(h.user, slot, &amount);
    ctx.r4.s64 = amount;
  }
  __imp__sub_8237F220(ctx, base);
}

REX_HOOK_RAW(sub_82345CB0) {
  auto& handlers = nier::mods::S().add_gold;
  if (!handlers.empty()) {
    int32_t amount = ctx.r4.s32;
    for (auto& h : handlers) if (h.mod->enabled) h.fn(h.user, &amount);
    ctx.r4.s64 = amount;
  }
  __imp__sub_82345CB0(ctx, base);
}

namespace nier::mods {
namespace {
// A hit on the character at 'actor': whether the mods ignore it.
bool HitIgnored(uint8_t* base, uint32_t actor) {
  auto& handlers = S().hit;
  if (handlers.empty() || !actor) return false;
  const uint32_t status = LoadBe32(base, actor + 3472);
  if (!status) return false;
  const int32_t party = LoadBe32(base, status) == kPartyStatusVtable ? 1 : 0;
  int32_t ignore = 0;
  for (auto& h : handlers) if (h.mod->enabled) h.fn(h.user, status, party, &ignore);
  return ignore != 0;
}
}  // namespace
}  // namespace nier::mods

namespace nier::moveset {
void Tick(const PPCContext& ctx, uint8_t* base);  // bullet_trace.cpp: the mod's shots hurt the enemies they touch
}
REX_HOOK_RAW(sub_821A7D00) {
  __imp__sub_821A7D00(ctx, base);
  nier::moveset::Tick(ctx, base);
  auto& handlers = nier::mods::S().camera;
  if (handlers.empty()) return;
  float m[16];
  for (int i = 0; i < 16; ++i) {
    const uint32_t v = nier::mods::LoadBe32(base, kCameraWorld + uint32_t(i) * 4);
    std::memcpy(&m[i], &v, 4);
  }
  float before[16];
  std::memcpy(before, m, sizeof m);
  for (auto& h : handlers) if (h.mod->enabled) h.fn(h.user, m);
  if (std::memcmp(before, m, sizeof m) == 0) return;
  for (uint32_t at : {kCameraWorld, kCameraWorldCopy}) {
    uint8_t* p = base + at;
    for (int i = 0; i < 16; ++i) {
      uint32_t v;
      std::memcpy(&v, &m[i], 4);
      p[i * 4] = uint8_t(v >> 24); p[i * 4 + 1] = uint8_t(v >> 16); p[i * 4 + 2] = uint8_t(v >> 8); p[i * 4 + 3] = uint8_t(v);
    }
  }
}

namespace nier::moveset {
void LogHit(const char* what, const PPCContext& ctx, uint8_t* base, uint32_t data, uint32_t words);  // TEMP
}
REX_HOOK_RAW(sub_821B95B8) {
  nier::moveset::LogHit("receive", ctx, base, ctx.r4.u32, 48);  // TEMP: the attack, to copy
  if (nier::mods::HitIgnored(base, ctx.r3.u32)) {
    // As an attack with no damage: result |= 514, no hit.
    if (ctx.r5.u32) {
      uint8_t* p = base + ctx.r5.u32;
      const uint32_t v = (uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]) | 514u;
      p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
    }
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_821B95B8(ctx, base);
}

REX_HOOK_RAW(sub_821C92B8) {
  nier::moveset::LogHit("hit", ctx, base, ctx.r5.u32, 24);  // TEMP: the hit's data, to copy
  {  // TEMP (the trainer's search): the character hit, with NIER_HPCALLER.
    static const bool log = std::getenv("NIER_HPCALLER") != nullptr;
    if (log && ctx.r3.u32) {
      const uint32_t status = nier::mods::LoadBe32(base, ctx.r3.u32 + 3472);
      REXLOG_INFO("HIT actor={:08X} vt={:08X} status={:08X} status_vt={:08X}", ctx.r3.u32,
                  nier::mods::LoadBe32(base, ctx.r3.u32), status, status ? nier::mods::LoadBe32(base, status) : 0);
    }
  }
  if (nier::mods::HitIgnored(base, ctx.r3.u32)) return;
  __imp__sub_821C92B8(ctx, base);
}

REX_HOOK_RAW(sub_822105B8) {
  auto& handlers = nier::mods::S().damage;
  static const bool log = std::getenv("NIER_HPCALLER") != nullptr;  // TEMP (the trainer's search)
  if (log)
    REXLOG_INFO("DAMAGE lr={:08X} target={:08X} vt={:08X} damage={} flags={:X} none={}", uint32_t(ctx.lr), ctx.r3.u32,
                nier::mods::LoadBe32(base, ctx.r3.u32), ctx.r4.s32, ctx.r5.u32, ctx.r6.u32 & 0xFF);
  if (!handlers.empty()) {
    const int32_t party = nier::mods::LoadBe32(base, ctx.r3.u32) == kPartyStatusVtable ? 1 : 0;
    int32_t damage = ctx.r4.s32, blocked = 0;
    for (auto& h : handlers) if (h.mod->enabled) h.fn(h.user, ctx.r3.u32, party, &damage, &blocked);
    if (blocked) {
      // As the game's own invulnerability does.
      uint8_t* result = base + ctx.r7.u32;
      result[0] = 0; result[1] = 0; result[2] = 0x20; result[3] = 0x02;  // 8194
      ctx.r3.u64 = 0;
      return;
    }
    ctx.r4.s64 = damage;
  }
  __imp__sub_822105B8(ctx, base);
}

// sub_8221AEB0(Nier, r4 = where): puts Nier back on the ground (2026-11-20, found with the write watch on his height:
// after ~7 s in the air the game brought him from 84.6 / 26.9 down to 9.25, the infinite jump's "teleport"). The mods
// may cancel it (on_player_warp).
// 2026-11-20 (the player: sent to the wrong place entering the library by its door, with the infinite jump on): it has
// two callers. sub_824888D0 works out the safe place itself - the fall's rescue, the one the mods may cancel (it
// returns to 0x82488980). sub_82391C08 is a script command putting Nier where the script says (doors, map entries):
// never cancelled - the Trainer did, when Nier was "in the air" through a door within 10 s of a jump.
extern "C" void __imp__sub_8221AEB0(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(sub_8221AEB0) {
  auto& handlers = nier::mods::S().warp;
  const uint32_t from = uint32_t(ctx.lr);
  // Logged when the caller changes: 821AF4C4 calls it every frame in Wendy's fight (2026-11-22: 12,000 lines).
  static uint32_t last_from = 0;
  if (ctx.r3.u32 == 0x848BE4B0u && from != last_from) {
    last_from = from;
    REXLOG_INFO("[mods] Nier posto no chao (por {:08X})", from);
  }
  if (!handlers.empty() && ctx.r3.u32 == 0x848BE4B0u && from == 0x82488980u) {
    int32_t cancel = 0;
    for (auto& h : handlers) if (h.mod->enabled) h.fn(h.user, &cancel);
    if (cancel) return;
  }
  __imp__sub_8221AEB0(ctx, base);
}