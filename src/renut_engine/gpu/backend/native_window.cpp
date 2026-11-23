/**
 * @file    gpu/backend/native_window.cpp
 */
#include "renut_engine/gpu/backend/native_window.h"

#include "renut_engine/gpu/log.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__SWITCH__)
#include <switch.h>
#else
#include <cstdlib>

#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>
#if defined(__ANDROID__)
#include <atomic>

#include <SDL3/SDL_events.h>
#include <android/native_window.h>
#endif
#endif

namespace renut::gpu {

#if defined(_WIN32)

namespace {

// The process's visible top-level window, preferring SDL's window class (the
// SDK creates one window, through SDL).
BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM param) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER))
    return TRUE;
  auto *found = reinterpret_cast<HWND *>(param);
  wchar_t class_name[32] = {};
  GetClassNameW(hwnd, class_name, 32);
  if (wcscmp(class_name, L"SDL_app") == 0) {
    *found = hwnd;
    return FALSE;
  }
  if (!*found)
    *found = hwnd;
  return TRUE;
}

HWND FindGameHwnd() {
  HWND hwnd = nullptr;
  EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&hwnd));
  return hwnd;
}

} // namespace

bool GetNativeRenderWindow(plume::RenderWindow &out) {
  out = static_cast<plume::RenderWindow>(FindGameHwnd());
  if (!out) {
    RGPU_ERROR("No game window yet");
    return false;
  }
  return true;
}

bool GetRenderWindowPixelSize(uint32_t &width, uint32_t &height) {
  const HWND hwnd = FindGameHwnd();
  RECT rect{};
  if (!hwnd || !GetClientRect(hwnd, &rect))
    return false;
  width = uint32_t(rect.right - rect.left);
  height = uint32_t(rect.bottom - rect.top);
  return true;
}

#elif defined(__SWITCH__)

// banjo-nx: the Switch has one window, libnx's default NWindow, which plume's VK_NN_vi_surface takes.
bool GetNativeRenderWindow(plume::RenderWindow &out) {
  out = nwindowGetDefault();
  if (!out) {
    RGPU_ERROR("No default NWindow");
    return false;
  }
  return true;
}

bool GetRenderWindowPixelSize(uint32_t &width, uint32_t &height) {
  NWindow *window = nwindowGetDefault();
  u32 w = 0, h = 0;
  if (!window || R_FAILED(nwindowGetDimensions(window, &w, &h)) || !w || !h)
    return false;
  width = w;
  height = h;
  return true;
}

#elif defined(__ANDROID__)

// NieR (2026-11-07): the app's one SDL window; plume's VK_KHR_android_surface takes its ANativeWindow.
namespace {
SDL_Window *FindGameSdlWindow() {
  int count = 0;
  SDL_Window **windows = SDL_GetWindows(&count);
  SDL_Window *sdl_window = (windows && count > 0) ? windows[0] : nullptr;
  SDL_free(windows);
  return sdl_window;
}
} // namespace

bool GetNativeRenderWindow(plume::RenderWindow &out) {
  SDL_Window *sdl_window = FindGameSdlWindow();
  out = sdl_window ? static_cast<ANativeWindow *>(SDL_GetPointerProperty(
                         SDL_GetWindowProperties(sdl_window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr))
                   : nullptr;
  if (!out) {
    RGPU_ERROR("No Android window for the Vulkan surface");
    return false;
  }
  return true;
}

bool GetRenderWindowPixelSize(uint32_t &width, uint32_t &height) {
  SDL_Window *sdl_window = FindGameSdlWindow();
  int w = 0, h = 0;
  if (!sdl_window || !SDL_GetWindowSizeInPixels(sdl_window, &w, &h))
    return false;
  width = uint32_t(w);
  height = uint32_t(h);
  return true;
}

void ApplySurfaceGeometry() {
  // NieR TEMP experiment (2026-11-11, Mali): env NIER_SURFACE_DIV=N makes the window's buffers 1/N of the view on each
  // axis; the compositor scales them to the screen, so the present's scan-out writes N*N times fewer pixels.
  static const int div = [] {
    const char *e = std::getenv("NIER_SURFACE_DIV");
    return e ? std::atoi(e) : 0;
  }();
  if (div < 2)
    return;
  plume::RenderWindow window = nullptr;
  uint32_t w = 0, h = 0;
  if (!GetNativeRenderWindow(window) || !GetRenderWindowPixelSize(w, h) || !w || !h)
    return;
  const int32_t bw = int32_t(w) / div, bh = int32_t(h) / div;
  ANativeWindow_setBuffersGeometry(window, bw, bh, 0);
  RGPU_INFO("[android] surface buffers {}x{} for a {}x{} view (NIER_SURFACE_DIV)", bw, bh, w, h);
}

namespace {
ANativeWindow *g_held = nullptr;  // render thread (under the video mutex)
std::atomic<bool> g_background{false};

bool SDLCALL WatchLifecycle(void *, SDL_Event *event) {
  if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND || event->type == SDL_EVENT_DID_ENTER_BACKGROUND) {
    g_background = true;
  } else if (event->type == SDL_EVENT_WILL_ENTER_FOREGROUND || event->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
    g_background = false;
  }
  return true;
}
} // namespace

