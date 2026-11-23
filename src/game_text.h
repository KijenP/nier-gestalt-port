// nier - game text languages and translation folders.
//
// The game's text lives in plain tab-separated files inside layer2.cpk
// (TEXT\NIER_TEXT.TXD with one column per language, TEXT\*\*.EN.* with one file
// per language, EVENT\CAPTION\*.EN.CAP for the cutscene subtitles). Next to the
// game there is a TEXT folder:
//   TEXT\EXAMPLE\    copy of those files, extracted on the first run
//   TEXT\<name>\     a translation: same layout, English files edited
// The Language entry in the game's Options lists English, Francais, Deutsch
// (the languages on the disc) and every folder in TEXT. Picking one applies at
// once. A folder language runs as "English" for the game and hands it that
// folder's files whenever the game asks for them.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace nier::text {

struct Language {
  std::string name;           // shown in the menu and saved in nier.toml
  int game_code = 2;          // the game's own number: 1 ja, 2 en, 3 fr, 4 de
  std::filesystem::path dir;  // translation folder; empty for the disc's languages
};

// The TEXT folder, and the saved choice (nier_language in nier.toml). Cheap;
// called at startup before the game reads its language.
void Init(const std::filesystem::path& root);

// Extracts TEXT\EXAMPLE from the game data once (slow: call from a thread).
void ExtractExample();

// Languages now available (rescans TEXT). The current one's name.
std::vector<Language> Languages();
std::string CurrentName();

// Switches language. Returns false if unknown or already selected. The game
// thread reloads its text at the next safe moment (TakeReloadRequest).
bool Select(const std::string& name);
bool TakeReloadRequest();

// What the game's language getter should answer (1..4), 0 = leave as is.
int GameLanguageOverride();

// The TEXT folder; the active translation folder (empty for the disc's languages).
std::filesystem::path Root();
std::filesystem::path CurrentDir();

// Content of a game file the active translation (or, for art - .2DV/.2DP, .MDV/.MDP,
// .EFV/.EFP - a mod in the MODS folder) replaces, null = use the disc's.
// `game_path` as the game asks it, e.g. "text\\nier_text.txd" or "2d\\system.2dp".
using Blob = std::shared_ptr<const std::vector<uint8_t>>;
Blob Resolve(const std::string& game_path);

// True for the pixel files of art packs (.2DP/.MDP/.EFP). Those are not handed to
// the game instead of its own file: the game loads its file (and does whatever it
// does with it), then the loader hook writes the replacement over the loaded bytes.
bool IsPixelPath(const std::string& game_path);

// The enabled mod folders (MODS\<name>, not starting with '_' or '.'), first = wins
// (reverse name order, as for every other kind of mod).
std::vector<std::filesystem::path> ModDirs();
// The TEXT folder known before Init (2026-11-19): the code mods load (OnPostLoadXexImage) before OnPostSetup's Init,
// and ModDirs, with no root yet, looked for "MODS" in the working folder: the exe's on the PC (it worked by chance),
// "/" on Android (no code mod ever loaded there).
void SetRoot(const std::filesystem::path& root);

}  // namespace nier::text
