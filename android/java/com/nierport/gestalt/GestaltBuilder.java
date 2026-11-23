package com.nierport.gestalt;

import android.content.Context;
import android.content.res.AssetManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;

import com.android.apksig.ApkSigner;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.KeyFactory;
import java.security.PrivateKey;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.security.spec.PKCS8EncodedKeySpec;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Enumeration;
import java.util.List;
import java.util.zip.CRC32;
import java.util.zip.Deflater;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipOutputStream;

// Project Gestalt's work (2026-11-15), off the UI thread: checks the player's copy of NieR Gestalt (an Xbox 360 disc
// image or the extracted folder: NieR's title ID in default.xex, the media folder), takes the disc's tile (nxeart)
// as the game's icon, puts the bundled translation and nier.toml in NierPort, then makes the "NieR Gestalt" app: the
// game's manifest and resources (assets/game_base.apk, built on the PC) with the disc's icon, this app's code and the
// game's native libraries (assets/lib), signed on the phone (apksig) with the key in assets/sign. Nothing of the game
// is in this app: the icon comes from the player's disc. Same disc reading as tools/GameImage.cs and
// src/disc_image.h.
final class GestaltBuilder {
    static final int NIER_TITLE_ID = 0x535107E8;

    // Steps, in the order Listener.step reports them.
    static final int STEP_OPEN = 0, STEP_XEX = 1, STEP_CHECK = 2, STEP_FILES = 3, STEP_ICON = 4, STEP_TEXT = 5,
            STEP_CONFIG = 6, STEP_APK = 7, STEP_SIGN = 8, STEPS = 9;

    // Errors (SetupActivity shows the text).
    static final class Failure extends Exception {
        final String key, detail;

        Failure(String key) { this(key, ""); }

        Failure(String key, String detail) {
            super(key);
            this.key = key;
            this.detail = detail;
        }
    }

    interface Listener {
        void step(int step, float fraction);  // fraction of the whole job, 0..1
    }

    private final Context context;
    private final Listener listener;

    GestaltBuilder(Context context, Listener listener) {
        this.context = context;
        this.listener = listener;
    }

    // ---- the copy of the game ---------------------------------------------------------------------------------

    // An Xbox 360 disc image's game partition (XDVDFS / GDFX).
    static final class Disc implements AutoCloseable {
        private static final long SECTOR = 2048;
        private static final long[] GAME_OFFSETS = {0x00000000L, 0x0000FB20L, 0x00020600L, 0x02080000L, 0x0FD90000L};
        private final RandomAccessFile file;
        private long gameOffset = -1;
        private byte[] root;

        Disc(File path) throws IOException {
            file = new RandomAccessFile(path, "r");
            for (long offset : GAME_OFFSETS) {
                byte[] head = read(offset + 32 * SECTOR, 28);
                if (head == null || !new String(head, 0, 20, StandardCharsets.US_ASCII).equals("MICROSOFT*XBOX*MEDIA"))
                    continue;
                long rootSector = le32(head, 20), rootSize = le32(head, 24);
                if (rootSize < 13 || rootSize > 32 << 20) return;
                root = read(offset + rootSector * SECTOR, (int) rootSize);
                if (root != null) gameOffset = offset;
                return;
            }
        }

        boolean isXbox() { return gameOffset >= 0; }

        // {sector, size, directory ? 1 : 0} of a path from the root ("media/x"), or null.
        long[] find(String path) throws IOException {
            byte[] dir = root;
            String[] parts = path.split("/");
            for (int i = 0; i < parts.length; i++) {
                long[] e = findIn(dir, 0, parts[i], 0);
                if (e == null) return null;
                if (i == parts.length - 1) return e;
                if (e[2] == 0 || e[1] > 32 << 20 || (dir = read(gameOffset + e[0] * SECTOR, (int) e[1])) == null)
                    return null;
            }
            return null;
        }

        byte[] readFile(String path, long max) throws IOException {
            long[] e = find(path);
            if (e == null || e[2] != 0 || e[1] > max) return null;
            return read(gameOffset + e[0] * SECTOR, (int) e[1]);
        }

