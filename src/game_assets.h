// nier - reads NieR's own 2D art (menu pages, icons) from the game's data, so
// PC-only screens can look like the game's menus without shipping any of it.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace nier {

struct GameImage {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;  // rows of R, G, B, A
};

// One file of a CRI CPK in the game data.
struct CpkFile {
  std::string dir;  // "TEXT/TALK_TEXT"
  std::string name;
  uint64_t offset = 0;
  uint64_t size = 0;          // stored size
  uint64_t extract_size = 0;  // size after CRILAYLA compression, if any
};

// Lists / reads a CPK by guest path, e.g. "game:\\media\\layer2.cpk". Only
// uncompressed files can be read (size == extract_size); all the text is.
std::vector<CpkFile> ListCpk(const std::string& guest_path);
bool ReadCpkFile(const std::string& guest_path, const CpkFile& file, std::vector<uint8_t>& out);

// Decodes the named textures of 2D/<pack>.2DV + .2DP inside
// game:\media\layer1.cpk (CRI CPK; Xbox 360 tiled DXT/8888 textures).
// Wanted is pack -> texture names; missing ones are left out of the result.
std::map<std::string, GameImage> LoadGameImages(
    const std::map<std::string, std::vector<std::string>>& wanted);

// One texture of a 2D pack as its .2DV describes it: its data's place in the .2DP and its fetch constant.
struct PackTexture {
  uint32_t offset = 0, size = 0;  // in the .2DP
  uint8_t fetch[24] = {};         // Xenos texture fetch constant (big-endian words)
};
// The textures of 2D/<pack>.2DV in layer1.cpk, by name (empty when unreadable).
std::map<std::string, PackTexture> ListPackTextures(const std::string& pack);
// Decodes one of them from the pack's bytes (the .2DP as loaded: a translation's, too).
bool DecodePackTexture(const PackTexture& t, const std::vector<uint8_t>& pix, GameImage& out);

}  // namespace nier
