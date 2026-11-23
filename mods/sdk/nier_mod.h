/* NieR Gestalt (PC/Android port) - mod API, version 1.
 *
 * A code mod is a library in its own folder under MODS (next to the game on the PC, NierPort/MODS on Android):
 *   MODS/<Mod>/<anything>.dll       Windows (x64)
 *   MODS/<Mod>/lib<anything>.so     Android (arm64)
 * The game loads it at startup and calls its exported nier_mod_init with this table. Everything the mod does goes
 * through the table, in C, so a mod built with any compiler keeps working with later versions of the game (new
 * entries are only ever added at the end; check api->version).
 *
 * A mod shows its settings in a panel: on the PC a key opens it, on Android an icon on the screen does. The values
 * of its toggles and numbers are saved by the game in MODS/<Mod>/settings.txt. With show_in_menus they also appear
 * in the game's own Options and pause menus, so the player can change them there.
 *
 * Guest memory (the Xbox 360's 4 GB space) is at api->guest_base and is big-endian: use the nier_be* helpers.
 *
 * Events run on the game's own threads, in the middle of the game's code: keep them short and never block.
 */
#ifndef NIER_MOD_H
#define NIER_MOD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NIER_MOD_API_VERSION 3

#if defined(_WIN32)
#define NIER_MOD_EXPORT __declspec(dllexport)
#else
#define NIER_MOD_EXPORT __attribute__((visibility("default")))
#endif

typedef struct NierModPanel NierModPanel;

typedef struct NierModPad {
  uint16_t buttons; /* XINPUT_GAMEPAD_*: 0x1000 A, 0x2000 B, 0x4000 X, 0x8000 Y, 0x0100 LB, 0x0200 RB, ... */
  uint8_t left_trigger, right_trigger;
  int16_t lx, ly, rx, ry;
} NierModPad;

/* Where show_in_menus puts a panel's settings. */
#define NIER_MENU_OPTIONS 1 /* the title screen's Options */
#define NIER_MENU_PAUSE 2   /* the pause menu's Options */

typedef struct NierModApi {
  uint32_t version;    /* NIER_MOD_API_VERSION of the game */
  uint8_t* guest_base; /* guest address 0 */
  const char* language; /* the player's language: "pt", "en" or "es" (pick your labels) */
  const char* mod_dir;  /* this mod's folder (UTF-8) */
  const char* platform; /* "windows" or "android" */

  void (*log)(const char* text); /* to the game's log, prefixed with the mod's folder name */

  /* A panel: 'hotkey' is a key name ("F5", "F6", ... "Insert"; PC), 'icon_png' a PNG in the mod's folder for the
   * on-screen icon (Android; null = a default one). */
  NierModPanel* (*create_panel)(const char* title, const char* hotkey, const char* icon_png);
  /* Controls, in order. 'id' names the value in settings.txt; *value is read back from there now and written by the
   * game whenever the player changes it. */
  void (*add_toggle)(NierModPanel* panel, const char* id, const char* label, int32_t* value);
  void (*add_int)(NierModPanel* panel, const char* id, const char* label, int32_t* value, int32_t min, int32_t max);
  void (*add_button)(NierModPanel* panel, const char* label, void (*on_click)(void* user), void* user);
  void (*add_text)(NierModPanel* panel, const char* text);
  void (*show_in_menus)(NierModPanel* panel, int32_t where);

  /* Game events. 'slot' is the party member (0 = Nier). Change the pointed value to change what happens. */
  void (*on_set_hp)(void (*fn)(void* user, uint32_t slot, int32_t current, int32_t* new_hp), void* user);
  void (*on_add_mp)(void (*fn)(void* user, uint32_t slot, float* delta), void* user);
  void (*on_add_exp)(void (*fn)(void* user, uint32_t slot, int32_t* amount), void* user);
  void (*on_add_gold)(void (*fn)(void* user, int32_t* amount), void* user);

  /* A hit about to be applied to a character's status (the game's own damage function): 'target' its guest address,
   * 'is_party' 1 for Nier and his companions. *damage before defence; set *blocked to 1 for no hit at all (as the
   * game's own invulnerability: no damage, no reaction). */
  void (*on_damage)(void (*fn)(void* user, uint32_t target, int32_t is_party, int32_t* damage, int32_t* blocked),
                    void* user);

  /* A character is about to receive a hit (before any reaction: flinch, knock-back, damage). 'target' the guest
   * address of its status (the one on_damage gets), 'is_party' 1 for Nier and his companions. Set *ignore to 1 and
   * the hit does not happen at all. */
  void (*on_hit)(void (*fn)(void* user, uint32_t target, int32_t is_party, int32_t* ignore), void* user);

  /* The game's camera, each frame after the game moved it: its world matrix (row major: right, up, forward,
   * position; 16 floats, host order). Change it to move the view. */
  void (*on_camera)(void (*fn)(void* user, float* world), void* user);
  /* The player's controller as the game would read it (the keyboard and mouse included, as the pad they drive):
   * buttons (XINPUT_GAMEPAD_* bits), triggers 0..255, sticks -32768..32767. Read from any thread. */
  void (*get_pad)(NierModPad* out);
  /* 1: the game gets no controller input (the mod uses it), 0: back to the game. */
  void (*block_game_input)(int32_t block);

  /* Each time the game reads the controller, before it acts on it (its own thread): change *pad to change what it
   * gets. Guest memory written here is seen by the game's logic of this same frame. */
  void (*on_pad)(void (*fn)(void* user, NierModPad* pad), void* user);

  /* ---- Version 2 ---- */
  /* A choice among 'count' names (copied): *value is the index chosen (saved in settings.txt as the others). */
  void (*add_choice)(NierModPanel* panel, const char* id, const char* label, int32_t* value, const char* const* names,
                     int32_t count);
  /* Sets one of the game's options (its nier.toml name, e.g. "nier_moveset_test") for this session, from text; the
   * game's own settings file is not written. 0 when there is no such option. Only options named "nier_moveset_*". */
  int32_t (*set_option)(const char* name, const char* value);

  /* ---- Version 3 ---- */
  /* The game is about to put Nier back on the ground (sub_8221AEB0: after ~7 s in the air, a fall, a script's warp).
   * Set *cancel to 1 and he stays where he is. */
  void (*on_player_warp)(void (*fn)(void* user, int32_t* cancel), void* user);
} NierModApi;

