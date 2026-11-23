// nier - ReXGlue Recompiled Project

#include "generated/default/nier_init.h"

#include "nier_app.h"

// Extra supersampling on top of the chosen resolution (see
// NierApp::OnFinalizePaths, which turns both into the SDK's integer scale).
REXCVAR_DEFINE_INT32(render_scale_percent, 100, "Nier",
                     "Render scale relative to the chosen resolution, in percent (below 100 "
                     "renders fewer pixels and upscales them with present_effect)")
    .range(10, 400);

// Graphics API of the xenos GPU plugin (NierApp::OnPreSetup). Vulkan is the
// one renderer kept up to date; an SDK built without it falls back to D3D12.
REXCVAR_DEFINE_STRING(renderer, "vulkan", "Nier", "Graphics API: vulkan (or d3d12)");

REX_DEFINE_APP(nier, NierApp::Create)
