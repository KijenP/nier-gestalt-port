// Nier Plus - Poderes: the moveset mod's test panel (mods/sdk/nier_mod.h, API 2). Not part of the game: a library the
// player puts in MODS/Poderes/ (powers.dll on the PC, libpowers.so on Android). F7 (or its icon on Android) opens it.
//
// The game's side (src/bullet_trace.cpp, the nier_moveset_* options): a button holding Dark Lance fires a shot pattern
// instead, aimed at the nearest enemy; its hit is a real one (Nier's level x the damage), and the shot ends on its
// first hit.
//
// The monsters' magic (2026-11-19, from the game's own files): the exe's INI gives each model a shot library, and only
// two exist: book.bin (the books, Kaine, the Shadowlord, Halua, Popola and Devola, Geppetto) and
// wendy_shot_patternA.bin (Hook, Wendy, the ball Shades). Each one's AI script names the patterns it fires: those are
// the character's entries here. Emil's magic is not a shot of this system (no script of his fires any).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../sdk/nier_mod.h"

namespace {

const NierModApi* g_api = nullptr;

constexpr const char* kBook = "book.bin";
constexpr const char* kWendy = "wendy_shot_patternA.bin";

struct Power {
  std::string label;
  const char* file;
  const char* pattern;
};
std::vector<Power> g_powers;
std::vector<const char*> g_labels;

// shot_hand and carrier crash from Nier (they need their usual owner's parts): left out everywhere.
const char* const kBookAll[] = {
    "1way_1", "1way_2", "1way_3", "1way_4", "3d_mohican_1", "3d_mohican_2", "3d_mohican_3", "3d_mohican_4",
    "3d_shot1_1", "3d_shot1_2", "3d_shot1_3", "3d_shot1_4", "3d_shot2_1", "3d_shot2_2", "3d_shot2_3", "3d_shot2_4",
    "3d_shot3_1", "3d_shot3_2", "3d_shot3_3", "3d_shot3_4", "3d_shot4_1", "3d_shot4_2", "3d_shot4_3", "3d_shot4_4",
    "3d_spiraldown_1", "3d_spiraldown_2", "3d_spiraldown_3", "3d_spiraldown_4", "3way_1", "3way_2", "3way_3", "3way_4",
    "fan_1", "fan_2", "fan_3", "fan_4", "flare_1", "flare_2", "flare_3", "flare_4", "Homing", "Laser", "round_1",
    "round_2", "round_3", "round_4", "scissors_1", "scissors_2", "scissors_3", "scissors_4", "screw_1", "screw_1_sub",
    "screw_2", "screw_3", "screw_4", "shot_1", "spiral_1", "spiral_2", "spiral_3", "spiral_4",
    "spredwide_1", "spredwide_2", "spredwide_3", "spredwide_4", "spredwide_4_sub", "widehoming_1", "widehoming_2",
    "widehoming_3", "widehoming_4", "wide_1", "wide_2", "wide_3", "wide_4", "butterfly", "command_bullet",
    "coulombforce", "delay_senior", "finger", "fingerclose", "homing", "homingsnake", "homing_breakup",
    "homing_point_change_angle", "homing_wavepattern", "laser", "lorenz", "parabola", "parabolapoint", "poll_bullet",
    "round_straight", "search_homing", "shot_hand_homing", "shot_round_poll", "stop", "stopworld", "stop_expand",
    "stop_expand_senior", "straight", "straightwave", "straight_and_hit_collision", "straight_and_homing_point",
    "straight_last_shot", "straight_no_se", "tornadoparts"};
// wendy_shot_patternA.bin's own (the rest are the same as book.bin's).
const char* const kWendyAll[] = {
    "ButterflyShot", "RandomConeShot", "RandomConeShotStraight", "RandomShot", "RandomShotStraight",
    "RandomShotStraightWithX", "RandomShotStraightWithXSide", "Shot", "ShotOneWhip", "ShotTornado", "ShotWhip",
    "ShotWhipX", "ShotWhipXLoop", "ShotWide", "ShotWide10", "ShotWideOne", "ShotWideOneBreakable", "ShotWideOneLoop",
    "WayNormal", "WayNormalX", "WayShot"};

void Add(const char* who, const char* file, std::initializer_list<const char*> patterns) {
  for (const char* p : patterns) g_powers.push_back({std::string(who) + ": " + p, file, p});
}

void BuildList() {
  Add("Kaine", kBook, {"homing", "parabolapoint", "stop_expand", "straight_last_shot", "command_bullet"});
  Add("Rei das Sombras", kBook,
      {"homing_breakup", "homing_wavepattern", "straight_and_homing_point", "straight_and_hit_collision", "stop_expand",
       "command_bullet"});
  Add("Grimorio Noir/Rubrum", kBook,
      {"fan_4", "spiral_4", "3d_spiraldown_4", "3d_shot4_4", "flare_4", "scissors_4", "screw_4", "spredwide_4",
       "widehoming_4", "round_4", "wide_4", "3way_4", "1way_4", "3d_mohican_4", "Laser", "Homing"});
  Add("Halua", kBook, {"1way_1", "3d_mohican_1", "flare_4", "spiral_4"});
  Add("Popola/Devola", kBook, {"command_bullet", "parabolapoint"});
  Add("Geppetto", kBook, {"Laser", "search_homing"});
  Add("Hook", kWendy, {"RandomConeShot", "RandomConeShotStraight"});
  Add("Wendy", kWendy,
      {"ShotWhip", "ShotWhipX", "ShotWhipXLoop", "ShotOneWhip", "ShotWideOne", "ShotWideOneLoop", "RandomShot",
       "RandomShotStraightWithX", "ButterflyShot", "ShotTornado"});
  Add("Shade bola", kWendy, {"straight"});
  for (const char* p : kBookAll) g_powers.push_back({std::string("Livro: ") + p, kBook, p});
  for (const char* p : kWendyAll) g_powers.push_back({std::string("Wendy (todos): ") + p, kWendy, p});
  for (const Power& p : g_powers) g_labels.push_back(p.label.c_str());
}

int32_t g_on = 1;
int32_t g_pattern = 0;
int32_t g_damage = 10;
int32_t g_replace = 1;
int32_t g_anim = 0;  // kAnims
// Nier's action played with the power (the game's nier_moveset_action): with the Kaine body mod, Dark Lance's (222)
// shows Kaine's magic (the swap puts her at_magic^c03 in at_magic^spear).
const char* const kAnimNames[] = {"Nenhuma", "Magia (Dark Lance, 222)", "Dark Blast (221)"};
const int32_t kAnimActions[] = {-1, 222, 221};

const char* Text(const char* pt, const char* en, const char* es) {
  if (!std::strcmp(g_api->language, "pt")) return pt;
  if (!std::strcmp(g_api->language, "es")) return es;
  return en;
}

// The panel's values to the game's options, when they change (checked at each read of the pad: the panel has no
// change event of its own).
void Apply() {
  static std::string last_pattern = "?", last_file, last_damage, last_replace, last_anim;
  const bool valid = g_on && g_pattern >= 0 && size_t(g_pattern) < g_powers.size();
  const std::string pattern = valid ? g_powers[size_t(g_pattern)].pattern : "";
  const std::string file = valid ? g_powers[size_t(g_pattern)].file : kBook;
  const std::string damage = std::to_string(g_damage), replace = g_replace ? "true" : "false";
  if (file != last_file) g_api->set_option("nier_moveset_file", file.c_str()), last_file = file;
  if (pattern != last_pattern) {
    g_api->set_option("nier_moveset_test", pattern.c_str());
    last_pattern = pattern;
    char line[160];
    std::snprintf(line, sizeof line, "power: %s", valid ? g_powers[size_t(g_pattern)].label.c_str() : "(off)");
    g_api->log(line);
  }
  if (damage != last_damage) g_api->set_option("nier_moveset_damage", damage.c_str()), last_damage = damage;
  if (replace != last_replace) g_api->set_option("nier_moveset_replace", replace.c_str()), last_replace = replace;
  const std::string anim = std::to_string(g_anim >= 0 && g_anim < 3 ? kAnimActions[g_anim] : -1);
  if (anim != last_anim) g_api->set_option("nier_moveset_action", anim.c_str()), last_anim = anim;
}

void OnPad(void*, NierModPad*) { Apply(); }

void Next(void*) { g_pattern = size_t(g_pattern) + 1 >= g_powers.size() ? 0 : g_pattern + 1; }

}  // namespace

