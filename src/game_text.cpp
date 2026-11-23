// nier - game text languages and translation folders (see game_text.h).

#include "game_text.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "game_assets.h"

namespace rex::graphics {
void SetGuestTextureOverride(const std::vector<uint8_t>* (*fn)(uint64_t key));  // SDK: texture replacement hook
// SDK: shader replacement hook (type 0 vertex / 1 pixel, ucode hash, modification) -> file bytes.
void SetGuestShaderOverride(const std::vector<uint8_t>* (*fn)(uint32_t type, uint64_t hash, uint64_t modification));
}
namespace rex::mods {
// SDK: called after each file read of the game (path, byte offset, bytes asked, bytes read, the
// buffer); returns how many bytes of the buffer are valid (more than read: served past the file's end).
void SetFileReadHook(uint32_t (*fn)(const char* path, uint64_t offset, uint32_t length, uint32_t got,
                                    uint8_t* buffer));
// SDK: the size the game is told for a file (path, size of the host file).
void SetFileSizeHook(uint64_t (*fn)(const char* path, uint64_t size));
}

REXCVAR_DEFINE_STRING(nier_language, "English", "Nier",
                      "Game text language: English, Francais, Deutsch, or a folder inside TEXT");

namespace nier::text {
namespace {

namespace fs = std::filesystem;

struct State {
  std::mutex mutex;
  fs::path root;
  std::vector<Language> languages;  // disc languages, then one per folder
  std::string current = "English";
  std::map<std::string, Blob> cache;                    // "<language>|<game path>"
  std::map<std::string, std::map<std::string, fs::path>> index;  // folder -> lowercase relative path
  std::map<uint64_t, Blob> texmod;                      // texture key -> replacement (null: none)
  std::map<std::string, Blob> shadermod;                // "type_hash_modification" -> replacement (null: none)
  struct Redirect {
    std::string cpk;  // lowercase file name, e.g. "layer1.cpk"
    uint64_t offset = 0, size = 0;
    fs::path file;
    Blob data;
  };
  struct Patch {
    std::string cpk;
    uint64_t offset = 0;
    std::vector<uint8_t> bytes;
  };
  std::vector<Patch> patches;       // bytes of a pack overwritten as the game reads them (its table)
  std::vector<Redirect> redirects;  // byte ranges of the game's packs served from a mod's files
  bool redirects_scanned = false;
};
std::atomic<bool> g_has_redirects{false};

State& S() {
  static State s;
  return s;
}

std::atomic<int> g_override{0};
std::atomic<bool> g_reload{false};

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// "Text\\Talk_Text/Talk_A.en" -> "text/talk_text/talk_a.en"
std::string GameKey(const std::string& path) {
  std::string key = Lower(path);
  for (char& c : key) {
    if (c == '\\') c = '/';
  }
  while (!key.empty() && key.front() == '/') key.erase(key.begin());
  return key;
}

std::vector<Language> DiscLanguages() {
  return {{"English", 2, {}}, {"Francais", 3, {}}, {"Deutsch", 4, {}}};
}

// Rebuilds the language list from the TEXT folder. Caller holds the lock.
void ScanLocked(State& s) {
  s.languages = DiscLanguages();
  std::error_code ec;
  if (s.root.empty() || !fs::is_directory(s.root, ec)) return;
  std::vector<Language> folders;
  for (const auto& entry : fs::directory_iterator(s.root, ec)) {
    if (!entry.is_directory(ec)) continue;
    std::string name = entry.path().filename().string();
    std::string lower = Lower(name);
    if (lower == "example" || name.empty() || name[0] == '.' || name[0] == '_') continue;
    bool taken = false;
    for (const Language& l : s.languages) taken = taken || Lower(l.name) == lower;
    if (!taken) folders.push_back({name, 2, entry.path()});
  }
  std::sort(folders.begin(), folders.end(),
            [](const Language& a, const Language& b) { return Lower(a.name) < Lower(b.name); });
  s.languages.insert(s.languages.end(), folders.begin(), folders.end());
}

const Language* FindLocked(const State& s, const std::string& name) {
  for (const Language& l : s.languages) {
    if (Lower(l.name) == Lower(name)) return &l;
  }
  return nullptr;
}

// Sets the active language (falls back to English). Caller holds the lock.
void ApplyLocked(State& s, std::string name) {  // by value: the scan rebuilds the list
  ScanLocked(s);
  const Language* l = FindLocked(s, name);
  if (!l) {
    REXLOG_WARN("Idioma '{}' não encontrado; usando English", name);
    l = &s.languages.front();
  }
  s.current = l->name;
  s.cache.clear();
  g_override = l->game_code;
}

// Editors add a BOM. Line breaks stay as they are: records end in CRLF, and a
// message with several lines has bare LFs inside its cell.
std::vector<uint8_t> Normalize(const std::vector<uint8_t>& in) {
  const bool bom = in.size() >= 3 && in[0] == 0xEF && in[1] == 0xBB && in[2] == 0xBF;
  return std::vector<uint8_t>(in.begin() + (bom ? 3 : 0), in.end());
}

bool ReadHostFile(const fs::path& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

// 2026-11-19 (a body mod's own names: Kaine's powers in the magic menu): MODS\<mod>\texto.txt, lines "<id><TAB><text>",
// replace the game text of that id in NIER_TEXT.TXD (its records: id, ja, en, fr, de; a translation's text is in the
// en column). The enabled mods, first in ModDirs order wins. Caller holds the lock.
void ApplyModTextLocked(State& s, std::vector<uint8_t>& txd) {
  std::map<std::string, std::string> over;
  std::error_code ec;
  std::vector<fs::path> mods;
  for (const auto& entry : fs::directory_iterator(s.root.parent_path() / "MODS", ec)) {
    const std::string name = entry.path().filename().string();
    if (entry.is_directory(ec) && !name.empty() && name[0] != '_' && name[0] != '.') mods.push_back(entry.path());
  }
  std::sort(mods.begin(), mods.end(), std::greater<>());
  for (const fs::path& dir : mods) {
    std::ifstream in(dir / "texto.txt", std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.size() >= 3 && uint8_t(line[0]) == 0xEF) line.erase(0, 3);  // BOM
      const size_t tab = line.find('\t');
      if (line.empty() || line[0] == '#' || tab == std::string::npos) continue;
      over.emplace(line.substr(0, tab), line.substr(tab + 1));  // first mod wins
    }
  }
  if (over.empty()) return;
  std::string text(txd.begin(), txd.end()), out;
  out.reserve(text.size() + 1024);
  size_t pos = 0, done = 0;
  while (pos < text.size()) {
    size_t end = text.find("\r\n", pos);
    if (end == std::string::npos) end = text.size();
    std::string rec = text.substr(pos, end - pos);
    const size_t t0 = rec.find('\t');
    if (t0 != std::string::npos) {
      auto it = over.find(rec.substr(0, t0));
      const size_t t1 = rec.find('\t', t0 + 1);
      const size_t t2 = t1 == std::string::npos ? std::string::npos : rec.find('\t', t1 + 1);
      if (it != over.end() && t2 != std::string::npos) {
        rec = rec.substr(0, t1 + 1) + it->second + rec.substr(t2);
        ++done;
      }
    }
    out += rec;
    if (end < text.size()) out += "\r\n";
    pos = end + 2;
  }
  txd.assign(out.begin(), out.end());
  REXLOG_INFO("Textos: {} linhas trocadas por texto.txt de mods", done);
}

// The folder's files by lowercase relative path. Caller holds the lock.
const std::map<std::string, fs::path>& IndexLocked(State& s, const fs::path& dir) {
  auto [it, fresh] = s.index.try_emplace(dir.string());
  if (fresh) {
    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
      if (!entry.is_regular_file(ec)) continue;
      it->second[GameKey(fs::relative(entry.path(), dir, ec).generic_string())] = entry.path();
    }
  }
  return it->second;
}

// Texture replacement mods: MODS\<mod>\TEXMOD\<16 hex digits>.bin holds the raw bytes of a
// texture (as in the game's pixel packs) to put over the one with that key (see the SDK's
// GuestTextureKey). Called by the GPU for every texture it loads.
const std::vector<uint8_t>* TexOverride(uint64_t key) {
  State& s = S();
  std::lock_guard lock(s.mutex);
  auto it = s.texmod.find(key);
  if (it == s.texmod.end()) {
    char hex[48];
    std::snprintf(hex, sizeof hex, "texmod/%016llx.bin", static_cast<unsigned long long>(key));
    Blob blob;
    std::error_code ec;
    std::vector<fs::path> mods;
    for (const auto& entry : fs::directory_iterator(s.root.parent_path() / "MODS", ec)) {
      const std::string name = entry.path().filename().string();
      if (entry.is_directory(ec) && !name.empty() && name[0] != '_' && name[0] != '.') mods.push_back(entry.path());
    }
    std::sort(mods.begin(), mods.end(), std::greater<>());
    for (const fs::path& dir : mods) {
      const auto& index = IndexLocked(s, dir);
      auto f = index.find(hex);
      std::vector<uint8_t> raw;
      if (f == index.end() || !ReadHostFile(f->second, raw) || raw.empty()) continue;
      blob = std::make_shared<std::vector<uint8_t>>(std::move(raw));
      REXLOG_INFO("Textura: {:016x} <- {}", key, dir.filename().string());
      break;
    }
    it = s.texmod.emplace(key, blob).first;
  }
  return it->second.get();
}

// Shader replacement mods: MODS\<mod>\SHADERS\<v|p>_<ucode hash, 16 hex>[_<modification, 16 hex>].glsl
// (GLSL 450 compiled by the GPU backend) or .spv (SPIR-V). The name with the modification wins over
// the one without. The hash of every shader is in the frame report (NIER_FRAME_REPORT). Called once
// for each translation of a guest shader; the answer is cached.
const std::vector<uint8_t>* ShaderOverride(uint32_t type, uint64_t hash, uint64_t modification) {
  State& s = S();
  std::lock_guard lock(s.mutex);
  char key[64];
  std::snprintf(key, sizeof key, "%u_%016llx_%016llx", type, static_cast<unsigned long long>(hash),
                static_cast<unsigned long long>(modification));
  auto it = s.shadermod.find(key);
  if (it != s.shadermod.end()) return it->second.get();
  Blob blob;
  std::error_code ec;
  std::vector<fs::path> mods;
  for (const auto& entry : fs::directory_iterator(s.root.parent_path() / "MODS", ec)) {
    const std::string name = entry.path().filename().string();
    if (entry.is_directory(ec) && !name.empty() && name[0] != '_' && name[0] != '.') mods.push_back(entry.path());
  }
  std::sort(mods.begin(), mods.end(), std::greater<>());
  const char kind = type == 0 ? 'v' : 'p';
  for (const fs::path& dir : mods) {
    const auto& index = IndexLocked(s, dir);
    for (int with_mod = 1; with_mod >= 0 && !blob; --with_mod) {
      for (const char* ext : {"glsl", "spv"}) {
        char name[96];
        if (with_mod) {
          std::snprintf(name, sizeof name, "shaders/%c_%016llx_%016llx.%s", kind, static_cast<unsigned long long>(hash),
                        static_cast<unsigned long long>(modification), ext);
        } else {
          std::snprintf(name, sizeof name, "shaders/%c_%016llx.%s", kind, static_cast<unsigned long long>(hash), ext);
        }
        auto f = index.find(name);
        std::vector<uint8_t> raw;
        if (f == index.end() || !ReadHostFile(f->second, raw) || raw.empty()) continue;
        blob = std::make_shared<std::vector<uint8_t>>(std::move(raw));
        REXLOG_INFO("Shader: {} <- {} ({})", name, dir.filename().string(), blob->size());
        break;
      }
    }
    if (blob) break;
  }
  it = s.shadermod.emplace(key, blob).first;
  return it->second.get();
}

// Model swap mods: MODS\<mod>\swap.txt, one line per range of a game pack:
//   <pack file> <offset hex> <size hex> <file inside the mod>
// When the game reads that range of the pack (e.g. the .MDP of a model inside layer1.cpk), it gets
// the bytes of the mod's file instead (zeros past its end). First mod in reverse name order wins.
// A file bigger than the one it replaces is placed past the end of the pack (its table entry is
// patched to point there), so it never covers a neighbour; those reads are served from here.
void ScanRedirectsLocked(State& s) {
  s.redirects_scanned = true;
  std::error_code ec;
  std::vector<fs::path> mods;
  for (const auto& entry : fs::directory_iterator(s.root.parent_path() / "MODS", ec)) {
    const std::string name = entry.path().filename().string();
    if (entry.is_directory(ec) && !name.empty() && name[0] != '_' && name[0] != '.') mods.push_back(entry.path());
  }
  std::sort(mods.begin(), mods.end(), std::greater<>());
  for (const fs::path& dir : mods) {
    std::ifstream f(dir / "swap.txt");
    std::string line;
    while (std::getline(f, line)) {
      std::istringstream in(line);
      std::string first;
      if (!(in >> first)) continue;
      if (first == "patch") {  // patch <pack> <offset hex> <bytes hex>
        State::Patch p;
        std::string hex;
        unsigned long long off = 0;
        if (!(in >> p.cpk >> std::hex >> off >> std::dec >> hex)) continue;
        p.cpk = Lower(p.cpk);
        p.offset = off;
        for (size_t i = 0; i + 1 < hex.size(); i += 2) p.bytes.push_back(uint8_t(std::stoi(hex.substr(i, 2), nullptr, 16)));
        s.patches.push_back(std::move(p));
        continue;
      }
      State::Redirect r;  // <pack> <offset hex> <size hex> <file inside the mod>
      std::string rel;
      unsigned long long off = 0, size = 0;
      if (!(in >> std::hex >> off >> size >> std::dec >> rel)) continue;
      r.cpk = Lower(first);
      r.offset = off;
      r.size = size;
      // Through the mod's index: swap.txt is written on Windows ("SWAP\KAINE010.MDP"), and on Android the backslash
      // is no separator and the names' case counts (2026-11-18: Kaine's mod on the phone, the file not read, the
      // load waited forever).
      const auto& index = IndexLocked(s, dir);
      const auto found = index.find(GameKey(rel));
      r.file = found != index.end() ? found->second : dir / GameKey(rel);
      if (found == index.end()) REXLOG_WARN("Troca de modelo: {} nao existe no mod {}", rel, dir.filename().string());
      s.redirects.push_back(std::move(r));
      REXLOG_INFO("Troca de modelo: {} @{:X}+{:X} <- {}", first, off, size, rel);
    }
  }
  g_has_redirects = !s.redirects.empty() || !s.patches.empty();
}

uint32_t FileReadHook(const char* path, uint64_t offset, uint32_t length, uint32_t got, uint8_t* buffer) {
  State& s = S();
  std::lock_guard lock(s.mutex);
  if (!s.redirects_scanned) ScanRedirectsLocked(s);
  if (!g_has_redirects) return got;
  const std::string p = Lower(path);
  auto is_pack = [&](const std::string& cpk) {
    return p.size() >= cpk.size() && p.compare(p.size() - cpk.size(), cpk.size(), cpk) == 0;
  };
  for (const auto& patch : s.patches) {
    if (!is_pack(patch.cpk)) continue;
    for (size_t i = 0; i < patch.bytes.size(); ++i) {
      const uint64_t at = patch.offset + i;
      if (at >= offset && at < offset + got) buffer[at - offset] = patch.bytes[i];
    }
  }
  uint32_t valid = got;
  // Last to first, so the first mod (in reverse name order) is written last and wins a range two
  // mods replace.
  for (auto it = s.redirects.rbegin(); it != s.redirects.rend(); ++it) {
    auto& r = *it;
    if (!is_pack(r.cpk)) continue;
    // Past what the host file had, only a range that continues the valid bytes can be served.
    const uint64_t limit = r.offset <= offset + valid ? offset + length : offset + valid;
    const uint64_t from = std::max(offset, r.offset), to = std::min(limit, r.offset + r.size);
    if (from >= to) continue;
    if (!r.data) {
      std::vector<uint8_t> raw;
      if (!ReadHostFile(r.file, raw)) {
        REXLOG_WARN("Troca de modelo: nao consegui ler {}", r.file.string());
        r.size = 0;
        continue;
      }
      r.data = std::make_shared<std::vector<uint8_t>>(std::move(raw));
    }
    for (uint64_t i = from; i < to; ++i) {
      const uint64_t k = i - r.offset;
      buffer[i - offset] = k < r.data->size() ? (*r.data)[k] : 0;
    }
    if (to > offset + valid) valid = uint32_t(to - offset);
  }
  return valid;
}

// A pack with files placed past its end is reported that much bigger, or the game won't read there.
uint64_t FileSizeHook(const char* path, uint64_t size) {
  State& s = S();
  std::lock_guard lock(s.mutex);
  if (!s.redirects_scanned) ScanRedirectsLocked(s);
  if (!g_has_redirects) return size;
  const std::string p = Lower(path);
  for (const auto& r : s.redirects) {
    if (p.size() >= r.cpk.size() && p.compare(p.size() - r.cpk.size(), r.cpk.size(), r.cpk) == 0)
      size = std::max(size, r.offset + r.size);
  }
  return size;
}

}  // namespace

