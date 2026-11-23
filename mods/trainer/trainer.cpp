// Nier Plus - Trainer: an example code mod (mods/sdk/nier_mod.h). Not part of the game: it is a library the player
// puts in MODS/Trainer/ (trainer.dll on the PC, libtrainer.so on Android). F5 (or its icon on Android) opens its panel.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../sdk/nier_mod.h"

namespace {

const NierModApi* g_api = nullptr;

int32_t g_infinite_hp = 0;
int32_t g_infinite_mp = 0;
int32_t g_exp_times = 1;
int32_t g_gold_times = 1;
int32_t g_invincible = 0;
int32_t g_hit_kill = 0;
int32_t g_damage_times = 1;
int32_t g_infinite_jump = 0;
uint32_t g_player = 0;
uint16_t g_last_buttons = 0;
std::chrono::steady_clock::time_point g_last_air_jump;  // the infinite jump's last push in the air
int32_t g_free_cam = 0;
int32_t g_free_cam_speed = 8;  // metres a second

// The free camera: where it is and where it looks (taken from the game's camera when it is turned on).
struct FreeCam {
  bool active = false;
  float pos[3] = {};
  float yaw = 0, pitch = 0;
  std::chrono::steady_clock::time_point last;
} g_cam;

float Stick(int16_t v) {
  constexpr float kDead = 7000.0f;
  const float f = float(v);
  if (std::fabs(f) < kDead) return 0.0f;
  return (f - std::copysign(kDead, f)) / (32767.0f - kDead);
}

// Each frame after the game moved its camera: ours instead while the free camera is on. Left stick (WASD) moves,
// right stick (the mouse) looks, the triggers go down / up, A held goes 4x faster. The game gets no input meanwhile.
void OnCamera(void*, float* m) {
  if (!g_free_cam) {
    if (g_cam.active) {
      g_cam.active = false;
      g_api->block_game_input(0);
    }
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (!g_cam.active) {
    g_cam.active = true;
    g_cam.pos[0] = m[12];
    g_cam.pos[1] = m[13];
    g_cam.pos[2] = m[14];
    g_cam.yaw = std::atan2(m[8], m[10]);
    g_cam.pitch = std::asin(std::fmax(-1.0f, std::fmin(1.0f, m[9])));
    g_cam.last = now;
    g_api->block_game_input(1);
    char line[256];
    std::snprintf(line, sizeof line, "free camera from right (%.2f %.2f %.2f) up (%.2f %.2f %.2f) fwd (%.2f %.2f %.2f) "
                  "pos (%.2f %.2f %.2f)", m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14]);
    g_api->log(line);
  }
  float dt = std::chrono::duration<float>(now - g_cam.last).count();
  g_cam.last = now;
  if (dt < 0.0f) dt = 0.0f;
  if (dt > 0.1f) dt = 0.1f;
  NierModPad pad{};
  g_api->get_pad(&pad);
  g_cam.yaw -= Stick(pad.rx) * 2.2f * dt;  // the game's yaw turns the other way (the player, 2026-11-15)
  g_cam.pitch += Stick(pad.ry) * 1.6f * dt;
  if (g_cam.pitch > 1.5f) g_cam.pitch = 1.5f;
  if (g_cam.pitch < -1.5f) g_cam.pitch = -1.5f;
  const float cp = std::cos(g_cam.pitch), sp = std::sin(g_cam.pitch);
  const float cy = std::cos(g_cam.yaw), sy = std::sin(g_cam.yaw);
  const float fwd[3] = {sy * cp, sp, cy * cp};
  const float right[3] = {cy, 0.0f, -sy};
  const float up[3] = {fwd[1] * right[2] - fwd[2] * right[1], fwd[2] * right[0] - fwd[0] * right[2],
                       fwd[0] * right[1] - fwd[1] * right[0]};
  const float speed = float(g_free_cam_speed) * ((pad.buttons & 0x1000) ? 4.0f : 1.0f) * dt;
  // The game's "right" row points to the screen's left (a right-handed world seen along +forward): the turn above and
  // the side step here both go against it (the player, 2026-11-15: left / right still swapped after the turn's fix).
  const float mx = -Stick(pad.lx), mz = Stick(pad.ly);
  const float my = (float(pad.right_trigger) - float(pad.left_trigger)) / 255.0f;
  for (int i = 0; i < 3; ++i) g_cam.pos[i] += (fwd[i] * mz + right[i] * mx) * speed;
  g_cam.pos[1] += my * speed;
  for (int i = 0; i < 3; ++i) {
    m[i] = right[i];
    m[4 + i] = up[i];
    m[8 + i] = fwd[i];
    m[12 + i] = g_cam.pos[i];
  }
  m[3] = m[7] = m[11] = 0.0f;
  m[15] = 1.0f;
}

const char* Text(const char* pt, const char* en, const char* es) {
  if (!std::strcmp(g_api->language, "pt")) return pt;
  if (!std::strcmp(g_api->language, "es")) return es;
  return en;
}

// A loss of HP becomes a refill (the game clamps any larger value to the maximum).
void OnSetHp(void*, uint32_t, int32_t current, int32_t* new_hp) {
  if (g_infinite_hp && *new_hp < current) *new_hp = 0x7FFFFFFF;
}

// A cost of MP becomes a refill (clamped to the maximum by the game).
void OnAddMp(void*, uint32_t, float* delta) {
  if (g_infinite_mp && *delta < 0.0f) *delta = 100000.0f;
}

void OnAddExp(void*, uint32_t, int32_t* amount) {
  if (g_exp_times > 1 && *amount > 0) {
    const long long v = static_cast<long long>(*amount) * g_exp_times;
    *amount = v > 0x7FFFFFFF ? 0x7FFFFFFF : static_cast<int32_t>(v);
  }
}

void OnAddGold(void*, int32_t* amount) {
  const int32_t before = *amount;
  if (g_gold_times > 1 && *amount > 0) {
    const long long v = static_cast<long long>(*amount) * g_gold_times;
    *amount = v > 9999999 ? 9999999 : static_cast<int32_t>(v);
  }
  char line[96];
  std::snprintf(line, sizeof line, "gold %+d -> %+d (now %u)", before, *amount, nier_be32(g_api, NIER_SAVE_GOLD));
  g_api->log(line);
}

// Hits: the party takes none (invincible); everything else dies at the first one, or takes more.
void OnDamage(void*, uint32_t, int32_t is_party, int32_t* damage, int32_t* blocked) {
  if (is_party) {
    if (g_invincible) *blocked = 1;
    return;
  }
  if (g_hit_kill) {
    *damage = 9999999;
  } else if (g_damage_times > 1 && *damage > 0) {
    const long long v = static_cast<long long>(*damage) * g_damage_times;
    *damage = v > 9999999 ? 9999999 : static_cast<int32_t>(v);
  }
}

// Infinite jump: the game's jump (sub_8222D908) jumps while the jumps made since leaving the ground are fewer than the
// most allowed (2: the double jump); in the air it is the double jump, with its animation and push. A pressed in the
// air sets the count back to 0 before the game reads the pad, so every press is one more double jump.
void OnPad(void*, NierModPad* pad) {
  const uint16_t pressed = pad->buttons & ~g_last_buttons;
  g_last_buttons = pad->buttons;
  if (!g_infinite_jump || !(pressed & 0x1000)) return;
  g_player = nier_find_player(g_api, g_player);
  if (!g_player) return;
  if (nier_be32(g_api, g_player + NIER_PLAYER_AIRBORNE) != 0x3F800000u) return;  // 1.0: in the air
  if (nier_be32(g_api, g_player + NIER_PLAYER_JUMPS) != 0) nier_set_be32(g_api, g_player + NIER_PLAYER_JUMPS, 0);
  // 2026-11-20 (the player: after a while in the air the game pulled him down; against an invisible wall the jumps
  // stopped): the double jump (sub_8222D908) runs only in the jump's own state and not while the rise (animations 133,
  // 137) plays; a long fall or a wall leaves it. The push of the double jump (0.215, the game's) is given here too, in
  // any state in the air.
  const float push = 0.215f;
  uint32_t bits;
  std::memcpy(&bits, &push, 4);
  nier_set_be32(g_api, g_player + NIER_PLAYER_VSPEED, bits);
  g_last_air_jump = std::chrono::steady_clock::now();
}

// The game brings Nier down after ~7 s in the air (sub_8221AEB0, 2026-11-20): not while the infinite jump keeps him up
// (on, in the air, a push in the last 10 s). A fall into the void or a script's warp with none of that still happens.
void OnPlayerWarp(void*, int32_t* cancel) {
  if (!g_infinite_jump) return;
  g_player = nier_find_player(g_api, g_player);
  if (!g_player || nier_be32(g_api, g_player + NIER_PLAYER_AIRBORNE) != 0x3F800000u) return;
  if (std::chrono::steady_clock::now() - g_last_air_jump < std::chrono::seconds(10)) *cancel = 1;
}

// Invincible: hits on the party do not happen (no flinch, no knock-back, no damage).
void OnHit(void*, uint32_t, int32_t is_party, int32_t* ignore) {
  if (g_invincible && is_party) *ignore = 1;
}

void AddGold(void*) {
  uint32_t gold = nier_be32(g_api, NIER_SAVE_GOLD) + 100000;
  if (gold > 9999999) gold = 9999999;
  nier_set_be32(g_api, NIER_SAVE_GOLD, gold);
}

}  // namespace