        private static long[] findIn(byte[] dir, int ordinal, String name, int depth) {
            int p = ordinal * 4;
            if (depth > 64 || p + 14 > dir.length) return null;
            int left = (dir[p] & 0xFF) | (dir[p + 1] & 0xFF) << 8, right = (dir[p + 2] & 0xFF) | (dir[p + 3] & 0xFF) << 8;
            int len = dir[p + 13] & 0xFF;
            if (p + 14 + len > dir.length) return null;
            if (new String(dir, p + 14, len, StandardCharsets.US_ASCII).equalsIgnoreCase(name))
                return new long[] {le32(dir, p + 4), le32(dir, p + 8), (dir[p + 12] & 0x10) != 0 ? 1 : 0};
            long[] r = null;
            if (left != 0 && left != 0xFFFF) r = findIn(dir, left, name, depth + 1);
            if (r == null && right != 0 && right != 0xFFFF) r = findIn(dir, right, name, depth + 1);
            return r;
        }

        private byte[] read(long offset, int count) throws IOException {
            if (offset < 0 || offset + count > file.length()) return null;
            byte[] b = new byte[count];
            file.seek(offset);
            file.readFully(b);
            return b;
        }

        @Override
        public void close() throws IOException { file.close(); }
    }

    static long le32(byte[] b, int o) {
        return (b[o] & 0xFFL) | (b[o + 1] & 0xFFL) << 8 | (b[o + 2] & 0xFFL) << 16 | (b[o + 3] & 0xFFL) << 24;
    }

    static int be32(byte[] b, int o) {
        return (b[o] & 0xFF) << 24 | (b[o + 1] & 0xFF) << 16 | (b[o + 2] & 0xFF) << 8 | (b[o + 3] & 0xFF);
    }

    // XEX2: optional header 0x00040006 is the execution info; the title ID is at +12. 0 if none.
    static int titleId(byte[] xex) {
        if (xex == null || xex.length < 0x18 || !new String(xex, 0, 4, StandardCharsets.US_ASCII).equals("XEX2")) return 0;
        int count = be32(xex, 0x14);
        for (int i = 0; i < count && 0x18 + i * 8 + 8 <= xex.length; i++) {
            int key = be32(xex, 0x18 + i * 8), value = be32(xex, 0x18 + i * 8 + 4);
            if (key == 0x00040006 && value >= 0 && value + 16 <= xex.length) return be32(xex, value + 12);
        }
        return 0;
    }

    // nxeart (an STFS package) carries the title tile as a PNG: size at 0x1712, data at 0x171A.
    static byte[] tile(byte[] stfs) {
        if (stfs == null || stfs.length < 0x171A + 8) return null;
        String magic = new String(stfs, 0, 4, StandardCharsets.US_ASCII);
        if (!magic.equals("PIRS") && !magic.equals("LIVE") && !magic.equals("CON ")) return null;
        int size = be32(stfs, 0x1712);
        if (size <= 8 || 0x171A + size > stfs.length || (stfs[0x171A] & 0xFF) != 0x89) return null;
        byte[] png = new byte[size];
        System.arraycopy(stfs, 0x171A, png, 0, size);
        return png;
    }

    static byte[] readAll(File f, long max) throws IOException {
        if (!f.isFile() || f.length() > max) return null;
        return Files.readAllBytes(f.toPath());
    }