std::vector<fs::path> ModDirs() {
  fs::path root;
  {
    std::lock_guard lock(S().mutex);
    root = S().root;
  }
  std::vector<fs::path> mods;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(root.parent_path() / "MODS", ec)) {
    const std::string name = entry.path().filename().string();
    if (entry.is_directory(ec) && !name.empty() && name[0] != '_' && name[0] != '.') mods.push_back(entry.path());
  }
  std::sort(mods.begin(), mods.end(), std::greater<>());
  return mods;
}

void SetRoot(const fs::path& root) {
  std::lock_guard lock(S().mutex);
  if (S().root.empty()) S().root = root;
}

void Init(const fs::path& root) {
  State& s = S();
  std::lock_guard lock(s.mutex);
  s.root = root;
  rex::mods::SetFileReadHook(&FileReadHook);
  rex::mods::SetFileSizeHook(&FileSizeHook);
  rex::graphics::SetGuestTextureOverride(&TexOverride);
  rex::graphics::SetGuestShaderOverride(&ShaderOverride);
  ApplyLocked(s, REXCVAR_GET(nier_language));
}

void ExtractExample() {
  fs::path root;
  {
    std::lock_guard lock(S().mutex);
    root = S().root;
  }
  if (root.empty()) return;
  std::error_code ec;
  const fs::path example = root / "EXAMPLE";
  if (fs::exists(example, ec)) return;

  const std::string kCpk = "game:\\media\\layer2.cpk";
  std::vector<CpkFile> files = ListCpk(kCpk);
  if (files.empty()) {
    REXLOG_WARN("Textos: layer2.cpk não pôde ser lido; TEXT\\EXAMPLE não criado");
    return;
  }
  // Built next to the final folder and renamed at the end, so an interrupted
  // extraction is redone instead of leaving a half-empty EXAMPLE.
  const fs::path temp = root / "EXAMPLE.tmp";
  fs::remove_all(temp, ec);
  int written = 0;
  for (const CpkFile& f : files) {
    // TEXT\* -> EXAMPLE\*, EVENT\CAPTION\* -> EXAMPLE\EVENT\CAPTION\*
    std::string dir = f.dir;
    std::string rel;
    if (dir == "TEXT") rel = "";
    else if (StartsWith(dir, "TEXT/")) rel = dir.substr(5);
    else if (dir == "EVENT/CAPTION") rel = dir;
    else continue;
    std::vector<uint8_t> data;
    if (!ReadCpkFile(kCpk, f, data)) continue;
    fs::path out = temp / fs::path(rel) / f.name;
    fs::create_directories(out.parent_path(), ec);
    std::ofstream o(out, std::ios::binary);
    if (!o) continue;
    o.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    ++written;
  }
  {
    std::ofstream o(temp / "LEIA-ME.txt", std::ios::binary);
    static const char kReadme[] =
        "\xEF\xBB\xBF"
        "TRADUZIR O NIER\r\n"
        "================\r\n\r\n"
        "Esta pasta (EXAMPLE) tem os textos originais do jogo. Ela é só um exemplo e é\r\n"
        "refeita se você apagar a pasta.\r\n\r\n"
        "COMO FAZER UMA TRADUÇÃO\r\n"
        "1. Copie a pasta EXAMPLE (ela fica dentro da pasta TEXT, ao lado do jogo).\r\n"
        "2. Dê à cópia o nome do idioma, por exemplo \"Portugues\".\r\n"
        "3. Edite os arquivos da cópia com o Bloco de Notas (UTF-8) ou uma planilha.\r\n"
        "4. Abra o jogo: em Options > Language aparece \"Portugues\". Escolha e o texto\r\n"
        "   muda na hora. (Telas já abertas mudam quando você abrir de novo.)\r\n\r\n"
        "O QUE EDITAR\r\n"
        "- Os arquivos são texto puro: uma mensagem por linha, colunas separadas por TAB.\r\n"
        "- NIER_TEXT.TXD e TALK_TEXT\\TALKER_NAME.X360.TND: colunas  número, japonês,\r\n"
        "  INGLÊS, francês, alemão. Sua tradução entra na 3ª coluna (inglês).\r\n"
        "- Os demais (TALK_*, INFO_TEXT, RADIO_TEXT, CAPTION_TEXT, EVENT\\CAPTION): use os\r\n"
        "  arquivos com .EN. no nome e troque só o texto (a última coluna). Arquivos .DE.,\r\n"
        "  .FR. e .JA. não são usados por uma tradução: pode apagá-los.\r\n"
        "- Arquivo que não existir na sua pasta continua em inglês, como no disco.\r\n"
        "- Mantenha os TABs, o número de linhas e códigos como <ICON=2> exatamente iguais.\r\n"
        "- Cada mensagem termina em fim de linha do Windows (CRLF). Mensagens de várias\r\n"
        "  linhas têm quebras simples dentro da célula: não mexa nelas.\r\n"
        "- Letras com acento dependem da fonte do jogo: francês e alemão funcionam;\r\n"
        "  ã e õ ainda podem não aparecer.\r\n";
    o.write(kReadme, sizeof(kReadme) - 1);
  }
  fs::rename(temp, example, ec);
  if (ec) {
    REXLOG_WARN("Textos: não consegui criar {}: {}", example.string(), ec.message());
    return;
  }
  REXLOG_INFO("Textos: {} arquivos extraídos para {}", written, example.string());
}

