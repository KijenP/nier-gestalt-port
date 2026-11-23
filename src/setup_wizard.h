// nier - the first-run window on the PC (2026-11-15). The released game carries none of NieR's files, so on the
// first start (or when the saved copy is gone) it asks for the player's copy: the disc image (.iso) or the
// extracted folder (default.xex + media). It explains why, in Portuguese, English or Spanish, checks the copy
// (an Xbox 360 image, NieR's title ID, the game files), takes the disc's own tile as the window's icon and the
// desktop shortcut's, saves the choice in nier.toml (game_data_root, and the game's text language), then the
// game starts. Drawn with ImGui over Renut's UI pump, which presents while no guest runs yet (OnFinalizePaths's
// asynchronous path, rex_app.h). Windows only: Android has its own launcher (SetupActivity.java).
#pragma once

#if defined(_WIN32)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <imgui.h>
#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/window.h>

#include "disc_image.h"
#include "pc_options.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")

namespace nier::setup {

// ---------------------------------------------------------------------------------------------------------------
// Texts

struct Texts {
  const char* language;  // the button
  const char* welcome;
  const char* why;
  const char* choose;
  const char* iso;
  const char* iso_hint;
  const char* folder;
  const char* folder_hint;
  const char* shortcut;
  const char* note;
  const char* game_language;  // what the game's texts will be in
  const char* preparing;
  const char* steps[7];
  const char* done;
  const char* done_hint;
  const char* play;
  const char* other;
  const char* picking;
  // Errors
  const char* not_found;  // + the old path
  const char* open_failed;
  const char* not_xbox;
  const char* no_xex;
  const char* bad_xex;
  const char* wrong_game;  // + the title ID found
  const char* no_media;
  const char* save_failed;
};

constexpr int kPortuguese = 0, kEnglish = 1, kSpanish = 2;

inline const Texts& TextsFor(int language) {
  static const Texts pt = {
      "Português",
      "Bem-vindo! Falta só um passo para jogar.",
      "Este é o NieR Gestalt recompilado para PC, mas nenhum arquivo do jogo vem junto: as imagens, os sons e os "
      "textos são lidos da sua própria cópia de NieR Gestalt para Xbox 360.",
      "Onde está a sua cópia?",
      "Arquivo ISO",
      "A imagem do disco (.iso), do jeito que está. Não precisa extrair nada.",
      "Pasta extraída",
      "A pasta onde a ISO foi extraída: a que tem o default.xex e a pasta media.",
      "Criar atalho na área de trabalho",
      "A escolha fica salva no nier.toml. Para trocar depois, apague a linha game_data_root dele.",
      "Os textos do jogo ficarão em português.",
      "Preparando o jogo",
      {"Abrindo a sua cópia...", "Lendo o default.xex...", "Conferindo se é o NieR Gestalt...",
       "Conferindo os arquivos do jogo...", "Extraindo o ícone do disco...", "Salvando a configuração...",
       "Criando o atalho..."},
      "Tudo pronto!",
      "Sua cópia foi reconhecida. Das próximas vezes o jogo abre direto.",
      "Jogar",
      "Escolher outra cópia",
      "Escolha a sua cópia na janela que abriu...",
      "Não encontrei o jogo em:",
      "Não consegui abrir esse arquivo.",
      "Esse arquivo não parece uma imagem de disco de Xbox 360.",
      "Não achei o default.xex nessa cópia.",
      "O default.xex está danificado ou não é um executável de Xbox 360.",
      "Essa cópia não é o NieR Gestalt de Xbox 360. ID do título encontrado:",
      "Faltam os arquivos do jogo (a pasta media) nessa cópia.",
      "Não consegui gravar o nier.toml. Coloque o jogo numa pasta sua (fora de Arquivos de Programas) e tente "
      "de novo.",
  };
  static const Texts en = {
      "English",
      "Welcome! Just one step left before you play.",
      "This is NieR Gestalt recompiled for the PC, but none of the game's files come with it: the graphics, sound "
      "and text are read from your own copy of NieR Gestalt for the Xbox 360.",
      "Where is your copy?",
      "ISO file",
      "The disc image (.iso), as it is. Nothing needs to be extracted.",
      "Extracted folder",
      "The folder the ISO was extracted to: the one with default.xex and the media folder.",
      "Create a desktop shortcut",
      "Your choice is saved in nier.toml. To change it later, delete its game_data_root line.",
      "The game's text will be in English.",
      "Preparing the game",
      {"Opening your copy...", "Reading default.xex...", "Checking that it is NieR Gestalt...",
       "Checking the game files...", "Taking the icon from the disc...", "Saving the settings...",
       "Creating the shortcut..."},
      "All set!",
      "Your copy was recognized. From now on the game starts right away.",
      "Play",
      "Choose another copy",
      "Pick your copy in the window that opened...",
      "The game was not found at:",
      "That file could not be opened.",
      "That file does not look like an Xbox 360 disc image.",
      "default.xex was not found in this copy.",
      "default.xex is damaged or is not an Xbox 360 executable.",
      "This copy is not NieR Gestalt for the Xbox 360. Title ID found:",
      "The game files (the media folder) are missing from this copy.",
      "nier.toml could not be written. Put the game in a folder of your own (outside Program Files) and try "
      "again.",
  };
  static const Texts es = {
      "Español",
      "¡Bienvenido! Solo falta un paso para jugar.",
      "Este es NieR Gestalt recompilado para PC, pero no incluye ningún archivo del juego: los gráficos, el sonido "
      "y los textos se leen de tu propia copia de NieR Gestalt para Xbox 360.",
      "¿Dónde está tu copia?",
      "Archivo ISO",
      "La imagen del disco (.iso), tal como está. No hace falta extraer nada.",
      "Carpeta extraída",
      "La carpeta donde se extrajo la ISO: la que tiene default.xex y la carpeta media.",
      "Crear acceso directo en el escritorio",
      "La elección se guarda en nier.toml. Para cambiarla, borra su línea game_data_root.",
      "El juego no trae español: los textos estarán en inglés.",
      "Preparando el juego",
      {"Abriendo tu copia...", "Leyendo default.xex...", "Comprobando que sea NieR Gestalt...",
       "Comprobando los archivos del juego...", "Sacando el icono del disco...", "Guardando la configuración...",
       "Creando el acceso directo..."},
      "¡Todo listo!",
      "Tu copia fue reconocida. A partir de ahora el juego abre directamente.",
      "Jugar",
      "Elegir otra copia",
      "Elige tu copia en la ventana que se abrió...",
      "No encontré el juego en:",
      "No pude abrir ese archivo.",
      "Ese archivo no parece una imagen de disco de Xbox 360.",
      "No encontré default.xex en esta copia.",
      "default.xex está dañado o no es un ejecutable de Xbox 360.",
      "Esta copia no es NieR Gestalt de Xbox 360. ID de título encontrado:",
      "Faltan los archivos del juego (la carpeta media) en esta copia.",
      "No pude escribir nier.toml. Pon el juego en una carpeta tuya (fuera de Archivos de programa) e inténtalo "
      "de nuevo.",
  };
  return language == kEnglish ? en : language == kSpanish ? es : pt;
}

inline int SystemLanguage() {
  switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_PORTUGUESE: return kPortuguese;
    case LANG_SPANISH: return kSpanish;
    default: return kEnglish;
  }
}

