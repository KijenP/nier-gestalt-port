// nier - hooks on the game's own file loader and language getter, used to hand
// the game text from the TEXT folder and to switch language while it runs
// (see game_text.h).
//
// The game builds names such as "text\info_text\infowindow.en.inw" and asks its
// CRI file layer for the size (sub_8211F748) and then the content
// (sub_821204F8), always synchronously for text. Text files are not probed as
// loose files first, so the name is the only place to step in. Its language is
// one number in the settings object (1 ja, 2 en, 3 fr, 4 de), read through
// sub_82337120, which also picks the ".en." in those names and the column of
// NIER_TEXT.TXD.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>

#include "game_text.h"
#include "keyboard_icons_sheet.h"
#include "start_prompt_sheets.h"
#include "pc_options.h"
#include "pc_text.h"

namespace renut::gpu {
void ForgetTextureMemory(uint32_t physical, uint32_t size);  // render/textures.cpp
}

namespace {
// NieR (2026-11-14): a host write into a texture the game may already have drawn (the title's prompt, the icon
// sheet): the renderer drops its copies over that memory and uploads the new bytes at the next frame. Before, the
// phone kept half the old image (its check samples a few spans; an upload racing the write).
void MarkGuestWritten(uint8_t* base, uint32_t va, uint32_t size) {
  (void)base;
  if (!size) return;
  const uint32_t physical = (va & 0x1FFFFFFFu) + (va >= 0xE0000000u ? 0x1000u : 0u);
  renut::gpu::ForgetTextureMemory(physical, size);
}
}  // namespace

#include <cctype>
#include <string_view>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>  // VirtualQuery (the Quit dialog question copies)
#endif

extern "C" void __imp__sub_8211F748(PPCContext& ctx, uint8_t* base);  // file size by name
extern "C" void __imp__sub_821204F8(PPCContext& ctx, uint8_t* base);  // load file by name
extern "C" void __imp__sub_82337120(PPCContext& ctx, uint8_t* base);  // game language getter
extern "C" void __imp__sub_823CC4F0(PPCContext& ctx, uint8_t* base);  // load + parse a .txd
extern "C" void __imp__sub_823CB810(PPCContext& ctx, uint8_t* base);  // after the .txd: index it
extern "C" void __imp__sub_8211B8B8(PPCContext& ctx, uint8_t* base);  // free(heap, ptr)
extern "C" void __imp__sub_825EEEA8(PPCContext& ctx, uint8_t* base);  // pad read, once per frame
extern "C" void __imp__sub_823C4548(PPCContext& ctx, uint8_t* base);  // load the info-window text

void TextReloadTick(PPCContext& ctx, uint8_t* base);
void NativeMenuTick(PPCContext& ctx, uint8_t* base);  // video_options_menu.cpp

