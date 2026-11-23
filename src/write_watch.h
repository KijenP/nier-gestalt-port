#pragma once

#include <cstdint>

namespace nier {

// Dev aid, see write_watch.cpp (NIER_WATCH_WRITE).
void StartWriteWatch(uint8_t* guest_base);

}  // namespace nier