extern "C" NIER_MOD_EXPORT int32_t nier_mod_init(const NierModApi* api) {
  if (api->version < 1) return 1;
  g_api = api;
  NierModPanel* panel = api->create_panel("Trainer", "F5", "icon.png");
  api->add_toggle(panel, "invincible", Text("Invencibilidade", "Invincibility", "Invencibilidad"), &g_invincible);
  api->add_toggle(panel, "infinite_hp", Text("Vida infinita", "Infinite HP", "Vida infinita"), &g_infinite_hp);
  api->add_toggle(panel, "infinite_mp", Text("Magia infinita", "Infinite MP", "Magia infinita"), &g_infinite_mp);
  api->add_toggle(panel, "hit_kill", Text("Matar com um golpe", "One-hit kill", "Matar de un golpe"), &g_hit_kill);
  api->add_int(panel, "damage_times", Text("Dano x", "Damage x", "Dano x"), &g_damage_times, 1, 100);
  api->add_toggle(panel, "infinite_jump", Text("Pulo infinito", "Infinite jump", "Salto infinito"), &g_infinite_jump);
  api->add_toggle(panel, "free_cam", Text("Camera livre", "Free camera", "Camara libre"), &g_free_cam);
  api->add_int(panel, "free_cam_speed", Text("Velocidade da camera", "Camera speed", "Velocidad de camara"),
               &g_free_cam_speed, 1, 50);
  api->add_int(panel, "exp_times", Text("Experiencia x", "Experience x", "Experiencia x"), &g_exp_times, 1, 100);
  api->add_int(panel, "gold_times", Text("Ouro x", "Gold x", "Oro x"), &g_gold_times, 1, 100);
  api->add_button(panel, Text("+100.000 de ouro", "+100,000 gold", "+100.000 de oro"), &AddGold, nullptr);
  api->show_in_menus(panel, NIER_MENU_OPTIONS | NIER_MENU_PAUSE);
  api->on_set_hp(&OnSetHp, nullptr);
  api->on_add_mp(&OnAddMp, nullptr);
  api->on_add_exp(&OnAddExp, nullptr);
  api->on_add_gold(&OnAddGold, nullptr);
  if (api->on_damage) api->on_damage(&OnDamage, nullptr);
  if (api->on_hit) api->on_hit(&OnHit, nullptr);
  if (api->on_camera && api->get_pad && api->block_game_input) api->on_camera(&OnCamera, nullptr);
  if (api->on_pad) api->on_pad(&OnPad, nullptr);
  if (api->version >= 3 && api->on_player_warp) api->on_player_warp(&OnPlayerWarp, nullptr);
  // A camera left on at the last exit starts off (the player could not move until finding the panel).
  g_free_cam = 0;
  api->log("ready");
  return 0;
}
