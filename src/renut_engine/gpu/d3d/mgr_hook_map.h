// NieR (2026-11-04): Renut's guest code guards for NieR Gestalt's executable (work/renut-tools/guards.py, from a
// guest memory dump): an FNV-1a 32 hash of the first bytes of every hooked function. Named mgr:: as Renut reads it.
#pragma once
#include <cstdint>
namespace renut::gpu::mgr {
struct GuestCodeGuard { uint32_t address, bytes, fnv32; const char* name; };
inline constexpr GuestCodeGuard kGuestCodeGuards[] = {
  {0x8257A250u, 64u, 0x4A78C5B9u, "Direct3D_CreateDevice"},
  {0x8257D3F8u, 64u, 0x5FC0D2E2u, "InitializePresentationParameters"},
  {0x8257D190u, 64u, 0xFB14CEA5u, "InitializeApiState"},
  {0x8257D890u, 64u, 0xC73F048Eu, "XenonGpu_Initialize"},
  {0x8257D900u, 64u, 0x454C1608u, "HardwareDeviceSetup_reference"},
  {0x825744F8u, 64u, 0x6C77E31Bu, "KickOff"},
  {0x82574370u, 64u, 0xA07D7816u, "KickOffSegment"},
  {0x82574728u, 64u, 0x3ED44F9Du, "BeginRingBig"},
  {0x82573440u, 64u, 0xDBD37CDFu, "RingBufferDeviceAllocate"},
  {0x82573670u, 64u, 0xC21C7326u, "AllocateInlineData"},
  {0x82573A38u, 64u, 0xA9A7433Du, "NewSegment"},
  {0x82574870u, 64u, 0xC8E5843Du, "SetRingBufferParameters"},
  {0x82573758u, 64u, 0x1256E7ECu, "BlockOnFence"},
  {0x82579D50u, 64u, 0x5AE13878u, "CBlocker_Check"},
  {0x82574630u, 64u, 0x9BF08BD8u, "IsFencePending"},
  {0x82574818u, 64u, 0x0D455650u, "InsertCallback"},
  {0x8257A708u, 64u, 0x72A6ADC7u, "SynchronizeToPresentationInterval"},
  {0x8257AB38u, 64u, 0x2B7083FFu, "Swap_shared"},
  {0x82571508u, 64u, 0xA01250B0u, "DrawVertices_shared"},
  {0x825718F8u, 64u, 0x184ADC1Du, "DrawIndexedVertices_shared"},
  {0x82571070u, 64u, 0xCE91856Du, "BeginVertices"},
  {0x82587940u, 64u, 0x5209804Cu, "ClearF"},
  {0x82585888u, 64u, 0xA3B47821u, "Resolve"},
  {0x82570E28u, 64u, 0x2A323845u, "XGRegisterVertexShader"},
  {0x82570530u, 52u, 0x3B59C114u, "XGRegisterPixelShader"},
  {0x82570910u, 64u, 0x1558195Eu, "VertexDeclarationInit"},
  {0x825729D8u, 64u, 0x4996421Fu, "D3DResource_Destroy"},
  {0x8256A030u, 64u, 0x28E76078u, "WindowScissor"},
};
}  // namespace renut::gpu::mgr
