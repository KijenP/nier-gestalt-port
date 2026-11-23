/**
 * @file    gpu/backend/native_window.h
 * @brief   The game window, found from the platform rather than the SDK.
 */
#pragma once

#include <cstdint>

#include <plume_render_interface.h>

namespace renut::gpu {

// Windows: the HWND of this process's game window (the SDK's SDL window).
// Linux: the Xlib display + window of the SDL window (plume's Linux Vulkan
// surface is Xlib, so SDL is pinned to its X11 driver).
bool GetNativeRenderWindow(plume::RenderWindow &out);

// The game window's client area, in pixels.
bool GetRenderWindowPixelSize(uint32_t &width, uint32_t &height);

#if defined(__ANDROID__)
// NieR TEMP (2026-11-11): NIER_SURFACE_DIV, the window's buffers smaller than the view (native_window.cpp).
void ApplySurfaceGeometry();

// NieR (2026-11-13): Android destroys the window's surface when the app leaves the screen and makes a new one when
// it comes back; the swapchain made on the old one then presents nothing for good (the screen stayed black).
// HoldSurface keeps a reference to the window the swapchain was made on (so its address is never reused);
// NewSurface returns SDL's current window when it is another one (the swapchain must be made again on it), else
// nullptr; InBackground: the app is off screen (presenting then only fails).
void HoldSurface(plume::RenderWindow window);
plume::RenderWindow NewSurface();
bool InBackground();
// The held surface's size now (Android sets the window to full screen after the app starts, sometimes after the
// swapchain was made: a band of the screen stayed black).
bool HeldSurfaceSize(uint32_t &width, uint32_t &height);
#endif

// The swapchain's format, which the present and overlay pipelines draw into. NieR (2026-11-07): Android's surfaces
// (Mali) offer R8G8B8A8, not B8G8R8A8, and plume leaves the swapchain unmade (then the driver crashes) without a match.
#if defined(__ANDROID__)
inline constexpr plume::RenderFormat kNativeSwapChainFormat = plume::RenderFormat::R8G8B8A8_UNORM;
#else
inline constexpr plume::RenderFormat kNativeSwapChainFormat = plume::RenderFormat::B8G8R8A8_UNORM;
#endif

} // namespace renut::gpu