std::vector<Language> Languages() {
  State& s = S();
  std::lock_guard lock(s.mutex);
  ScanLocked(s);
  s.index.clear();  // folders may have been edited
  return s.languages;
}

std::string CurrentName() {
  State& s = S();
  std::lock_guard lock(s.mutex);
  return s.current;
}

bool Select(const std::string& name) {
  State& s = S();
  {
    std::lock_guard lock(s.mutex);
    ScanLocked(s);
    const Language* l = FindLocked(s, name);
    if (!l || l->name == s.current) return false;
    ApplyLocked(s, std::string(l->name));
  }
  rex::cvar::SetFlagByName("nier_language", name);
  g_reload = true;
  REXLOG_INFO("Idioma do texto: {}", name);
  return true;
}

bool TakeReloadRequest() { return g_reload.exchange(false); }

int GameLanguageOverride() { return g_override; }

fs::path Root() {
  std::lock_guard lock(S().mutex);
  return S().root;
}

fs::path CurrentDir() {
  State& s = S();
  std::lock_guard lock(s.mutex);
  const Language* l = FindLocked(s, s.current);
  return l ? l->dir : fs::path();
}

namespace {

// The pixel half of a pack (.2DP, .MDP, .EFP): raw texture bytes. The other half
// (.2DV ...) is loaded and fixed up by the game's own loader, so it is not replaced.
bool IsArtKey(const std::string& key) {
  for (const char* ext : {".2dp", ".mdp", ".efp"}) {
    const size_t n = std::strlen(ext);
    if (key.size() > n && key.compare(key.size() - n, n, ext) == 0) return true;
  }
  return false;
}

}  // namespace

