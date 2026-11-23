// nier - downloadable content (2026-11-22): the player's DLC packages in a "DLC" folder next to nier.toml.
#pragma once

#include <filesystem>

namespace nier::dlc {

// Installs each Xbox 360 content package (LIVE / PIRS / CON, this game's title id) found under `dir` that is not
// installed yet: the SDK extracts it into the content folder (ContentManager::InstallContent), where the game's own
// content enumeration finds it. Any layout works (the bare file, or the console's 535107E8\00000002\<file>).
// Call after the XEX is loaded (the title id is known).
void InstallFrom(const std::filesystem::path& dir);

}  // namespace nier::dlc