namespace {

constexpr uint32_t kSettingsObject = 0x84C01E80;  // lis -31552 / addi 7808
constexpr uint32_t kLanguageField = 0x12140;      // sub_82337120 reads obj + this

// The text-table load as the game first did it, to be repeated on a language
// change: sub_823CC4F0(table, "nier_text", mode), then sub_823CB810(index).
struct TxdCall {
  std::atomic<bool> have{false};
  uint32_t table = 0, name = 0, mode = 0, index = 0;
};
TxdCall g_txd;

// The info-window text ("Checking system data."...) is loaded once at startup by
// sub_823C4548(window, "InfoWindow", language); repeated on a language change.
struct InfoCall {
  std::atomic<bool> have{false};
  uint32_t window = 0, name = 0;
};
InfoCall g_info;

// The Quit Game dialog's question and where the game keeps it, for SetQuitDialogText.
struct Spot {
  uint32_t addr = 0;
  std::vector<uint8_t> game, ours;
};
struct InwText {
  std::mutex mutex;
  std::string question;  // in the language loaded
  std::vector<Spot> spots;
  bool swapped = false;
};
InwText g_inw;
void RestoreQuitLocked(uint8_t* base);

// During a reload the old text buffer is not freed: menus already built may
// still point into it (they show the old language until they are rebuilt).
thread_local bool tl_keep_old = false;

// TEMP (TEXTDBG): NIER_TEXTDBG=1 logs every distinct name that reaches the loader.
void DebugPath(const char* what, const std::string& path, const PPCContext& ctx) {
  static const bool dbg = std::getenv("NIER_TEXTDBG") != nullptr;
  if (!dbg) return;
  static std::mutex m;
  static std::set<std::string> seen;
  std::lock_guard lock(m);
  if (!seen.insert(std::string(what) + path).second) return;
  REXLOG_INFO("TEXTDBG {} '{}' r5={:08X} r6={:08X} r7={:08X} r8={:08X}", what, path,
              ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
}

std::string GuestPath(uint8_t* base, uint32_t addr) {
  if (!addr) return {};
  const char* p = reinterpret_cast<const char*>(base + addr);
  size_t n = 0;
  while (n < 260 && p[n]) ++n;
  return std::string(p, n);
}

}  // namespace

REX_HOOK_RAW(sub_8211F748) {
  std::string path = GuestPath(base, ctx.r4.u32);
  DebugPath("size", path, ctx);
  if (nier::text::IsPixelPath(path)) {
    __imp__sub_8211F748(ctx, base);
    return;
  }
  if (nier::text::Blob blob = nier::text::Resolve(path)) {
    ctx.r3.s64 = int64_t(blob->size());
    return;
  }
  __imp__sub_8211F748(ctx, base);
}

// The buttons' icons (2026-11-13, native in place of the PS3-Botoes mod): COMMON.2DP (and its _DE / _FR) carries
// both the Xbox 360's sheet (bt_icon_360_bt_icon_D5, data at 0x60000) and the PS3's (bt_icon_ps3_bt_icon_D5, at
// 0x68000), 0x8000 bytes each, same format and size (read from the packs' .2DV); the game draws the 360's. With
// nier_button_icons = ps3 the PS3 sheet is copied over it as the pack loads (a restart applies it: COMMON stays
// loaded). More styles later: another sheet's bytes the same way.
REXCVAR_DEFINE_STRING(nier_button_icons, "x360", "Nier", "Button icons: x360 (the game's) or ps3");
namespace {
constexpr uint32_t kIcon360 = 0x60000, kIconPs3 = 0x68000, kIconSize = 0x8000;
// The loaded sheet (guest address of the 360 sheet's bytes, 0: none yet) and the sheets it can show: the game's
// two, copied as the pack loaded, and the keyboard's (keyboard_icons_sheet.h). The game's thread only.
struct IconSheets {
  uint32_t at = 0;
  std::vector<uint8_t> x360, ps3;
  int shown = -1;  // 0 the 360's, 1 the PS3's, 2 the keyboard's
} g_icons;

// Where in its buffer the game puts a 2D pack: 4 KB in on the PC, at the start on the phone (measured 2026-11-14: the
// art hook found +0 there; the title's prompt and the icon sheet, written 4 KB off, showed half old, half shifted).
// The art hook's measure replaces it when it finds one.
#if defined(__ANDROID__)
uint32_t g_pack_shift = 0;
#else
uint32_t g_pack_shift = 0x1000;
#endif

void ApplyButtonIcons(uint8_t* base, const std::string& path, uint32_t buffer, int64_t loaded) {
  constexpr int64_t kCommonSize = 3162112;
  if (!buffer || loaded != kCommonSize) return;
  std::string lower = path;
  for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
  const size_t slash = lower.find_last_of("\\/");
  const std::string name = slash == std::string::npos ? lower : lower.substr(slash + 1);
  if (name != "common.2dp" && name != "common_de.2dp" && name != "common_fr.2dp") return;
  const uint32_t kShift = g_pack_shift;
  const uint8_t* sheets = base + buffer + kShift;
  g_icons.x360.assign(sheets + kIcon360, sheets + kIcon360 + kIconSize);
  g_icons.ps3.assign(sheets + kIconPs3, sheets + kIconPs3 + kIconSize);
  g_icons.at = buffer + kShift + kIcon360;
  g_icons.shown = 0;
  REXLOG_INFO("Icones: folha em {:08X} ({})", g_icons.at, path);
}
}  // namespace

// The title's "Press START" (2D\TITLE.2DP: title_start + title_startselection) in the words of the device in use: the
// keyboard's on the PC, touch's on the phone (start_prompt_sheets.h, PT or EN by the language). Its loaded address
// and the game's own bytes (the translation's, if one wrote them) of the two spans.
struct StartPrompt {
  uint32_t at = 0;  // guest address of the pack's bytes
  std::vector<uint8_t> game[2];
  int shown = 0;  // 0 the game's, 1 the device's
} g_start;

void NoteTitlePack(const std::string& path, uint32_t buffer, int64_t loaded, uint8_t* base) {
  std::string lower = path;
  for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
  const size_t slash = lower.find_last_of("\\/");
  const std::string name = slash == std::string::npos ? lower : lower.substr(slash + 1);
  if (name != "title.2dp" || loaded != 409600 || !buffer) return;
  g_start.at = buffer + g_pack_shift;
  for (int i = 0; i < 2; ++i) {
    const uint8_t* from = base + g_start.at + nier::icons::kStartSpans[i][0];
    g_start.game[i].assign(from, from + nier::icons::kStartSpans[i][1]);
  }
  g_start.shown = 0;
}

// The phone's title cards (title_carousel.cpp) read the words' images from the loaded pack.
namespace nier::title_carousel {
uint32_t TitlePackAddress() { return g_start.at; }
}  // namespace nier::title_carousel

void StartPromptTick(uint8_t* base) {
  if (!g_start.at || g_start.game[0].empty()) return;
#if defined(__ANDROID__)
  const bool device = true;  // the phone: always touch's words (a pad shows its own START on screen too)
#else
  const bool device = nier::pc_options_state().input_kind.load() == 1;
#endif
  int want = device ? 1 : 0;
  const bool pt = nier::text::CurrentName() == "Portugues";
  const uint8_t* spans[2] = {pt ? nier::icons::kStartPt0 : nier::icons::kStartEn0,
                             pt ? nier::icons::kStartPt1 : nier::icons::kStartEn1};
  if (want == g_start.shown) {
    // NieR (2026-11-14): on the phone the game still wrote into the pack after its load hook (half the prompt was
    // the game's again, PC fine): what we wrote is checked every frame and written again when it changed.
    if (!want) return;
    bool same = true;
    for (int i = 0; i < 2 && same; ++i)
      same = std::memcmp(base + g_start.at + nier::icons::kStartSpans[i][0], spans[i],
                         nier::icons::kStartSpans[i][1]) == 0;
    if (same) return;
    static int logged = 0;
    if (logged++ < 8) REXLOG_INFO("Titulo: o jogo reescreveu o \"Press START\"; escrito de novo");
  }
  for (int i = 0; i < 2; ++i) {
    std::memcpy(base + g_start.at + nier::icons::kStartSpans[i][0], want ? spans[i] : g_start.game[i].data(),
                nier::icons::kStartSpans[i][1]);
  }
  MarkGuestWritten(base, g_start.at, 409600);  // the whole pack: the textures' data starts before the spans
  g_start.shown = want;
  REXLOG_INFO("Titulo: \"Press START\" {}", want ? "do dispositivo" : "do jogo");
}

// Once a frame: the sheet the game draws follows the device last used (the keyboard's keys with the keyboard and
// mouse, as the remake shows them) and the style chosen for controllers (nier_button_icons). The renderer hashes the
// texture when it draws it, so a change shows at once.
void ButtonIconsTick(uint8_t* base) {
  if (!g_icons.at || g_icons.x360.size() != kIconSize) return;
  // The device's own sheet (2): the keyboard's on the PC, touch's on the phone; else the controller style chosen.
#if defined(__ANDROID__)
  const bool device = nier::pc_options_state().input_kind.load() == 2;
  const uint8_t* device_sheet = nier::icons::kTouchSheet;
#else
  const bool device = nier::pc_options_state().input_kind.load() == 1;
  const uint8_t* device_sheet = nier::icons::kKeyboardSheet;
#endif
  int want = device ? 2 : REXCVAR_GET(nier_button_icons) == "ps3" ? 1 : 0;
  const uint8_t* from = want == 2 ? device_sheet : want == 1 ? g_icons.ps3.data() : g_icons.x360.data();
  if (want == g_icons.shown) {
    // As the title's prompt (StartPromptTick): written again when the game wrote over it.
    if (!want || std::memcmp(base + g_icons.at, from, kIconSize) == 0) return;
    static int logged = 0;
    if (logged++ < 8) REXLOG_INFO("Icones: o jogo reescreveu a folha; escrita de novo");
  }
  std::memcpy(base + g_icons.at, from, kIconSize);
  MarkGuestWritten(base, g_icons.at, kIconSize);
  g_icons.shown = want;
  REXLOG_INFO("Icones: {}", want == 2 ? "do dispositivo (teclado / toque)" : want == 1 ? "PS3" : "Xbox 360");
}

REX_HOOK_RAW(sub_821204F8) {
  std::string path = GuestPath(base, ctx.r4.u32);
  DebugPath("load", path, ctx);
  {  // The title's or a loading screen's art: the menu flag is trusted again until gameplay (pc_options.h).
    std::string lower = path;
    for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("2d") != std::string::npos &&
        (lower.find("title") != std::string::npos || lower.find("loading") != std::string::npos)) {
      nier::pc_options_state().gameplay_seen = false;
    }
    // Yes in the Quit Game dialog that our Quit to Desktop opened: the game heads for the title.
    auto& s = nier::pc_options_state();
    if (lower.find("title") != std::string::npos && s.quit_desktop_armed.exchange(false)) {
      REXLOG_INFO("Menu: Fechar o jogo (confirmado)");
      if (!s.config_path.empty()) nier::SavePcSettings(s.config_path);
      if (s.quit_game) s.quit_game();
    }
  }
  if (nier::text::IsPixelPath(path)) {  // art: the game loads it, we write over it
    const uint32_t buffer = ctx.r5.u32;
    __imp__sub_821204F8(ctx, base);
    const int64_t loaded = ctx.r3.s64;
    if (nier::text::Blob blob = nier::text::Resolve(path); blob && buffer) {
      // The game puts the file at buffer + a small offset (4 KB for the packs seen).
      // The offset is the one where the loaded bytes best match the replacement,
      // which differs from the original in few places.
      const size_t size = blob->size();
      int best_shift = 0;
      size_t best_diff = size_t(-1), second_diff = size_t(-1);
      for (int shift : {0, 0x1000, 0x2000, 0x800, 0x400, 0x200, 0x100, 0x80, 0x40, 0x20, 0x10, -0x1000}) {
        if (buffer + shift < 0x10000) continue;
        size_t diff = 0;
        for (size_t i = 0; i + 4 <= size; i += 4) {
          diff += std::memcmp(base + buffer + shift + i, blob->data() + i, 4) != 0;
        }
        if (diff < best_diff) {
          second_diff = best_diff;
          best_diff = diff;
          best_shift = shift;
        } else if (diff < second_diff) {
          second_diff = diff;
        }
      }
      // The right offset is clearly better than every other one.
      if (ctx.r3.s64 == int64_t(size) && best_diff < second_diff / 2) {
        std::memcpy(base + buffer + best_shift, blob->data(), size);
        MarkGuestWritten(base, buffer + best_shift, uint32_t(size));
        if (best_shift >= 0) g_pack_shift = uint32_t(best_shift);
        REXLOG_INFO("Arte: {} escrito sobre o carregado (deslocamento {:+X}, {} palavras diferentes)", path,
                    best_shift, best_diff);      } else {
        REXLOG_WARN("Arte: {} não bate com o que o jogo carregou (tamanho {} / {}, melhor diferença {}); ignorado",
                    path, size, ctx.r3.s64, best_diff);
      }
    }
    // After a translation's / mod's bytes: the sheets swapped live are taken from what the game will draw.
    ApplyButtonIcons(base, path, buffer, loaded);
    NoteTitlePack(path, buffer, loaded, base);
    return;
  }
  const uint32_t buffer = ctx.r5.u32;
  if (nier::text::Blob blob = nier::text::Resolve(path); blob && ctx.r5.u32) {
    std::memcpy(base + ctx.r5.u32, blob->data(), blob->size());
    ctx.r3.s64 = int64_t(blob->size());
  } else {
    __imp__sub_821204F8(ctx, base);
  }
  std::string lower_path = path;
  for (char& c : lower_path) c = char(std::tolower(static_cast<unsigned char>(c)));
  if (lower_path.find("infowindow") != std::string::npos && ctx.r3.s64 > 0) {
    // The Quit Game dialog's question, read from the file as loaded (the game then parses it into
    // its own copies): record INFO_MAIN_MENU_SYS_ESCAPE_GAME_MESS_SID, "<loss>\n\n<question>\n\n<Yes/No>".
    std::lock_guard lock(g_inw.mutex);
    RestoreQuitLocked(base);
    g_inw.question.clear();
    g_inw.spots.clear();
    const uint8_t* text = base + buffer;
    const uint8_t* end = text + ctx.r3.s64;
    static const char kName[] = "INFO_MAIN_MENU_SYS_ESCAPE_GAME_MESS_SID";
    const uint8_t* p = std::search(text, end, kName, kName + sizeof(kName) - 1);
    for (int sep = 0; sep < 9 && p < end; ++p) {  // the message is the 10th field after the name
      if (*p == '\t') ++sep;
    }
    const uint8_t* cell_end = p;
    while (cell_end < end && *cell_end != '\t' && *cell_end != '\r') ++cell_end;
    const std::string cell(p, cell_end);
    const size_t q0 = cell.find("\n\n");
    const size_t q1 = q0 == std::string::npos ? q0 : cell.find('\n', q0 + 2);
    if (q1 != std::string::npos) g_inw.question = cell.substr(q0 + 2, q1 - (q0 + 2));
    if (g_inw.question.empty()) REXLOG_WARN("Menu: pergunta do Sair do Jogo nao achada em {}", path);
  }
}

