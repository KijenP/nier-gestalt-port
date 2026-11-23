// nier - TEMP (2026-11-19, the moveset mod's research): which BULLET script pieces the game sets up, and from where.
//
// The attacks and magic run in the game's own script machine (BULLET/*.BIN, "sall" files: NOTAS.md, "Mod de
// movesets"). sub_821C62A0 (r3 = a script object, r4 = a "scpt" chunk) reads one script: its info (the name),
// sone (the code), smtb (the functions), dict. book.bin holds 106 of them, the shot patterns of Nier's magic and of
// the books (1way_1, 3d_shot1_2, fan_3, finger...). Each new (name, caller) pair is written once to the log as
// "[bullet] script <name> (caller <lr>)": playing a while shows which pattern each magic and level uses, and the
// code that chose it.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <map>
#include <set>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>

#include "game_text.h"

namespace {
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
bool Guest(uint32_t a) { return a >= 0x1000 && a < 0xF0000000u; }

// The scpt chunk's name: its first chunk is "info" (tag, size, payload), the payload holds "nm" and then the name.
std::string ScriptName(const uint8_t* base, uint32_t scpt) {
  if (!Guest(scpt) || std::memcmp(base + scpt, "scpt", 4) != 0) return {};
  const uint8_t* info = base + scpt + 8;
  if (std::memcmp(info, "info", 4) != 0) return {};
  const uint32_t size = Be32(info + 4);
  if (size == 0 || size > 256) return {};
  const uint8_t* p = info + 8;
  for (uint32_t i = 0; i + 4 < size; ++i) {
    if (p[i] != 'n' || p[i + 1] != 'm') continue;
    std::string s;
    for (uint32_t k = i + 4; k < size && p[k] >= 32 && p[k] < 127 && s.size() < 64; ++k) s += char(p[k]);
    return s;
  }
  return {};
}
}  // namespace

