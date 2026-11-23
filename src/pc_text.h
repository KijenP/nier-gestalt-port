// nier - the text of the PC port's own menus (Video Options, Control Options, Language, Quit to
// Desktop, the achievements...) in the game's languages, and in translations.
//
// The port's texts live in a table shaped like the game's NIER_TEXT.TXD: one message per line
// (CRLF), columns id, Japanese, English, French, German separated by TAB. English, French and
// German are built in. TEXT\EXAMPLE\PC_TEXT.TXD is written next to the game's own texts for
// translators; a translation folder (TEXT\<name>) may hold its own PC_TEXT.TXD, whose English
// column is used while that language is active (missing rows stay in English).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace nier::pc_text {

// `en` in the current language (UTF-8). `en` is the built-in English text, which is the key.
std::string T(const char* en);
inline std::string T(const std::string& en) { return T(en.c_str()); }

// An achievement's name (field 0), description (1) or text while locked (2), by its id; `fallback`
// (the game's own, from its XDBF) when no translation has one.
std::string Achievement(uint32_t id, int field, const std::string& fallback);

// Writes TEXT\EXAMPLE\PC_TEXT.TXD (built-in rows, then the achievements the game has) when it is
// missing or out of date.
void WriteExample();

}  // namespace nier::pc_text