namespace {

// Puts the game's question back wherever ours was written. Caller holds the lock.
void RestoreQuitLocked(uint8_t* base) {
  int restored = 0;
  for (const auto& spot : g_inw.spots) {
    uint8_t* at = base + spot.addr;
    if (std::memcmp(at, spot.ours.data(), spot.ours.size()) == 0) {
      std::memcpy(at, spot.game.data(), spot.game.size());
      ++restored;
    }
  }
  (void)restored;
  g_inw.swapped = false;
}

}  // namespace

// While Quit to Desktop asks with the game's Quit Game dialog, its question is ours: every copy of
// the game's question in guest memory (the parsed text the dialog shows) is overwritten, and put
// back after. The copies are found once (a scan of the guest's memory) and checked before each use.
void SetQuitDialogText(uint8_t* base, bool ours) {
  std::lock_guard lock(g_inw.mutex);
  if (ours == g_inw.swapped || g_inw.question.empty()) return;
  if (!ours) {
    RestoreQuitLocked(base);
    return;
  }
  const std::string& q = g_inw.question;
  std::string mine = nier::pc_text::T("Quit the game and return to the desktop?");
  // Each copy: the question up to the end of its string (the game's NUL), rewritten as ours plus
  // what followed the question there, padded with NULs.
  auto make = [&](uint32_t addr, Spot& spot) {
    const char* at = reinterpret_cast<const char*>(base + addr);
    size_t n = q.size();
    while (n < q.size() + 512 && at[n]) ++n;
    spot.addr = addr;
    spot.game.assign(at, at + n);
    std::string text = mine.size() <= q.size() ? mine : mine.substr(0, q.size());
    while (!text.empty() && text.size() < mine.size() && (uint8_t(mine[text.size()]) & 0xC0) == 0x80) text.pop_back();
    text += std::string(at + q.size(), at + n);
    text.resize(n, '\0');
    spot.ours.assign(text.begin(), text.end());
  };
  bool valid = !g_inw.spots.empty();
  for (const auto& spot : g_inw.spots) {
    valid = valid && std::memcmp(base + spot.addr, spot.game.data(), spot.game.size()) == 0;
  }
  if (!valid) {
    g_inw.spots.clear();
#if defined(_WIN32)
    for (uint64_t a = 0x40000000ull; a < 0x100000000ull;) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (!VirtualQuery(base + a, &mbi, sizeof(mbi))) break;
      const uint64_t region_end = uint64_t(reinterpret_cast<uint8_t*>(mbi.BaseAddress) - base) + mbi.RegionSize;
      if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && (mbi.Protect & PAGE_READWRITE)) {
        const std::string_view view(reinterpret_cast<const char*>(base + a), size_t(region_end - a));
        for (size_t at = view.find(q); at != std::string_view::npos && g_inw.spots.size() < 16; at = view.find(q, at + 1)) {
          const char next = at + q.size() < view.size() ? view[at + q.size()] : 0;
          if (next != '\n' && next != 0) continue;
          Spot spot;
          make(uint32_t(a + at), spot);
          g_inw.spots.push_back(std::move(spot));
        }
      }
      a = region_end;
    }
