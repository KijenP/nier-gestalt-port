// nier - the title menu on the phone as a row of big cards (see title_carousel.h).
#include "title_carousel.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>

#include "game_assets.h"
#include "menu_mouse.h"

namespace renut::gpu {
void NierHideTextureSizes(const uint32_t* sizes, int n, uint32_t page_lo, uint32_t page_hi);  // render/draw.cpp
void NierHideTextures(bool on);
uint32_t NierHideFound(int i);
}  // namespace renut::gpu

namespace nier::title_carousel {
uint32_t TitlePackAddress();  // game_text_hooks.cpp: the loaded 2D\TITLE.2DP (0: none yet)

namespace {
constexpr uint32_t kPackSize = 409600;

uint64_t Now() { return menu_mouse::NowMs(); }

// The game's title rows: hidden while the title runs. The first three are the lists' lines (the menu's three, its
// Downloadable Content, the difficulty's three): the cards' words, cut from them.
const char* const kHidden[] = {"title_gameselection", "title_gameselection_dl", "title_diffselection", "title_new",
                               "title_load",          "title_setting",          "title_download content",
                               "title_easy",          "title_normal",           "title_hard"};
constexpr int kSheets = 3;

// A card's word: a line of one of the sheets (found by its ink when the sheet is read: the lines do not sit where the
// sheet's size says, measured 2026-11-16: ~24 rows lower, in its 32-row padding).
struct Word {
  const char* sheet;
  int line;
};
Word WordOf(int kind, int i) {
  if (kind == 1) return i < 3 ? Word{"title_diffselection", i} : Word{nullptr, 0};
  if (i < 3) return {"title_gameselection", i};
  return {"title_gameselection_dl", 0};
}

// The lines of text of a sheet: runs of rows with ink, joined across small gaps (accents, descenders).
std::vector<std::pair<int, int>> InkLines(const GameImage& image) {
  std::vector<int> ink(size_t(image.height), 0);
  for (int y = 0; y < image.height; ++y)
    for (int x = 0; x < image.width; ++x) ink[size_t(y)] += image.rgba[(size_t(y) * image.width + x) * 4 + 3] > 40;
  std::vector<std::pair<int, int>> lines;
  for (int y = 0; y < image.height; ++y) {
    if (!ink[size_t(y)]) continue;
    if (!lines.empty() && y - lines.back().second <= 3) lines.back().second = y + 1;
    else lines.push_back({y, y + 1});
  }
  std::erase_if(lines, [](const auto& l) { return l.second - l.first < 5; });  // specks
  for (auto& l : lines) l = {std::max(0, l.first - 2), std::min(image.height, l.second + 2)};
  return lines;
}

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// A card's box on the screen (fractions of the window) and its row, as last drawn.
struct CardBox {
  int row;
  float x0, y0, x1, y1;
};

struct Shared {
  std::mutex m;
  // The list (game's thread).
  int kind = 0, rows = 0, row = 0;
  std::string names[4];
  uint64_t list_ms = 0;
  std::atomic<uint64_t> tick_ms{0};
  // What the cards ask (input / UI threads) and the game's thread takes.
  int request = -1;
  int confirm_row = -1;
  uint64_t confirm_ms = 0;
  uint64_t a_until = 0;
  // The cards as drawn (UI thread), for the touches.
  std::vector<CardBox> boxes;
  int boxes_kind = -1;
  // The finger driving the cards (input thread).
  bool finger = false;
  uint64_t finger_id = 0;
  float fx0 = 0, fy0 = 0, fx = 0, fy = 0;
  int base_row = 0;
  bool dragged = false;
  float step_heights = 0.3f;  // a card's spacing, in screen heights (set by the drawing)
} g;

bool ShowingLocked(uint64_t now) { return g.rows > 0 && now - g.list_ms < 250; }

}  // namespace

// The game's 1280x720 point on the window (fractions), as touch_controls.h's ToGameSpace the other way round.
void GameToScreen(float gx, float gy, float aspect, float& fx, float& fy) {
  static const float kGame = 1280.0f / 720.0f;
  const std::string mode = rex::cvar::GetFlagByName("nier_screen_fit");
  if (mode == "stretch") {
    fx = gx / 1280.0f;
    fy = gy / 720.0f;
    return;
  }
  const float kMaxTrim = mode == "bars" || mode == "wide" || mode == "widereal" ? 0.0f : 0.15f;
  float vis_x = 1.0f, vis_y = 1.0f;
  if (aspect <= 0.0f) aspect = kGame;
  if (kGame > aspect)
    vis_x = std::max(aspect / kGame, 1.0f - kMaxTrim);
  else
    vis_y = std::max(kGame / aspect, 1.0f - kMaxTrim);
  const float shown = kGame * vis_x / vis_y;
  const float sw = shown > aspect ? 1.0f : shown / aspect;
  const float sh = shown > aspect ? aspect / shown : 1.0f;
  fx = 0.5f + (gx / 1280.0f - 0.5f) * sw / vis_x;
  fy = 0.5f + (gy / 720.0f - 0.5f) * sh / vis_y;
}

namespace {

// Asks for a row (clamped to the list).
void RequestLocked(int row) {
  if (g.rows <= 0) return;
  g.request = std::clamp(row, 0, g.rows - 1);
}

// A touch at x, y (not a drag): a side card comes to the middle, the middle one is chosen.
void TapLocked(float x, float y) {
  for (const CardBox& b : g.boxes) {
    if (x < b.x0 || x >= b.x1 || y < b.y0 || y >= b.y1) continue;
    if (b.row == g.row && g.request < 0) {
      g.confirm_row = b.row;
      g.confirm_ms = Now();
      REXLOG_INFO("Carrossel: escolhido {}", b.row);
    } else {
      RequestLocked(b.row);
    }
    return;
  }
}

// --- The pack: the words' images and the pages of the rows to hide -------------------------------------------

struct Pack {
  std::mutex m;
  bool listing = false, listed = false;
  std::map<std::string, PackTexture> textures;
  uint32_t at = 0;              // the address the hidden textures were set for
  std::vector<uint64_t> sums;   // each sheet's bytes when read (a language change rewrites them)
  int delta = 0;                // where the sheets' data sat from the page seen (bytes)
  uint64_t checked = 0;         // ms
  std::map<std::string, GameImage> words;
  std::map<std::string, std::vector<std::pair<int, int>>> lines;
  bool words_new = false;
} pack;

void EnsureListed() {
  std::lock_guard lock(pack.m);
  if (pack.listed || pack.listing) return;
  pack.listing = true;
  std::thread([] {
    auto t = ListPackTextures("TITLE");
    std::lock_guard lock2(pack.m);
    pack.textures = std::move(t);
    pack.listed = true;
    pack.listing = false;
    REXLOG_INFO("Carrossel: TITLE.2DV com {} texturas", pack.textures.size());
  }).detach();
}

// The sheets' bytes where the renderer saw them (physical pages), for a change of language.
uint64_t SheetSum(uint32_t page) {
  const uint8_t* p = REX_KERNEL_MEMORY()->TranslatePhysical<const uint8_t*>(page << 12);
  uint64_t h = 1469598103934665603ull;
  for (uint32_t i = 0; i < 4096; i += 7) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

// White letters whose alpha is the sheet's brightness (the menu's sheet is white, the difficulty's grey).
void MakeWhite(GameImage& image) {
  int top = 1;
  for (size_t i = 0; i < image.rgba.size(); i += 4)
    top = std::max(top, int(image.rgba[i]) * image.rgba[i + 3] / 255);
  for (size_t i = 0; i < image.rgba.size(); i += 4) {
    const int v = std::min(255, int(image.rgba[i]) * image.rgba[i + 3] / top);
    image.rgba[i] = image.rgba[i + 1] = image.rgba[i + 2] = 255;
    image.rgba[i + 3] = uint8_t(v);
  }
}

// UI thread, each frame: the rows to hide follow the pack; the sheets are read where the renderer saw them, again
// when their bytes change (a language change).
void UpdatePack(bool title_running) {
  const uint32_t at = TitlePackAddress();
  if (!title_running || !at) {
    renut::gpu::NierHideTextures(false);
    return;
  }
  EnsureListed();
  std::lock_guard lock(pack.m);
  if (!pack.listed || pack.textures.empty()) return;
  if (at != pack.at) {
    pack.at = at;
    std::vector<uint32_t> sizes;
    for (const char* n : kHidden) {
      auto it = pack.textures.find(n);
      sizes.push_back(it == pack.textures.end() ? 0xFFFFFFFFu : Be32(it->second.fetch + 8));
    }
    const uint32_t page = (at & 0x1FFFFFFFu) >> 12;
    renut::gpu::NierHideTextureSizes(sizes.data(), int(sizes.size()), page - 8, page + kPackSize / 4096 + 8);
    REXLOG_INFO("Carrossel: pacote TITLE em {:08X} (paginas {:05X}..)", at, page);
    pack.sums.assign(kSheets, 0);
    pack.checked = 0;
  }
  renut::gpu::NierHideTextures(true);
  const uint64_t now = Now();
  if (now - pack.checked < 500) return;
  pack.checked = now;
  for (int j = 0; j < kSheets; ++j) {
    const uint32_t page = renut::gpu::NierHideFound(j);
    if (!page) continue;
    const uint64_t sum = SheetSum(page) ^ page;
    if (sum == pack.sums[size_t(j)]) continue;
    pack.sums[size_t(j)] = sum;
    auto it = pack.textures.find(kHidden[j]);
    const uint8_t* p = REX_KERNEL_MEMORY()->TranslatePhysical<const uint8_t*>(page << 12);
    // Where the data really starts next to the page the renderer saw differs by platform (measured 2026-11-16 on
    // the PC: 4 KB further on, the pack's 4 KB shift): each place is tried, the one whose lines of text all fit
    // the sheet wins.
    const size_t want = j == 1 ? 1 : 3;
    GameImage image;
    bool found = false;
    // The one-line sheet fits anywhere: the place the three-line ones fitted goes first.
    for (const int delta : {pack.delta, 0, 0x1000, -0x1000}) {
      std::vector<uint8_t> bytes(p + delta, p + delta + 96 * 1024);
      PackTexture t = it->second;
      t.offset = 0;
      GameImage candidate;
      if (!DecodePackTexture(t, bytes, candidate)) {
        REXLOG_WARN("Carrossel: {} nao decodificada (formato {:02X})", kHidden[j], unsigned(t.fetch[7] & 0x3F));
        break;
      }
      MakeWhite(candidate);
      const auto lines = InkLines(candidate);
      const bool fits = lines.size() == want && lines.front().first <= 6 && lines.back().second <= candidate.height;
      if (fits || delta == 0) image = std::move(candidate);
      if (fits) {
        found = true;
        if (want > 1) pack.delta = delta;
        REXLOG_INFO("Carrossel: {} no deslocamento {}", kHidden[j], delta);
        break;
      }
    }
    if (image.rgba.empty()) continue;
    if (!found) REXLOG_WARN("Carrossel: {}: linhas nao conferem, usando a pagina", kHidden[j]);
    if (const char* dir = std::getenv("NIER_CAROUSEL_DUMP")) {  // TEMP (2026-11-16): the sheets as TGA
      if (std::FILE* f = std::fopen((std::string(dir) + "/" + kHidden[j] + ".tga").c_str(), "wb")) {
        uint8_t h[18] = {0, 0, 2};
        h[12] = uint8_t(image.width), h[13] = uint8_t(image.width >> 8);
        h[14] = uint8_t(image.height), h[15] = uint8_t(image.height >> 8);
        h[16] = 32, h[17] = 0x28;
        std::fwrite(h, 1, 18, f);
        for (size_t i = 0; i < image.rgba.size(); i += 4) {
          const uint8_t px[4] = {image.rgba[i + 2], image.rgba[i + 1], image.rgba[i], image.rgba[i + 3]};
          std::fwrite(px, 1, 4, f);
        }
        std::fclose(f);
      }
    }
    auto lines = InkLines(image);
    std::string ls;
    for (auto& l : lines) ls += fmt::format(" {}-{}", l.first, l.second);
    REXLOG_INFO("Carrossel: {} {}x{} lida (pagina {:05X}), linhas:{}", kHidden[j], image.width, image.height, page, ls);
    pack.lines[kHidden[j]] = std::move(lines);
    pack.words[kHidden[j]] = std::move(image);
    pack.words_new = true;
  }
}

// --- The drawing ---------------------------------------------------------------------------------------------

class CarouselDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit CarouselDialog(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

  void OnDraw(ImGuiIO& io) override {
    const uint64_t now = Now();
    const bool title_running = now - g.tick_ms.load() < 500;
    UpdatePack(title_running);
    EnsureTextures();
    const ImVec2 screen = io.DisplaySize;
    if (screen.x < 1 || screen.y < 1) return;
    const float aspect = screen.x / screen.y;
#if !defined(__ANDROID__)
    PcMouse(io, aspect);
#endif

    int kind, rows, row;
    bool showing;
    {
      std::lock_guard lock(g.m);
      showing = ShowingLocked(now);
      kind = g.kind;
      rows = g.rows;
      row = g.request >= 0 ? g.request : g.row;
    }
    // Fades in and out; the cards slide to the row.
    const float dt = std::clamp(io.DeltaTime, 0.0f, 0.1f);
    fade_ = std::clamp(fade_ + (showing ? dt * 6.0f : -dt * 8.0f), 0.0f, 1.0f);
    if (kind != kind_) {
      kind_ = kind;
      pos_ = float(row);
    }
    pos_ += (float(row) - pos_) * std::min(1.0f, dt * 12.0f);
    if (fade_ <= 0.0f || rows <= 0) {
      std::lock_guard lock(g.m);
      g.boxes.clear();
      return;
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    auto P = [&](float gx, float gy) {
      float fx, fy;
      GameToScreen(gx, gy, aspect, fx, fy);
      return ImVec2(fx * screen.x, fy * screen.y);
    };
    // In the game's 1280x720 space: the rows' band (y 445..575), between the logo and the copyright.
    constexpr float kCy = 515.0f, kW = 250.0f, kH = 128.0f, kSpacing = 272.0f;
    std::vector<CardBox> boxes;
    // Far cards first, the middle one last (on top).
    std::vector<int> order;
    for (int i = 0; i < rows; ++i) order.push_back(i);
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return std::fabs(float(a) - pos_) > std::fabs(float(b) - pos_); });
    for (int i : order) {
      const float d = float(i) - pos_, ad = std::fabs(d);
      const float s = 1.0f - 0.17f * std::min(ad, 2.0f);
      const float cx = 640.0f + kSpacing * d * (1.0f - 0.07f * std::min(ad, 3.0f));
      const float w = kW * s, h = kH * s;
      // The edge away from the middle shorter: the row of cards turns away, as on a drum.
      const float k = 0.14f * std::min(ad, 1.5f);
      const float hl = d > 0 ? h : h * (1.0f - k), hr = d > 0 ? h * (1.0f - k) : h;
      const ImVec2 tl = P(cx - w * 0.5f, kCy - hl * 0.5f), tr = P(cx + w * 0.5f, kCy - hr * 0.5f);
      const ImVec2 br = P(cx + w * 0.5f, kCy + hr * 0.5f), bl = P(cx - w * 0.5f, kCy + hl * 0.5f);
      const float a = fade_ * std::clamp(1.0f - 0.32f * ad, 0.0f, 1.0f);
      if (a <= 0.02f) continue;
      const bool middle = ad < 0.5f;
      const float lit = std::clamp(1.0f - ad * 1.6f, 0.0f, 1.0f);  // 1 in the middle
      auto col = [&](int r, int gg, int b, float al) {
        return IM_COL32(r, gg, b, int(std::clamp(al, 0.0f, 1.0f) * 255));
      };
      // The plate: near-black, a warm grey line round it (the game's menu lines), brighter on the middle card.
      dl->AddQuadFilled(tl, tr, br, bl, col(10, 10, 10, a * 0.86f));
      const float line = std::max(1.0f, screen.y / 360.0f) * s;
      dl->AddQuad(tl, tr, br, bl, col(196, 190, 172, a * (0.45f + 0.5f * lit)), line);
      if (lit > 0.01f) {
        // An inner line and the small marks at the middle of the top and bottom edges (the game's cursor marks).
        auto in = [&](ImVec2 p, ImVec2 c, float t) { return ImVec2(p.x + (c.x - p.x) * t, p.y + (c.y - p.y) * t); };
        const ImVec2 c((tl.x + br.x) * 0.5f, (tl.y + br.y) * 0.5f);
        dl->AddQuad(in(tl, c, 0.06f), in(tr, c, 0.06f), in(br, c, 0.06f), in(bl, c, 0.06f),
                    col(226, 220, 200, a * 0.55f * lit), line * 0.7f);
        const float m = screen.y * 0.012f * s;
        const ImVec2 top((tl.x + tr.x) * 0.5f, (tl.y + tr.y) * 0.5f), bot((bl.x + br.x) * 0.5f, (bl.y + br.y) * 0.5f);
        for (const ImVec2& p : {top, bot})
          dl->AddQuadFilled({p.x, p.y - m}, {p.x + m, p.y}, {p.x, p.y + m}, {p.x - m, p.y}, col(236, 230, 210, a * lit));
      }
      // The word: the game's own image, fitted in the card.
      const Word word = WordOf(kind, i);
      if (word.sheet) {
        auto it = words_tex_.find(word.sheet);
        if (it != words_tex_.end() && it->second.tex && word.line < int(it->second.lines.size())) {
          const auto [y0, y1] = it->second.lines[size_t(word.line)];
          const float sv0 = float(y0) / float(it->second.h), sv1 = float(y1) / float(it->second.h);
          const float iw = float(it->second.w), ih = float(y1 - y0);
          // Fit: at most 82% of the card's width and 40% of its height.
          const float scale = std::min(0.82f * w / iw, 0.40f * h / ih);
          const float fw = iw * scale / w, fh = ih * scale / h;
          auto at = [&](float u, float v) {  // a point of the card by its fractions (bilinear over the 4 corners)
            const ImVec2 top(tl.x + (tr.x - tl.x) * u, tl.y + (tr.y - tl.y) * u);
            const ImVec2 bot(bl.x + (br.x - bl.x) * u, bl.y + (br.y - bl.y) * u);
            return ImVec2(top.x + (bot.x - top.x) * v, top.y + (bot.y - top.y) * v);
          };
          const float u0 = 0.5f - fw * 0.5f, u1 = 0.5f + fw * 0.5f, v0 = 0.5f - fh * 0.5f, v1 = 0.5f + fh * 0.5f;
          const float word_a = a * (middle ? 1.0f : 0.55f + 0.45f * lit);
          const ImTextureID tex = reinterpret_cast<ImTextureID>(it->second.tex.get());
          if (lit > 0.01f) {  // the middle card's word glows, as the game's chosen row
            const float r = screen.y * 0.004f;
            for (int k = 0; k < 8; ++k) {
              const float ang = float(k) * 0.785398f, ox = std::cos(ang) * r, oy = std::sin(ang) * r;
              auto o = [&](ImVec2 p) { return ImVec2(p.x + ox, p.y + oy); };
              dl->AddImageQuad(tex, o(at(u0, v0)), o(at(u1, v0)), o(at(u1, v1)), o(at(u0, v1)), {0, sv0}, {1, sv0},
                               {1, sv1}, {0, sv1}, col(255, 250, 235, word_a * 0.22f * lit));
            }
          }
          dl->AddImageQuad(tex, at(u0, v0), at(u1, v0), at(u1, v1), at(u0, v1), {0, sv0}, {1, sv0}, {1, sv1},
                           {0, sv1}, col(255, 255, 255, word_a));
        }
      }
      boxes.push_back({i, std::min(tl.x, bl.x) / screen.x, std::min(tl.y, tr.y) / screen.y,
                       std::max(tr.x, br.x) / screen.x, std::max(bl.y, br.y) / screen.y});
    }
    float fx0, fy0, fx1, fy1;
    GameToScreen(640.0f, kCy, aspect, fx0, fy0);
    GameToScreen(640.0f + kSpacing, kCy, aspect, fx1, fy1);
    std::lock_guard lock(g.m);
    g.boxes = std::move(boxes);
    g.step_heights = std::max(0.05f, (fx1 - fx0) * aspect * 0.55f);
  }

 private:
  struct Tex {
    std::unique_ptr<rex::ui::ImmediateTexture> tex;
    int w = 0, h = 0;
    std::vector<std::pair<int, int>> lines;
  };
  void EnsureTextures() {
    rex::ui::ImmediateDrawer* drawer = imgui_drawer()->immediate_drawer();
    if (drawer != owner_) {
      for (auto& [n, t] : words_tex_) t.tex.release();  // the old drawer's, gone with it
      words_tex_.clear();
      owner_ = drawer;
    }
    if (!drawer) return;
    std::lock_guard lock(pack.m);
    if (!pack.words_new) return;
    pack.words_new = false;
    words_tex_.clear();
    for (auto& [name, image] : pack.words) {
      Tex t;
      t.w = image.width;
      t.h = image.height;
      t.lines = pack.lines[name];
      t.tex = drawer->CreateTexture(uint32_t(image.width), uint32_t(image.height),
                                    rex::ui::ImmediateTextureFilter::kLinear, false, image.rgba.data());
      words_tex_[name] = std::move(t);
    }
  }

#if !defined(__ANDROID__)
  // The PC (NIER_TITLE_CAROUSEL=1): the mouse as the finger.
  void PcMouse(ImGuiIO& io, float aspect) {
    if (!Showing()) {
      mouse_was_ = false;
      return;
    }
    const bool down = io.MouseDown[0];
    const Touch t{1, io.MousePos.x / io.DisplaySize.x, io.MousePos.y / io.DisplaySize.y};
    if (down) Fingers(&t, 1, aspect);
    else if (mouse_was_) Fingers(nullptr, 0, aspect);
    mouse_was_ = down;
  }
  bool mouse_was_ = false;
#endif

  rex::ui::ImmediateDrawer* owner_ = nullptr;
  std::map<std::string, Tex> words_tex_;
  float fade_ = 0.0f, pos_ = 0.0f;
  int kind_ = -1;
};

}  // namespace

bool Enabled() {
#if defined(__ANDROID__)
  return true;
#else
  static const bool on = [] {
    const char* v = std::getenv("NIER_TITLE_CAROUSEL");
    return v && *v && *v != '0';
  }();
  return on;
#endif
}

void OnTitleTick() { g.tick_ms = Now(); }

int OnTitleList(int kind, int rows, int row, const char* const names[4]) {
  const uint64_t now = Now();
  // The mouse's title path (menu_mouse.h) eats the click's own A off the rows: the cards give theirs.
  auto& tm = menu_mouse::title();
  tm.ms = now;
  tm.hovered = -1;
  std::lock_guard lock(g.m);
  if (kind != g.kind || !ShowingLocked(now)) {
    g.request = -1;
    g.confirm_row = -1;
  }
  static bool logged[2] = {};
  if (!logged[kind & 1]) {
    logged[kind & 1] = true;
    REXLOG_INFO("Carrossel: lista {} com {} linhas: {} / {} / {} / {}", kind, rows, names[0] ? names[0] : "-",
                names[1] ? names[1] : "-", names[2] ? names[2] : "-", names[3] ? names[3] : "-");
  }
  g.kind = kind;
  g.rows = rows;
  g.row = row;
  for (int i = 0; i < 4; ++i) g.names[i] = names[i] ? names[i] : "";
  g.list_ms = now;
  g.tick_ms = now;
  const int want = g.request;
  if (want == row) g.request = -1;
  return want;
}

bool Showing() {
  std::lock_guard lock(g.m);
  return ShowingLocked(Now());
}

void Fingers(const Touch* touches, int count, float aspect) {
  std::lock_guard lock(g.m);
  const Touch* f = nullptr;
  for (int i = 0; i < count; ++i)
    if (g.finger && touches[i].id == g.finger_id) f = &touches[i];
  if (g.finger && !f) {  // lifted: a touch on a card unless it dragged
    g.finger = false;
    if (!g.dragged) TapLocked(g.fx, g.fy);
  }
  if (!g.finger && count > 0) {  // a new finger
    f = &touches[0];
    g.finger = true;
    g.finger_id = f->id;
    g.fx0 = g.fx = f->x;
    g.fy0 = g.fy = f->y;
    g.base_row = g.request >= 0 ? g.request : g.row;
    g.dragged = false;
    return;
  }
  if (!f) return;
  g.fx = f->x;
  g.fy = f->y;
  const float dx = (f->x - g.fx0) * aspect;  // screen heights
  if (!g.dragged && std::fabs(dx) > 0.035f) g.dragged = true;
  if (g.dragged) {
    // The finger to the left shows the cards on the right, as a phone's pages.
    const int steps = int(-dx / g.step_heights);
    const int want = std::clamp(g.base_row + steps, 0, std::max(0, g.rows - 1));
    if (want != (g.request >= 0 ? g.request : g.row)) RequestLocked(want);
  }
}

void Tap(float x, float y, float aspect) {
  (void)aspect;
  std::lock_guard lock(g.m);
  TapLocked(x, y);
}

bool WantA() {
  const uint64_t now = Now();
  std::lock_guard lock(g.m);
  if (g.confirm_row >= 0) {
    if (now - g.confirm_ms > 2000 || !ShowingLocked(now)) {
      g.confirm_row = -1;
    } else if (g.row == g.confirm_row && g.request < 0 &&
               now - menu_mouse::title().moved_ms.load() >= 320) {  // the game takes no A as its highlight starts
      g.a_until = now + 150;
      REXLOG_INFO("Carrossel: A na linha {}", g.confirm_row);
      g.confirm_row = -1;
    }
  }
  return now < g.a_until;
}

std::unique_ptr<rex::ui::ImGuiDialog> MakeDialog(rex::ui::ImGuiDrawer* drawer) {
  return std::make_unique<CarouselDialog>(drawer);
}

}  // namespace nier::title_carousel