// ---------------------------------------------------------------------------------------------------------------
// Helpers

inline std::string Utf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

// The system's file / folder picker (on its own thread, so the window keeps drawing). Empty if cancelled.
inline std::filesystem::path PickWithSystemDialog(HWND owner, bool folder, int language) {
  std::filesystem::path result;
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return result;
  IFileOpenDialog* dialog = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | (folder ? FOS_PICKFOLDERS : 0));
    if (!folder) {
      const wchar_t* label = language == kEnglish   ? L"NieR Gestalt (ISO or default.xex)"
                             : language == kSpanish ? L"NieR Gestalt (ISO o default.xex)"
                                                    : L"NieR Gestalt (ISO ou default.xex)";
      COMDLG_FILTERSPEC types[] = {{label, L"*.iso;*.xex"}, {L"*.*", L"*.*"}};
      dialog->SetFileTypes(2, types);
    }
    dialog->SetTitle(folder ? (language == kEnglish   ? L"The folder with default.xex"
                               : language == kSpanish ? L"La carpeta con default.xex"
                                                      : L"A pasta com o default.xex")
                            : L"NieR Gestalt");
    if (SUCCEEDED(dialog->Show(owner))) {
      IShellItem* item = nullptr;
      if (SUCCEEDED(dialog->GetResult(&item))) {
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
          result = path;
          CoTaskMemFree(path);
        }
        item->Release();
      }
    }
    dialog->Release();
  }
  CoUninitialize();
  return result;
}