    // Checks the copy; returns the disc tile (or null). `game` is an .iso or a folder.
    byte[] check(File game) throws Failure {
        try {
            listener.step(STEP_OPEN, 0.01f);
            byte[] xex, art;
            if (game.isDirectory()) {
                listener.step(STEP_XEX, 0.03f);
                xex = readAll(new File(game, "default.xex"), 64 << 20);
                if (xex == null) throw new Failure("no_xex");
                check(xex);
                listener.step(STEP_FILES, 0.07f);
                if (!new File(game, "media").isDirectory()) throw new Failure("no_media");
                listener.step(STEP_ICON, 0.09f);
                art = readAll(new File(game, "nxeart"), 32 << 20);
            } else {
                if (!game.canRead()) throw new Failure("open_failed");
                try (Disc disc = new Disc(game)) {
                    if (!disc.isXbox()) throw new Failure("not_xbox");
                    listener.step(STEP_XEX, 0.03f);
                    xex = disc.readFile("default.xex", 64 << 20);
                    if (xex == null) throw new Failure("no_xex");
                    check(xex);
                    listener.step(STEP_FILES, 0.07f);
                    long[] media = disc.find("media");
                    if (media == null || media[2] == 0) throw new Failure("no_media");
                    listener.step(STEP_ICON, 0.09f);
                    art = disc.readFile("nxeart", 32 << 20);
                }
            }
            return tile(art);
        } catch (IOException e) {
            throw new Failure("open_failed", e.getMessage() == null ? "" : e.getMessage());
        }
    }

    private void check(byte[] xex) throws Failure {
        listener.step(STEP_CHECK, 0.05f);
        int title = titleId(xex);
        if (title == 0) throw new Failure("bad_xex");
        if (title != NIER_TITLE_ID) throw new Failure("wrong_game", String.format("%08X", title));
    }

    // ---- NierPort ---------------------------------------------------------------------------------------------

    // The bundled translation (assets/TEXT) into NierPort/TEXT.
    void copyTranslation(File nierPort) throws Failure {
        listener.step(STEP_TEXT, 0.10f);
        try {
            List<String> files = new ArrayList<>();
            listAssets(context.getAssets(), "TEXT", files);
            byte[] buf = new byte[1 << 16];
            for (int i = 0; i < files.size(); i++) {
                File out = new File(nierPort, files.get(i));
                out.getParentFile().mkdirs();
                try (InputStream in = context.getAssets().open(files.get(i));
                     OutputStream o = new FileOutputStream(out)) {
                    for (int n; (n = in.read(buf)) > 0; ) o.write(buf, 0, n);
                }
                listener.step(STEP_TEXT, 0.10f + 0.08f * (i + 1) / files.size());
            }
        } catch (IOException e) {
            throw new Failure("save_failed", e.getMessage() == null ? "" : e.getMessage());
        }
    }

    private static void listAssets(AssetManager assets, String dir, List<String> out) throws IOException {
        String[] names = assets.list(dir);
        if (names == null) return;
        for (String name : names) {
            String path = dir + "/" + name;
            String[] sub = assets.list(path);
            if (sub != null && sub.length > 0) listAssets(assets, path, out);
            else out.add(path);
        }
    }

    // ---- the NieR Gestalt app ---------------------------------------------------------------------------------

    // The launcher icon (192x192, xxxhdpi) from the disc's tile, or null to keep the base's.
    static byte[] launcherIcon(byte[] tile) {
        if (tile == null) return null;
        Bitmap src = BitmapFactory.decodeByteArray(tile, 0, tile.length);
        if (src == null) return null;
        Bitmap icon = Bitmap.createBitmap(192, 192, Bitmap.Config.ARGB_8888);
        Paint paint = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
        new Canvas(icon).drawBitmap(src, new Rect(0, 0, src.getWidth(), src.getHeight()), new Rect(0, 0, 192, 192), paint);
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        icon.compress(Bitmap.CompressFormat.PNG, 100, out);
        return out.toByteArray();
    }

