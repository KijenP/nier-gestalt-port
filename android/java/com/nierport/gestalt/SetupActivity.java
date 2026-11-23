package com.nierport.gestalt;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

// The launcher (2026-11-15). The same code runs as two apps, told apart by the package name:
//  - Project Gestalt (com.nierport.project), the installer that is shipped: in Portuguese, English or Spanish it
//    explains what it does, asks for "All files access", lets the player pick the copy of NieR Gestalt (the .iso or
//    the extracted folder), checks it and makes the "NieR Gestalt" app with the disc's icon (GestaltBuilder), installs
//    it and offers to uninstall itself.
//  - NieR Gestalt (com.nierport.gestalt), the game: asks for "All files access" if it lacks it and starts the game
//    from the copy saved in NierPort/nier.toml (or asks for one).
public class SetupActivity extends Activity {
    static final String PROJECT = "com.nierport.project", GAME = "com.nierport.gestalt";
    private static final int PICK_FILE = 1, PICK_FOLDER = 2;
    private static final String INSTALLED = "com.nierport.gestalt.INSTALLED";

    // NieR's menu colours.
    private static final int PAPER = 0xFFD6D3CA, PAPER_DARK = 0xFFBEBAAF, INK = 0xFF1E2028, SOFT = 0xFF505260,
            LIGHT = 0xFFEEECE6, RED = 0xFF962020, BACK = 0xFF101014;

    private int language;  // 0 pt, 1 en, 2 es
    private boolean picking, working, installed;
    private String error, errorDetail;
    private LinearLayout page;
    private TextView stepText, percentText;
    private Bar bar;
    private File builtApk;

    private boolean isProject() { return PROJECT.equals(getPackageName()); }

    static File nierPort() { return new File(Environment.getExternalStorageDirectory(), "NierPort"); }

    // ---- texts -----------------------------------------------------------------------------------------------

    private String t(String key) {
        String[] v = TEXTS.get(key);
        return v == null ? key : v[language];
    }

    private static final java.util.Map<String, String[]> TEXTS = new java.util.HashMap<>();

    private static void text(String key, String pt, String en, String es) { TEXTS.put(key, new String[] {pt, en, es}); }

