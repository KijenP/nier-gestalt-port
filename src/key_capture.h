// nier - next key pressed, for rebinding (key_capture.cpp). Desktop only: on
// Android it never returns a key.
#pragma once

#include <string>

namespace nier {

void BeginKeyCapture();           // keys already down do not count
std::string PollCapturedKey();    // "" until a key goes down; "Escape" cancels

}  // namespace nier