    // Makes the signed APK; returns it.
    File buildApk(byte[] icon) throws Failure {
        File dir = new File(context.getCacheDir(), "gestalt");
        dir.mkdirs();
        File unsigned = new File(dir, "nier-unsigned.apk"), signed = new File(dir, "NieR Gestalt.apk");
        unsigned.delete();
        signed.delete();
        try {
            listener.step(STEP_APK, 0.20f);
            File self = new File(context.getApplicationInfo().sourceDir);
            File base = new File(dir, "game_base.apk");
            try (InputStream in = context.getAssets().open("game_base.apk")) {
                Files.copy(in, base.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
            }
            try (ZipFile game = new ZipFile(base); ZipFile app = new ZipFile(self);
                 ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(unsigned))) {
                // The game's manifest and resources, the icon replaced by the disc's.
                for (Enumeration<? extends ZipEntry> e = game.entries(); e.hasMoreElements(); ) {
                    ZipEntry entry = e.nextElement();
                    String name = entry.getName();
                    if (name.startsWith("META-INF/")) continue;
                    byte[] data = icon != null && name.startsWith("res/") && name.endsWith("ic_launcher.png")
                            ? icon : readEntry(game, entry);
                    // resources.arsc stays uncompressed (Android 11+ requires it; apksig aligns it).
                    put(zip, name, data, name.equals("resources.arsc"));
                }
                // This app's code (the same classes run the game).
                put(zip, "classes.dex", readEntry(app, app.getEntry("classes.dex")), false);
                // The game's native libraries, carried here as assets (not installed for this app).
                List<ZipEntry> libs = new ArrayList<>();
                long total = 0;
                for (Enumeration<? extends ZipEntry> e = app.entries(); e.hasMoreElements(); ) {
                    ZipEntry entry = e.nextElement();
                    if (entry.getName().startsWith("assets/lib/") && entry.getName().endsWith(".so")) {
                        libs.add(entry);
                        total += Math.max(1, entry.getSize());
                    }
                }
                if (libs.isEmpty()) throw new Failure("apk_failed", "assets/lib");
                zip.setLevel(Deflater.BEST_SPEED);
                long done = 0;
                byte[] buf = new byte[1 << 16];
                for (ZipEntry entry : libs) {
                    String leaf = entry.getName().substring("assets/lib/".length());
                    zip.putNextEntry(new ZipEntry("lib/arm64-v8a/" + leaf));
                    try (InputStream in = app.getInputStream(entry)) {
                        for (int n; (n = in.read(buf)) > 0; ) {
                            zip.write(buf, 0, n);
                            done += n;
                            listener.step(STEP_APK, 0.20f + 0.60f * Math.min(1f, done / (float) total));
                        }
                    }
                    zip.closeEntry();
                }
            }
            base.delete();

            listener.step(STEP_SIGN, 0.82f);
            PrivateKey key;
            X509Certificate cert;
            try (InputStream k = context.getAssets().open("sign/key.pk8");
                 InputStream c = context.getAssets().open("sign/cert.der")) {
                key = KeyFactory.getInstance("RSA").generatePrivate(new PKCS8EncodedKeySpec(readStream(k)));
                cert = (X509Certificate) CertificateFactory.getInstance("X.509").generateCertificate(c);
            }
            ApkSigner.SignerConfig signer =
                    new ApkSigner.SignerConfig.Builder("gestalt", key, Collections.singletonList(cert)).build();
            new ApkSigner.Builder(Collections.singletonList(signer))
                    .setInputApk(unsigned)
                    .setOutputApk(signed)
                    .setMinSdkVersion(30)
                    .setV1SigningEnabled(false)
                    .setV2SigningEnabled(true)
                    .setV3SigningEnabled(false)
                    .build()
                    .sign();
            unsigned.delete();
            listener.step(STEP_SIGN, 0.97f);
            return signed;
        } catch (Failure f) {
            throw f;
        } catch (Exception e) {
            throw new Failure("apk_failed", e.getClass().getSimpleName() + ": " + e.getMessage());
        }
    }

    private static void put(ZipOutputStream zip, String name, byte[] data, boolean stored) throws IOException {
        ZipEntry entry = new ZipEntry(name);
        if (stored) {
            CRC32 crc = new CRC32();
            crc.update(data);
            entry.setMethod(ZipEntry.STORED);
            entry.setSize(data.length);
            entry.setCompressedSize(data.length);
            entry.setCrc(crc.getValue());
        }
        zip.putNextEntry(entry);
        zip.write(data);
        zip.closeEntry();
    }

    private static byte[] readEntry(ZipFile zip, ZipEntry entry) throws IOException {
        if (entry == null) throw new IOException("missing entry");
        try (InputStream in = zip.getInputStream(entry)) {
            return readStream(in);
        }
    }

    private static byte[] readStream(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[1 << 16];
        for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
        return out.toByteArray();
    }
}