bool IsPixelPath(const std::string& game_path) { return IsArtKey(GameKey(game_path)); }

Blob Resolve(const std::string& game_path) {
  const std::string key = GameKey(game_path);
  // The loader sees every file the game opens: only text and art are ours.
  const bool is_text = StartsWith(key, "text/") || StartsWith(key, "event/caption/");
  const bool is_art = !is_text && IsArtKey(key);
  if (!is_text && !is_art) return nullptr;

  State& s = S();
  std::lock_guard lock(s.mutex);
  const Language* l = FindLocked(s, s.current);
  const bool custom_language = l && !l->dir.empty();  // the disc's own languages: the game decides
  if (is_text && !custom_language) return nullptr;
  const std::string cache_key = (custom_language ? l->name : std::string()) + "|" + key;
  if (auto it = s.cache.find(cache_key); it != s.cache.end()) return it->second;

  // The language folder mirrors the disc: TEXT\* at its top, EVENT\CAPTION\* under
  // EVENT, and 2D\*, BG\* ... as they are. Art can also come from mods (see
  // game_text.h): the MODS folder next to TEXT, one folder per mod; when several
  // have the file, the one last in name order wins. The language folder wins over all.
  const std::string rel = StartsWith(key, "text/") ? key.substr(5) : key;
  std::vector<fs::path> dirs;
  if (custom_language) dirs.push_back(l->dir);
  if (is_art) {
    std::error_code ec;
    const fs::path mods = s.root.parent_path() / "MODS";
    std::vector<fs::path> found;
    for (const auto& entry : fs::directory_iterator(mods, ec)) {
      const std::string name = entry.path().filename().string();
      // A mod folder starting with "_" or "." is switched off.
      if (entry.is_directory(ec) && !name.empty() && name[0] != '_' && name[0] != '.') {
        found.push_back(entry.path());
      }
    }
    std::sort(found.begin(), found.end(), std::greater<>());  // first in the list wins
    dirs.insert(dirs.end(), found.begin(), found.end());
  }
  Blob blob;
  std::string from;
  try {
    for (const fs::path& dir : dirs) {
      const auto& index = IndexLocked(s, dir);
      auto f = index.find(rel);
      if (f == index.end()) continue;
      std::vector<uint8_t> raw;
      if (!ReadHostFile(f->second, raw)) continue;
      auto data = std::make_shared<std::vector<uint8_t>>(is_text ? Normalize(raw) : std::move(raw));
      if (data->empty()) continue;
      if (rel == "nier_text.txd") ApplyModTextLocked(s, *data);
      blob = data;
      from = dir.filename().string();
      break;
    }
  } catch (const std::exception& e) {
    REXLOG_WARN("Textos: erro lendo {}: {}", rel, e.what());
  }
  s.cache[cache_key] = blob;
  if (blob) REXLOG_INFO("{}: {} <- {}", is_text ? "Textos" : "Arte", key, from);
  return blob;
}

}  // namespace nier::text
