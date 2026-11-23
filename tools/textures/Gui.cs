// NierTextureTool - the window: browse the game's 2D packs, replace a texture with
// a PNG, build a mod (MODS\<name>) or a language's art (TEXT\<language>).
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Windows.Forms;

static partial class Gui
{
    static readonly Dictionary<string, Dictionary<string, string>> L = new Dictionary<string, Dictionary<string, string>>
    {
        { "pt", new Dictionary<string, string> {
            {"title","NieR — Editor de Texturas e Modelos"}, {"media","Jogo (ISO ou pasta media):"}, {"browse","Procurar…"},
            {"search","Buscar pacote…"}, {"textonly","só imagens com texto"}, {"packs","{0} pacotes"},
            {"replace","Substituir…"}, {"revert","Restaurar"}, {"export","Salvar PNG original"}, {"edited","● editada"},
            {"build","Gerar"}, {"dest","Destino:"}, {"asmod","Mod (pasta MODS):"}, {"aslang","Idioma (pasta TEXT):"},
            {"openproj","Abrir pasta das edições"}, {"nomedia","Escolha a pasta da ISO do jogo (ou a pasta media, com layer1.cpk e layer2.cpk)."},
            {"loading","Lendo o jogo…"}, {"sizewarn","A imagem tem {0}x{1}, a original {2}x{3}.\nTamanho diferente (HD) ainda não é aplicado no jogo. Continuar mesmo assim?"},
            {"built","Pronto: {0}\nSalvo em {1}"}, {"nothing","Nenhuma imagem editada para gerar."}, {"err","Erro: {0}"},
            {"png","Imagens PNG|*.png"}, {"noname","Digite um nome para o mod."}, {"uilang","Idioma:"}, {"tex","{0}x{1}  {2}"}, {"project","Edições ficam em: {0}"} } },
        { "en", new Dictionary<string, string> {
            {"title","NieR — Texture and Model Editor"}, {"media","Game (ISO or media folder):"}, {"browse","Browse…"},
            {"search","Search pack…"}, {"textonly","images with text only"}, {"packs","{0} packs"},
            {"replace","Replace…"}, {"revert","Restore"}, {"export","Save original PNG"}, {"edited","● edited"},
            {"build","Build"}, {"dest","Destination:"}, {"asmod","Mod (MODS folder):"}, {"aslang","Language (TEXT folder):"},
            {"openproj","Open edits folder"}, {"nomedia","Choose the folder of the game's ISO (or the media folder, with layer1.cpk and layer2.cpk)."},
            {"loading","Reading the game…"}, {"sizewarn","The image is {0}x{1}, the original {2}x{3}.\nA different size (HD) is not applied in the game yet. Continue anyway?"},
            {"built","Done: {0}\nSaved to {1}"}, {"nothing","No edited image to build."}, {"err","Error: {0}"},
            {"png","PNG images|*.png"}, {"noname","Type a name for the mod."}, {"uilang","Language:"}, {"tex","{0}x{1}  {2}"}, {"project","Edits are kept in: {0}"} } },
        { "es", new Dictionary<string, string> {
            {"title","NieR — Editor de Texturas y Modelos"}, {"media","Juego (ISO o carpeta media):"}, {"browse","Buscar…"},
            {"search","Buscar paquete…"}, {"textonly","solo imágenes con texto"}, {"packs","{0} paquetes"},
            {"replace","Reemplazar…"}, {"revert","Restaurar"}, {"export","Guardar PNG original"}, {"edited","● editada"},
            {"build","Generar"}, {"dest","Destino:"}, {"asmod","Mod (carpeta MODS):"}, {"aslang","Idioma (carpeta TEXT):"},
            {"openproj","Abrir carpeta de ediciones"}, {"nomedia","Elige la carpeta de la ISO del juego (o la carpeta media, con layer1.cpk y layer2.cpk)."},
            {"loading","Leyendo el juego…"}, {"sizewarn","La imagen es de {0}x{1}, la original {2}x{3}.\nUn tamaño distinto (HD) aún no se aplica en el juego. ¿Continuar de todos modos?"},
            {"built","Listo: {0}\nGuardado en {1}"}, {"nothing","Ninguna imagen editada para generar."}, {"err","Error: {0}"},
            {"png","Imágenes PNG|*.png"}, {"noname","Escribe un nombre para el mod."}, {"uilang","Idioma:"}, {"tex","{0}x{1}  {2}"}, {"project","Las ediciones se guardan en: {0}"} } },
    };

