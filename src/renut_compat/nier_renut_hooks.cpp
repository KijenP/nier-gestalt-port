// NieR: the hooks at the addresses Renut shares with the app (banjo_shared_hooks.h). Renut's side is
// renut_native_sub_X; with nier_renderer = "renut" these hooks run it, otherwise the game's original. The Swap's
// (sub_8257AB38) is the performance meter's hook (perf_overlay.cpp).
#include <rex/hook.h>
#include <rex/ppc.h>

#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/banjo_shared_hooks.h"

REX_EXTERN(__imp__sub_825718F8);
REX_HOOK_RAW(sub_825718F8) {  // D3DDevice_DrawIndexedVertices
  BANJO_RENUT_YIELD_SHARED(sub_825718F8);
  __imp__sub_825718F8(ctx, base);
}

REX_EXTERN(__imp__sub_82571508);
REX_HOOK_RAW(sub_82571508) {  // D3DDevice_DrawVertices
  BANJO_RENUT_YIELD_SHARED(sub_82571508);
  __imp__sub_82571508(ctx, base);
}
