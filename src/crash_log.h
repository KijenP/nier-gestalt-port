#pragma once

namespace nier {
// Android: native crashes (signal, registers, call stack) go to the game's log. Nothing elsewhere.
void InstallCrashLog();
}  // namespace nier