extern "C" NIER_MOD_EXPORT int32_t nier_mod_init(const NierModApi* api) {
  if (api->version < 2 || !api->add_choice || !api->set_option) return 1;
  g_api = api;
  BuildList();
  NierModPanel* panel = api->create_panel(Text("Poderes", "Powers", "Poderes"), "F7", "icon.png");
  api->add_text(panel, Text("O botao do Dark Lance dispara o poder escolhido, mirando no inimigo mais perto. "
                            "Um poder da Wendy/Hook carrega o arquivo dele no 1o uso (o 2o sai).",
                            "The Dark Lance button fires the chosen power, aimed at the nearest enemy. A Wendy/Hook "
                            "power loads its file on first use (the second one fires).",
                            "El boton de Dark Lance dispara el poder elegido, apuntando al enemigo mas cercano. Un poder "
                            "de Wendy/Hook carga su archivo al primer uso (el segundo sale)."));
  api->add_toggle(panel, "on", Text("Ligado", "On", "Activado"), &g_on);
  api->add_choice(panel, "power", Text("Poder", "Power", "Poder"), &g_pattern, g_labels.data(),
                  int32_t(g_labels.size()));
  api->add_button(panel, Text("Proximo poder", "Next power", "Siguiente poder"), &Next, nullptr);
  api->add_choice(panel, "anim", Text("Animacao do Nier", "Nier's animation", "Animacion de Nier"), &g_anim, kAnimNames, 3);
  api->add_int(panel, "damage", Text("Dano (x nivel do Nier)", "Damage (x Nier's level)", "Dano (x nivel de Nier)"),
               &g_damage, 1, 100);
  api->add_toggle(panel, "replace",
                  Text("Substituir o Dark Lance (desligado: os dois saem)", "Replace Dark Lance (off: both come out)",
                       "Sustituir Dark Lance (apagado: salen los dos)"),
                  &g_replace);
  api->show_in_menus(panel, NIER_MENU_PAUSE);
  api->on_pad(&OnPad, nullptr);
  Apply();
  api->log("ready");
  return 0;
}