inline bool CreateDesktopShortcut(const std::filesystem::path& exe, const std::filesystem::path& icon) {
  PWSTR desktop = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop))) return false;
  const std::filesystem::path lnk = std::filesystem::path(desktop) / L"NieR Gestalt.lnk";
  CoTaskMemFree(desktop);
  bool ok = false;
  const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  IShellLinkW* link = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
    link->SetPath(exe.c_str());
    link->SetWorkingDirectory(exe.parent_path().c_str());
    link->SetDescription(L"NieR Gestalt");
    if (!icon.empty())
      link->SetIconLocation(icon.c_str(), 0);
    else
      link->SetIconLocation(exe.c_str(), 0);
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file)))) {
      ok = SUCCEEDED(file->Save(lnk.c_str(), TRUE));
      file->Release();
    }
    link->Release();
  }
  if (SUCCEEDED(init)) CoUninitialize();
  return ok;
}

// game_data_root and nier_language into nier.toml, first, keeping every other line.
inline bool SaveChoice(const std::filesystem::path& config, const std::filesystem::path& game,
                       const std::string& text_language) {
  std::vector<std::string> keep;
  std::FILE* in = nullptr;
  if (_wfopen_s(&in, config.c_str(), L"r") == 0 && in) {
    std::FILE* f = in;
    char buf[2048];
    while (std::fgets(buf, sizeof(buf), f)) {
      std::string line(buf);
      if (keep.empty() && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);  // Notepad's BOM
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
      auto is_key = [&line](std::string_view key) {
        return line.rfind(key, 0) == 0 && line.size() > key.size() &&
               (line[key.size()] == ' ' || line[key.size()] == '=');
      };
      if (is_key("game_data_root") || is_key("nier_language")) continue;
      // The release's placeholder comment about game_data_root.
      if (line.rfind("# Troque o caminho", 0) == 0 || line.rfind("# Também serve a pasta", 0) == 0) continue;
      keep.push_back(line);
    }
    std::fclose(f);
  }
  std::string path = Utf8(game.wstring());
  std::replace(path.begin(), path.end(), '\\', '/');
  std::FILE* f = nullptr;
  if (_wfopen_s(&f, config.c_str(), L"w") != 0 || !f) return false;
  bool ok = std::fprintf(f, "game_data_root = '%s'\nnier_language = \"%s\"\n", path.c_str(),
                         text_language.c_str()) > 0;
  for (const auto& l : keep) ok = ok && std::fprintf(f, "%s\n", l.c_str()) >= 0;
  ok = std::fclose(f) == 0 && ok;
  return ok;
}

// The disc tile as the window's icon (also on later starts: SetWindowIconFromFile).
inline void SetWindowIconPng(const std::vector<uint8_t>& png) {
  if (png.empty()) return;
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  if (!windows) return;
  if (count > 0) {
    if (SDL_Surface* icon = SDL_LoadPNG_IO(SDL_IOFromConstMem(png.data(), png.size()), true)) {
      SDL_SetWindowIcon(windows[0], icon);
      SDL_DestroySurface(icon);
    }
  }
  SDL_free(windows);
}

