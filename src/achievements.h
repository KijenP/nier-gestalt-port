// nier - native achievements: the game's own unlock calls, kept in a file next to nier.toml.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace nier::achievements {

struct Info {
  uint32_t id = 0;
  std::string name;         // the game's own text (its XDBF), its default language
  std::string description;  // how it was earned
  std::string locked;       // what a locked one says (the secret ones hide the story)
  uint32_t points = 0;
  uint32_t image_id = 0;  // its icon in the game's XDBF
  bool unlocked = false;
  std::string when;  // "YYYY-MM-DD HH:MM" once unlocked
};

// Every achievement of the game, in its order, in the game's language (2 en, 3 fr, 4 de; 0 = the
// current one). Empty before the game's image is loaded.
std::vector<Info> List(int game_language = 0);

// Marks one unlocked (once), saves the file and logs it.
void Unlock(uint32_t id);

// An icon of the game's XDBF (achievements' image_id; 0 = the game's own icon), decoded to RGBA.
bool Icon(uint32_t image_id, int& width, int& height, std::vector<uint8_t>& rgba);

// The achievement unlocked last, once (for the on-screen notice); false when none is new.
bool TakeNotice(Info& out);

// True once after an unlock: the game thread plays the game's own menu sound for it.
bool TakeSound();

}  // namespace nier::achievements