#endif
    std::string where;
    for (const auto& spot : g_inw.spots) where += fmt::format(" {:08X}", spot.addr);
    REXLOG_INFO("Menu: pergunta do Sair do Jogo em {} lugar(es):{}", g_inw.spots.size(), where);
  }
  for (const auto& spot : g_inw.spots) std::memcpy(base + spot.addr, spot.ours.data(), spot.ours.size());
  g_inw.swapped = true;
}
REX_HOOK_RAW(sub_82337120) {
  const uint32_t object = ctx.r3.u32;
  __imp__sub_82337120(ctx, base);
  static const bool dbg = std::getenv("NIER_TEXTDBG") != nullptr;
  if (dbg) {
    static std::atomic<uint32_t> last{0};
    if (last.exchange(object) != object) {
      REXLOG_INFO("TEXTDBG language getter object {:08X} -> {}", object, ctx.r3.u32);
    }
  }
  if (object != kSettingsObject) return;
  if (int code = nier::text::GameLanguageOverride()) {
    ctx.r3.u64 = uint32_t(code);
    // Code that reads the field directly sees the same number.
    uint32_t be = __builtin_bswap32(uint32_t(code));
    std::memcpy(base + object + kLanguageField, &be, sizeof(be));
  }
}

REX_HOOK_RAW(sub_823CC4F0) {
  if (!g_txd.have) {
    g_txd.table = ctx.r3.u32;
    g_txd.name = ctx.r4.u32;
    g_txd.mode = ctx.r5.u32;
  }
  __imp__sub_823CC4F0(ctx, base);
}

