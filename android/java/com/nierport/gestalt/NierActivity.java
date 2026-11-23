package com.nierport.gestalt;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

// SDL3 is linked statically into librexruntime.so (its JNI_OnLoad registers
// SDL's natives); libmain.so is the game, exporting SDL_main.
public class NierActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // The game is read from NierPort/ in shared storage, which needs "All
        // files access" (granted by the user in the system settings).
        if (!Environment.isExternalStorageManager()) {
            Toast.makeText(this, "Permita o acesso a todos os arquivos e abra o jogo de novo",
                    Toast.LENGTH_LONG).show();
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName())));
            } catch (Exception e) {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            }
        }
        useCustomDriver();
        super.onCreate(savedInstanceState);
        preferFastestRefresh();
        // 2026-11-19: the status bar came back over the game now and then (black, 104 px on the Poco: the top of the
        // picture and the touch buttons cut). SDL hides it with the old setSystemUiVisibility flags, which HyperOS
        // dropped after a focus change; the insets controller hides it, and again whenever it shows for good (a
        // swipe's transient bars report no insets and hide by themselves).
        hideSystemBars();
        getWindow().getDecorView().setOnApplyWindowInsetsListener(new android.view.View.OnApplyWindowInsetsListener() {
            @Override
            public android.view.WindowInsets onApplyWindowInsets(android.view.View v, android.view.WindowInsets insets) {
                if (insets.isVisible(android.view.WindowInsets.Type.statusBars())
                        || insets.isVisible(android.view.WindowInsets.Type.navigationBars())) {
                    v.removeCallbacks(hideBars);
                    v.postDelayed(hideBars, 400);
                }
                return v.onApplyWindowInsets(insets);
            }
        });
    }

    private final Runnable hideBars = new Runnable() {
        @Override
        public void run() {
            hideSystemBars();
        }
    };

    private void hideSystemBars() {
        try {
            getWindow().setDecorFitsSystemWindows(false);
            android.view.WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.setSystemBarsBehavior(android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                c.hide(android.view.WindowInsets.Type.statusBars() | android.view.WindowInsets.Type.navigationBars());
            }
        } catch (Exception e) {
            // SDL's own flags stay.
        }
    }

    // 2026-11-19 (the user: lying in bed, the picture turned over with the phone): sensorLandscape (the manifest and
    // SDL's choice) follows the sensor between both landscape sides even with the system's rotation locked. Both
    // sides, as the system's auto-rotate allows (locked there: the side the phone is in stays).
    // (A lock to one side was too much: the user wants both, 2026-11-19.)
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    // 2026-11-15: Turnip (Mesa's Adreno driver) instead of the system's when NierPort/turnip/libvulkan_freedreno.so
    // exists: copied into the app's own files (a driver can't be loaded from shared storage) and handed to the native
    // side (android_driver.cpp, through libadrenotools) before the game starts. Delete the folder for the system's.
    private void useCustomDriver() {
        try {
            java.io.File source = new java.io.File(Environment.getExternalStorageDirectory(),
                    "NierPort/turnip/libvulkan_freedreno.so");
            java.io.File dir = new java.io.File(getFilesDir(), "turnip");
            java.io.File dest = new java.io.File(dir, "libvulkan_freedreno.so");
            if (!source.isFile()) {
                if (dest.exists()) dest.delete();
                return;
            }
            dir.mkdirs();
            if (!dest.isFile() || dest.length() != source.length() || dest.lastModified() < source.lastModified()) {
                java.nio.file.Files.copy(source.toPath(), dest.toPath(),
                        java.nio.file.StandardCopyOption.REPLACE_EXISTING);
            }
            android.system.Os.setenv("NIER_DRIVER_DIR", dir.getPath(), true);
            android.system.Os.setenv("NIER_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir, true);
        } catch (Exception e) {
            // The system's driver.
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        preferFastestRefresh();
        hideSystemBars();
    }

    // The display's refresh for the game's frame rate limit (frame_rate_limit in NierPort/nier.toml): the slowest
    // mode at its current size that still shows every frame (60 Hz for 60 fps or less, 120 Hz above it; the fastest
    // with no limit). 2026-11-15: it was always the fastest (120 Hz on the Poco) also at 60 fps, a panel at twice the
    // rate for nothing, and heat.
    private void preferFastestRefresh() {
        try {
            int limit = 60;
            try {
                for (String line : java.nio.file.Files.readAllLines(new java.io.File(
                        Environment.getExternalStorageDirectory(), "NierPort/nier.toml").toPath())) {
                    String s = line.trim();
                    if (s.startsWith("frame_rate_limit")) {
                        limit = Integer.parseInt(s.substring(s.indexOf('=') + 1).trim().replace("\"", ""));
                    }
                }
            } catch (Exception e) {
                // 60
            }
            final float wanted = limit <= 0 ? 10000f : limit;
            android.view.Display display = getWindowManager().getDefaultDisplay();
            android.view.Display.Mode current = display.getMode();
            android.view.Display.Mode best = null;
            android.view.Display.Mode fastest = current;
            for (android.view.Display.Mode m : display.getSupportedModes()) {
                if (m.getPhysicalWidth() != current.getPhysicalWidth()
                        || m.getPhysicalHeight() != current.getPhysicalHeight()) continue;
                if (m.getRefreshRate() > fastest.getRefreshRate()) fastest = m;
                // At least the frame rate (and not below 60: some panels flicker or judder at 30/48).
                if (m.getRefreshRate() + 0.5f >= Math.max(wanted, 60f)
                        && (best == null || m.getRefreshRate() < best.getRefreshRate())) best = m;
            }
            if (best == null) best = fastest;
            android.view.WindowManager.LayoutParams lp = getWindow().getAttributes();
            if (lp.preferredDisplayModeId != best.getModeId()) {
                lp.preferredDisplayModeId = best.getModeId();
                lp.preferredRefreshRate = best.getRefreshRate();
                getWindow().setAttributes(lp);
            }
        } catch (Exception e) {
            // Keep the system's choice.
        }
    }

    // The battery's temperature in tenths of a degree (-1: unknown), for the performance meter (perf_overlay.cpp,
    // called from its own thread). The phone's thermal files are closed to apps; the battery's sticky broadcast is not.
    public int nierBatteryTemperature() {
        try {
            Intent battery = registerReceiver(null, new android.content.IntentFilter(Intent.ACTION_BATTERY_CHANGED));
            return battery != null ? battery.getIntExtra(android.os.BatteryManager.EXTRA_TEMPERATURE, -1) : -1;
        } catch (Exception e) {
            return -1;
        }
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {"c++_shared", "TracyClient", "rexruntime", "rexgpu-xenos", "main"};
    }
}
