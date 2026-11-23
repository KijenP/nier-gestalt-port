// nier - player model mods: which of the game's own Nier models the player uses.
//
// sub_823372C8 hands the player's model name ("nier030" ...) to the code that loads the
// player (its only caller, 0x82190EBC). The name comes from a table of six 16-byte
// entries at 0x8203A008, picked by the index at 0x84CC6BA0 + 40:
//   0 nier010  1 nier011  2 nier020   (the young Nier of the disc: the brother)
//   3 nier030  4 nier031  5 nier040   (the father Gestalt uses)
//
// A mod (MODS\<mod>\player_model.txt) changes it, one line per model:
//   nier030=nier010
//   nier040=nier020
// or a single name ("nier010") for "every model the game asks for". '#' starts a comment.
// Not an in-game option: a downloadable mod. player_skin (cvar, command line only) does
// the same for tests: default | brother | father.

#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game_text.h"

REXCVAR_DEFINE_STRING(player_skin, "default", "Nier",
                      "Tests only: player model default | brother | father (mods use player_model.txt)");

extern "C" void __imp__sub_823372C8(PPCContext& ctx, uint8_t* base);  // player model name

namespace {

constexpr uint32_t kModelTable = 0x8203A008;  // 6 entries of 16 bytes
constexpr int kEntries = 6;
const char* const kNames[kEntries] = {"nier010", "nier011", "nier020", "nier030", "nier031", "nier040"};

int IndexOf(std::string name) {
  for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
  for (int i = 0; i < kEntries; ++i) {
    if (name == kNames[i]) return i;
  }
  return -1;
}

std::string Trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// map[i] = the model the player gets when the game asks for model i. Read once from the mods.
struct ModMap {
  int to[kEntries] = {0, 1, 2, 3, 4, 5};
  bool any = false;
  std::string from_mod;
};

const ModMap& Mods() {
  static ModMap map;
  static std::once_flag once;
  std::call_once(once, [] {
    for (const auto& dir : nier::text::ModDirs()) {
      std::ifstream f(dir / "player_model.txt");
      if (!f) continue;
      std::string line;
      while (std::getline(f, line)) {
        if (auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        line = Trim(line);
        if (line.empty()) continue;
        if (auto eq = line.find('='); eq != std::string::npos) {
          const int a = IndexOf(Trim(line.substr(0, eq))), b = IndexOf(Trim(line.substr(eq + 1)));
          if (a >= 0 && b >= 0) { map.to[a] = b; map.any = true; }
        } else if (const int b = IndexOf(line); b >= 0) {
          for (int& t : map.to) t = b;
          map.any = true;
        }
      }
      if (map.any) {
        map.from_mod = dir.filename().string();
        REXLOG_INFO("Skin: modelo do jogador pelo mod {}", map.from_mod);
        break;  // first mod that has one wins
      }
    }
  });
  return map;
}

int Wanted(int index) {
  const std::string skin = REXCVAR_GET(player_skin);
  if (skin == "brother") return index % 3;
  if (skin == "father") return 3 + index % 3;
  return Mods().to[index];
}

int g_last_game_index = -1;

}  // namespace

REX_HOOK_RAW(sub_823372C8) {
  __imp__sub_823372C8(ctx, base);
  const uint32_t name = ctx.r3.u32;
  if (name < kModelTable || name >= kModelTable + kEntries * 16 || (name - kModelTable) % 16) return;
  const int index = int((name - kModelTable) / 16);
  const int wanted = Wanted(index);
  if (index != g_last_game_index && wanted != index) {
    REXLOG_INFO("Skin: o jogo pediu {} -> {}", kNames[index], kNames[wanted]);
  }
  g_last_game_index = index;
  ctx.r3.u64 = kModelTable + uint32_t(wanted) * 16;
}