    static {
        text("lang0", "Português", "Português", "Português");
        text("lang1", "English", "English", "English");
        text("lang2", "Español", "Español", "Español");
        text("project_sub", "Instalador do NieR Gestalt para Android", "NieR Gestalt installer for Android",
                "Instalador de NieR Gestalt para Android");
        text("game_sub", "Falta só um passo para jogar.", "Just one step left before you play.",
                "Solo falta un paso para jugar.");
        text("why",
                "Este app não traz nenhum arquivo do jogo. Ele lê a sua própria cópia de NieR Gestalt (Xbox 360), "
                        + "confere se está tudo certo e cria o app \"NieR Gestalt\" com o ícone do seu disco. Depois "
                        + "disso, este instalador pode ser desinstalado.",
                "This app carries none of the game's files. It reads your own copy of NieR Gestalt (Xbox 360), checks "
                        + "it and creates the \"NieR Gestalt\" app with your disc's icon. After that, this installer "
                        + "can be uninstalled.",
                "Esta app no trae ningún archivo del juego. Lee tu propia copia de NieR Gestalt (Xbox 360), la "
                        + "comprueba y crea la app \"NieR Gestalt\" con el icono de tu disco. Después, este instalador "
                        + "se puede desinstalar.");
        text("game_why",
                "Os gráficos, sons e textos são lidos da sua própria cópia de NieR Gestalt (Xbox 360).",
                "The graphics, sound and text are read from your own copy of NieR Gestalt (Xbox 360).",
                "Los gráficos, el sonido y los textos se leen de tu propia copia de NieR Gestalt (Xbox 360).");
        text("perm",
                "Primeiro, permita o acesso a todos os arquivos: o jogo lê a sua ISO direto do armazenamento. Toque "
                        + "no botão, ative a permissão e volte para cá.",
                "First, allow access to all files: the game reads your ISO straight from storage. Tap the button, "
                        + "turn the permission on and come back.",
                "Primero, permite el acceso a todos los archivos: el juego lee tu ISO directamente del "
                        + "almacenamiento. Toca el botón, activa el permiso y vuelve.");
        text("perm_btn", "Dar permissão", "Allow", "Dar permiso");
        text("choose", "Onde está a sua cópia?", "Where is your copy?", "¿Dónde está tu copia?");
        text("iso", "Arquivo ISO", "ISO file", "Archivo ISO");
        text("iso_hint", "A imagem do disco (.iso), do jeito que está. Não precisa extrair nada.",
                "The disc image (.iso), as it is. Nothing needs to be extracted.",
                "La imagen del disco (.iso), tal como está. No hace falta extraer nada.");
        text("folder", "Pasta extraída", "Extracted folder", "Carpeta extraída");
        text("folder_hint", "A pasta onde a ISO foi extraída: a que tem o default.xex e a pasta media.",
                "The folder the ISO was extracted to: the one with default.xex and the media folder.",
                "La carpeta donde se extrajo la ISO: la que tiene default.xex y la carpeta media.");
        text("text_lang", "Os textos do jogo ficarão em português.", "The game's text will be in English.",
                "El juego no trae español: los textos estarán en inglés.");
        text("preparing", "Preparando o NieR Gestalt", "Preparing NieR Gestalt", "Preparando NieR Gestalt");
        text("step0", "Abrindo a sua cópia...", "Opening your copy...", "Abriendo tu copia...");
        text("step1", "Lendo o default.xex...", "Reading default.xex...", "Leyendo default.xex...");
        text("step2", "Conferindo se é o NieR Gestalt...", "Checking that it is NieR Gestalt...",
                "Comprobando que sea NieR Gestalt...");
        text("step3", "Conferindo os arquivos do jogo...", "Checking the game files...",
                "Comprobando los archivos del juego...");
        text("step4", "Extraindo o ícone do disco...", "Taking the icon from the disc...", "Sacando el icono del disco...");
        text("step5", "Copiando a tradução...", "Copying the translation...", "Copiando la traducción...");
        text("step6", "Salvando a configuração...", "Saving the settings...", "Guardando la configuración...");
        text("step7", "Montando o app NieR Gestalt...", "Building the NieR Gestalt app...",
                "Montando la app NieR Gestalt...");
        text("step8", "Assinando o app...", "Signing the app...", "Firmando la app...");
        text("ready", "Pronto para instalar", "Ready to install", "Listo para instalar");
        text("ready_hint",
                "O app NieR Gestalt foi criado com o ícone do seu disco. Agora é só instalar.",
                "The NieR Gestalt app was created with your disc's icon. Now just install it.",
                "La app NieR Gestalt se creó con el icono de tu disco. Ahora solo hay que instalarla.");
        text("install", "Instalar o NieR Gestalt", "Install NieR Gestalt", "Instalar NieR Gestalt");
        text("unknown",
                "O Android precisa deixar o Project Gestalt instalar apps. Toque no botão, ative \"Permitir desta "
                        + "fonte\" e volte para cá.",
                "Android must allow Project Gestalt to install apps. Tap the button, turn on \"Allow from this "
                        + "source\" and come back.",
                "Android debe permitir que Project Gestalt instale apps. Toca el botón, activa \"Permitir de esta "
                        + "fuente\" y vuelve.");
        text("unknown_btn", "Permitir", "Allow", "Permitir");
        text("installing", "Instalando...", "Installing...", "Instalando...");
        text("done", "NieR Gestalt instalado!", "NieR Gestalt installed!", "¡NieR Gestalt instalado!");
        text("done_hint",
                "O app NieR Gestalt já está na sua tela inicial. Este instalador não é mais necessário.",
                "The NieR Gestalt app is on your home screen. This installer is no longer needed.",
                "La app NieR Gestalt ya está en tu pantalla de inicio. Este instalador ya no hace falta.");
        text("open", "Abrir o NieR Gestalt", "Open NieR Gestalt", "Abrir NieR Gestalt");
        text("uninstall", "Desinstalar o Project Gestalt", "Uninstall Project Gestalt", "Desinstalar Project Gestalt");
        text("other", "Escolher outra cópia", "Choose another copy", "Elegir otra copia");
        text("retry", "Tentar de novo", "Try again", "Intentar de nuevo");
        text("install_failed",
                "A instalação não terminou. Se já existe um NieR instalado por outro APK, desinstale-o e tente de "
                        + "novo.",
                "The installation did not finish. If a NieR from another APK is installed, uninstall it and try "
                        + "again.",
                "La instalación no terminó. Si ya hay un NieR instalado desde otro APK, desinstálalo e inténtalo de "
                        + "nuevo.");
        text("uninstall_old", "Desinstalar o antigo", "Uninstall the old one", "Desinstalar el antiguo");
        // Errors
        text("not_found", "Não encontrei o jogo em:", "The game was not found at:", "No encontré el juego en:");
        text("not_path",
                "Não consegui abrir esse arquivo direto. Escolha pelo armazenamento interno ou cartão SD (não por "
                        + "\"Recentes\" ou \"Downloads\" do menu lateral).",
                "That file could not be opened directly. Pick it from internal storage or the SD card (not from "
                        + "\"Recent\" or \"Downloads\" in the side menu).",
                "No pude abrir ese archivo directamente. Elígelo desde el almacenamiento interno o la tarjeta SD (no "
                        + "desde \"Recientes\" o \"Descargas\" del menú lateral).");
        text("open_failed", "Não consegui abrir esse arquivo.", "That file could not be opened.",
                "No pude abrir ese archivo.");
        text("not_xbox", "Esse arquivo não parece uma imagem de disco de Xbox 360.",
                "That file does not look like an Xbox 360 disc image.",
                "Ese archivo no parece una imagen de disco de Xbox 360.");
        text("no_xex", "Não achei o default.xex nessa cópia.", "default.xex was not found in this copy.",
                "No encontré default.xex en esta copia.");
        text("bad_xex", "O default.xex está danificado ou não é um executável de Xbox 360.",
                "default.xex is damaged or is not an Xbox 360 executable.",
                "default.xex está dañado o no es un ejecutable de Xbox 360.");
        text("wrong_game", "Essa cópia não é o NieR Gestalt de Xbox 360. ID do título encontrado:",
                "This copy is not NieR Gestalt for the Xbox 360. Title ID found:",
                "Esta copia no es NieR Gestalt de Xbox 360. ID de título encontrado:");
        text("no_media", "Faltam os arquivos do jogo (a pasta media) nessa cópia.",
                "The game files (the media folder) are missing from this copy.",
                "Faltan los archivos del juego (la carpeta media) en esta copia.");
        text("save_failed", "Não consegui gravar na pasta NierPort.", "Could not write to the NierPort folder.",
                "No pude escribir en la carpeta NierPort.");
        text("apk_failed", "Não consegui montar o app.", "Could not build the app.", "No pude montar la app.");
    }