REX_HOOK_RAW(sub_823CB810) {
  if (!g_txd.have && g_txd.table) {
    g_txd.index = ctx.r3.u32;
    g_txd.have = true;
  }
  __imp__sub_823CB810(ctx, base);
}

REX_HOOK_RAW(sub_823C4548) {
  if (!g_info.have) {
    g_info.window = ctx.r3.u32;
    g_info.name = ctx.r4.u32;
    g_info.have = true;
  }
  __imp__sub_823C4548(ctx, base);
}

REX_HOOK_RAW(sub_8211B8B8) {
  if (tl_keep_old) return;
  __imp__sub_8211B8B8(ctx, base);
}

// Runs on the game thread once per logic frame, on every screen (the game's
// pad read, sub_825EEEA8 -> XamInputGetState). A language change takes effect
// here: the text table is loaded again the way the game first did it.
void TextReloadTick(PPCContext& ctx, uint8_t* base) {
  {  // TEMP (dev): NIER_TEXT_SWITCH=<seconds>|<language> picks a language by itself.
    static const char* env = std::getenv("NIER_TEXT_SWITCH");
    static const auto start = std::chrono::steady_clock::now();
    static bool done = false;
    if (env && !done &&
        std::chrono::steady_clock::now() - start > std::chrono::seconds(std::atoi(env))) {
      done = true;
      if (const char* bar = std::strchr(env, '|')) nier::text::Select(bar + 1);
    }
  }
  if (!g_txd.have || !nier::text::TakeReloadRequest()) return;
  PPCContext saved = ctx;
  tl_keep_old = true;
  ctx.r3.u64 = g_txd.table;
  ctx.r4.u64 = g_txd.name;
  ctx.r5.u64 = g_txd.mode;
  __imp__sub_823CC4F0(ctx, base);
  ctx.r3.u64 = g_txd.index;
  __imp__sub_823CB810(ctx, base);
  if (g_info.have) {
    ctx.r3.u64 = g_info.window;
    ctx.r4.u64 = g_info.name;
    ctx.r5.u64 = uint32_t(nier::text::GameLanguageOverride() ? nier::text::GameLanguageOverride() : 2);
    __imp__sub_823C4548(ctx, base);
  }
  tl_keep_old = false;
  ctx = saved;
  REXLOG_INFO("Texto recarregado ({})", nier::text::CurrentName());
}

REX_HOOK_RAW(sub_825EEEA8) {
  ButtonIconsTick(base);
  StartPromptTick(base);
  TextReloadTick(ctx, base);
  NativeMenuTick(ctx, base);
  __imp__sub_825EEEA8(ctx, base);
}