void HoldSurface(plume::RenderWindow window) {
  static const bool watching = [] { return SDL_AddEventWatch(WatchLifecycle, nullptr); }();
  (void)watching;
  auto *w = static_cast<ANativeWindow *>(window);
  if (w == g_held)
    return;
  if (w)
    ANativeWindow_acquire(w);
  if (g_held)
    ANativeWindow_release(g_held);
  g_held = w;
}

plume::RenderWindow NewSurface() {
  SDL_Window *sdl_window = FindGameSdlWindow();
  if (!sdl_window)
    return nullptr;
  // SDL leaves the property on the old (released) window while the app is away and sets it to the new one when
  // the surface comes back. The held reference keeps the old one alive: a different address is a new surface.
  auto *w = static_cast<ANativeWindow *>(
      SDL_GetPointerProperty(SDL_GetWindowProperties(sdl_window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr));
  return w && w != g_held ? w : nullptr;
}

bool InBackground() { return g_background.load(); }

bool HeldSurfaceSize(uint32_t &width, uint32_t &height) {
  if (!g_held)
    return false;
  const int32_t w = ANativeWindow_getWidth(g_held), h = ANativeWindow_getHeight(g_held);
  if (w <= 0 || h <= 0)
    return false;
  width = uint32_t(w);
  height = uint32_t(h);
  return true;
}

#else

namespace {

// Runs before main, so before the SDK initialises SDL. plume builds its Vulkan
// surface with VK_KHR_xlib_surface, which needs the X11 video driver (XWayland
// on Wayland sessions). An explicit user setting is left alone.
[[maybe_unused]] const bool kPreferX11 = [] {
  setenv("SDL_VIDEO_DRIVER", "x11", 0);
  return true;
}();

// The app owns one window.
SDL_Window *FindGameSdlWindow() {
  int count = 0;
  SDL_Window **windows = SDL_GetWindows(&count);
  SDL_Window *sdl_window = (windows && count > 0) ? windows[0] : nullptr;
  SDL_free(windows);
  return sdl_window;
}

} // namespace

bool GetNativeRenderWindow(plume::RenderWindow &out) {
  SDL_Window *sdl_window = FindGameSdlWindow();
  if (!sdl_window) {
    RGPU_ERROR("No SDL window for the Vulkan surface");
    return false;
  }

  const SDL_PropertiesID props = SDL_GetWindowProperties(sdl_window);
  out.display = static_cast<Display *>(
      SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr));
  out.window = static_cast<Window>(
      SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
  if (!out.display || !out.window) {
    RGPU_ERROR("SDL window has no X11 handles (video driver '{}'); the Vulkan "
               "surface needs SDL_VIDEO_DRIVER=x11",
               SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none");
    return false;
  }
  return true;
}

bool GetRenderWindowPixelSize(uint32_t &width, uint32_t &height) {
  SDL_Window *sdl_window = FindGameSdlWindow();
  int w = 0, h = 0;
  if (!sdl_window || !SDL_GetWindowSizeInPixels(sdl_window, &w, &h))
    return false;
  width = uint32_t(w);
  height = uint32_t(h);
  return true;
}

#endif

} // namespace renut::gpu