    // ---- lifecycle -------------------------------------------------------------------------------------------

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        language = savedLanguage();
        getWindow().getDecorView().setBackgroundColor(BACK);
        handleInstallResult(getIntent());
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        handleInstallResult(intent);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (!picking && !working) check();
    }

    private void check() {
        if (isProject()) {
            if (installed) {
                showDone();
                return;
            }
            if (builtApk != null) {
                showReady();
                return;
            }
            showChoose(true);
            return;
        }
        // The game.
        if (!Environment.isExternalStorageManager()) {
            showChoose(false);
            return;
        }
        nierPort().mkdirs();
        copyHiddenLog();
        String game = savedGame();
        if (game != null && new File(game).exists()) {
            startGame();
            return;
        }
        if (new File(nierPort(), "Nier.iso").exists()) {
            startGame();
            return;
        }
        if (game != null && error == null) {
            error = "not_found";
            errorDetail = game;
        }
        showChoose(false);
    }

    // ---- pages -----------------------------------------------------------------------------------------------

    private float dp(float v) { return TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()); }

    private LinearLayout newPage(String subtitle, boolean languages) {
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(BACK);
        scroll.setFillViewport(true);
        LinearLayout outer = new LinearLayout(this);
        outer.setGravity(Gravity.CENTER_HORIZONTAL);
        outer.setPadding((int) dp(16), (int) dp(12), (int) dp(16), (int) dp(12));
        scroll.addView(outer, new ViewGroup.LayoutParams(-1, -2));
        page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        GradientDrawable paper = new GradientDrawable();
        paper.setColor(PAPER);
        paper.setCornerRadius(dp(4));
        paper.setStroke((int) dp(1), SOFT);
        page.setBackground(paper);
        int pad = (int) dp(24);
        page.setPadding(pad, (int) dp(18), pad, (int) dp(18));
        int width = Math.min(getResources().getDisplayMetrics().widthPixels - (int) dp(32), (int) dp(820));
        outer.addView(page, new LinearLayout.LayoutParams(width, -2));

        // Header: icon, title, languages.
        LinearLayout head = new LinearLayout(this);
        head.setGravity(Gravity.CENTER_VERTICAL);
        ImageView icon = new ImageView(this);
        Drawable d = getApplicationInfo().loadIcon(getPackageManager());
        icon.setImageDrawable(d);
        head.addView(icon, new LinearLayout.LayoutParams((int) dp(52), (int) dp(52)));
        LinearLayout titles = new LinearLayout(this);
        titles.setOrientation(LinearLayout.VERTICAL);
        titles.setPadding((int) dp(14), 0, 0, 0);
        titles.addView(label(isProject() ? "Project Gestalt" : "NieR Gestalt", 28, INK, true));
        titles.addView(label(subtitle, 15, SOFT, false));
        head.addView(titles, new LinearLayout.LayoutParams(0, -2, 1));
        if (languages) {
            for (int i = 0; i < 3; i++) {
                final int which = i;
                TextView b = label(t("lang" + i), 14, i == language ? LIGHT : INK, true);
                b.setPadding((int) dp(10), (int) dp(6), (int) dp(10), (int) dp(6));
                if (i == language) b.setBackground(round(INK, 0));
                b.setOnClickListener(v -> {
                    language = which;
                    saveLanguage();
                    check();
                });
                head.addView(b);
            }
        }
        page.addView(head);
        View line = new View(this);
        line.setBackgroundColor(SOFT);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, (int) Math.max(1, dp(1)));
        lp.setMargins(0, (int) dp(12), 0, (int) dp(14));
        page.addView(line, lp);
        setContentView(scroll);
        return page;
    }

    private TextView label(String s, float sp, int color, boolean bold) {
        TextView v = new TextView(this);
        v.setText(s);
        v.setTextSize(sp);
        v.setTextColor(color);
        v.setTypeface(Typeface.create(Typeface.SERIF, bold ? Typeface.BOLD : Typeface.NORMAL));
        return v;
    }

    private GradientDrawable round(int fill, int stroke) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(fill);
        g.setCornerRadius(dp(4));
        if (stroke != 0) g.setStroke((int) dp(1.5f), stroke);
        return g;
    }

    private void add(View v, int top) {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.setMargins(0, (int) dp(top), 0, 0);
        page.addView(v, lp);
    }

    // A button like the game's menus: a dark band when pressed.
    private TextView button(String s, float sp, View.OnClickListener action) {
        TextView b = label(s, sp, INK, true);
        b.setGravity(Gravity.CENTER);
        b.setPadding((int) dp(16), (int) dp(12), (int) dp(16), (int) dp(12));
        b.setBackground(round(PAPER_DARK, INK));
        b.setOnTouchListener((v, e) -> {
            boolean down = e.getAction() == android.view.MotionEvent.ACTION_DOWN
                    || e.getAction() == android.view.MotionEvent.ACTION_MOVE;
            v.setBackground(round(down ? INK : PAPER_DARK, INK));
            ((TextView) v).setTextColor(down ? LIGHT : INK);
            return false;
        });
        b.setOnClickListener(action);
        return b;
    }

    private void showError() {
        if (error == null) return;
        String s = t(error) + (errorDetail == null || errorDetail.isEmpty() ? "" : "\n" + errorDetail);
        add(label(s, 15, RED, true), 10);
    }

    // The first page: why, the permission or the two choices.
    private void showChoose(boolean project) {
        newPage(project ? t("project_sub") : t("game_sub"), true);
        add(label(project ? t("why") : t("game_why"), 16, INK, false), 0);
        if (!Environment.isExternalStorageManager()) {
            add(label(t("perm"), 16, INK, false), 14);
            showError();
            add(button(t("perm_btn"), 18, v -> openPermission()), 16);
            return;
        }
        showError();
        add(label(t("choose"), 20, INK, true), 16);
        // 2026-11-15: wrap_content heights (match_parent inside a wrap_content row left the cards 0 px tall).
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setBaselineAligned(false);
        row.addView(card(t("iso"), t("iso_hint"), true, v -> pick(false)), new LinearLayout.LayoutParams(0, -2, 1));
        View gap = new View(this);
        row.addView(gap, new LinearLayout.LayoutParams((int) dp(14), 1));
        row.addView(card(t("folder"), t("folder_hint"), false, v -> pick(true)), new LinearLayout.LayoutParams(0, -2, 1));
        add(row, 10);
        if (project) add(label(t("text_lang"), 14, SOFT, false), 12);
    }

    private View card(String title, String hint, boolean disc, View.OnClickListener action) {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setPadding((int) dp(16), (int) dp(14), (int) dp(16), (int) dp(14));
        c.setBackground(round(PAPER_DARK, INK));
        LinearLayout top = new LinearLayout(this);
        top.setGravity(Gravity.CENTER_VERTICAL);
        Glyph g = new Glyph(this, disc);
        top.addView(g, new LinearLayout.LayoutParams((int) dp(40), (int) dp(40)));
        TextView name = label(title, 20, INK, true);
        name.setPadding((int) dp(12), 0, 0, 0);
        top.addView(name);
        c.addView(top);
        TextView h = label(hint, 14, SOFT, false);
        h.setPadding(0, (int) dp(10), 0, 0);
        c.addView(h);
        c.setOnTouchListener((v, e) -> {
            boolean down = e.getAction() == android.view.MotionEvent.ACTION_DOWN
                    || e.getAction() == android.view.MotionEvent.ACTION_MOVE;
            v.setBackground(round(down ? INK : PAPER_DARK, INK));
            name.setTextColor(down ? LIGHT : INK);
            h.setTextColor(down ? LIGHT : SOFT);
            g.color = down ? LIGHT : INK;
            g.invalidate();
            return false;
        });
        c.setClickable(true);
        c.setMinimumHeight((int) dp(110));
        c.setOnClickListener(action);
        return c;
    }

    // A disc or a folder, drawn.
    private static final class Glyph extends View {
        final boolean disc;
        int color = INK;
        final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);

        Glyph(Context c, boolean disc) {
            super(c);
            this.disc = disc;
        }

        @Override
        protected void onDraw(Canvas canvas) {
            float w = getWidth(), h = getHeight(), s = Math.min(w, h);
            p.setColor(color);
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeWidth(s * 0.07f);
            if (disc) {
                canvas.drawCircle(w / 2, h / 2, s * 0.44f, p);
                canvas.drawCircle(w / 2, h / 2, s * 0.12f, p);
            } else {
                canvas.drawRoundRect(new RectF(s * 0.05f, s * 0.25f, s * 0.95f, s * 0.9f), s * 0.06f, s * 0.06f, p);
                p.setStyle(Paint.Style.FILL);
                canvas.drawRect(s * 0.05f, s * 0.14f, s * 0.42f, s * 0.27f, p);
            }
        }
    }

    // The progress bar, with a soft light running over the filled part.
    private static final class Bar extends View {
        float value, shown;
        final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);

        Bar(Context c) { super(c); }

        @Override
        protected void onDraw(Canvas canvas) {
            float w = getWidth(), h = getHeight(), r = h * 0.15f;
            shown += (value - shown) * 0.2f;
            if (Math.abs(value - shown) < 0.002f) shown = value;
            p.setStyle(Paint.Style.FILL);
            p.setColor(PAPER_DARK);
            canvas.drawRoundRect(new RectF(0, 0, w, h), r, r, p);
            p.setColor(INK);
            canvas.drawRoundRect(new RectF(0, 0, w * shown, h), r, r, p);
            float run = (System.currentTimeMillis() % 1700) / 1700f * w * shown;
            p.setShader(new android.graphics.LinearGradient(run - h * 2, 0, run, 0, 0x00FFFFFF, 0x40FFFFFF,
                    android.graphics.Shader.TileMode.CLAMP));
            canvas.drawRect(Math.max(0, run - h * 2), 0, run, h, p);
            p.setShader(null);
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeWidth(h * 0.06f);
            p.setColor(INK);
            canvas.drawRoundRect(new RectF(0, 0, w, h), r, r, p);
            postInvalidateOnAnimation();
        }
    }

    private void showPreparing() {
        newPage(t("preparing"), false);
        stepText = label(t("step0"), 18, INK, false);
        add(stepText, 30);
        bar = new Bar(this);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, (int) dp(26));
        lp.setMargins(0, (int) dp(14), 0, 0);
        page.addView(bar, lp);
        percentText = label("0%", 16, INK, true);
        percentText.setGravity(Gravity.CENTER);
        add(percentText, 8);
    }

    private void showReady() {
        newPage(t("ready"), false);
        add(label(t("ready_hint"), 16, INK, false), 0);
        if (!getPackageManager().canRequestPackageInstalls()) {
            add(label(t("unknown"), 16, INK, false), 14);
            add(button(t("unknown_btn"), 18, v -> {
                startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + getPackageName())));
            }), 16);
            return;
        }
        showError();
        add(button(t("install"), 20, v -> install()), 18);
        add(button(t("other"), 15, v -> {
            builtApk = null;
            error = null;
            check();
        }), 12);
    }

    private void showDone() {
        newPage(t("done"), false);
        add(label(t("done_hint"), 16, INK, false), 0);
        add(button(t("open"), 20, v -> {
            Intent launch = getPackageManager().getLaunchIntentForPackage(GAME);
            if (launch != null) startActivity(launch);
        }), 18);
        add(button(t("uninstall"), 15, v -> startActivity(new Intent(Intent.ACTION_DELETE, Uri.parse("package:" + PROJECT)))), 12);
    }

    // ---- actions ---------------------------------------------------------------------------------------------

    private void openPermission() {
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
        } catch (Exception e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    private void pick(boolean folder) {
        Intent intent;
        if (folder) {
            intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        } else {
            intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
        }
        picking = true;
        startActivityForResult(intent, folder ? PICK_FOLDER : PICK_FILE);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        picking = false;
        if ((request != PICK_FILE && request != PICK_FOLDER) || result != RESULT_OK || data == null || data.getData() == null) {
            check();
            return;
        }
        String path = request == PICK_FOLDER ? treePath(data.getData()) : pathOf(data.getData());
        if (path == null || !new File(path).exists()) {
            error = "not_path";
            errorDetail = null;
            check();
            return;
        }
        File file = new File(path);
        if (file.isFile() && file.getName().equalsIgnoreCase("default.xex")) file = file.getParentFile();
        error = null;
        if (isProject()) {
            prepare(file);
        } else {
            prepareGame(file);
        }
    }

    // The game app: check the copy, save it, play.
    private void prepareGame(File game) {
        working = true;
        showPreparing();
        new Thread(() -> {
            try {
                new GestaltBuilder(this, this::progress).check(game);
                progress(GestaltBuilder.STEP_CONFIG, 1f);
                if (!saveGame(game.getPath(), null)) throw new GestaltBuilder.Failure("save_failed");
                runOnUiThread(() -> {
                    working = false;
                    startGame();
                });
            } catch (GestaltBuilder.Failure f) {
                fail(f);
            }
        }).start();
    }

    // Project Gestalt: check, icon, translation, nier.toml, the app.
    private void prepare(File game) {
        working = true;
        showPreparing();
        final String textLanguage = language == 0 ? "Portugues" : "English";
        new Thread(() -> {
            try {
                GestaltBuilder b = new GestaltBuilder(this, this::progress);
                byte[] tile = b.check(game);
                b.copyTranslation(nierPort());
                progress(GestaltBuilder.STEP_CONFIG, 0.19f);
                if (!saveGame(game.getPath(), textLanguage)) throw new GestaltBuilder.Failure("save_failed");
                File apk = b.buildApk(GestaltBuilder.launcherIcon(tile));
                progress(GestaltBuilder.STEP_SIGN, 1f);
                Thread.sleep(400);
                runOnUiThread(() -> {
                    working = false;
                    builtApk = apk;
                    showReady();
                });
            } catch (GestaltBuilder.Failure f) {
                fail(f);
            } catch (InterruptedException e) {
                // ignore
            }
        }).start();
    }

    private void fail(GestaltBuilder.Failure f) {
        runOnUiThread(() -> {
            working = false;
            error = f.key;
            errorDetail = f.detail;
            check();
        });
    }

    private long lastProgress;

    private void progress(int step, float fraction) {
        long now = System.currentTimeMillis();
        if (now - lastProgress < 50 && fraction < 1f) return;
        lastProgress = now;
        runOnUiThread(() -> {
            if (bar == null) return;
            bar.value = fraction;
            stepText.setText(t("step" + Math.min(step, GestaltBuilder.STEPS - 1)));
            percentText.setText(Math.round(fraction * 100) + "%");
        });
    }

    private void install() {
        if (builtApk == null) return;
        working = true;
        newPage(t("installing"), false);
        add(label(t("installing"), 18, INK, false), 30);
        new Thread(() -> {
            try {
                PackageInstaller installer = getPackageManager().getPackageInstaller();
                PackageInstaller.SessionParams params =
                        new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
                params.setAppPackageName(GAME);
                int id = installer.createSession(params);
                try (PackageInstaller.Session session = installer.openSession(id)) {
                    try (OutputStream out = session.openWrite("base.apk", 0, builtApk.length());
                         InputStream in = new FileInputStream(builtApk)) {
                        byte[] buf = new byte[1 << 16];
                        for (int n; (n = in.read(buf)) > 0; ) out.write(buf, 0, n);
                        session.fsync(out);
                    }
                    Intent result = new Intent(this, SetupActivity.class).setAction(INSTALLED);
                    int flags = PendingIntent.FLAG_UPDATE_CURRENT | (Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0);
                    session.commit(PendingIntent.getActivity(this, 0, result, flags).getIntentSender());
                }
            } catch (Exception e) {
                fail(new GestaltBuilder.Failure("install_failed", e.getMessage() == null ? "" : e.getMessage()));
            }
        }).start();
    }

    private void handleInstallResult(Intent intent) {
        if (intent == null || !INSTALLED.equals(intent.getAction())) return;
        int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);
            if (confirm != null) startActivity(confirm);
            return;
        }
        working = false;
        if (status == PackageInstaller.STATUS_SUCCESS) {
            if (builtApk != null) builtApk.delete();
            builtApk = null;
            installed = true;
            showDone();
            return;
        }
        error = "install_failed";
        String message = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
        errorDetail = message == null ? "" : message;
        if (builtApk == null) {
            check();
            return;
        }
        showReady();
        if (status == PackageInstaller.STATUS_FAILURE_CONFLICT || status == PackageInstaller.STATUS_FAILURE_INCOMPATIBLE) {
            add(button(t("uninstall_old"), 15, v -> startActivity(new Intent(Intent.ACTION_DELETE, Uri.parse("package:" + GAME)))), 12);
        }
    }

    // ---- paths and files -------------------------------------------------------------------------------------

    // A picked document's path in shared storage: "primary:Dir/Nier.iso" (internal storage), "XXXX-XXXX:Dir/..."
    // (an SD card), "raw:/storage/..." (some Downloads providers), "home:" (Documents).
    private String pathOf(Uri uri) {
        try {
            return idToPath(DocumentsContract.getDocumentId(uri));
        } catch (Exception e) {
            return null;
        }
    }

    private String treePath(Uri uri) {
        try {
            return idToPath(DocumentsContract.getTreeDocumentId(uri));
        } catch (Exception e) {
            return null;
        }
    }

    private static String idToPath(String id) {
        if (id.startsWith("raw:")) return id.substring(4);
        int colon = id.indexOf(':');
        if (colon < 0) return null;
        String volume = id.substring(0, colon), rest = id.substring(colon + 1);
        String root = Environment.getExternalStorageDirectory().getPath();
        if (volume.equals("primary")) return rest.isEmpty() ? root : root + "/" + rest;
        if (volume.equals("home")) return root + "/Documents/" + rest;
        return "/storage/" + volume + (rest.isEmpty() ? "" : "/" + rest);
    }

    private String savedGame() {
        try {
            File toml = new File(nierPort(), "nier.toml");
            if (!toml.exists()) return null;
            for (String line : Files.readAllLines(toml.toPath(), StandardCharsets.UTF_8)) {
                String s = line.trim();
                if (!s.startsWith("game_data_root")) continue;
                int eq = s.indexOf('=');
                if (eq < 0) continue;
                String v = s.substring(eq + 1).trim();
                if (v.length() >= 2 && (v.charAt(0) == '\'' || v.charAt(0) == '"')) v = v.substring(1, v.length() - 1);
                return v.isEmpty() ? null : v;
            }
        } catch (Exception e) {
            // Unreadable: ask again.
        }
        return null;
    }

    // game_data_root (and nier_language, when given) first in NierPort/nier.toml, keeping the other lines.
    private static boolean saveGame(String path, String textLanguage) {
        try {
            nierPort().mkdirs();
            File toml = new File(nierPort(), "nier.toml");
            List<String> lines = new ArrayList<>();
            lines.add("game_data_root = '" + path.replace('\\', '/') + "'");
            if (textLanguage != null) lines.add("nier_language = \"" + textLanguage + "\"");
            if (toml.exists()) {
                for (String line : Files.readAllLines(toml.toPath(), StandardCharsets.UTF_8)) {
                    String s = line.trim();
                    if (s.startsWith("﻿")) s = s.substring(1);
                    if (s.startsWith("game_data_root") || (textLanguage != null && s.startsWith("nier_language"))) continue;
                    lines.add(line);
                }
            }
            try (FileOutputStream out = new FileOutputStream(toml)) {
                out.write((String.join("\n", lines) + "\n").getBytes(StandardCharsets.UTF_8));
            }
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    // The setup's language, shared by both apps (NierPort/idioma.txt); the system's at first.
    private int savedLanguage() {
        try {
            File f = new File(nierPort(), "idioma.txt");
            if (f.exists()) {
                String s = new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8).trim();
                if (s.equals("en")) return 1;
                if (s.equals("es")) return 2;
                if (s.equals("pt")) return 0;
            }
        } catch (Exception e) {
            // the system's
        }
        String l = Locale.getDefault().getLanguage();
        return l.equals("pt") ? 0 : l.equals("es") ? 2 : 1;
    }

    private void saveLanguage() {
        try {
            nierPort().mkdirs();
            try (FileOutputStream out = new FileOutputStream(new File(nierPort(), "idioma.txt"))) {
                out.write(new String[] {"pt", "en", "es"}[language].getBytes(StandardCharsets.UTF_8));
            }
        } catch (Exception e) {
            // only a preference (needs the permission)
        }
    }

    // 2026-11-14: when the game could not use NierPort it writes its log to the app's own folder (Android/data/<pkg>/
    // files), which recent Android hides from file managers. The launcher can read it: a copy goes to NierPort, with
    // the phone's model, Android version and memory, so the player can send it.
    private void copyHiddenLog() {
        try {
            File dir = getExternalFilesDir(null);
            StringBuilder info = new StringBuilder();
            info.append("Aparelho: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
                    .append(" (").append(Build.HARDWARE).append(", ")
                    .append(Build.VERSION.SDK_INT >= 31 ? Build.SOC_MODEL : "?")
                    .append(")\nAndroid ").append(Build.VERSION.RELEASE).append(" (SDK ")
                    .append(Build.VERSION.SDK_INT).append(")\n");
            android.app.ActivityManager am = (android.app.ActivityManager) getSystemService(ACTIVITY_SERVICE);
            android.app.ActivityManager.MemoryInfo mem = new android.app.ActivityManager.MemoryInfo();
            am.getMemoryInfo(mem);
            info.append("RAM total ").append(mem.totalMem >> 20).append(" MB, livre ").append(mem.availMem >> 20)
                    .append(" MB\nPasta oculta: ").append(dir).append('\n');
            File hidden = dir == null ? null : new File(dir, "log.txt");
            info.append("Log oculto: ").append(hidden != null && hidden.exists() ? hidden.length() + " bytes" : "nenhum")
                    .append('\n');
            try (FileOutputStream out = new FileOutputStream(new File(nierPort(), "aparelho.txt"))) {
                out.write(info.toString().getBytes(StandardCharsets.UTF_8));
            }
            if (hidden != null && hidden.exists())
                Files.copy(hidden.toPath(), new File(nierPort(), "log_oculto.txt").toPath(),
                        java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        } catch (Exception e) {
            // Only a help for reports.
        }
    }

    private void startGame() {
        startActivity(new Intent(this, NierActivity.class));
        finish();
    }
}