    static string ui = "pt";
    static string T(string k, params object[] a)
    {
        string s;
        if (!L[ui].TryGetValue(k, out s)) s = L["pt"][k];
        return a.Length == 0 ? s : string.Format(s, a);
    }

    // Packs whose images carry text (title, game over, loading, windows...).
    static readonly string[] TextPacks = { "TITLE", "GAME_EVENT", "COMMON", "SYSTEM", "FLASH01", "LOAD", "LOGO", "WINDOW", "MENU", "SHOP", "KEY", "BATTLE" };

    class PackInfo { public Cpk Cpk; public CpkEntry V, P; public string Dir, Name; public string Key { get { return Dir + "/" + Name; } } }

    static Form form;
    static TabControl tabs;
    static void AddMore()
    {
        L["pt"]["tab2d"] = "Imagens 2D"; L["pt"]["tabmdl"] = "Modelos 3D"; L["pt"]["charaonly"] = "só personagens"; L["pt"]["mdlhint"] = "Escolha um modelo. Arraste para girar, botão direito para mover, roda do mouse para zoom."; L["pt"]["noshared"] = "textura que não está no jogo: {0}"; L["en"]["noshared"] = "texture not in the game data: {0}"; L["es"]["noshared"] = "textura que no está en el juego: {0}";
        L["en"]["tab2d"] = "2D images"; L["en"]["tabmdl"] = "3D models"; L["en"]["charaonly"] = "characters only"; L["en"]["mdlhint"] = "Pick a model. Drag to turn, right button to move, mouse wheel to zoom.";
        L["es"]["tab2d"] = "Imágenes 2D"; L["es"]["tabmdl"] = "Modelos 3D"; L["es"]["charaonly"] = "solo personajes"; L["es"]["mdlhint"] = "Elige un modelo. Arrastra sobre la imagen para girarlo, rueda del ratón para el zoom.";
        string[] keys = { "kchara", "kmap", "kobj", "kweapon", "kevent", "kall", "exportobj", "resetview", "mdlstats", "orbithint", "flyhint", "exported" };
        string[] pt = { "Personagens", "Mapas (cenários e céus)", "Objetos", "Armas", "Cenas (eventos)", "Tudo", "Exportar OBJ...", "Câmera inicial",
            "{0} partes, {1} texturas, {2} triângulos", "arraste: girar | botão direito: mover | roda: zoom | R: câmera inicial",
            "arraste: olhar | W/A/S/D: andar | Q/E: descer/subir | Shift: rápido | roda: avançar | R: câmera inicial | C: colisões (paredes invisíveis)", "Exportado para {0}" };
        string[] en = { "Characters", "Maps (stages and skies)", "Objects", "Weapons", "Scenes (events)", "Everything", "Export OBJ...", "Reset camera",
            "{0} parts, {1} textures, {2} triangles", "drag: turn | right button: move | wheel: zoom | R: reset camera",
            "drag: look | W/A/S/D: move | Q/E: down/up | Shift: fast | wheel: forward | R: reset camera | C: collisions (invisible walls)", "Exported to {0}" };
        string[] es = { "Personajes", "Mapas (escenarios y cielos)", "Objetos", "Armas", "Escenas (eventos)", "Todo", "Exportar OBJ...", "Cámara inicial",
            "{0} partes, {1} texturas, {2} triángulos", "arrastra: girar | botón derecho: mover | rueda: zoom | R: cámara inicial",
            "arrastra: mirar | W/A/S/D: moverse | Q/E: bajar/subir | Shift: rápido | rueda: avanzar | R: cámara inicial | C: colisiones (paredes invisibles)", "Exportado a {0}" };
        for (int i = 0; i < keys.Length; i++) { L["pt"][keys[i]] = pt[i]; L["en"][keys[i]] = en[i]; L["es"][keys[i]] = es[i]; }
        L["pt"]["mdlhint"] = "Escolha um modelo ou mapa na lista."; L["en"]["mdlhint"] = "Pick a model or a map from the list."; L["es"]["mdlhint"] = "Elige un modelo o un mapa de la lista.";
        // A model edited in Blender comes back (GuiModels.ImportObj).
        L["pt"]["importobj"] = "Importar OBJ..."; L["en"]["importobj"] = "Import OBJ..."; L["es"]["importobj"] = "Importar OBJ...";
        L["pt"]["importask"] = "Criar o mod \"{0}\" na pasta MODS com {1} de {2} partes editadas?\n\nO jogo usa as novas posições dos vértices deste modelo. Feche o jogo antes.";
        L["en"]["importask"] = "Create the mod \"{0}\" in the MODS folder with {1} of {2} parts edited?\n\nThe game uses this model's new vertex positions. Close the game first.";
        L["es"]["importask"] = "¿Crear el mod \"{0}\" en la carpeta MODS con {1} de {2} partes editadas?\n\nEl juego usa las nuevas posiciones de los vértices de este modelo. Cierra el juego antes.";
        L["pt"]["importbad"] = "Ignoradas (vértices a mais ou a menos; não adicione nem apague vértices): {0}"; L["en"]["importbad"] = "Skipped (vertices added or removed; keep the count): {0}"; L["es"]["importbad"] = "Ignoradas (vértices de más o de menos; no añadas ni borres vértices): {0}";
        L["pt"]["importnone"] = "Nenhuma parte do OBJ bate com o modelo (exporte com \"Exportar OBJ...\", edite sem adicionar ou apagar vértices e mantenha os nomes partN). {0}";
        L["en"]["importnone"] = "No part of the OBJ matches the model (export with \"Export OBJ...\", edit without adding or removing vertices and keep the partN names). {0}";
        L["es"]["importnone"] = "Ninguna parte del OBJ coincide con el modelo (exporta con \"Exportar OBJ...\", edita sin añadir ni borrar vértices y mantén los nombres partN). {0}";
        L["pt"]["importdone"] = "Mod criado em {0}.\n\nAbra o jogo para ver. Para voltar ao normal, apague essa pasta.";
        L["en"]["importdone"] = "Mod created in {0}.\n\nStart the game to see it. To undo, delete that folder.";
        L["es"]["importdone"] = "Mod creado en {0}.\n\nAbre el juego para verlo. Para deshacerlo, borra esa carpeta.";
        // Animations in the model view (GuiModels.BuildAnimBar).
        L["pt"]["animrest"] = "(pose de repouso)"; L["en"]["animrest"] = "(rest pose)"; L["es"]["animrest"] = "(pose de reposo)";
        L["pt"]["animplay"] = "▶ Tocar"; L["en"]["animplay"] = "▶ Play"; L["es"]["animplay"] = "▶ Reproducir";
        L["pt"]["animpause"] = "❚❚ Pausar"; L["en"]["animpause"] = "❚❚ Pause"; L["es"]["animpause"] = "❚❚ Pausa";
        L["pt"]["animmove"] = "Andar pelo mundo"; L["en"]["animmove"] = "Move in the world"; L["es"]["animmove"] = "Moverse por el mundo";
        L["pt"]["animcount"] = "{0} animações"; L["en"]["animcount"] = "{0} animations"; L["es"]["animcount"] = "{0} animaciones";
        // Body swap: the player (the father, every outfit) plays with the chosen character's body.
        L["pt"]["playas"] = "Jogar com este corpo..."; L["en"]["playas"] = "Play with this body..."; L["es"]["playas"] = "Jugar con este cuerpo...";
        L["pt"]["playasno"] = "Escolha um personagem (lista \"Personagens\")."; L["en"]["playasno"] = "Pick a character (\"Characters\" list)."; L["es"]["playasno"] = "Elige un personaje (lista \"Personajes\").";
        L["pt"]["playasask"] = "Criar o mod \"{0}\" na pasta MODS?\n\nO protagonista (todas as roupas) passa a usar o corpo de {1}, com o esqueleto e as animações dele.\nFeche o jogo antes.";
        L["en"]["playasask"] = "Create the mod \"{0}\" in the MODS folder?\n\nThe hero (every outfit) will use the body of {1}, with the hero's skeleton and animations.\nClose the game first.";
        L["es"]["playasask"] = "¿Crear el mod \"{0}\" en la carpeta MODS?\n\nEl protagonista (todos los trajes) usará el cuerpo de {1}, con su esqueleto y sus animaciones.\nCierra el juego antes.";
        L["pt"]["playasdone"] = "Mod criado em {0}.\n\nAbra o jogo para ver. Para voltar ao normal, apague essa pasta.\nDeixe só um mod de corpo na pasta MODS.";
        L["en"]["playasdone"] = "Mod created in {0}.\n\nStart the game to see it. To go back to normal, delete that folder.\nKeep only one body mod in the MODS folder.";
        L["es"]["playasdone"] = "Mod creado en {0}.\n\nAbre el juego para verlo. Para volver a la normalidad, borra esa carpeta.\nDeja solo un mod de cuerpo en la carpeta MODS.";
    }
    static TextBox mediaBox, searchBox, modName;
    static CheckBox textOnly;
    static ListBox packList;
    static FlowLayoutPanel cards;
    static Label status, mediaLabel, destLabel, uiLabel, projLabel;
    static Button browse, build, openProj;
    static RadioButton asMod, asLang;
    static ComboBox langBox, uiBox;
    static List<PackInfo> allPacks = new List<PackInfo>();
    static string exeDir, projectDir, iniPath;
    static PackInfo current;
    static Pack2D currentPack;