// sub_821C0A58 (r3 = a script's owner, r4 = the native's number 10000..10107, r5 = its arguments): the BULLET scripts'
// own natives (create a shot, move, aim...). Each number once, with its caller: a boss firing shows which one makes
// the shot.
extern "C" void __imp__sub_821C0A58(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(sub_821C0A58) {
  {
    static std::mutex mutex;
    static std::set<uint32_t> seen;
    const uint32_t id = ctx.r4.u32;
    std::lock_guard lock(mutex);
    if (seen.size() < 400 && seen.insert(id).second)
      REXLOG_INFO("[bullet] native {} (owner {:08X}, args {:08X}, caller {:08X})", id, ctx.r3.u32, ctx.r5.u32,
                  uint32_t(ctx.lr));
  }
  __imp__sub_821C0A58(ctx, base);
}

// TEMP (2026-11-19, NIER_FXLOG=1): the script commands of everyone but Nier (owner's vtable, command, arguments as
// float and word, and the script text an argument names), each distinct line once: what Kaine's magic sets on its
// shots (LoopEffectId and the effect numbers) as the NPC casts it.
namespace {
std::string ArgText(const uint8_t* base, uint32_t strings, uint32_t w, float f) {
  auto text_at = [&](uint32_t a) -> std::string {
    if (!Guest(a)) return {};
    std::string s;
    for (uint32_t k = 0; k < 40; ++k) {
      const uint8_t c = base[a + k];
      if (!c) break;
      if (c < 32 || c > 126) return {};
      s += char(c);
    }
    return s.size() >= 3 ? s : std::string();
  };
  // Only through the script's own texts (a word read as an address crashed: 0x3F800000 is 1.0, 2026-11-19).
  std::string t;
  if (strings && w < 0x100000) t = text_at(strings + w);
  if (t.empty() && strings && f >= 0.0f && f < 1048576.0f && f == float(int(f))) t = text_at(strings + uint32_t(f));
  return t;
}
}  // namespace
REXCVAR_DEFINE_BOOL(nier_fxlog, false, "NieR/Mods", "TEMP: log the script commands of everyone but Nier ([fx] lines)");
void FxLog(const char* kind, const PPCContext& ctx, const uint8_t* base) {
  if (!REXCVAR_GET(nier_fxlog)) return;
  const uint32_t owner = ctx.r3.u32;
  if (owner == 0x848BE4B0u || !Guest(owner)) return;
  const uint32_t vt = Be32(base + owner);
  const int32_t n = std::clamp(ctx.r6.s32, 0, 8);
  std::string line = fmt::format("[fx] {} vt {:08X} cmd {} n {}:", kind, vt, ctx.r4.s32, ctx.r6.s32);
  // The key leaves the numbers out (the village's animals filled the 4000 lines with their moves, 2026-11-19); a line
  // naming an effect or a sound is always kept.
  std::string key = fmt::format("{} {:08X} {}", kind, vt, ctx.r4.s32);
  bool wanted = false;
  for (int32_t i = 0; i < n && Guest(ctx.r5.u32); ++i) {
    const uint32_t w = Be32(base + ctx.r5.u32 + uint32_t(i) * 4);
    float f;
    std::memcpy(&f, &w, 4);
    const std::string t = ArgText(base, ctx.r7.u32, w, f);
    line += t.empty() ? fmt::format(" {:g}", f) : fmt::format(" \"{}\"", t);
    if (!t.empty()) key += " " + t;
    if (t.find("ffect") != std::string::npos || t.find("SeId") != std::string::npos) wanted = true;
  }
  if (wanted) key = line;
  static std::mutex mutex;
  static std::set<std::string> seen;
  std::lock_guard lock(mutex);
  if (seen.size() < 6000 && seen.insert(key).second) REXLOG_INFO("{} (por {:08X})", line, uint32_t(ctx.lr));
}
extern "C" void __imp__sub_821CF980(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(sub_821CF980) {
  FxLog("geral", ctx, base);
  __imp__sub_821CF980(ctx, base);
}

// ---- The moveset mod's proof (2026-11-19, TEMP): Nier fires an enemy's shot pattern ----
// sub_82256B78(files 0x826BDA48, name) = a loaded BULLET file ("book.bin"), 0 if not loaded; sub_82256DC0(files, name,
// 0) loads it. sub_8236D728(r3 = the pattern's index in the file's "sary" list, r4 = the file, r5 = a 4x4 matrix (rows:
// right, up, forward, position), r6 = r8 = the owner's 64-bit handle (actor+160), r7 = a target actor or 0, f1 = 0,
// r10 = 0, a byte 1 at the caller's frame+87) makes the shot (native 37's call, sub_821DB288 case 139).
// r3 is NOT an index: it is the pattern's NAME (a guest string; sub_821C7C70 bsearches the file's "sary" list by name,
// sub_825DA490 with the compare at 0x821C71C0). Native 37's "index" is the name's offset in the calling script's own
// strings plus their base (r19). An index there read address 24 (the first try's crash, 2026-11-19).
REXCVAR_DEFINE_STRING(nier_moveset_test, "", "NieR/Mods",
                      "TEST of the moveset mod: when Nier casts Dark Lance, a shot of book.bin with this pattern name "
                      "(3d_spiraldown_1, straight, fan_1, spiral_1, finger, Laser...) comes out of him too. Empty: off");

// The shot's target (r7, kept at shot+428/432 as a handle): enemy shots seem to test their hits against it only
// (r7 = 0 hurt no one, 2026-11-19). "self": Nier himself (a test of that).
// The shot library the pattern is in (2026-11-19): the game's INI (in the exe) gives each model one; only two exist:
// book.bin (the books, Kaine, the Shadowlord, Halua, Popola...) and wendy_shot_patternA.bin (Hook, Wendy, the
// balls...). Loaded on the first cast that needs it (the next cast fires).
REXCVAR_DEFINE_STRING(nier_moveset_file, "book.bin", "NieR/Mods",
                      "TEST of the moveset mod: the BULLET shot library of the test pattern (book.bin, "
                      "wendy_shot_patternA.bin)");

REXCVAR_DEFINE_STRING(nier_moveset_target, "", "NieR/Mods",
                      "TEST of the moveset mod: the test shot's target: empty = none, self = Nier");

extern "C" void __imp__sub_82256B78(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82256DC0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8236D728(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8235B958(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_821C80D0(PPCContext& ctx, uint8_t* base);

namespace nier::moveset {
namespace {
constexpr uint32_t kFiles = 0x826BDA48;
constexpr uint32_t kNier = 0x848BE4B0;

void SetBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
// A context for a call of ours: a frame well below the caller's, its back chain set.
PPCContext Frame(const PPCContext& ctx, uint8_t* base) {
  PPCContext c = ctx;
  c.r1.u64 = (ctx.r1.u32 - 0x800) & ~0xFu;
  SetBe32(base + c.r1.u32, ctx.r1.u32);
  return c;
}
// The loaded file, or 0. Not loaded: its loading is asked for and 0 returned (the first try crashed: the shot's
// script read a file still on its way, 2026-11-19); a later cast finds it.
uint32_t ShotFile(const PPCContext& ctx, uint8_t* base, const char* name) {
  PPCContext c = Frame(ctx, base);
  const uint32_t text = c.r1.u32 + 0x700;
  std::memcpy(base + text, name, std::strlen(name) + 1);
  c.r3.u64 = kFiles, c.r4.u64 = text;
  __imp__sub_82256B78(c, base);
  const uint32_t file = c.r3.u32;
  if (file) {
    const bool sall = file < 0xF0000000u && std::memcmp(base + file, "sall", 4) == 0;
    REXLOG_INFO("[moveset] {} at {:08X} ({})", name, file, sall ? "sall" : "NOT a sall file");
    return sall ? file : 0;
  }
  static std::set<std::string> asked;  // each file's load asked for once
  if (asked.insert(name).second) {
    c = Frame(ctx, base);
    std::memcpy(base + text, name, std::strlen(name) + 1);
    c.r3.u64 = kFiles, c.r4.u64 = text, c.r5.u64 = 0;
    __imp__sub_82256DC0(c, base);
    REXLOG_INFO("[moveset] {} not loaded: asked for it (the next cast uses it)", name);
  }
  return 0;
}
}  // namespace

// ---- The mod's own damage (2026-11-19): an enemy's shot takes its damage from its owner's attack data, which Nier
// (the player's class) does not have: his shots hurt no one. The mod tracks its shots and, each frame, hurts an
// enemy a shot touches: Nier's level x nier_moveset_damage, once per shot and enemy.
// The objects: a table at 0x84841B50 (sub_8219ECC0): object i at +8 + i*4, its generation at +16392 + i*4, 4096 of
// them; a handle = {index, generation} (actor+160). A character's status at +3472 (Nier's party: vtable 0x82740048);
// the world matrix at +96 (position row at +144). Damage: sub_822105B8(status, damage, flags, no_damage, &result),
// as sub_821B95B8 (receive an attack) calls it.
REXCVAR_DEFINE_INT32(nier_moveset_damage, 10, "NieR/Mods",
                     "TEST of the moveset mod: the damage of a shot of the mod = Nier's level x this").range(0, 1000);

extern "C" void __imp__sub_822105B8(PPCContext& ctx, uint8_t* base);

namespace {
constexpr uint32_t kObjects = 0x84841B50;
constexpr uint32_t kPartyStatus = 0x82740048;
constexpr uint32_t kSaveLevel = 0x84CC6BA0 + 100;

struct Shot {
  uint32_t index, generation;
  uint32_t attack_id = 0;  // the hit's id (attack+88): the target remembers the last one and takes it once
  std::set<uint32_t> hit;  // the objects it already hurt
  int frames = 0;
};
std::vector<Shot> g_shots;  // game thread only

float F(uint8_t* base, uint32_t a) {
  const uint32_t w = Be32(base + a);
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}
// The object a handle names now (0: gone).
uint32_t Object(uint8_t* base, uint32_t index, uint32_t generation) {
  if (index >= 4096) return 0;
  if (Be32(base + kObjects + 16392 + index * 4) != generation) return 0;
  return Be32(base + kObjects + 8 + index * 4);
}
bool RData(uint32_t p) { return p >= 0x82000000u && p < 0x82100000u; }
}  // namespace

// An object of the table that is an enemy (a character with a status, not of Nier's party).
bool Enemy(uint8_t* base, uint32_t obj) {
  if (!Guest(obj) || obj == kNier || !RData(Be32(base + obj))) return false;
  const uint32_t status = Be32(base + obj + 3472);
  if (!Guest(status)) return false;
  const uint32_t svt = Be32(base + status);
  if (!RData(svt) || svt == kPartyStatus) return false;
  return !(status >= 0x82740000u && status < 0x82741000u);  // the party's status block (Kaine, Emil, Weiss)
}

// The aim (2026-11-19, the user: "the ball goes where it wants"): the nearest enemy in front of Nier (within 25, in a
// 75-degree cone each side), else the nearest around him (within 12). 0: none.
uint32_t AimTarget(uint8_t* base) {
  const float nx = F(base, kNier + 144), ny = F(base, kNier + 148), nz = F(base, kNier + 152);
  const float fx = F(base, kNier + 128), fz = F(base, kNier + 136);  // Nier's forward row (x, z)
  const float fl = std::sqrt(fx * fx + fz * fz);
  uint32_t front = 0, around = 0;
  float front_d = 25.0f * 25.0f, around_d = 12.0f * 12.0f;
  for (uint32_t i = 0; i < 4096; ++i) {
    const uint32_t obj = Be32(base + kObjects + 8 + i * 4);
    if (!Enemy(base, obj)) continue;
    const float dx = F(base, obj + 144) - nx, dy = F(base, obj + 148) - ny, dz = F(base, obj + 152) - nz;
    const float d = dx * dx + dy * dy + dz * dz;
    if (d < 0.01f) continue;
    const float h = std::sqrt(dx * dx + dz * dz);
    const bool ahead = fl > 0.0f && h > 0.0f && (dx * fx + dz * fz) / (fl * h) > 0.26f;  // cos 75
    if (ahead && d < front_d) front_d = d, front = obj;
    if (d < around_d) around_d = d, around = obj;
  }
  return front ? front : around;
}

// The shot's matrix aimed from 'origin' at the target's chest (rows: right, up, forward, position).
void AimMatrix(uint8_t* base, uint32_t matrix, uint32_t target) {
  const float ox = F(base, matrix + 48), oy = F(base, matrix + 52), oz = F(base, matrix + 56);
  float fx = F(base, target + 144) - ox, fy = F(base, target + 148) + 1.0f - oy, fz = F(base, target + 152) - oz;
  const float fl = std::sqrt(fx * fx + fy * fy + fz * fz);
  if (fl < 0.001f) return;
  fx /= fl, fy /= fl, fz /= fl;
  // right = up(0,1,0) x forward, normalised; up = forward x right
  float rx = fz, ry = 0.0f, rz = -fx;
  const float rl = std::sqrt(rx * rx + rz * rz);
  if (rl < 0.001f) return;
  rx /= rl, rz /= rl;
  // The game's handedness, from Nier's own matrix (his right against up x his forward).
  const float nrx = F(base, kNier + 96), nrz = F(base, kNier + 104), nfx = F(base, kNier + 128), nfz = F(base, kNier + 136);
  if (nrx * nfz + nrz * -nfx < 0.0f) rx = -rx, rz = -rz;
  float ux = fy * rz - fz * ry, uy = fz * rx - fx * rz, uz = fx * ry - fy * rx;
  if (uy < 0.0f) ux = -ux, uy = -uy, uz = -uz;
  const float rows[12] = {rx, ry, rz, 0.0f, ux, uy, uz, 0.0f, fx, fy, fz, 0.0f};
  for (int i = 0; i < 12; ++i) {
    uint32_t w;
    std::memcpy(&w, &rows[i], 4);
    SetBe32(base + matrix + uint32_t(i) * 4, w);
  }
}

// A magic of the game's own (2026-11-19: Kaine's, found by her animations' events): sub_8235B958(type, power,
// &owner+160, a 4x4 matrix, 0, 0, side 0, 0), as her handler sub_821EED90 calls it (types 102, 130, 131, 132).
// matrix 0: Nier's own, raised to the chest and aimed at the nearest enemy.
void SpawnGameMagic(const PPCContext& ctx, uint8_t* base, uint32_t type, uint32_t given_matrix);

// The "native" powers (action -2 in poderes.txt, 2026-11-19, the user: the animations did not play): Nier casts his
// own magic (its animation - a body swap's in its place -, its MP), and the first object it makes (sub_821CF800, owner
// Nier) becomes the power's game magic at the same matrix; none within 0.5 s: the power at Nier's chest.
struct NativeCast {
  uint32_t type = 0;
  std::chrono::steady_clock::time_point at;
  bool active = false, fired = false;
};
NativeCast g_cast;  // game thread

// Kaine's own magic, driven by her own code (2026-11-20, "path C"; poderes.txt pattern "kaine:<state>", action -2).
// Kaine010 (vtable 0x8201D088) starts a magic in sub_821EE5F0: her layer-0 motion 26 (_AT_MAGIC^C03^LO), the charge
// effect 5131, then her action state +6556 = 22 + the magic's number. Each frame her +856 (sub_821E7250) runs the
// state's handler, her vtable +952..+972 for states 19..24: 22 sub_821EEA58 (magic 102), 23 sub_821EED90 (charge
// orbs, then magic 130 at a frame of the motion), 24 sub_821EF1D0 (132, then 131). The handlers read only what every
// actor has (the motion player at +2272, the target at +428, the hand bone through vtable +572, the status for the
// power), and the frame of whatever motion plays. Here Nier casts his own magic (its MP, its animation: the body swap
// puts Kaine's c03 there), and while that animation plays her handler runs each frame with r3 = Nier. Nier's own
// magic, made by his animation's events (sub_82245004 -> sub_8235B958 / sub_821CF800 with him as the owner), is left
// out meanwhile; hers passes (g_in_kaine).
struct KaineCast {
  uint32_t state = 0;
  std::chrono::steady_clock::time_point at;
  bool active = false;
  int32_t press_anim = -1;  // Nier's motion when the button went down
  int32_t anim0 = -1;       // the first motion of his magic (its ^S; ^LO and ^E follow)
  int32_t last_anim = -1;
  uint32_t calls = 0;
  // Her magic is out: her handler is not called again (2026-11-20: after the release it called a function Kaine has
  // and Nier has not - a jump to 0 half a second later). Nier's own magic is still left out until his animation ends.
  bool released = false;
};
KaineCast g_kaine;    // game thread
bool g_in_kaine = false;  // inside our call of her handler
bool KaineCastActive() { return g_kaine.active; }

void Track(uint8_t* base, uint32_t shot) {
  if (!Guest(shot)) return;
  // Each shot its own hit id (2026-11-19: all of them id 1, a Shade took the first and let the next pass, as one
  // swing that had hit already). The game's own ids looked small (7, 0x21, 0xEE...): ours run from 0x200 up.
  static uint32_t next_id = 0x200;
  next_id = next_id >= 0x7FF ? 0x200 : next_id + 1;
  Shot s{Be32(base + shot + 160), Be32(base + shot + 164)};
  s.attack_id = next_id;
  g_shots.push_back(std::move(s));
}

// Each frame (game thread, ctx the hooked call's): the mod's shots against the enemies.
// TEMP (2026-11-19, with nier_fxlog): objects that appear in the table, each frame: their class (vtable), where they are
// against Kaine (0x84883BA0) and Nier, and a few words of them; each class's first 4 only. What Kaine's magic makes.
bool KaineCastActive();
void ObjScan(uint8_t* base) {
  // TEMP (2026-11-20): also while a Kaine cast runs, to see what makes Nier's lance (it passes none of the hooks).
  if (!REXCVAR_GET(nier_fxlog) && !KaineCastActive()) return;
  static std::vector<uint32_t> last(4096, 0);
  static std::map<uint32_t, int> per_class;
  static bool primed = false;
  constexpr uint32_t kKaine = 0x84883BA0;
  for (uint32_t i = 0; i < 4096; ++i) {
    const uint32_t obj = Be32(base + kObjects + 8 + i * 4);
    if (obj == last[i]) continue;
    last[i] = obj;
    if (!primed || !Guest(obj)) continue;
    const uint32_t vt = Be32(base + obj);
    if (!RData(vt) || per_class[vt]++ >= 4) continue;
    auto dist = [&](uint32_t o) {
      const float dx = F(base, obj + 144) - F(base, o + 144), dy = F(base, obj + 148) - F(base, o + 148),
                  dz = F(base, obj + 152) - F(base, o + 152);
      return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    std::string words;
    for (uint32_t k = 0; k < 12; ++k) words += fmt::format(" {:08X}", Be32(base + obj + 160 + k * 4));
    REXLOG_INFO("[objeto] novo {:08X} classe {:08X} idx {} a {:.1f} da Kaine, {:.1f} do Nier; +160:{}", obj, vt, i,
                dist(kKaine), dist(kNier), words);
  }
  primed = true;
}

extern "C" void sub_821EEA58(PPCContext& ctx, uint8_t* base);
extern "C" void sub_821EED90(PPCContext& ctx, uint8_t* base);
extern "C" void sub_821EF1D0(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82189238(PPCContext& ctx, uint8_t* base);  // has the motion (r3) ended?

// One frame of a Kaine cast (KaineCast above).
void KaineStep(const PPCContext& ctx, uint8_t* base) {
  KaineCast& k = g_kaine;
  const auto age = std::chrono::steady_clock::now() - k.at;
  const int32_t anim = int32_t(Be32(base + kNier + 2520));
  if (k.anim0 < 0) {
    // His magic's animation starts a frame or two after the press.
    if (anim != k.press_anim) {
      k.anim0 = anim;
      REXLOG_INFO("[kaine] estado {}: animacao do Nier {} (antes {})", k.state, anim, k.press_anim);
    } else if (age > std::chrono::milliseconds(600)) {
      REXLOG_INFO("[kaine] estado {}: o Nier nao comecou a magia", k.state);
      k.active = false;
    }
    if (k.anim0 < 0) return;
  }
  if (age > std::chrono::seconds(8) || anim < k.anim0 || anim > k.anim0 + 2) {
    REXLOG_INFO("[kaine] estado {}: fim (animacao {}, {} quadros)", k.state, anim, k.calls);
    k.active = false;
    return;
  }
  if (anim != k.last_anim) {
    k.last_anim = anim;
    REXLOG_INFO("[kaine] estado {}: animacao {} (quadro {})", k.state, anim, k.calls);
  }
  if (k.released) return;
  // Her handler calls the actor's vtable +572 (a bone's matrix), +216, and +904 when the motion has ended (Kaine's
  // ends her state there). 2026-11-20: Nier's +904 is not hers: the game jumped to 0 as his motion went from 110 to 111.
  // The motion's end is checked here as she does (sub_82189238 of the motion at +2352 when +2348 > 0): no call then.
  const uint32_t vt = Be32(base + kNier);
  if (k.calls == 0)
    REXLOG_INFO("[kaine] vtable do Nier {:08X}: +216 {:08X} +572 {:08X} +904 {:08X}", vt, Be32(base + vt + 216),
                Be32(base + vt + 572), Be32(base + vt + 904));
  if (!Be32(base + vt + 216) || !Be32(base + vt + 572)) {
    REXLOG_WARN("[kaine] o Nier nao tem uma funcao que a magia dela usa: cancelada");
    k.active = false;
    return;
  }
  {
    PPCContext e = Frame(ctx, base);
    e.r3.u64 = int32_t(Be32(base + kNier + 2348)) > 0 ? Be32(base + kNier + 2352) : 0;
    sub_82189238(e, base);
    if (e.r3.u32 & 0xFF) {
      ++k.calls;
      return;  // the motion ended this frame: Nier's own code goes on to the next
    }
  }
  void (*handler)(PPCContext&, uint8_t*) = k.state == 22   ? sub_821EEA58
                                           : k.state == 24 ? sub_821EF1D0
                                                           : sub_821EED90;
  PPCContext p = Frame(ctx, base);
  p.r3.u64 = kNier;
  g_in_kaine = true;
  handler(p, base);
  g_in_kaine = false;
  ++k.calls;
}

void Tick(const PPCContext& ctx, uint8_t* base) {
  ObjScan(base);
  if (g_kaine.active) KaineStep(ctx, base);
  // A native power whose magic made no object of that kind in 0.5 s: the power from Nier's chest.
  if (g_cast.active && !g_cast.fired && std::chrono::steady_clock::now() - g_cast.at > std::chrono::milliseconds(500)) {
    g_cast.fired = true;
    g_cast.active = false;
    SpawnGameMagic(ctx, base, g_cast.type, 0);
  }
  if (g_shots.empty()) return;
  const int level = int(Be32(base + kSaveLevel)) + 1;
  const int damage = std::max(1, level * REXCVAR_GET(nier_moveset_damage));
  for (auto it = g_shots.begin(); it != g_shots.end();) {
    const uint32_t shot = Object(base, it->index, it->generation);
    if (!shot || ++it->frames > 30 * 20) {  // gone, or 20 s
      it = g_shots.erase(it);
      continue;
    }
    const float sx = F(base, shot + 144), sy = F(base, shot + 148), sz = F(base, shot + 152);
    if (it->frames == 10)  // TEMP: is the shot's position at +144 (the actors' row)? Nier's and the +3248 row too
      REXLOG_INFO("[moveset] shot {:08X} at ({:.1f} {:.1f} {:.1f}), +3248 ({:.1f} {:.1f} {:.1f}); Nier ({:.1f} {:.1f} {:.1f})",
                  shot, sx, sy, sz, F(base, shot + 3248), F(base, shot + 3252), F(base, shot + 3256),
                  F(base, kNier + 144), F(base, kNier + 148), F(base, kNier + 152));
    for (uint32_t i = 0; i < 4096; ++i) {
      const uint32_t obj = Be32(base + kObjects + 8 + i * 4);
      if (obj == shot || it->hit.count(obj) || !Enemy(base, obj)) continue;
      const uint32_t status = Be32(base + obj + 3472), svt = Be32(base + status);
      const float dx = F(base, obj + 144) - sx, dz = F(base, obj + 152) - sz;
      const float dy = sy - F(base, obj + 148);  // the shot's height over the enemy's feet
      if (dx * dx + dz * dz > 1.6f * 1.6f || dy < -0.5f || dy > 3.5f) continue;
      it->hit.insert(obj);
      // A REAL hit (2026-11-19): only taking the life (sub_822105B8) left the Shades at 0, never dying: they are
      // script actors, their script decides the death. The game's collision (sub_82328230) calls the target's
      // vtable +548 (sub_821ADF48: target, attack, result, r6, r7 = 0) -> vtable +484 (the class's "hit") ->
      // sub_821BA230 -> vtable +736 (receive) -> sub_821B95B8. The attack as Nier's sword's ("[golpe]" logs 355/357):
      // +0 a matrix, +64/+68 the attacker's handle, +80/+84 kind (2, 1), +88 an id, +92 damage, +96/+100 break and guard
      // damage, +104/+108 rates, +112 the hit's position, +128 its direction, +144/+148 flags (0x10002000, 0x809),
      // +152 the push, +160..+172 1.0, +176 -1.0, +180 -1. The result's +8 is the attack (the defence reads it).
      PPCContext c = Frame(ctx, base);
      const uint32_t result = c.r1.u32 + 0x400, attack = c.r1.u32 + 0x500;
      std::memset(base + result, 0, 16);
      std::memset(base + attack, 0, 256);
      auto put = [&](uint32_t at, float f) {
        uint32_t w;
        std::memcpy(&w, &f, 4);
        SetBe32(base + attack + at, w);
      };
      for (uint32_t k = 0; k < 4; ++k) put(k * 20, 1.0f);  // identity
      SetBe32(base + attack + 64, Be32(base + kNier + 160));
      SetBe32(base + attack + 68, Be32(base + kNier + 164));
      SetBe32(base + attack + 80, 2);
      SetBe32(base + attack + 84, 1);
      SetBe32(base + attack + 88, it->attack_id);
      SetBe32(base + attack + 92, uint32_t(damage));
      put(104, 1.0f);
      put(112, sx), put(116, sy), put(120, sz), put(124, 1.0f);
      {
        float dx = F(base, obj + 144) - sx, dz = F(base, obj + 152) - sz;
        const float l = std::sqrt(dx * dx + dz * dz);
        if (l > 0.001f) dx /= l, dz /= l;
        put(128, dx), put(136, dz);
      }
      SetBe32(base + attack + 144, 0x10002000u);
      SetBe32(base + attack + 148, 0x809u);
      put(152, 1.0f);
      for (uint32_t k = 0; k < 4; ++k) put(160 + k * 4, 1.0f);
      put(176, -1.0f);
      SetBe32(base + attack + 180, 0xFFFFFFFFu);
      SetBe32(base + result + 8, attack);
      c.r3.u64 = obj;
      c.r4.u64 = attack;
      c.r5.u64 = result;
      c.r6.u64 = 0;
      c.r7.u64 = 0;
      const uint32_t hit = Be32(base + Be32(base + obj) + 548);
      if (auto* f = rex::runtime::ResolveIndirectFunction(hit)) f(c, base);
      // The attacker's side of the hit (sub_82328230 after the target's +548): an attacker whose +424 is <= 0 gets its
      // vtable +492 (attacker, target, result, attack): a shot's "it hit", which ends it (2026-11-19: the mod's
      // shots went through). Once per shot, on its first hit.
      bool ended = false;
      if (int32_t(Be32(base + shot + 424)) <= 0 && it->hit.size() == 1) {
        PPCContext e = Frame(ctx, base);
        e.r3.u64 = shot;
        e.r4.u64 = obj;
        e.r5.u64 = result;
        e.r6.u64 = attack;
        const uint32_t on_hit = Be32(base + Be32(base + shot) + 492);
        if (auto* f = rex::runtime::ResolveIndirectFunction(on_hit)) f(e, base), ended = true;
      }
      const int32_t life = ended ? 1 : 0;  // TEMP: 1 = the shot's +492 was called
      REXLOG_INFO("[moveset] shot {:08X} hit {:08X} (vtable {:08X}, status {:08X} vtable {:08X}): {} damage -> {} "
                  "result {:08X}, life {}",
                  shot, obj, Be32(base + obj), status, svt, damage, c.r3.s32, Be32(base + result), life);
      if (ended) break;  // the shot is over: no more targets this frame
    }
    ++it;
  }
}

// TEMP (2026-11-19): a real hit, to copy: sub_821C92B8 (a script actor hit: r3 the actor, r4 the attacker, r5 the
// hit's data) and sub_821B95B8 (receive an attack: r3 the actor, r4 the attack, r5 the result). Each call chain once,
// the structures' first words in hex.
void LogHit(const char* what, const PPCContext& ctx, uint8_t* base, uint32_t data, uint32_t words) {
  static std::mutex mutex;
  static std::set<std::string> seen;
  std::string chain = fmt::format("{:08X}", uint32_t(ctx.lr));
  uint32_t sp = ctx.r1.u32;
  for (int i = 0; i < 6; ++i) {
    const uint32_t up = Be32(base + sp);
    if (up <= sp || up - sp > 0x10000u || (up & 7)) break;
    sp = up;
    chain += fmt::format(" < {:08X}", Be32(base + sp - 8));
  }
  std::lock_guard lock(mutex);
  if (seen.size() > 60 || !seen.insert(std::string(what) + chain).second) return;
  std::string dump;
  for (uint32_t i = 0; Guest(data) && i < words; ++i) dump += fmt::format("{}{:08X}", i % 8 ? " " : " | ", Be32(base + data + i * 4));
  REXLOG_INFO("[golpe] {} actor {:08X} (vt {:08X}) r4 {:08X} (vt {:08X}) r5 {:08X} por {}{}", what, ctx.r3.u32,
              Guest(ctx.r3.u32) ? Be32(base + ctx.r3.u32) : 0, ctx.r4.u32, Guest(ctx.r4.u32) ? Be32(base + ctx.r4.u32) : 0,
              ctx.r5.u32, chain, dump);
}

// The magic replaced, not added (2026-11-19). Skipping Nier's lance action (sub_82380B00(r25, 222) after the MP) did not
// stop the lance (the user: "the lance came out as always"). So the magic's own code does not see it: the magics'
// dispatcher (sub_822396D8, r3 = Nier) runs with the button's slot (save 0x84CC6BA0 + 160 + button*4) emptied, and
// the mod casts in its place: the press as the dispatcher reads it (sub_82103CF0(0x827CD42C, mask at 0x826BD28C +
// button*4, 0, -1)), the cost sub_82215DC0(Nier, magic) against the MP sub_8237DB90(Nier+4764), spent with
// sub_8237DC10(Nier+4764, -cost), then the pattern.
REXCVAR_DEFINE_BOOL(nier_moveset_replace, true, "NieR/Mods",
                    "TEST of the moveset mod: the test shot replaces Dark Lance (off: both come out)");
// Nier's action played with a power (2026-11-19, the user: "Kaine's power plays her animation"): sub_82380B00(the
// player's action state 0x84D92CB8, the action), the call that starts the magics' own (221 Dark Blast, 222 Dark Lance:
// its animations at_magic^spear, Kaine's magic in the body swap). -1: none.
REXCVAR_DEFINE_INT32(nier_moveset_action, -1, "NieR/Mods",
                     "TEST of the moveset mod: Nier's action played with the power (-1 none, 222 Dark Lance's)")
    .range(-1, 4096);
constexpr uint32_t kActionState = 0x84D92CB8;

void TestSpawn(const PPCContext& ctx, uint8_t* base);
// From the MP hook (mods.cpp) as Dark Lance is cast: the old way (both come out), when not replacing.
void TestSpawnFromCast(const PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(nier_moveset_replace)) TestSpawn(ctx, base);
}

void SpawnPattern(const PPCContext& ctx, uint8_t* base, std::string lib, const std::string& pattern);
void TestSpawn(const PPCContext& ctx, uint8_t* base) {
  SpawnPattern(ctx, base, REXCVAR_GET(nier_moveset_file), REXCVAR_GET(nier_moveset_test));
}

// A body mod's powers (2026-11-19, the user: "a Kaine 100% in Nier's place"): MODS\<mod>\poderes.txt, one line per magic
// of Nier's it replaces: "<magic> <library> <pattern> <action>", magic by name (blast phantasm hand lance whirlwind
// gluttony wall execution) or ability number (3..10), action = Nier's action played with it (-1 none; 221 Dark Hand's,
// 222 Dark Lance's, 223 Dark Whirlwind's, 225 Dark Wall's: their animations, a body swap's magic in their place).
// The first enabled mod (ModDirs order) with the file wins; read once.
struct ModPower {
  std::string lib, pattern;
  int32_t action = -1;
  int32_t effect = -1;  // an effect of the game's (EFFECT number) on Nier as the power goes: Kaine's magic's 4170, 5101
};
const std::map<uint32_t, ModPower>& ModPowers() {
  static std::map<uint32_t, ModPower> table;
  static bool loaded = false;
  // 2026-11-20: read again when the file changes (checked once a second), to try powers while the game runs.
  static std::filesystem::path file;
  static std::filesystem::file_time_type stamp{};
  static auto next_check = std::chrono::steady_clock::time_point{};
  if (loaded) {
    const auto now = std::chrono::steady_clock::now();
    if (file.empty() || now < next_check) return table;
    next_check = now + std::chrono::seconds(1);
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(file, ec);
    if (ec || t == stamp) return table;
    table.clear();
    REXLOG_INFO("[poderes] {} mudou: lido de novo", file.string());
  }
  loaded = true;
  static const std::map<std::string, uint32_t> kNames = {{"blast", 3},     {"phantasm", 4},  {"hand", 5},
                                                         {"lance", 6},     {"whirlwind", 7}, {"gluttony", 8},
                                                         {"wall", 9},      {"execution", 10}};
  for (const auto& dir : nier::text::ModDirs()) {
    std::ifstream in(dir / "poderes.txt");
    if (!in) continue;
    file = dir / "poderes.txt";
    {
      std::error_code ec;
      stamp = std::filesystem::last_write_time(file, ec);
    }
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty() || line[0] == '#') continue;
      std::istringstream ls(line);
      std::string magic, lib, pattern;
      int32_t action = -1, effect = -1;
      if (!(ls >> magic >> lib >> pattern)) continue;
      ls >> action >> effect;
      uint32_t id = 0;
      std::string low = magic;
      for (char& ch : low) ch = char(std::tolower(uint8_t(ch)));
      if (auto it = kNames.find(low); it != kNames.end()) id = it->second;
      else id = uint32_t(std::atoi(magic.c_str()));
      if (id < 3 || id > 10) continue;
      table[id] = {lib, pattern, action, effect};
      REXLOG_INFO("[poderes] {}: magia {} -> {} {} (acao {})", dir.filename().string(), id, lib, pattern, action);
    }
    break;
  }
  return table;
}

void SpawnPattern(const PPCContext& ctx, uint8_t* base, std::string lib, const std::string& pattern) {
  if (pattern.empty() || pattern.size() > 60) return;
  // Patterns that crash from Nier (2026-11-19, the user: a read of 0x4 a few frames after the shot is made): they need
  // what their usual owner has (Weiss's hand, a carrier's own shots).
  for (const char* bad : {"shot_hand", "carrier"})
    if (pattern == bad) {
      static bool told = false;
      if (!told) {
        told = true;
        REXLOG_WARN("[moveset] pattern {} crashes from Nier: not fired", pattern);
      }
      return;
    }
  if (lib.empty() || lib.size() > 60) lib = "book.bin";
  const uint32_t file = ShotFile(ctx, base, lib.c_str());
  if (!file) return;
  PPCContext c = Frame(ctx, base);
  const uint32_t matrix = c.r1.u32 + 0x600;
  std::memcpy(base + matrix, base + kNier + 96, 64);  // Nier's world matrix
  {  // a little above his feet (the position row's y)
    const uint32_t yb = Be32(base + matrix + 52);
    float y;
    std::memcpy(&y, &yb, 4);
    y += 1.2f;
    uint32_t out;
    std::memcpy(&out, &y, 4);
    SetBe32(base + matrix + 52, out);
  }
  // The aim: at the nearest enemy ahead (else around); the target goes to the shot too (homing patterns follow it).
  const uint32_t target = AimTarget(base);
  if (target) AimMatrix(base, matrix, target);
  base[c.r1.u32 + 87] = 1;
  const uint64_t handle = uint64_t(Be32(base + kNier + 160)) << 32 | Be32(base + kNier + 164);
  const uint32_t name = c.r1.u32 + 0x780;  // the pattern's name, in our frame
  std::memcpy(base + name, pattern.c_str(), pattern.size() + 1);
  c.r3.u64 = name;
  c.r4.u64 = file;
  c.r5.u64 = matrix;
  c.r6.u64 = handle;
  c.r7.u64 = REXCVAR_GET(nier_moveset_target) == "self" ? kNier : target;
  c.r8.u64 = handle;
  c.r10.u64 = 0;
  c.f1.f64 = 0.0;
  __imp__sub_8236D728(c, base);
  if (const uint32_t shot = c.r3.u32) {  // native 37's last step: the shot's script starts
    PPCContext s = Frame(ctx, base);
    s.r3.u64 = shot + 2676;
    s.r4.u64 = 0;
    __imp__sub_821C80D0(s, base);
    c.r3.u64 = shot;
    Track(base, shot);
  }
  REXLOG_INFO("[moveset] pattern {} from {} ({:08X}): shot {:08X}, aimed at {:08X}", pattern, lib, file, c.r3.u32,
              target);
}
void SpawnGameMagic(const PPCContext& ctx, uint8_t* base, uint32_t type, uint32_t given_matrix) {
  PPCContext m = Frame(ctx, base);
  uint32_t matrix = given_matrix;
  if (!matrix) {
    matrix = m.r1.u32 + 0x600;
    std::memcpy(base + matrix, base + kNier + 96, 64);
    const float y = F(base, matrix + 52) + 1.2f;
    uint32_t yw;
    std::memcpy(&yw, &y, 4);
    SetBe32(base + matrix + 52, yw);
    if (const uint32_t target = AimTarget(base)) AimMatrix(base, matrix, target);
  }
  const int level = int(Be32(base + 0x84CC6BA0 + 100)) + 1;
  m.r3.u64 = type;
  m.f1.f64 = double(std::max(1, level * REXCVAR_GET(nier_moveset_damage)));
  m.r5.u64 = kNier + 160;
  m.r6.u64 = matrix;
  m.r7.u64 = 0;
  m.r8.u64 = 0;
  m.r9.u64 = 0;
  m.r10.u64 = 0;
  __imp__sub_8235B958(m, base);
  REXLOG_INFO("[poderes] magia {} do jogo (forca {:g}, matriz {})", type, m.f1.f64, given_matrix ? "do Nier" : "mirada");
}
}  // namespace nier::moveset

// TEMP (2026-11-19): Nier's actions as they start, each new one once with its state object: which number is the
// plunge's ground strike (the user's "shockwave" power), the other magics...
extern "C" void __imp__sub_82380B00(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(sub_82380B00) {
  {
    static std::mutex mutex;
    static std::set<uint64_t> seen;
    std::lock_guard lock(mutex);
    if (seen.size() < 600 && seen.insert(uint64_t(ctx.r3.u32) << 32 | ctx.r4.u32).second)
      REXLOG_INFO("[acao] {} (estado {:08X}, por {:08X})", ctx.r4.s32, ctx.r3.u32, uint32_t(ctx.lr));
  }
  __imp__sub_82380B00(ctx, base);
}

// The magics' dispatcher (each frame in play, r3 = Nier): the buttons holding the replaced magic are cast by the mod.
extern "C" void __imp__sub_822396D8(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_821AF900(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_821CD318(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82215AE0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_82215DC0(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__sub_8237DB90(PPCContext& ctx, uint8_t* base);
extern "C" void sub_8237DC10(PPCContext& ctx, uint8_t* base);
extern "C" void sub_82103CF0(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(sub_822396D8) {
  using namespace nier::moveset;
  constexpr uint32_t kSlots = 0x84CC6BA0 + 160, kMasks = 0x826BD28C, kPad = 0x827CD42C, kLance = 6;
  const uint32_t nier = ctx.r3.u32;
  // The magics replaced: a body mod's (poderes.txt), else the test panel's (Dark Lance, nier_moveset_*).
  const auto& powers = ModPowers();
  const bool test = powers.empty() && REXCVAR_GET(nier_moveset_replace) && !REXCVAR_GET(nier_moveset_test).empty();
  uint32_t hidden[4] = {};  // the ability hidden on each button (0: none)
  bool any = false;
  if (nier == kNier)
    for (uint32_t b = 0; b < 4; ++b) {
      const uint32_t ability = Be32(base + kSlots + b * 4);
      auto it = powers.find(ability);
      if (it != powers.end() && it->second.action == -2) {
        // Native: the game casts it (animation, MP); its first object becomes the power (sub_821CF800's hook).
        PPCContext p = Frame(ctx, base);
        p.r3.u64 = kPad;
        p.r4.u64 = Be32(base + kMasks + b * 4);
        p.r5.u64 = 0;
        p.r6.u64 = 0xFFFFFFFFFFFFFFFFull;
        sub_82103CF0(p, base);
        if ((p.r3.u32 & 0xFF) && it->second.pattern.rfind("magia:", 0) == 0) {
          g_cast.type = uint32_t(std::atoi(it->second.pattern.c_str() + 6));
          g_cast.at = std::chrono::steady_clock::now();
          g_cast.active = true;
          g_cast.fired = false;
        } else if ((p.r3.u32 & 0xFF) && it->second.pattern.rfind("kaine:", 0) == 0 && !g_kaine.active) {
          // Her own magic, by her code (KaineCast).
          g_kaine = KaineCast{};
          g_kaine.state = uint32_t(std::atoi(it->second.pattern.c_str() + 6));
          if (g_kaine.state < 22 || g_kaine.state > 24) g_kaine.state = 23;
          g_kaine.at = std::chrono::steady_clock::now();
          g_kaine.press_anim = int32_t(Be32(base + kNier + 2520));
          g_kaine.active = true;
          REXLOG_INFO("[kaine] botao {}: estado {}", b, g_kaine.state);
        }
        continue;
      }
      if (it != powers.end() || (test && ability == kLance)) hidden[b] = ability, any = true;
    }
  if (!any) {
    __imp__sub_822396D8(ctx, base);
    return;
  }
  // The game's own gate (Nier can cast now), as the dispatcher's start.
  PPCContext g = Frame(ctx, base);
  g.r3.u64 = nier;
  __imp__sub_82215AE0(g, base);
  const bool can_cast = (g.r3.u32 & 0xFF) != 0;
  for (uint32_t b = 0; b < 4 && can_cast; ++b) {
    if (!hidden[b]) continue;
    PPCContext p = Frame(ctx, base);
    p.r3.u64 = kPad;
    p.r4.u64 = Be32(base + kMasks + b * 4);
    p.r5.u64 = 0;
    p.r6.u64 = 0xFFFFFFFFFFFFFFFFull;
    sub_82103CF0(p, base);
    if ((p.r3.u32 & 0xFF) == 0) continue;
    const uint32_t ch = Be32(base + nier + 4764);
    PPCContext k = Frame(ctx, base);
    k.r3.u64 = nier;
    k.r4.u64 = hidden[b];
    __imp__sub_82215DC0(k, base);
    const double cost = k.f1.f64;
    PPCContext m = Frame(ctx, base);
    m.r3.u64 = ch;
    __imp__sub_8237DB90(m, base);
    if (cost > m.f1.f64) continue;  // not enough MP
    PPCContext s = Frame(ctx, base);
    s.r3.u64 = ch;
    s.f1.f64 = -cost;
    sub_8237DC10(s, base);  // through its hook: the mods see the MP spent
    int32_t action = REXCVAR_GET(nier_moveset_action);
    if (auto it = powers.find(hidden[b]); it != powers.end()) {
      if (it->second.pattern.rfind("magia:", 0) == 0) {
        SpawnGameMagic(ctx, base, uint32_t(std::atoi(it->second.pattern.c_str() + 6)), 0);
      } else if (it->second.pattern != "-") {
        SpawnPattern(ctx, base, it->second.lib, it->second.pattern);
      }
      action = it->second.action;
      if (it->second.effect >= 0) {  // sub_821AF900(actor, effect, -2, 0, 1.0, 1.0): as Kaine's magic (sub_821F02C0) makes hers
        PPCContext e = Frame(ctx, base);
        e.r3.u64 = kNier;
        e.r4.u64 = uint32_t(it->second.effect);
        e.r5.u64 = 0xFFFFFFFFFFFFFFFEull;
        e.r6.u64 = 0;
        e.f1.f64 = 1.0;
        e.f2.f64 = 1.0;
        __imp__sub_821AF900(e, base);
        REXLOG_INFO("[poderes] efeito {} no Nier", it->second.effect);
      }
    } else {
      TestSpawn(ctx, base);
    }
    if (action >= 1000) {
      // 2026-11-19: an animation of Nier's table (0x826C435C, 33-byte names; the body swap's in its place) played
      // straight: sub_821CD318(actor, layer 0, motion, 0, 1.0, 0, 1, 2), as the save's restore plays one. 1105 = at_magic^
      // spear^s (Kaine's c03), 1110 = slash^s (c01), 1112 = hand^s (c04).
      PPCContext a = Frame(ctx, base);
      a.r3.u64 = kNier;
      a.r4.u64 = 0;
      a.r5.u64 = uint32_t(action - 1000);
      a.f1.f64 = 0.0;
      a.f2.f64 = 1.0;
      a.f3.f64 = 0.0;
      a.r7.u64 = 1;
      a.r8.u64 = 2;
      __imp__sub_821CD318(a, base);
      REXLOG_INFO("[poderes] animacao {} no Nier", action - 1000);
    } else if (action >= 0) {
      PPCContext a = Frame(ctx, base);
      a.r3.u64 = kActionState;
      a.r4.u64 = uint32_t(action);
      __imp__sub_82380B00(a, base);
    }
  }
  for (uint32_t b = 0; b < 4; ++b)
    if (hidden[b]) SetBe32(base + kSlots + b * 4, 0);
  __imp__sub_822396D8(ctx, base);
  for (uint32_t b = 0; b < 4; ++b)
    if (hidden[b]) SetBe32(base + kSlots + b * 4, hidden[b]);
}

// sub_821DB288(r3 = the actor, r4 = the native, r5 = its float arguments, r6 = their count, r7 = the script's strings):
// the actor natives. 37/38 make a shot and then register it with the actor (its vtable +816: the attack, the side):
// that virtual and the arguments, once per actor class.
extern "C" void __imp__sub_821DB288(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(sub_821DB288) {
  FxLog("ator", ctx, base);
  const int32_t id = ctx.r4.s32;
  if (id == 37 || id == 38) {
    static std::mutex mutex;
    static std::set<uint32_t> seen;
    const uint32_t vt = Be32(base + ctx.r3.u32);
    std::lock_guard lock(mutex);
    if (Guest(vt) && seen.size() < 64 && seen.insert(vt).second) {
      std::string args;
      const int32_t n = std::min<int32_t>(ctx.r6.s32, 16);
      for (int32_t i = 0; i < n; ++i) {
        const uint32_t w = Be32(base + ctx.r5.u32 + uint32_t(i) * 4);
        float f;
        std::memcpy(&f, &w, 4);
        args += fmt::format(" {:g}", f);
      }
      REXLOG_INFO("[bullet] actor native {} by {:08X} (vtable {:08X}, +816 {:08X}, +8296 {:08X}) args{}", id, ctx.r3.u32,
                  vt, Be32(base + vt + 816), Be32(base + ctx.r3.u32 + 8296), args);
    }
  }
  __imp__sub_821DB288(ctx, base);
}

extern "C" void __imp__sub_821C62A0(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(sub_821C62A0) {
  {
    static std::mutex mutex;
    static std::set<std::string> seen;
    const std::string name = ScriptName(base, ctx.r4.u32);
    // The callers above (back chain at 0(r1), each frame's LR at -8 of the caller's frame): who made the object.
    std::string chain = fmt::format("{:08X}", uint32_t(ctx.lr));
    uint32_t sp = ctx.r1.u32;
    for (int i = 0; i < 8; ++i) {
      const uint32_t up = Be32(base + sp);
      if (up <= sp || up - sp > 0x10000u || (up & 7)) break;
      sp = up;
      chain += fmt::format(" < {:08X}", Be32(base + sp - 8));
    }
    std::lock_guard lock(mutex);
    if (seen.size() < 4000 && seen.insert(name + chain).second)
      REXLOG_INFO("[bullet] script {} (object {:08X}) por {}", name.empty() ? "?" : name, ctx.r3.u32, chain);
  }
  __imp__sub_821C62A0(ctx, base);
}

// Dark Blast's own update (sub_8221F458, from sub_82243010 like the dispatcher) reads the slots too: the replaced
// magics are hidden from it as well (no Blast of the game's next to the mod's power).
extern "C" void __imp__sub_8221F458(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(sub_8221F458) {
  constexpr uint32_t kSlots = 0x84CC6BA0 + 160;
  const auto& powers = nier::moveset::ModPowers();
  uint32_t hidden[4] = {};
  for (uint32_t b = 0; b < 4 && !powers.empty(); ++b) {
    const uint32_t ability = Be32(base + kSlots + b * 4);
    if (powers.count(ability)) hidden[b] = ability, nier::moveset::SetBe32(base + kSlots + b * 4, 0);
  }
  __imp__sub_8221F458(ctx, base);
  for (uint32_t b = 0; b < 4; ++b)
    if (hidden[b]) nier::moveset::SetBe32(base + kSlots + b * 4, hidden[b]);
}

// TEMP (2026-11-19, with nier_fxlog): every effect an actor makes (sub_821AF900: r3 the actor, r4 the effect), once per
// (actor's class, effect, caller): Kaine's magic (sub_821F02C0: 4170, 5101) and Emil's, as they cast with the party.
REX_HOOK_RAW(sub_821AF900) {
  // TEMP (2026-11-20): during a Kaine cast every effect on Nier, with its caller (the lance he still shows is one?).
  // 2026-11-20 (nier_396): his Dark Lance in his hand is effect 5101, made every frame by his code (822361F0) while
  // the button is held: Nier's own effects on him are left out during her cast; hers (made inside her handler) stay.
  if (nier::moveset::KaineCastActive() && ctx.r3.u32 == nier::moveset::kNier) {
    if (!nier::moveset::g_in_kaine) {
      static uint32_t told = 0;
      if (told++ < 6)
        REXLOG_INFO("[kaine] efeito {} do Nier deixado de fora (por {:08X})", ctx.r4.s32, uint32_t(ctx.lr));
      return;
    }
    REXLOG_INFO("[kaine] efeito {} dela no Nier", ctx.r4.s32);
  }
  if (REXCVAR_GET(nier_fxlog)) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    const uint32_t actor = ctx.r3.u32;
    const uint32_t vt = Guest(actor) ? Be32(base + actor) : 0;
    const std::string key = fmt::format("{:08X} {} {:08X}", vt, ctx.r4.s32, uint32_t(ctx.lr));
    std::lock_guard lock(mutex);
    if (seen.size() < 3000 && seen.insert(key).second)
      REXLOG_INFO("[efeito] ator {:08X} (vt {:08X}) efeito {} r5 {} r6 {} f1 {:g} f2 {:g} por {:08X}", actor, vt,
                  ctx.r4.s32, ctx.r5.s32, ctx.r6.s32, ctx.f1.f64, ctx.f2.f64, uint32_t(ctx.lr));
  }
  __imp__sub_821AF900(ctx, base);
}

// TEMP (2026-11-19, with nier_fxlog): who makes the magic bullets (class 8201BC20, sub_821CF800) and class 8203B508
// (sub_82365FB0) - the objects seen at Kaine's side as she cast: the call chain (8 levels) and the owner (r5 -> +160),
// each chain once.
namespace {
std::string Chain(const PPCContext& ctx, const uint8_t* base) {
  std::string chain = fmt::format("{:08X}", uint32_t(ctx.lr));
  uint32_t sp = ctx.r1.u32;
  for (int i = 0; i < 8; ++i) {
    const uint32_t up = Be32(base + sp);
    if (up <= sp || up - sp > 0x10000u || (up & 7)) break;
    sp = up;
    chain += fmt::format(" < {:08X}", Be32(base + sp - 8));
  }
  return chain;
}
void LogMaker(const char* what, const PPCContext& ctx, const uint8_t* base) {
  if (!REXCVAR_GET(nier_fxlog)) return;
  static std::mutex mutex;
  static std::set<std::string> seen;
  const std::string chain = Chain(ctx, base);
  std::lock_guard lock(mutex);
  if (seen.size() < 200 && seen.insert(std::string(what) + chain).second)
    REXLOG_INFO("[magia-obj] {} r3 {:08X} r4 {:08X} r5 {:08X} (dono {:08X}{:08X}) r6 {} r8 {} f1 {:g} por {}", what,
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, Guest(ctx.r5.u32) ? Be32(base + ctx.r5.u32) : 0,
                Guest(ctx.r5.u32) ? Be32(base + ctx.r5.u32 + 4) : 0, ctx.r6.s32, ctx.r8.s32, ctx.f1.f64, chain);
}
}  // namespace
extern "C" void __imp__sub_821CF800(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(sub_821CF800) {
  LogMaker("bala 8201BC20", ctx, base);
  using namespace nier::moveset;
  // A native power's cast: Nier's magic object (owner Nier) is the power, at the same matrix; the rest of its objects
  // in the next 0.6 s are left out (Dark Hand makes several).
  // A Kaine cast: Nier's own visual shots are left out (hers have no owner).
  if (g_kaine.active && !g_in_kaine && Guest(ctx.r5.u32) && Be32(base + ctx.r5.u32) == Be32(base + kNier + 160) &&
      Be32(base + ctx.r5.u32 + 4) == Be32(base + kNier + 164)) {
    ctx.r3.u64 = 0;
    return;
  }
  if (g_cast.active && ctx.r5.u32 == kNier + 160) {
    const auto age = std::chrono::steady_clock::now() - g_cast.at;
    if (age < std::chrono::milliseconds(2500)) {
      if (!g_cast.fired) {
        g_cast.fired = true;
        g_cast.at = std::chrono::steady_clock::now() - std::chrono::milliseconds(1900);  // 0.6 s more of leaving out
        SpawnGameMagic(ctx, base, g_cast.type, ctx.r4.u32);
      }
      ctx.r3.u64 = 0;
      return;
    }
    g_cast.active = false;
  }
  __imp__sub_821CF800(ctx, base);
}
// A Kaine cast (KaineCast): the magic Nier's animation events make (owner Nier, r5 = &owner + 160) is left out; her
// handler's, called by us, and SpawnGameMagic's (the __imp__ directly) pass. Its callers do not use the result.
REX_HOOK_RAW(sub_8235B958) {
  using namespace nier::moveset;
  // The owner by its handle (2026-11-20: Nier's own calls pass a copy of it, not &Nier + 160).
  const bool nier_owner = Guest(ctx.r5.u32) && Be32(base + ctx.r5.u32) == Be32(base + kNier + 160) &&
                          Be32(base + ctx.r5.u32 + 4) == Be32(base + kNier + 164);
  if (g_kaine.active && !g_in_kaine && nier_owner) {
    static uint32_t told = 0;
    if (told++ < 8) REXLOG_INFO("[kaine] magia {} do Nier deixada de fora", ctx.r3.u32);
    ctx.r3.u64 = 0;
    return;
  }
  if (g_in_kaine) {
    // Her power comes from her status (sub_82210A48: 0 on Nier's): the mod's, as SpawnGameMagic's.
    if (ctx.f1.f64 <= 0.0) {
      const int level = int(Be32(base + 0x84CC6BA0 + 100)) + 1;
      ctx.f1.f64 = double(std::max(1, level * REXCVAR_GET(nier_moveset_damage)));
    }
    g_kaine.released = true;
    REXLOG_INFO("[kaine] magia {} da Kaine (forca {:g})", ctx.r3.u32, ctx.f1.f64);
  }
  __imp__sub_8235B958(ctx, base);
}
extern "C" void __imp__sub_82365FB0(PPCContext& ctx, uint8_t* base);
REX_HOOK_RAW(sub_82365FB0) {
  LogMaker("objeto 8203B508", ctx, base);
  __imp__sub_82365FB0(ctx, base);
}