inline void SetWindowIconFromFile(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return;
  std::vector<uint8_t> png((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  SetWindowIconPng(png);
}

inline std::filesystem::path IconPngPath(const std::filesystem::path& config) {
  return config.parent_path() / "nier_icon.png";
}

// True when the game cannot start from what nier.toml says.
inline bool Needed(const std::filesystem::path& game) {
  std::error_code ec;
  return game.empty() || !std::filesystem::exists(game, ec);
}

// ---------------------------------------------------------------------------------------------------------------
// The window

class SetupWizard final : public rex::ui::ImGuiDialog {
 public:
  using Done = std::function<void(rex::PathConfig)>;

  SetupWizard(rex::ui::ImGuiDrawer* drawer, rex::PathConfig paths, Done done)
      : ImGuiDialog(drawer), paths_(std::move(paths)), done_(std::move(done)), language_(SystemLanguage()) {
    if (!paths_.game_data_root.empty()) {
      error_ = Error::kNotFound;
      error_detail_ = Utf8(paths_.game_data_root.wstring());
    }
  }

  ~SetupWizard() override {
    if (worker_.joinable()) worker_.join();
    if (picker_.joinable()) picker_.join();
    tile_texture_.release();  // the drawer may be gone by now (as PcOptionsDialog's textures)
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (rex::ui::Window* window = imgui_drawer()->window();
        window && window->GetCursorVisibility() != rex::ui::Window::CursorVisibility::kVisible)
      window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
    TakeResults();
    Draw(io);
  }

 private:
  enum class Page { kChoose, kPreparing, kDone };
  enum class Error { kNone, kNotFound, kOpenFailed, kNotXbox, kNoXex, kBadXex, kWrongGame, kNoMedia, kSaveFailed };

  struct Shared {
    std::mutex mutex;
    bool picked = false;  // the picker thread finished
    std::filesystem::path pick;
    bool finished = false;  // the worker finished
    Error error = Error::kNone;
    std::string detail;
    std::vector<uint8_t> tile;
    std::filesystem::path game;
    std::atomic<int> step{0};
  };

  // ---- work -----------------------------------------------------------------------------------------------------

  void StartPick(bool folder) {
    if (picking_ || page_ != Page::kChoose) return;
    if (picker_.joinable()) picker_.join();
    picking_ = true;
    HWND owner = nullptr;
    int count = 0;
    if (SDL_Window** windows = SDL_GetWindows(&count)) {
      if (count > 0)
        owner = static_cast<HWND>(
            SDL_GetPointerProperty(SDL_GetWindowProperties(windows[0]), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
      SDL_free(windows);
    }
    const int language = language_;
    picker_ = std::thread([this, owner, folder, language] {
      std::filesystem::path path = PickWithSystemDialog(owner, folder, language);
      std::lock_guard lock(shared_.mutex);
      shared_.pick = std::move(path);
      shared_.picked = true;
    });
  }

  void StartWork(std::filesystem::path pick) {
    if (worker_.joinable()) worker_.join();
    page_ = Page::kPreparing;
    shown_progress_ = 0.0f;
    step_started_ = Now();
    shown_step_ = 0;
    shared_.step = 0;
    {
      std::lock_guard lock(shared_.mutex);
      shared_.finished = false;
    }
    const bool shortcut = shortcut_;
    const std::string text_language = TextLanguage();
    const std::filesystem::path config = paths_.config_path;
    worker_ = std::thread([this, pick = std::move(pick), shortcut, text_language, config] {
      std::vector<uint8_t> tile;
      std::string detail;
      std::filesystem::path game = pick;
      const Error error = Prepare(game, shortcut, text_language, config, tile, detail);
      std::lock_guard lock(shared_.mutex);
      shared_.error = error;
      shared_.detail = std::move(detail);
      shared_.tile = std::move(tile);
      shared_.game = std::move(game);
      shared_.finished = true;
    });
  }

  // On the worker thread: the checks, the icon, nier.toml, the shortcut.
  Error Prepare(std::filesystem::path& game, bool shortcut, const std::string& text_language,
                const std::filesystem::path& config, std::vector<uint8_t>& tile, std::string& detail) {
    std::error_code ec;
    // default.xex picked: its folder.
    if (std::filesystem::is_regular_file(game, ec) && game.filename().wstring().size() == 11 &&
        _wcsicmp(game.filename().c_str(), L"default.xex") == 0)
      game = game.parent_path();
    const bool folder = std::filesystem::is_directory(game, ec);
    std::vector<uint8_t> xex, art;
    disc::Image image;

    shared_.step = 0;  // opening
    if (!folder && !image.Open(game)) {
      std::ifstream probe(game, std::ios::binary);
      return probe ? Error::kNotXbox : Error::kOpenFailed;
    }
    shared_.step = 1;  // default.xex
    if (folder) {
      std::ifstream in(game / "default.xex", std::ios::binary);
      if (!in) return Error::kNoXex;
      xex.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } else if (!image.ReadFile("default.xex", xex)) {
      return Error::kNoXex;
    }
    shared_.step = 2;  // NieR?
    const uint32_t title = disc::XexTitleId(xex);
    if (!title) return Error::kBadXex;
    if (title != disc::kNierTitleId) {
      char id[16];
      std::snprintf(id, sizeof(id), "%08X", title);
      detail = id;
      return Error::kWrongGame;
    }
    shared_.step = 3;  // game files
    if (folder) {
      if (!std::filesystem::is_directory(game / "media", ec)) return Error::kNoMedia;
    } else {
      disc::Image::Entry media;
      if (!image.Find("media", media) || !media.directory) return Error::kNoMedia;
    }
    shared_.step = 4;  // icon
    if (folder) {
      std::ifstream in(game / "nxeart", std::ios::binary);
      if (in) art.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } else {
      image.ReadFile("nxeart", art);
    }
    tile = disc::TileFromNxeArt(art);
    std::filesystem::path ico;
    if (!tile.empty()) {
      std::ofstream(IconPngPath(config), std::ios::binary)
          .write(reinterpret_cast<const char*>(tile.data()), std::streamsize(tile.size()));
      const std::vector<uint8_t> data = disc::IcoFromPng(tile);
      ico = config.parent_path() / "nier.ico";
      std::ofstream out(ico, std::ios::binary);
      out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
      if (!out) ico.clear();
    }
    shared_.step = 5;  // nier.toml
    if (!SaveChoice(config, game, text_language)) return Error::kSaveFailed;
    if (shortcut) {
      shared_.step = 6;
      wchar_t exe[MAX_PATH * 4];
      const DWORD n = GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
      if (n && n < std::size(exe) && !CreateDesktopShortcut(exe, ico))
        REXLOG_WARN("Setup: the desktop shortcut could not be created");
    }
    REXLOG_INFO("Setup: game copy {} (title {:08X}), text language {}", Utf8(game.wstring()), title, text_language);
    return Error::kNone;
  }

  // The game's text language for the setup's language: the Portuguese translation when it is there.
  std::string TextLanguage() const {
    std::error_code ec;
    if (language_ == kPortuguese && std::filesystem::is_directory(paths_.config_path.parent_path() / "TEXT" / "Portugues", ec))
      return "Portugues";
    return "English";
  }

  void TakeResults() {
    std::filesystem::path picked;
    {
      std::lock_guard lock(shared_.mutex);
      if (shared_.picked) {
        shared_.picked = false;
        picking_ = false;
        picked = std::move(shared_.pick);
        shared_.pick.clear();
      }
    }
    if (!picked.empty()) {
      error_ = Error::kNone;
      StartWork(std::move(picked));  // takes the mutex itself
    }
    std::lock_guard lock(shared_.mutex);
    // An error goes back at once; success once the bar is full.
    if (shared_.finished && page_ == Page::kPreparing &&
        (shared_.error != Error::kNone || (shown_progress_ >= 1.0f && Now() - step_started_ > 0.3))) {
      shared_.finished = false;
      if (shared_.error != Error::kNone) {
        error_ = shared_.error;
        error_detail_ = shared_.detail;
        page_ = Page::kChoose;
      } else {
        tile_ = std::move(shared_.tile);
        game_ = shared_.game;
        rex::cvar::SetFlagByName("game_data_root", Utf8(game_.wstring()));
        rex::cvar::SetFlagByName("nier_language", TextLanguage());
        SetWindowIconPng(tile_);
        page_ = Page::kDone;
      }
    }
  }

  static double Now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  void Play() {
    if (page_ != Page::kDone || finishing_) return;
    finishing_ = true;
    rex::PathConfig paths = paths_;
    paths.game_data_root = game_;
    done_(std::move(paths));
  }

  // ---- drawing --------------------------------------------------------------------------------------------------

  void EnsureTile() {
    rex::ui::ImmediateDrawer* drawer = imgui_drawer()->immediate_drawer();
    if (!drawer || tile_.empty() || tile_texture_) return;
    SDL_Surface* png = SDL_LoadPNG_IO(SDL_IOFromConstMem(tile_.data(), tile_.size()), true);
    if (!png) return;
    if (SDL_Surface* rgba = SDL_ConvertSurface(png, SDL_PIXELFORMAT_ABGR8888)) {
      std::vector<uint8_t> pixels(size_t(rgba->w) * rgba->h * 4);
      for (int y = 0; y < rgba->h; ++y)
        std::memcpy(&pixels[size_t(y) * rgba->w * 4], static_cast<uint8_t*>(rgba->pixels) + size_t(y) * rgba->pitch,
                    size_t(rgba->w) * 4);
      tile_texture_ = drawer->CreateTexture(uint32_t(rgba->w), uint32_t(rgba->h),
                                            rex::ui::ImmediateTextureFilter::kLinear, false, pixels.data());
      SDL_DestroySurface(rgba);
    }
    SDL_DestroySurface(png);
  }

  void Draw(ImGuiIO& io) {
    const Texts& t = TextsFor(language_);
    const ImVec2 screen = io.DisplaySize;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    // Laid out in 1280x720 units, fitted to the window.
    const float s = std::min(screen.x / 1280.0f, screen.y / 720.0f);
    const float ox = (screen.x - 1280.0f * s) * 0.5f, oy = (screen.y - 720.0f * s) * 0.5f;
    auto P = [&](float x, float y) { return ImVec2(ox + x * s, oy + y * s); };
    ImFont* body = pc_options_title_font() ? pc_options_title_font() : ImGui::GetFont();
    ImFont* bold = pc_options_font() ? pc_options_font() : body;
    const ImU32 ink = IM_COL32(30, 32, 40, 255), soft_ink = IM_COL32(70, 72, 80, 255);
    const ImU32 paper = IM_COL32(214, 211, 202, 255), paper_dark = IM_COL32(190, 186, 175, 255);
    const ImU32 light = IM_COL32(238, 236, 230, 255), red = IM_COL32(150, 30, 30, 255);
    const ImVec2 mouse = io.MousePos;
    const bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    auto inside = [&](ImVec2 a, ImVec2 b) { return mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y; };
    auto text = [&](ImFont* font, float size, float x, float y, ImU32 color, const char* str, float wrap = 0.0f) {
      dl->AddText(font, size * s, P(x, y), color, str, nullptr, wrap * s);
      return font->CalcTextSizeA(size * s, FLT_MAX, wrap * s, str).y / s;
    };
    auto centered = [&](ImFont* font, float size, float cx, float y, ImU32 color, const char* str) {
      const float w = font->CalcTextSizeA(size * s, FLT_MAX, 0.0f, str).x / s;
      dl->AddText(font, size * s, P(cx - w * 0.5f, y), color, str);
    };
    // A button like the game's menus: a dark band under the pointer.
    auto button = [&](float x, float y, float w, float h, const char* label, float size, bool enabled = true) {
      const ImVec2 a = P(x, y), b = P(x + w, y + h);
      const bool hot = enabled && inside(a, b);
      dl->AddRectFilled(a, b, hot ? ink : paper_dark, 3 * s);
      dl->AddRect(a, b, ink, 3 * s, 0, 1.5f * s);
      const ImVec2 sz = bold->CalcTextSizeA(size * s, FLT_MAX, 0.0f, label);
      dl->AddText(bold, size * s, {a.x + (b.x - a.x - sz.x) * 0.5f, a.y + (b.y - a.y - sz.y) * 0.5f},
                  enabled ? (hot ? light : ink) : soft_ink, label);
      if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      return hot && click;
    };

    // Backdrop and the paper panel.
    dl->AddRectFilled({0, 0}, screen, IM_COL32(12, 12, 14, 255));
    dl->AddRectFilledMultiColor(P(0, 0), P(1280, 720), IM_COL32(28, 28, 34, 255), IM_COL32(28, 28, 34, 255),
                                IM_COL32(8, 8, 10, 255), IM_COL32(8, 8, 10, 255));
    dl->AddRectFilled(P(150, 40), P(1130, 680), paper, 4 * s);
    dl->AddRect(P(160, 50), P(1120, 670), soft_ink, 2 * s, 0, 1.0f * s);

    // Title and the languages.
    if (tile_texture_ || (EnsureTile(), tile_texture_))
      dl->AddImage(reinterpret_cast<ImTextureID>(tile_texture_.get()), P(190, 70), P(254, 134));
    const float title_x = tile_texture_ ? 270.0f : 190.0f;
    text(bold, 46, title_x, 70, ink, "NieR Gestalt");
    text(body, 20, title_x + 2, 122, soft_ink, page_ == Page::kChoose ? t.welcome : page_ == Page::kPreparing ? t.preparing : t.done);
    if (page_ == Page::kChoose) {
      float x = 1090;
      for (int i = 2; i >= 0; --i) {
        const char* name = TextsFor(i).language;
        const float w = bold->CalcTextSizeA(17 * s, FLT_MAX, 0.0f, name).x / s + 24;
        x -= w;
        const ImVec2 a = P(x, 72), b = P(x + w, 102);
        const bool active = i == language_, hot = inside(a, b);
        if (active || hot) dl->AddRectFilled(a, b, active ? ink : paper_dark, 3 * s);
        dl->AddText(bold, 17 * s, P(x + 12, 77), active ? light : ink, name);
        if (hot && click) language_ = i;
        x -= 8;
      }
    }
    dl->AddLine(P(190, 152), P(1090, 152), soft_ink, 1.0f * s);

    if (page_ == Page::kChoose) {
      float y = 172;
      y += text(body, 20, 190, y, ink, t.why, 900) + 18;
      if (error_ != Error::kNone) {
        const char* message = ErrorText(t);
        std::string full = message;
        if (!error_detail_.empty()) full += (error_ == Error::kNotFound ? "\n" : " ") + error_detail_;
        y += text(bold, 18, 190, y, red, full.c_str(), 900) + 14;
      }
      text(bold, 24, 190, y, ink, t.choose);
      y += 42;
      // The two choices.
      const float card_w = 435, card_h = 190;
      for (int i = 0; i < 2; ++i) {
        const float x = 190 + i * (card_w + 30);
        const ImVec2 a = P(x, y), b = P(x + card_w, y + card_h);
        const bool hot = !picking_ && inside(a, b);
        dl->AddRectFilled(a, b, hot ? ink : paper_dark, 4 * s);
        dl->AddRect(a, b, ink, 4 * s, 0, 1.5f * s);
        // A small drawing: a disc or a folder.
        const ImU32 c = hot ? light : ink;
        if (i == 0) {
          dl->AddCircle(P(x + 50, y + 52), 26 * s, c, 32, 2.5f * s);
          dl->AddCircle(P(x + 50, y + 52), 7 * s, c, 16, 2.0f * s);
        } else {
          dl->AddRect(P(x + 24, y + 34), P(x + 78, y + 74), c, 3 * s, 0, 2.5f * s);
          dl->AddRectFilled(P(x + 24, y + 28), P(x + 46, y + 35), c, 2 * s);
        }
        text(bold, 26, x + 100, y + 34, c, i == 0 ? t.iso : t.folder);
        text(body, 17, x + 24, y + 96, hot ? light : soft_ink, i == 0 ? t.iso_hint : t.folder_hint, card_w - 48);
        if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (hot && click) StartPick(i == 1);
      }
      y += card_h + 22;
      // The shortcut box.
      {
        const ImVec2 a = P(190, y), b = P(212, y + 22);
        dl->AddRect(a, b, ink, 2 * s, 0, 1.5f * s);
        if (shortcut_) {
          dl->AddLine(P(194, y + 11), P(200, y + 18), ink, 2.5f * s);
          dl->AddLine(P(200, y + 18), P(209, y + 4), ink, 2.5f * s);
        }
        const float w = body->CalcTextSizeA(18 * s, FLT_MAX, 0.0f, t.shortcut).x / s;
        text(body, 18, 224, y, ink, t.shortcut);
        if (click && inside(a, P(224 + w, y + 24))) shortcut_ = !shortcut_;
      }
      text(body, 16, 190, y + 36, soft_ink, t.game_language);
      text(body, 15, 190, 636, soft_ink, picking_ ? t.picking : t.note, 900);
    } else if (page_ == Page::kPreparing) {
      // The bar follows the real steps, each shown for at least 0.35 s.
      const int real = shared_.step.load();
      if (shown_step_ < real && Now() - step_started_ > 0.35) {
        ++shown_step_;
        step_started_ = Now();
      }
      bool finished;
      {
        std::lock_guard lock(shared_.mutex);
        finished = shared_.finished;
      }
      const float target = finished && shown_step_ >= real ? 1.0f : float(shown_step_ + 1) / 8.0f;
      shown_progress_ += (target - shown_progress_) * std::min(1.0f, io.DeltaTime * 6.0f);
      if (std::fabs(target - shown_progress_) < 0.004f) shown_progress_ = target;
      text(body, 22, 190, 300, ink, t.steps[std::min(shown_step_, 6)]);
      const ImVec2 a = P(190, 345), b = P(1090, 377);
      dl->AddRectFilled(a, b, paper_dark, 3 * s);
      dl->AddRectFilled(a, {a.x + (b.x - a.x) * shown_progress_, b.y}, ink, 3 * s);
      // A soft light running over the filled part.
      const float run = std::fmod(float(ImGui::GetTime()) * 0.6f, 1.0f) * (b.x - a.x) * shown_progress_;
      dl->AddRectFilledMultiColor({a.x + std::max(0.0f, run - 60 * s), a.y}, {a.x + run, b.y}, IM_COL32(255, 255, 255, 0),
                                  IM_COL32(255, 255, 255, 50), IM_COL32(255, 255, 255, 50), IM_COL32(255, 255, 255, 0));
      dl->AddRect(a, b, ink, 3 * s, 0, 1.5f * s);
      char pct[8];
      std::snprintf(pct, sizeof(pct), "%d%%", int(shown_progress_ * 100.0f + 0.5f));
      centered(bold, 20, 640, 392, ink, pct);
    } else {
      float y = 230;
      y += text(body, 22, 190, y, ink, t.done_hint, 900) + 12;
      text(body, 16, 190, y, soft_ink, Utf8(game_.wstring()).c_str(), 900);
      if (button(440, 400, 400, 70, t.play, 32)) Play();
      if (button(490, 500, 300, 44, t.other, 18)) {
        page_ = Page::kChoose;
        tile_.clear();
        tile_texture_.reset();
      }
      if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) Play();
    }
  }

  const char* ErrorText(const Texts& t) const {
    switch (error_) {
      case Error::kNotFound: return t.not_found;
      case Error::kOpenFailed: return t.open_failed;
      case Error::kNotXbox: return t.not_xbox;
      case Error::kNoXex: return t.no_xex;
      case Error::kBadXex: return t.bad_xex;
      case Error::kWrongGame: return t.wrong_game;
      case Error::kNoMedia: return t.no_media;
      case Error::kSaveFailed: return t.save_failed;
      default: return "";
    }
  }

  rex::PathConfig paths_;
  Done done_;
  int language_;
  Page page_ = Page::kChoose;
  Error error_ = Error::kNone;
  std::string error_detail_;
  bool shortcut_ = true;
  bool picking_ = false;
  bool finishing_ = false;
  Shared shared_;
  std::thread picker_, worker_;
  int shown_step_ = 0;
  double step_started_ = 0.0;
  float shown_progress_ = 0.0f;
  std::vector<uint8_t> tile_;
  std::filesystem::path game_;
  std::unique_ptr<rex::ui::ImmediateTexture> tile_texture_;
};

}  // namespace nier::setup

#endif  // _WIN32