    public static string TestModel, TestShot;  // --gui-test <model> <png>: open the window, select it, save a screenshot, exit

    [STAThread]
    public static int Run()
    {
        Application.EnableVisualStyles();
        AddMore();
        exeDir = AppDomain.CurrentDomain.BaseDirectory;
        projectDir = Path.Combine(exeDir, "TEXTURAS");
        iniPath = Path.Combine(exeDir, "NierTextureTool.ini");
        string culture = CultureInfo.CurrentUICulture.TwoLetterISOLanguageName;
        ui = L.ContainsKey(culture) ? culture : "en";

        form = new Form { Width = 1280, Height = 820, StartPosition = FormStartPosition.CenterScreen, MinimumSize = new Size(900, 560) };

        var top = new Panel { Dock = DockStyle.Top, Height = 36, Padding = new Padding(8, 6, 8, 0) };
        mediaLabel = new Label { AutoSize = true, Left = 8, Top = 10 };
        mediaBox = new TextBox { Left = 230, Top = 6, Width = 520, ReadOnly = true };
        browse = new Button { Left = 758, Top = 5, Width = 90 };
        uiLabel = new Label { AutoSize = true, Left = 900, Top = 10 };
        uiBox = new ComboBox { Left = 960, Top = 6, Width = 110, DropDownStyle = ComboBoxStyle.DropDownList };
        uiBox.Items.AddRange(new object[] { "Português", "English", "Español" });
        uiBox.SelectedIndex = ui == "pt" ? 0 : ui == "en" ? 1 : 2;
        top.Controls.AddRange(new Control[] { mediaLabel, mediaBox, browse, uiLabel, uiBox });

        var left = new Panel { Dock = DockStyle.Left, Width = 290 };
        searchBox = new TextBox { Dock = DockStyle.Top };
        textOnly = new CheckBox { Dock = DockStyle.Top, Checked = true, Height = 24 };
        packList = new ListBox { Dock = DockStyle.Fill, IntegralHeight = false };
        left.Controls.Add(packList); left.Controls.Add(textOnly); left.Controls.Add(searchBox);

        cards = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoScroll = true, Padding = new Padding(8), BackColor = Color.FromArgb(34, 34, 38) };