/* The mod's entry point: return 0 on success. */
typedef int32_t (*NierModInitFn)(const NierModApi* api);

static inline uint32_t nier_be32(const NierModApi* api, uint32_t address) {
  const uint8_t* p = api->guest_base + address;
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline void nier_set_be32(const NierModApi* api, uint32_t address, uint32_t value) {
  uint8_t* p = api->guest_base + address;
  p[0] = (uint8_t)(value >> 24);
  p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8);
  p[3] = (uint8_t)value;
}

/* The party's saved stats (the save data block). */
#define NIER_SAVE_BLOCK 0x84CC6BA0u
#define NIER_SAVE_HP(slot) (NIER_SAVE_BLOCK + 76u + (slot) * 4u)      /* int */
#define NIER_SAVE_MP(slot) (NIER_SAVE_BLOCK + 88u + (slot) * 4u)      /* float */
#define NIER_SAVE_LEVEL(slot) (NIER_SAVE_BLOCK + 100u + (slot) * 4u)  /* int, level - 1 */
#define NIER_SAVE_EXP(slot) (NIER_SAVE_BLOCK + 112u + (slot) * 4u)    /* int */
#define NIER_SAVE_GOLD (NIER_SAVE_BLOCK + 188u)                       /* int, 0..9999999 */

/* Nier's character object (big-endian; found 2026-11-15, NOTAS.md "Nier Plus"): its status (+3472) is the party's first
 * (NIER_SAVE_BLOCK's slot 0 status, 0x82740048). At 0x848BE4B0 in the saves tried; nier_find_player checks it (0: not there). */
#define NIER_PLAYER_STATUS_OFFSET 3472u
#define NIER_PLAYER_STATUS 0x82740048u
#define NIER_PLAYER_HEIGHT 0x94u    /* float: the y of its position */
#define NIER_PLAYER_VSPEED 0x1B4u   /* float: vertical speed (the jump sets 0.18, the double jump 0.215; gravity
                                       takes from it each frame) */
#define NIER_PLAYER_LAST_MOVE_Y 0x164u /* float: the height moved in the last frame (sub_821B1558) */
#define NIER_PLAYER_AIRBORNE 0x16Cu /* float: -1.0 on the ground, 1.0 in the air */
#define NIER_PLAYER_ANIMATION 0x9D8u /* int: the animation playing (132/136 jump start, 133/137 rise, 134/138 double) */
#define NIER_PLAYER_JUMPS 0x477Cu    /* int: jumps made since leaving the ground (the jump, sub_8222D908) */
#define NIER_PLAYER_MAX_JUMPS 0x4780u /* int: the most it allows (2: the double jump) */
static inline uint32_t nier_find_player(const NierModApi* api, uint32_t hint) {
  if (hint && nier_be32(api, hint + NIER_PLAYER_STATUS_OFFSET) == NIER_PLAYER_STATUS) return hint;
  if (nier_be32(api, 0x848BE4B0u + NIER_PLAYER_STATUS_OFFSET) == NIER_PLAYER_STATUS) return 0x848BE4B0u;
  return 0;
}


#ifdef __cplusplus
}
#endif

#endif /* NIER_MOD_H */
