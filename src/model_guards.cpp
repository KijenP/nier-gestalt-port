// nier - guards for models the game was not made for (another body on the father: "Play with this body").
//
// sub_8259B560 (r3 = the model, r4 = a skeleton node; it calls itself for the children +8 and walks the siblings
// +16) links each node's "PARTS_GROUPnnn" attributes to the model's part groups: the node's list at +164 holds
// entries of type 3 (+12) with a list at +24 of type 4 entries whose +16 is the group (its +12 the group's id).
// The father's skeleton names the father's groups (PARTS_GROUP000, 001, 010, 1001, 2001, 3001); an NPC's polygons
// carry their own (SCW010: 100, 200, 300), so on the father's skeleton those attributes found no group and the
// walk read 0x0000000C (2026-11-17: SCW010, SFM010 crashed seconds into the game). Such an attribute (+16 = 0) is
// made one the walk skips (type 4 -> 0x7E04): the node is then just not tied to a part group.
#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

namespace {
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void SetBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
bool Guest(uint32_t a) { return a >= 0x1000 && a < 0xF0000000u; }
}  // namespace

extern "C" void __imp__sub_8259B560(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(sub_8259B560) {
  for (uint32_t node = ctx.r4.u32, siblings = 0; Guest(node) && siblings < 4096; node = Be32(base + node + 16), ++siblings) {
    int guard = 0;
    for (uint32_t e = Be32(base + node + 164); Guest(e) && guard < 4096; e = Be32(base + e + 4), ++guard) {
      if (Be32(base + e + 12) != 3) continue;
      int inner = 0;
      for (uint32_t a = Be32(base + e + 24); Guest(a) && inner < 4096; a = Be32(base + a + 4), ++inner) {
        if (Be32(base + a + 12) != 4 || Be32(base + a + 16) != 0) continue;
        SetBe32(base + a + 12, 0x7E04);
        static int logged = 0;
        if (logged++ < 16)
          REXLOG_WARN("Modelo: atributo de grupo de pecas sem grupo (no {:08X}) ignorado: corpo de outro personagem?",
                      node);
      }
    }
  }
  __imp__sub_8259B560(ctx, base);
}

// sub_82231E90 (r3 = a character; +5168 a state, +5172 the handle of another object): in state 2 it looks the other
// object up (sub_821A3280: index + generation, 0 when that object is gone) and sends it message 203 through its
// vtable (+520) with no check. 2026-11-18 (log nier_336): a wolf held Nier in its teeth, a cutscene began, the wolf
// was gone -> read of 0x0. The message is left out when the object is gone; the rest (sub_82214A08 on +5168) runs.
extern "C" void __imp__sub_82231E90(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_821A3280(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82214A08(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(sub_82231E90) {
  const uint32_t self = ctx.r3.u32;
  if (Guest(self) && Be32(base + self + 5168) == 2) {
    PPCContext probe = ctx;
    probe.r3.u64 = self + 5172;
    __imp__sub_821A3280(probe, base);
    if (probe.r3.u32 == 0) {
      static int logged = 0;
      if (logged++ < 8) REXLOG_WARN("Personagem {:08X}: o objeto que o segurava sumiu (cena?); mensagem 203 pulada", self);
      PPCContext rest = ctx;
      rest.r3.u64 = self + 5168;
      __imp__sub_82214A08(rest, base);
      ctx.r3.u64 = rest.r3.u64;
      return;
    }
  }
  __imp__sub_82231E90(ctx, base);
}