        var bottom = new Panel { Dock = DockStyle.Bottom, Height = 78 };
        destLabel = new Label { AutoSize = true, Left = 8, Top = 10 };
        asMod = new RadioButton { Left = 80, Top = 6, Width = 150, Checked = true };
        modName = new TextBox { Left = 232, Top = 6, Width = 180, Text = "MeuMod" };
        asLang = new RadioButton { Left = 430, Top = 6, Width = 170 };
        langBox = new ComboBox { Left = 602, Top = 6, Width = 170, DropDownStyle = ComboBoxStyle.DropDownList };
        build = new Button { Left = 790, Top = 4, Width = 110, Height = 26, Font = new Font(SystemFonts.DefaultFont, FontStyle.Bold) };
        openProj = new Button { Left = 910, Top = 4, Width = 190, Height = 26 };
        status = new Label { Left = 8, Top = 36, Width = 1200, Height = 36, AutoSize = false };
        projLabel = new Label { Visible = false };
        bottom.Controls.AddRange(new Control[] { destLabel, asMod, modName, asLang, langBox, build, openProj, status });

        tabs = new TabControl { Dock = DockStyle.Fill };
        var page2D = new TabPage(); var pageMdl = new TabPage(); var pageEd = new TabPage(); var pageImp = new TabPage();
        page2D.Controls.Add(cards); page2D.Controls.Add(left);
        tabs.TabPages.Add(page2D); tabs.TabPages.Add(pageMdl); tabs.TabPages.Add(pageEd); tabs.TabPages.Add(pageImp);
        BuildModelTab(pageMdl);
        BuildEditorTab(pageEd);
        BuildImportTab(pageImp);
        form.Controls.Add(tabs); form.Controls.Add(top); form.Controls.Add(bottom);

