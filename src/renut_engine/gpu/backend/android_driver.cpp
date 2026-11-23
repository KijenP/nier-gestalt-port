// NieR (2026-11-15): another Vulkan driver on Adreno phones: Turnip (Mesa's open driver) loaded through libadrenotools
// (thirdparty/libadrenotools, BSD-2), as Renut-Android does. NierActivity copies NierPort/turnip/libvulkan_freedreno.so
// into the app's own files (a driver can't be loaded from shared storage) and passes the folders in NIER_DRIVER_DIR
// and NIER_NATIVE_LIB_DIR (where libadrenotools' hooks are installed). plume asks plume_android_custom_vulkan before
// loading the system's Vulkan; null keeps the system's driver.
#if defined(__ANDROID__)

#include <dlfcn.h>
#include <sys/stat.h>

#include <cstdlib>
#include <string>

#include <adrenotools/driver.h>
#include <android/log.h>

#include <rex/logging.h>

extern "C" void *plume_android_custom_vulkan() {
  const char *dir = std::getenv("NIER_DRIVER_DIR");
  const char *hooks = std::getenv("NIER_NATIVE_LIB_DIR");
  if (!dir || !*dir || !hooks || !*hooks) return nullptr;
  std::string folder = dir;
  if (folder.back() != '/') folder += '/';
  std::string hook_folder = hooks;
  if (hook_folder.back() != '/') hook_folder += '/';
  const char *name = "libvulkan_freedreno.so";
  struct stat st{};
  if (stat((folder + name).c_str(), &st) != 0) return nullptr;
  void *vulkan = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr, hook_folder.c_str(),
                                            folder.c_str(), name, nullptr, nullptr);
  void *get_instance_proc_addr = vulkan ? dlsym(vulkan, "vkGetInstanceProcAddr") : nullptr;
  __android_log_print(ANDROID_LOG_INFO, "NieR", "custom Vulkan driver %s%s: %s", folder.c_str(), name,
                      get_instance_proc_addr ? "loaded" : "FAILED");
  REXLOG_INFO("Vulkan driver: {} ({}{})", get_instance_proc_addr ? "Turnip (libadrenotools)" : "Turnip FAILED, system's",
              folder, name);
  return get_instance_proc_addr;
}

#endif