        browse.Click += (s, e) => PickMedia();
        uiBox.SelectedIndexChanged += (s, e) => { ui = uiBox.SelectedIndex == 0 ? "pt" : uiBox.SelectedIndex == 1 ? "en" : "es"; ApplyText(); };
        searchBox.TextChanged += (s, e) => FillPacks();
        textOnly.CheckedChanged += (s, e) => FillPacks();
        packList.SelectedIndexChanged += (s, e) => ShowPack();
        build.Click += (s, e) => DoBuild();
        openProj.Click += (s, e) => { Directory.CreateDirectory(projectDir); System.Diagnostics.Process.Start("explorer.exe", "\"" + projectDir + "\""); };

        ApplyText();
        RefreshLanguages();
        form.Shown += (s, e) => StartLoad();
        Application.Run(form);
        return 0;
    }

    static void ApplyText()
    {
        form.Text = T("title"); mediaLabel.Text = T("media"); browse.Text = T("browse");
        searchBox.Tag = T("search"); textOnly.Text = T("textonly"); build.Text = T("build");
        destLabel.Text = T("dest"); asMod.Text = T("asmod"); asLang.Text = T("aslang"); openProj.Text = T("openproj");
        uiLabel.Text = T("uilang");
        if (tabs != null) { tabs.TabPages[0].Text = T("tab2d"); tabs.TabPages[1].Text = T("tabmdl"); ApplyModelText(); ApplyEditorText(); ApplyImportText(); }
        if (current != null) ShowPack();
        status.Text = T("project", projectDir);
    }

    static void RefreshLanguages()
    {
        langBox.Items.Clear();
        string text = Path.Combine(exeDir, "TEXT");
        if (Directory.Exists(text))
            foreach (var d in Directory.GetDirectories(text))
                if (!Path.GetFileName(d).Equals("EXAMPLE", StringComparison.OrdinalIgnoreCase)) langBox.Items.Add(Path.GetFileName(d));
        if (langBox.Items.Count > 0) langBox.SelectedIndex = 0;
    }

    // ---- game data ----
    static string FindMedia()
    {
        try { if (File.Exists(iniPath)) { string p = File.ReadAllText(iniPath).Trim(); if (HasGame(p)) return p; } } catch { }
        string toml = Path.Combine(exeDir, "nier.toml");
        var cands = new List<string> { Path.Combine(exeDir, "media"), Path.Combine(exeDir, "..", "media") };
        try
        {
            if (File.Exists(toml))
                foreach (var line in File.ReadAllLines(toml))
                    if (line.TrimStart().StartsWith("game_data_root"))
                    {
                        string v = line.Substring(line.IndexOf('=') + 1).Trim().Trim('"').Replace('/', '\\');
                        cands.Add(Path.Combine(v, "media")); cands.Add(v);
                    }
        }
        catch { }
        foreach (var c in cands) if (HasGame(c)) return Path.GetFullPath(c);
        return null;
    }

    // The game's media folder (layer1.cpk) or its .iso (the file, or a folder holding just it).
    static bool HasGame(string p) { try { return File.Exists(Path.Combine(p, "layer1.cpk")) || XIso.ImageAt(p) != null; } catch { return false; } }

    static void PickMedia()
    {
        using (var d = new FolderBrowserDialog { Description = T("nomedia") })
            if (d.ShowDialog() == DialogResult.OK)
            {
                if (!HasGame(d.SelectedPath)) { MessageBox.Show(T("nomedia")); return; }
                File.WriteAllText(iniPath, d.SelectedPath);
                StartLoad();
            }
    }

    static void StartLoad()
    {
        string media = FindMedia();
        if (media == null) { status.Text = T("nomedia"); PickMedia(); return; }
        mediaBox.Text = media;
        status.Text = T("loading");
        form.Cursor = Cursors.WaitCursor;
        var th = new System.Threading.Thread(() =>
        {
            try
            {
                var cpks = Program.OpenCpks(media);
                var list = new List<PackInfo>();
                var mlist = new List<PackInfo>();
                foreach (var c in cpks)
                {
                    var byPath = c.Files.ToDictionary(f => f.Path, f => f, StringComparer.OrdinalIgnoreCase);
                    foreach (var f in c.Files)
                    {
                        bool is2d = f.Name.EndsWith(".2DV", StringComparison.OrdinalIgnoreCase), ismdl = f.Name.EndsWith(".MDV", StringComparison.OrdinalIgnoreCase);
                        if (!is2d && !ismdl) continue;
                        string name = f.Name.Substring(0, f.Name.Length - 4);
                        CpkEntry p;
                        if (!byPath.TryGetValue(f.Dir + "/" + name + (is2d ? ".2DP" : ".MDP"), out p)) continue;
                        (is2d ? list : mlist).Add(new PackInfo { Cpk = c, V = f, P = p, Dir = f.Dir, Name = name });
                    }
                }
                list.Sort((a, b) => string.Compare(a.Key, b.Key, StringComparison.OrdinalIgnoreCase));
                mlist.Sort((a, b) => string.Compare(a.Key, b.Key, StringComparison.OrdinalIgnoreCase));
                form.BeginInvoke((Action)(() => { allPacks = list; allModels = mlist; form.Cursor = Cursors.Default; FillPacks(); FillModels(); status.Text = T("project", projectDir); if (TestModel != null) RunTest(); }));
            }
            catch (Exception e) { form.BeginInvoke((Action)(() => { form.Cursor = Cursors.Default; status.Text = T("err", e.Message); })); }
        });
        th.IsBackground = true; th.Start();
    }

    static bool HasText(PackInfo p)
    {
        return TextPacks.Any(t => p.Name.StartsWith(t, StringComparison.OrdinalIgnoreCase));
    }

    static void FillPacks()
    {
        string q = searchBox.Text.Trim();
        var shown = allPacks.Where(p => (!textOnly.Checked || HasText(p)) && (q == "" || p.Key.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0)).ToList();
        packList.BeginUpdate();
        packList.Items.Clear();
        foreach (var p in shown) packList.Items.Add(p);
        packList.EndUpdate();
        packList.DisplayMember = "Key";
        form.Text = T("title") + "  —  " + T("packs", shown.Count);
    }

    // ---- one pack ----
    static string ProjectFile(PackInfo p, Tex2D t) { return Path.Combine(projectDir, p.Dir.Replace('/', Path.DirectorySeparatorChar), p.Name, Program.Safe(t.Name) + ".png"); }

    static Bitmap ToBitmap(byte[] rgba, int w, int h)
    {
        var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb);
        var d = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
        var row = new byte[w * 4];
        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                int s = (y * w + x) * 4;
                row[x * 4] = rgba[s + 2]; row[x * 4 + 1] = rgba[s + 1]; row[x * 4 + 2] = rgba[s]; row[x * 4 + 3] = rgba[s + 3];
            }
            System.Runtime.InteropServices.Marshal.Copy(row, 0, d.Scan0 + y * d.Stride, row.Length);
        }
        bmp.UnlockBits(d);
        return bmp;
    }

    static Bitmap Checker()
    {
        var b = new Bitmap(16, 16);
        using (var g = Graphics.FromImage(b)) { g.Clear(Color.FromArgb(70, 70, 76)); g.FillRectangle(new SolidBrush(Color.FromArgb(90, 90, 98)), 0, 0, 8, 8); g.FillRectangle(new SolidBrush(Color.FromArgb(90, 90, 98)), 8, 8, 8, 8); }
        return b;
    }
    static Bitmap checker = Checker();

    static void ShowPack()
    {
        var p = packList.SelectedItem as PackInfo;
        if (p == null) return;
        current = p;
        form.Cursor = Cursors.WaitCursor;
        try
        {
            currentPack = Pack2D.Load(p.Name, p.Cpk.Read(p.V), p.Cpk.Read(p.P));
            cards.SuspendLayout();
            foreach (Control c in cards.Controls) c.Dispose();
            cards.Controls.Clear();
            foreach (var t in currentPack.Textures) cards.Controls.Add(MakeCard(p, currentPack, t));
            cards.ResumeLayout();
        }
        catch (Exception e) { status.Text = T("err", e.Message); }
        form.Cursor = Cursors.Default;
    }

    static Control MakeCard(PackInfo p, Pack2D pack, Tex2D t)
    {
        var card = new Panel { Width = 300, Height = 250, Margin = new Padding(6), BackColor = Color.FromArgb(48, 48, 54) };
        var pic = new PictureBox { Left = 6, Top = 6, Width = 288, Height = 150, SizeMode = PictureBoxSizeMode.Zoom, BackgroundImage = checker, BackgroundImageLayout = ImageLayout.Tile };
        var name = new Label { Left = 6, Top = 160, Width = 200, Height = 18, Text = t.Name, ForeColor = Color.White };
        var mark = new Label { Left = 206, Top = 160, Width = 88, Height = 18, ForeColor = Color.Gold, TextAlign = ContentAlignment.MiddleRight };
        var info = new Label { Left = 6, Top = 180, Width = 288, Height = 18, ForeColor = Color.Silver, Text = T("tex", t.Width, t.Height, t.FormatName) };
        var rep = new Button { Left = 6, Top = 204, Width = 100, Height = 26, Text = T("replace") };
        var rev = new Button { Left = 110, Top = 204, Width = 84, Height = 26, Text = T("revert") };
        var exp = new Button { Left = 198, Top = 204, Width = 96, Height = 26, Text = "PNG ⤓" };
        foreach (var bt in new[] { rep, rev, exp }) { bt.FlatStyle = FlatStyle.Flat; bt.BackColor = Color.WhiteSmoke; bt.ForeColor = Color.Black; }
        card.Controls.AddRange(new Control[] { pic, name, mark, info, rep, rev, exp });

        Action refresh = () =>
        {
            if (pic.Image != null) { var old = pic.Image; pic.Image = null; old.Dispose(); }
            string f = ProjectFile(p, t);
            bool edited = File.Exists(f);
            try
            {
                if (edited) { using (var src = new Bitmap(f)) pic.Image = new Bitmap(src); }
                else if (t.Supported) pic.Image = ToBitmap(pack.Decode(t), t.Width, t.Height);
            }
            catch { }
            mark.Text = edited ? T("edited") : "";
            rev.Enabled = edited;
            TextureChanged();
        };
        refresh();
        rep.Enabled = exp.Enabled = t.Supported;

        Action<string> apply = file =>
        {
            try
            {
                int w, h; PngIo.Load(file, out w, out h);
                if ((w != t.Width || h != t.Height) && MessageBox.Show(T("sizewarn", w, h, t.Width, t.Height), form.Text, MessageBoxButtons.YesNo) != DialogResult.Yes) return;
                string dst = ProjectFile(p, t);
                Directory.CreateDirectory(Path.GetDirectoryName(dst));
                File.Copy(file, dst, true);
                refresh();
            }
            catch (Exception e) { MessageBox.Show(T("err", e.Message)); }
        };
        rep.Click += (s, e) => { using (var d = new OpenFileDialog { Filter = T("png") }) if (d.ShowDialog() == DialogResult.OK) apply(d.FileName); };
        rev.Click += (s, e) => { try { File.Delete(ProjectFile(p, t)); } catch { } refresh(); };
        exp.Click += (s, e) =>
        {
            using (var d = new SaveFileDialog { Filter = T("png"), FileName = Program.Safe(t.Name) + ".png" })
                if (d.ShowDialog() == DialogResult.OK) PngIo.Save(d.FileName, pack.Decode(t), t.Width, t.Height);
        };
        card.AllowDrop = pic.AllowDrop = true;
        DragEventHandler enter = (s, e) => { if (e.Data.GetDataPresent(DataFormats.FileDrop)) e.Effect = DragDropEffects.Copy; };
        DragEventHandler drop = (s, e) =>
        {
            var files = e.Data.GetData(DataFormats.FileDrop) as string[];
            if (files != null && files.Length > 0 && t.Supported) apply(files[0]);
        };
        card.DragEnter += enter; pic.DragEnter += enter; card.DragDrop += drop; pic.DragDrop += drop;
        return card;
    }

    // ---- build ----
    static void DoBuild()
    {
        try
        {
            if (!Directory.Exists(projectDir) || !Directory.EnumerateFiles(projectDir, "*.png", SearchOption.AllDirectories).Any()) { MessageBox.Show(T("nothing")); return; }
            string outDir;
            if (asMod.Checked)
            {
                string n = modName.Text.Trim();
                if (n == "" || n.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0) { MessageBox.Show(T("noname")); return; }
                outDir = Path.Combine(exeDir, "MODS", n);
            }
            else
            {
                if (langBox.SelectedItem == null) return;
                outDir = Path.Combine(exeDir, "TEXT", (string)langBox.SelectedItem);
            }
            form.Cursor = Cursors.WaitCursor;
            status.Text = T("loading");
            Application.DoEvents();
            var cpksB = Program.OpenCpks(mediaBox.Text);
            string result = Program.Build(cpksB, projectDir, outDir) + "; " + Program.BuildModels(cpksB, projectDir, outDir);
            form.Cursor = Cursors.Default;
            status.Text = T("built", result, outDir).Replace("\n", "  ");
            MessageBox.Show(T("built", result, outDir), form.Text);
        }
        catch (Exception e) { form.Cursor = Cursors.Default; status.Text = T("err", e.Message); MessageBox.Show(T("err", e.Message)); }
    }
}
