// The "Import 3D model" tab: a model from outside the game (FBX with its own skeleton and skin, or an OBJ) becomes
// the hero's body, through Outfit.Build (2026-11-16, what was learned porting 2B: the bone translator, the auto-rig
// for skeleton-less OBJs, the levels of detail, the texture atlas, the flat normal map). One page: pick the file,
// tick the meshes to keep, build a preview, install it as the one body mod in MODS.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using System.Windows.Forms;

static partial class Gui
{
    static TextBox impFile, impName, impLog;
    static ComboBox impBase;
    static CheckedListBox impMeshes;
    static ComboBox impFollowBox, impShadeBox;
    static string[][] ImpShadeOptions()
    {
        return new[] {
            new[] { "", E("shading: auto", "shading: auto", "sombreado: auto") },
            new[] { "nulo", E("nulo (fosco)", "none (matte)", "nulo (mate)") },
            new[] { "tecido", E("tecido", "cloth", "tela") },
            new[] { "pele", E("pele", "skin", "piel") },
            new[] { "couro", E("couro", "leather", "cuero") },
            new[] { "metal", E("metal", "metal", "metal") } };
    }
    static CheckBox impSplitLegs;
    static NumericUpDown impSpread, impLegs, impArms;
    static ComboBox impMoveOf;
    static float impCrotch, impCx; static bool impHaveCrotch;
    static bool impFollowSetting;
    static string[][] ImpFollowOptions()
    {
        return new[] {
            new[] { "", E("automático (osso mais perto)", "automatic (nearest bone)", "automático (hueso más cercano)") },
            new[] { "CAPE", E("capa (balança como o pano do pai)", "cape (swings like the father's cloth)", "capa (se mueve como la tela del padre)") },
            new[] { "TORSO", E("tronco (casaco)", "torso (coat)", "tronco (abrigo)") },
            new[] { "CHEST", E("peito", "chest", "pecho") },
            new[] { "HIPS", E("quadril (saia, aba)", "hips (skirt, flap)", "cadera (falda, faldón)") },
            new[] { "HEAD", E("cabeça (cabelo, chapéu)", "head (hair, hat)", "cabeza (pelo, sombrero)") },
            new[] { "LARM", E("braço esquerdo", "left arm", "brazo izquierdo") },
            new[] { "RARM", E("braço direito", "right arm", "brazo derecho") },
            new[] { "LLEG", E("perna esquerda", "left leg", "pierna izquierda") },
            new[] { "RLEG", E("perna direita", "right leg", "pierna derecha") } };
    }
    static DataGridView impTex;
    static PictureBox impPreview, impAtlas;
    static Button impPick, impBuild, impInstall, impOpen;
    static Label impInfo;
    static string impOut;     // the last build (a temporary folder)
    static GlView impView;
    static CheckBox impFatherBody, impFatherSkel;
    static NumericUpDown[] impXform;
    static bool impXformSetting;
    static Outfit.Alignment impAlign;                 // how the generator lays the model over the father
    static float[] impFatherTris, impFatherLines;     // his body (rest pose) and skeleton, in the game's frame
    static Cpk[] impCpks; static string impCpksMedia;
    static RadioButton impShowSrc, impShowTex, impShowGame;
    static ComboBox impAnims;
    static CheckBox impBones;
    static Label impViewInfo;
    static Timer impAnimTimer;
    static float impAnimAt;
    // The source model, a part per mesh (FBX) or group (OBJ), in the list's order; its joints; its pictures.
    static List<MdlFile.MeshPart> impSrcParts = new List<MdlFile.MeshPart>();
    static List<string> impSrcTex = new List<string>();
    static float[] impSrcJoints;
    // The built model (the game's view): parts, pictures, skeleton and the father's animations.
    static List<MdlFile.MeshPart> impGameParts;
    static Dictionary<string, MdlFile.Img> impGameImgs;
    static Skeleton impGameSkel;
    static List<Motion> impGameMotions = new List<Motion>();
    static bool impBusy;

    // Weapons too, by plain names (the game draws its own; Zero's AM_00 is her arm, not a weapon).
    static readonly string kDefaultSkip = "broken|damage|serious|angry|sad|surprise|lod[1-9]|sword|weapon|blade|katana";

    static void BuildImportTab(TabPage page)
    {
        page.BackColor = Color.FromArgb(34, 34, 38);
        var left = new Panel { Dock = DockStyle.Left, Width = 430, Padding = new Padding(8), BackColor = SystemColors.Control };
        int y = 8;
        Func<string, Label> label = t => { var l = new Label { Left = 8, Top = y, AutoSize = true, Text = t }; left.Controls.Add(l); y += 20; return l; };

        label(E("1. Modelo (FBX com esqueleto, ou OBJ):", "1. Model (FBX with a skeleton, or OBJ):", "1. Modelo (FBX con esqueleto, u OBJ):"));
        impFile = new TextBox { Left = 8, Top = y, Width = 310, ReadOnly = true };
        impPick = new Button { Left = 324, Top = y - 1, Width = 90, Height = 24, Text = E("Procurar…", "Browse…", "Buscar…") };
        left.Controls.Add(impFile); left.Controls.Add(impPick); y += 32;

        label(E("2. Partes do modelo (desmarque roupas rasgadas, rostos extras, LODs):",
                "2. Model parts (untick torn clothes, extra faces, LODs):",
                "2. Partes del modelo (desmarca ropa rota, caras extra, LODs):"));
        impMeshes = new CheckedListBox { Left = 8, Top = y, Width = 406, Height = 104, CheckOnClick = true, IntegralHeight = false };
        left.Controls.Add(impMeshes); y += 108;
        // What a part moves with (2026-11-16: a cape or a coat tail went with the nearest arm or leg).
        left.Controls.Add(new Label { Left = 8, Top = y + 4, AutoSize = true, Text = E("A parte selecionada segue:", "The selected part follows:", "La parte seleccionada sigue:") });
        impFollowBox = new ComboBox { Left = 170, Top = y, Width = 128, DropDownStyle = ComboBoxStyle.DropDownList, DropDownWidth = 260 };
        // Its shading (2026-11-17: imports looked metallic with the father's gloss map): none, cloth, skin, leather, metal.
        impShadeBox = new ComboBox { Left = 302, Top = y, Width = 112, DropDownStyle = ComboBoxStyle.DropDownList };
        foreach (var o in ImpShadeOptions()) impShadeBox.Items.Add(o[1]);
        impShadeBox.SelectedIndex = 0;
        impShadeBox.SelectedIndexChanged += (s, e) =>
        {
            if (impFollowSetting) return;
            string code = ImpShadeOptions()[impShadeBox.SelectedIndex][0];
            foreach (int i in impMeshes.SelectedIndices.Cast<int>().ToList())
            {
                var m = (ImpMesh)impMeshes.Items[i]; m.Shade = code == "" ? null : code;
                bool on = impMeshes.GetItemChecked(i);
                impMeshes.Items[i] = m;
                impMeshes.SetItemChecked(i, on);
            }
        };
        left.Controls.Add(impShadeBox);
        foreach (var o in ImpFollowOptions()) impFollowBox.Items.Add(o[1]);
        impFollowBox.SelectedIndex = 0;
        impFollowBox.SelectedIndexChanged += (s, e) =>
        {
            if (impFollowSetting) return;
            string code = ImpFollowOptions()[impFollowBox.SelectedIndex][0];
            foreach (int i in impMeshes.SelectedIndices.Cast<int>().ToList())
            {
                var m = (ImpMesh)impMeshes.Items[i]; m.Follow = code == "" ? null : code;
                bool on = impMeshes.GetItemChecked(i);
                impMeshes.Items[i] = m;  // the text again
                impMeshes.SetItemChecked(i, on);
            }
        };
        left.Controls.Add(impFollowBox); y += 28;
        impSplitLegs = new CheckBox { Left = 8, Top = y, Width = 250, AutoSize = false, Height = 20,
                                      Text = E("Separar as pernas (calça ou roupa que liga as duas pernas)", "Separate the legs (trousers or clothes joining both legs)", "Separar las piernas (pantalón o ropa que une las dos piernas)") };
        left.Controls.Add(impSplitLegs);
        left.Controls.Add(new Label { Left = 262, Top = y + 2, AutoSize = true, Text = E("afastar (cm):", "spread (cm):", "separar (cm):") });
        impSpread = new NumericUpDown { Left = 350, Top = y - 1, Width = 64, DecimalPlaces = 1, Increment = 0.5m, Minimum = -20, Maximum = 40, Value = 0 };
        impSpread.ValueChanged += (s, e) => { if (!impShowGame.Checked) ImpShowScene(true); };
        left.Controls.Add(impSpread); y += 24;
        // The body's movement, apart from the father's (2026-11-18): whose animations, and its own posture.
        left.Controls.Add(new Label { Left = 8, Top = y + 3, AutoSize = true, Text = E("Movimento de:", "Movement of:", "Movimiento de:") });
        impMoveOf = new ComboBox { Left = 100, Top = y, Width = 110, DropDownStyle = ComboBoxStyle.DropDown };
        impMoveOf.Items.AddRange(new object[] { E("pai", "father", "padre"), "KAINE010", "NIER010", "EMIL010" });
        impMoveOf.SelectedIndex = 0;
        left.Controls.Add(impMoveOf);
        left.Controls.Add(new Label { Left = 216, Top = y + 3, AutoSize = true, Text = E("pernas°:", "legs°:", "piernas°:") });
        impLegs = new NumericUpDown { Left = 270, Top = y, Width = 50, Minimum = -30, Maximum = 30, Value = 0 };
        left.Controls.Add(impLegs);
        left.Controls.Add(new Label { Left = 324, Top = y + 3, AutoSize = true, Text = E("braços°:", "arms°:", "brazos°:") });
        impArms = new NumericUpDown { Left = 376, Top = y, Width = 44, Minimum = -30, Maximum = 30, Value = 0 };
        left.Controls.Add(impArms); y += 26;
        impMeshes.SelectedIndexChanged += (s, e) =>
        {
            var m = impMeshes.SelectedItem as ImpMesh;
            impFollowSetting = true;
            int k = m == null ? 0 : Math.Max(0, Array.FindIndex(ImpFollowOptions(), o => o[0] == (m.Follow ?? "")));
            impFollowBox.SelectedIndex = k;
            impShadeBox.SelectedIndex = m == null ? 0 : Math.Max(0, Array.FindIndex(ImpShadeOptions(), o => o[0] == (m.Shade ?? "")));
            impFollowSetting = false;
            impXformSetting = true;
            var xv = ImpParseXform(m == null ? null : m.Xform);
            for (int i = 0; i < 7; i++) impXform[i].Value = (decimal)Math.Max((double)impXform[i].Minimum, Math.Min((double)impXform[i].Maximum, i < 3 ? xv[i] * 100 : i == 6 ? xv[i] * 100 : xv[i]));
            impXformSetting = false;
            if (!impShowGame.Checked) ImpShowScene(true);
        };
        impMeshes.ItemCheck += (s, e) => form.BeginInvoke((Action)(() => { if (!impShowGame.Checked) ImpShowScene(true); }));
        impInfo = new Label { Left = 8, Top = y, Width = 406, Height = 32, ForeColor = Color.DimGray };
        left.Controls.Add(impInfo); y += 34;

        label(E("3. Texturas (cada parte e a imagem dela; troque com o botão ou dois cliques):",
                "3. Textures (each part and its image; change with the button or a double click):",
                "3. Texturas (cada parte y su imagen; cámbiala con el botón o doble clic):"));
        impTex = new DataGridView { Left = 8, Top = y, Width = 406, Height = 130, AllowUserToAddRows = false, RowHeadersVisible = false,
                                    AutoSizeColumnsMode = DataGridViewAutoSizeColumnsMode.Fill, BackgroundColor = SystemColors.Window,
                                    SelectionMode = DataGridViewSelectionMode.FullRowSelect, ReadOnly = true };
        impTex.Columns.Add("part", E("Parte / material", "Part / material", "Parte / material"));
        impTex.Columns.Add("now", E("Textura do arquivo", "File's texture", "Textura del archivo"));
        impTex.Columns.Add("new", E("Nova imagem", "New image", "Imagen nueva"));
        impTex.CellDoubleClick += (s, e) => { if (e.RowIndex >= 0) ImpImportTextures(new[] { impTex.Rows[e.RowIndex] }); };
        left.Controls.Add(impTex); y += 134;
        var texBtn = new Button { Left = 8, Top = y, Width = 250, Height = 26, Text = E("Importar texturas…", "Import textures…", "Importar texturas…") };
        var texClear = new Button { Left = 264, Top = y, Width = 150, Height = 26, Text = E("Desfazer troca", "Undo change", "Deshacer cambio") };
        texBtn.Click += (s, e) => ImpImportTextures(impTex.SelectedRows.Cast<DataGridViewRow>().ToArray());
        texClear.Click += (s, e) => { foreach (DataGridViewRow r in impTex.SelectedRows) r.Cells[2].Value = null; };
        left.Controls.Add(texBtn); left.Controls.Add(texClear); y += 32;
        label(E("4. Personagem que recebe o modelo:", "4. Character that gets the model:", "4. Personaje que recibe el modelo:"));
        impBase = new ComboBox { Left = 8, Top = y, Width = 406, DropDownStyle = ComboBoxStyle.DropDownList };
        impBase.Items.Add(E("NIER030 — o pai (todas as roupas e idades)", "NIER030 — the father (every outfit and age)", "NIER030 — el padre (todos los trajes y edades)"));
        impBase.SelectedIndex = 0;
        left.Controls.Add(impBase); y += 32;

        label(E("5. Nome do mod:", "5. Mod name:", "5. Nombre del mod:"));
        impName = new TextBox { Left = 8, Top = y, Width = 406, Text = "corpo-novo" };
        left.Controls.Add(impName); y += 34;

        impBuild = new Button { Left = 8, Top = y, Width = 200, Height = 30, Font = new Font(SystemFonts.DefaultFont, FontStyle.Bold),
                                Text = E("Gerar prévia", "Build preview", "Generar vista previa") };
        impInstall = new Button { Left = 214, Top = y, Width = 200, Height = 30, Enabled = false, Font = new Font(SystemFonts.DefaultFont, FontStyle.Bold),
                                  Text = E("Instalar no jogo (MODS)", "Install in the game (MODS)", "Instalar en el juego (MODS)") };
        left.Controls.Add(impBuild); left.Controls.Add(impInstall); y += 36;
        impOpen = new Button { Left = 8, Top = y, Width = 406, Height = 26, Text = E("Abrir pasta MODS", "Open MODS folder", "Abrir carpeta MODS") };
        left.Controls.Add(impOpen); y += 32;

        // Right: the preview (the game's own skeleton posing it) and the texture atlas, the log below.
        var right = new Panel { Dock = DockStyle.Fill, Padding = new Padding(6) };
        impLog = new TextBox { Dock = DockStyle.Bottom, Height = 200, Multiline = true, ScrollBars = ScrollBars.Vertical, ReadOnly = true,
                               BackColor = Color.FromArgb(24, 24, 28), ForeColor = Color.Gainsboro, Font = new Font("Consolas", 9) };
        var pics = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1 };
        pics.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 68)); pics.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 32));
        impPreview = new PictureBox { Visible = false };
        impAtlas = new PictureBox { Dock = DockStyle.Fill, SizeMode = PictureBoxSizeMode.Zoom, BackColor = Color.FromArgb(48, 48, 54) };
        var viewPanel = new Panel { Dock = DockStyle.Fill };
        impView = new GlView { Dock = DockStyle.Fill, BackColor = Color.FromArgb(30, 30, 34) };
        var bar = new FlowLayoutPanel { Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, BackColor = Color.FromArgb(28, 28, 32), Padding = new Padding(2), WrapContents = true };
        impShowSrc = new RadioButton { Text = E("Original (cor por parte)", "Original (colour per part)", "Original (color por parte)"), Checked = true, AutoSize = true, ForeColor = Color.White, Margin = new Padding(4, 5, 4, 0) };
        impShowTex = new RadioButton { Text = E("Original (texturas)", "Original (textures)", "Original (texturas)"), AutoSize = true, ForeColor = Color.White, Margin = new Padding(4, 5, 4, 0) };
        impShowGame = new RadioButton { Text = E("Resultado no jogo", "Result in the game", "Resultado en el juego"), AutoSize = true, ForeColor = Color.White, Enabled = false, Margin = new Padding(4, 5, 4, 0) };
        impAnims = new ComboBox { Width = 230, DropDownStyle = ComboBoxStyle.DropDownList, MaxDropDownItems = 30, Enabled = false };
        impBones = new CheckBox { Text = E("Juntas", "Joints", "Articulaciones"), Checked = true, AutoSize = true, ForeColor = Color.White, Margin = new Padding(8, 5, 4, 0) };
        impFatherBody = new CheckBox { Text = E("Corpo do pai (rosa)", "Father's body (pink)", "Cuerpo del padre (rosa)"), Checked = true, AutoSize = true, ForeColor = Color.Pink, Margin = new Padding(8, 5, 4, 0) };
        impFatherSkel = new CheckBox { Text = E("Esqueleto do pai", "Father's skeleton", "Esqueleto del padre"), Checked = true, AutoSize = true, ForeColor = Color.HotPink, Margin = new Padding(4, 5, 4, 0) };
        bar.Controls.AddRange(new Control[] { impShowSrc, impShowTex, impShowGame, impAnims, impBones, impFatherBody, impFatherSkel });
        // The selected part moved by hand (onto the father's skeleton): centimetres, degrees, percent.
        var xbar = new FlowLayoutPanel { Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, BackColor = Color.FromArgb(36, 36, 42), Padding = new Padding(2), WrapContents = true };
        xbar.Controls.Add(new Label { Text = E("Parte selecionada:", "Selected part:", "Parte seleccionada:"), AutoSize = true, ForeColor = Color.White, Margin = new Padding(4, 7, 2, 0) });
        string[] xl = { E("mover X", "move X", "mover X"), "Y", "Z", E("girar X", "turn X", "girar X"), "Y", "Z", E("escala %", "scale %", "escala %") };
        impXform = new NumericUpDown[7];
        for (int i = 0; i < 7; i++)
        {
            xbar.Controls.Add(new Label { Text = xl[i], AutoSize = true, ForeColor = Color.Silver, Margin = new Padding(i == 3 || i == 6 ? 10 : 2, 7, 1, 0) });
            var n = new NumericUpDown { Width = 54, DecimalPlaces = i < 3 ? 1 : 0, Increment = i < 3 ? 0.5m : i < 6 ? 5 : 2,
                                        Minimum = i < 3 ? -200 : i < 6 ? -180 : 10, Maximum = i < 3 ? 200 : i < 6 ? 180 : 400, Value = i == 6 ? 100 : 0, Margin = new Padding(0, 3, 0, 0) };
            n.ValueChanged += (s, e) => ImpXformChanged();
            impXform[i] = n; xbar.Controls.Add(n);
        }
        var xreset = new Button { Text = E("Zerar", "Reset", "Restablecer"), Width = 60, Height = 24, ForeColor = Color.White, FlatStyle = FlatStyle.Flat, Margin = new Padding(8, 2, 0, 0) };
        xreset.Click += (s, e) => { impXformSetting = true; for (int i = 0; i < 7; i++) impXform[i].Value = i == 6 ? 100 : 0; impXformSetting = false; ImpXformChanged(); };
        xbar.Controls.Add(xreset);
        impViewInfo = new Label { Dock = DockStyle.Bottom, Height = 18, ForeColor = Color.Silver, BackColor = Color.FromArgb(28, 28, 32),
                                  Text = E("arraste: girar | botão direito: mover | roda: zoom | R: câmera inicial | clique numa parte da lista para destacá-la",
                                           "drag: turn | right button: move | wheel: zoom | R: reset | click a part in the list to highlight it",
                                           "arrastra: girar | botón derecho: mover | rueda: zoom | R: cámara inicial | clic en una parte de la lista para resaltarla") };
        viewPanel.Controls.Add(impView); viewPanel.Controls.Add(impViewInfo); viewPanel.Controls.Add(xbar); viewPanel.Controls.Add(bar);
        pics.Controls.Add(viewPanel, 0, 0); pics.Controls.Add(impAtlas, 1, 0);
        EventHandler redraw = (s, e) => { if (s is RadioButton && !((RadioButton)s).Checked) return; ImpShowScene(false); };
        impShowSrc.CheckedChanged += redraw; impShowTex.CheckedChanged += redraw; impShowGame.CheckedChanged += redraw; impBones.CheckedChanged += redraw;
        impFatherBody.CheckedChanged += redraw; impFatherSkel.CheckedChanged += redraw;
        impAnims.SelectedIndexChanged += (s, e) => { impAnimAt = 0; if (impShowGame.Checked) ImpShowScene(false); };
        impAnimTimer = new Timer { Interval = 33 };
        impAnimTimer.Tick += (s, e) => { if (!impShowGame.Checked || impAnims.SelectedIndex < 1 || impGameSkel == null) return; impAnimAt += 1; ImpShowScene(false); };
        impAnimTimer.Start();
        right.Controls.Add(pics); right.Controls.Add(impLog);
        page.Controls.Add(right); page.Controls.Add(left);

        impPick.Click += (s, e) => ImpPick();
        impBuild.Click += (s, e) => ImpBuild();
        impInstall.Click += (s, e) => ImpInstall();
        impOpen.Click += (s, e) => { string m = Path.Combine(exeDir, "MODS"); Directory.CreateDirectory(m); System.Diagnostics.Process.Start("explorer.exe", "\"" + m + "\""); };
        // Test (NT_IMPORT_TEST = a model file, NT_IMPORT_SHOT = a PNG): once the game is read, open this tab, load the
        // file, build, save a picture of the window and close.
        string test = Environment.GetEnvironmentVariable("NT_IMPORT_TEST");
        if (!string.IsNullOrEmpty(test))
        {
            var timer = new Timer { Interval = 500 };
            timer.Tick += (s, e) =>
            {
                if (allModels == null || allModels.Count == 0) return;
                timer.Stop();
                tabs.SelectedIndex = 3;
                ImpLoadFile(test);
                ImpBuild();
            };
            form.Shown += (s, e) => timer.Start();
        }
        ImpLogLine(E("Escolha um FBX (de preferência com esqueleto e pesos) ou um OBJ. Feche o jogo antes de instalar.",
                     "Pick an FBX (preferably with a skeleton and weights) or an OBJ. Close the game before installing.",
                     "Elige un FBX (mejor con esqueleto y pesos) o un OBJ. Cierra el juego antes de instalar."));
    }

    static void ImpLogLine(string s)
    {
        if (impLog.InvokeRequired) { impLog.BeginInvoke((Action)(() => ImpLogLine(s))); return; }
        impLog.AppendText(s + Environment.NewLine);
    }

    static void ImpPick()
    {
        using (var d = new OpenFileDialog { Filter = E("Modelos 3D", "3D models", "Modelos 3D") + "|*.fbx;*.obj|FBX|*.fbx|OBJ|*.obj" })
        {
            if (d.ShowDialog() != DialogResult.OK) return;
            ImpLoadFile(d.FileName);
        }
    }

    static void ImpLoadFile(string fileName)
    {
        {
            var d = new { FileName = fileName };
            impFile.Text = d.FileName;
            impName.Text = "corpo-" + Regex.Replace(Path.GetFileNameWithoutExtension(d.FileName).ToLowerInvariant(), "[^a-z0-9]+", "-").Trim('-');
            impMeshes.Items.Clear(); impTex.Rows.Clear(); impInstall.Enabled = false;
            impPreview.Image = null; impAtlas.Image = null;
            impGameParts = null; impGameSkel = null; impSceneShown = false; impLastMode = -1; impShowGame.Enabled = false; impAnims.Enabled = false; impShowSrc.Checked = true;
            var skip = new Regex(kDefaultSkip, RegexOptions.IgnoreCase);
            try
            {
                if (d.FileName.EndsWith(".fbx", StringComparison.OrdinalIgnoreCase))
                {
                    form.Cursor = Cursors.WaitCursor;
                    var fbx = FbxModel.Load(d.FileName);
                    form.Cursor = Cursors.Default;
                    foreach (var me in fbx.Meshes)
                        impMeshes.Items.Add(new ImpMesh { Name = me.Name, Tris = me.Polys.Sum(p => Math.Max(0, p.Length - 2)) }, !skip.IsMatch(me.Name));
                    var auto = Outfit.AutoTextures(fbx, d.FileName);
                    foreach (var me in fbx.Meshes) ImpTexRow(me.Name, "mesh:" + me.Name, auto[me.Name]);
                    ImpSourceFromFbx(fbx, auto);
                    int weighted = fbx.Meshes.Count(m => m.Weights != null && m.Weights.Any(w => w != null && w.Count > 0));
                    impInfo.Text = string.Format(E("{0} ossos, {1} partes ({2} com pesos). {3}", "{0} bones, {1} parts ({2} weighted). {3}", "{0} huesos, {1} partes ({2} con pesos). {3}"),
                        fbx.Bones.Count, fbx.Meshes.Count, weighted,
                        fbx.Bones.Count == 0 ? E("Sem esqueleto: será montado um automaticamente.", "No skeleton: one is fitted automatically.", "Sin esqueleto: se ajusta uno automáticamente.") : "");
                }
                else
                {
                    var names = new List<string>();
                    foreach (var line in File.ReadLines(d.FileName))
                        if ((line.StartsWith("o ") || line.StartsWith("g ")) && !names.Contains(line.Substring(2).Trim())) names.Add(line.Substring(2).Trim());
                    var objMats = ImpObjMaterials(d.FileName);
                    // One group (Zero): its materials are the parts (the build leaves out a material by the same list).
                    bool byMat = names.Count <= 1 && objMats.Count > 1;
                    if (byMat) names = objMats.Keys.ToList();
                    foreach (var n in names) impMeshes.Items.Add(new ImpMesh { Name = n, Tris = -1 }, !skip.IsMatch(n));
                    foreach (var kv in objMats) ImpTexRow(kv.Key, kv.Key, kv.Value);
                    ImpSourceFromObj(d.FileName, names, objMats, byMat);
                    impInfo.Text = E("OBJ não tem esqueleto: ele é montado automaticamente (fique em pé, braços abertos ajuda).",
                                     "OBJ has no skeleton: one is fitted automatically (standing, arms out helps).",
                                     "OBJ no tiene esqueleto: se ajusta uno automáticamente (de pie, brazos abiertos ayuda).");
                }
                ImpLogLine(Path.GetFileName(d.FileName) + ": " + impMeshes.Items.Count + E(" partes", " parts", " partes"));
            }
            catch (Exception ex) { form.Cursor = Cursors.Default; ImpLogLine(T("err", ex.Message)); }
            form.Cursor = Cursors.WaitCursor; ImpAlignSource(d.FileName); form.Cursor = Cursors.Default;
            ImpShowScene(false);
        }
    }

    // A row of the texture list: what it is called, the key Outfit.Build reads ("mesh:<name>" for an FBX mesh, the
    // material's name for an OBJ), the picture the file links (shown by name; "flat" or none: an automatic colour).
    static void ImpTexRow(string label, string key, string now)
    {
        string shown = now == null ? E("(cor automática)", "(automatic colour)", "(color automático)") : now.StartsWith("flat:") ? E("(cor lisa)", "(flat colour)", "(color liso)") : Path.GetFileName(now);
        int i = impTex.Rows.Add(label, shown, null);
        impTex.Rows[i].Tag = key;
        if (now != null && !now.StartsWith("flat:")) impTex.Rows[i].Cells[1].ToolTipText = now;
    }

    // An OBJ's materials (its .mtl files) and their colour maps.
    static Dictionary<string, string> ImpObjMaterials(string obj)
    {
        var r = new Dictionary<string, string>(); string dir = Path.GetDirectoryName(Path.GetFullPath(obj));
        foreach (var line in File.ReadLines(obj))
        {
            var t = line.Trim();
            if (t.StartsWith("usemtl ")) { string n = t.Substring(7).Trim(); if (!r.ContainsKey(n)) r[n] = null; }
            else if (t.StartsWith("mtllib "))
            {
                string mf = Path.Combine(dir, t.Substring(7).Trim()), name = null;
                if (File.Exists(mf))
                    foreach (var ml in File.ReadLines(mf))
                    {
                        var u = ml.Trim().Split(new[] { ' ', '\t' }, 2, StringSplitOptions.RemoveEmptyEntries);
                        if (u.Length < 2) continue;
                        if (u[0] == "newmtl") { name = u[1]; if (!r.ContainsKey(name)) r[name] = null; }
                        else if (u[0] == "map_Kd" && name != null) r[name] = Path.Combine(dir, u[1]);
                    }
            }
        }
        return r;
    }

    // Images for the list: with rows selected, the first image goes to all of them; with none (or several images),
    // each image goes to the part whose name is most like the file's (2B_hair.png -> 21-Hair-2).
    static void ImpImportTextures(DataGridViewRow[] rows)
    {
        if (impTex.Rows.Count == 0) { MessageBox.Show(E("Escolha o modelo primeiro.", "Pick the model first.", "Elige el modelo primero."), form.Text); return; }
        string[] files;
        using (var d = new OpenFileDialog { Multiselect = true, Filter = E("Imagens", "Images", "Imágenes") + "|*.png;*.dds;*.tga;*.jpg;*.jpeg;*.bmp|*.*|*.*" })
        {
            if (d.ShowDialog() != DialogResult.OK) return;
            files = d.FileNames;
        }
        if (files.Length == 1 && rows.Length > 0)
        {
            foreach (var r in rows) r.Cells[2].Value = files[0];
            ImpLogLine(Path.GetFileName(files[0]) + " -> " + string.Join(", ", rows.Select(r => Convert.ToString(r.Cells[0].Value))));
            return;
        }
        Func<string, string[]> words = s => Regex.Split(s.ToLowerInvariant(), "[^a-z]+").Where(w => w.Length >= 3 && w != "png" && w != "dds" && w != "tga").ToArray();
        var unmatched = new List<string>();
        foreach (var f in files)
        {
            var fw = words(Path.GetFileNameWithoutExtension(f));
            DataGridViewRow best = null; int bestScore = 0;
            foreach (DataGridViewRow r in impTex.Rows)
            {
                var rw = words(Convert.ToString(r.Cells[0].Value) + " " + Convert.ToString(r.Cells[1].Value));
                int score = fw.Count(w => rw.Any(x => x.Contains(w) || w.Contains(x)));
                if (score > bestScore) { bestScore = score; best = r; }
            }
            if (best == null) { unmatched.Add(Path.GetFileName(f)); continue; }
            best.Cells[2].Value = f;
            ImpLogLine(Path.GetFileName(f) + " -> " + best.Cells[0].Value);
        }
        if (unmatched.Count > 0)
            MessageBox.Show(E("Sem parte com nome parecido (selecione a parte na lista e importe de novo):", "No part with a similar name (select the part in the list and import again):", "Ninguna parte con nombre parecido (selecciona la parte y vuelve a importar):")
                            + "\n  " + string.Join("\n  ", unmatched), form.Text);
    }

    // ---- the father, and the model laid over him ----
    static Cpk[] ImpGetCpks()
    {
        if (impCpks == null || impCpksMedia != mediaBox.Text) { impCpks = Program.OpenCpks(mediaBox.Text); impCpksMedia = mediaBox.Text; }
        return impCpks;
    }

    static void ImpLoadFather()
    {
        if (impFatherTris != null || string.IsNullOrEmpty(mediaBox.Text)) return;
        CpkEntry ve, pe; var c = Program.FindModel(ImpGetCpks(), "CHARA/NIER030", out ve, out pe);
        if (c == null) return;
        var vb = c.Read(ve);
        var m = MdlFile.Load("NIER030", vb, c.Read(pe));
        // the near body only: the biggest part of each texture (the far and middle copies left out), no sword
        var parts = m.Parts().Where(p => p.Tex == null || p.Tex.IndexOf("SWORD", StringComparison.OrdinalIgnoreCase) < 0)
                             .GroupBy(p => p.Tex ?? "").Select(gq => gq.OrderByDescending(p => p.F.Length).First()).ToList();
        var tris = new List<float>();
        foreach (var p in parts)
            for (int i = 0; i + 2 < p.F.Length; i += 3)
                for (int k = 0; k < 3; k++) { int v = p.F[i + k]; tris.Add(p.V[v * 3]); tris.Add(p.V[v * 3 + 1]); tris.Add(p.V[v * 3 + 2]); }
        impFatherTris = tris.ToArray();
        var sk = Anim.Load(vb, new List<Motion>());
        var body = new Regex("^(HIPSBONE|SPINE\\d*|NECK|HEAD|(LEFT|RIGHT)(SHOULDER|ARM|FOREARM|HAND|UPLEG|LEG|FOOT|TOEBASE))$");
        var lines = new List<float>();
        for (int j = 0; j < sk.Names.Length; j++)
        {
            if (!body.IsMatch(sk.Names[j])) continue;
            int p = sk.Parent[j]; while (p >= 0 && !body.IsMatch(sk.Names[p])) p = sk.Parent[p];
            if (p < 0) continue;
            lines.AddRange(new[] { sk.RestWorld[j][12], sk.RestWorld[j][13], sk.RestWorld[j][14], sk.RestWorld[p][12], sk.RestWorld[p][13], sk.RestWorld[p][14] });
        }
        impFatherLines = lines.ToArray();
        int fh = Array.IndexOf(sk.Names, "HIPSBONE"), fl = Array.IndexOf(sk.Names, "LEFTUPLEG"), fr = Array.IndexOf(sk.Names, "RIGHTUPLEG");
        if (fh >= 0 && fl >= 0 && fr >= 0) { impCx = sk.RestWorld[fh][12]; impCrotch = (sk.RestWorld[fl][13] + sk.RestWorld[fr][13]) / 2; impHaveCrotch = true; }
    }

    // The generator's own alignment of this file (a dry run of it).
    static void ImpAlignSource(string file)
    {
        impAlign = null;
        try
        {
            ImpLoadFather();
            var map = new Dictionary<string, string> { { "alignonly", "1" } };
            Outfit.LastAlign = null;
            Outfit.Build(ImpGetCpks(), "NIER030", file, Path.Combine(exeDir, "IMPORTAR-PREVIA-alinhar"), s => { }, map);
            impAlign = Outfit.LastAlign;
        }
        catch (Exception ex) { ImpLogLine(E("alinhamento: ", "alignment: ", "alineación: ") + ex.Message); }
    }

    static float[] ImpParseXform(string spec)
    {
        var d = new float[] { 0, 0, 0, 0, 0, 0, 1 };
        if (spec == null) return d;
        var a = spec.Split(' ');
        for (int i = 0; i < 7 && i < a.Length; i++) float.TryParse(a[i], NumberStyles.Float, CultureInfo.InvariantCulture, out d[i]);
        return d;
    }

    static void ImpXformChanged()
    {
        if (impXformSetting || impMeshes.SelectedIndices.Count == 0) return;
        float[] v = new float[7];
        for (int i = 0; i < 7; i++) v[i] = (float)impXform[i].Value;
        v[0] /= 100; v[1] /= 100; v[2] /= 100; v[6] /= 100;
        bool none = v[0] == 0 && v[1] == 0 && v[2] == 0 && v[3] == 0 && v[4] == 0 && v[5] == 0 && v[6] == 1;
        string spec = none ? null : string.Join(" ", v.Select(x => x.ToString("0.####", CultureInfo.InvariantCulture)));
        foreach (int i in impMeshes.SelectedIndices.Cast<int>().ToList())
        {
            var m = (ImpMesh)impMeshes.Items[i]; m.Xform = spec;
            bool on = impMeshes.GetItemChecked(i);
            impXformSetting = true; impMeshes.Items[i] = m; impMeshes.SetItemChecked(i, on); impXformSetting = false;
        }
        if (!impShowGame.Checked) ImpShowScene(true);
    }

    // A source part as the generator will see it: laid over the father, then moved by hand.
    static float[] ImpPlaced(int i)
    {
        var p = impSrcParts[i];
        if (impAlign == null) return p.V;
        var pts = new List<float[]>(p.N);
        for (int v = 0; v < p.N; v++) pts.Add(impAlign.Apply(new[] { p.V[v * 3], p.V[v * 3 + 1], p.V[v * 3 + 2] }));
        var m = i < impMeshes.Items.Count ? impMeshes.Items[i] as ImpMesh : null;
        if (m != null && m.Xform != null) pts = Outfit.PartXform(pts, m.Xform);
        float sp = (float)impSpread.Value / 100;
        if (sp != 0 && impHaveCrotch) foreach (var q in pts) q[0] += Outfit.SpreadOffset(q[0], q[1], impCx, impCrotch, sp);
        var r = new float[p.N * 3];
        for (int v = 0; v < p.N; v++) { r[v * 3] = pts[v][0]; r[v * 3 + 1] = pts[v][1]; r[v * 3 + 2] = pts[v][2]; }
        return r;
    }

    // ---- the 3D view ----
    static MdlFile.Img ImpSolid(int r, int g, int b)
    {
        var px = new byte[16 * 4];
        for (int i = 0; i < 16; i++) { px[i * 4] = (byte)r; px[i * 4 + 1] = (byte)g; px[i * 4 + 2] = (byte)b; px[i * 4 + 3] = 255; }
        return MdlFile.Img.Make(px, 4, 4);
    }

    // A distinct colour for part i (golden-angle hues).
    static Color ImpPartColour(int i)
    {
        double h = (i * 137.508) % 360, s = 0.55, v = 0.95;
        int hi = (int)(h / 60) % 6; double f = h / 60 - Math.Floor(h / 60);
        double p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
        double r = 0, g = 0, b = 0;
        switch (hi) { case 0: r = v; g = t; b = p; break; case 1: r = q; g = v; b = p; break; case 2: r = p; g = v; b = t; break; case 3: r = p; g = q; b = v; break; case 4: r = t; g = p; b = v; break; default: r = v; g = p; b = q; break; }
        return Color.FromArgb((int)(r * 255), (int)(g * 255), (int)(b * 255));
    }

    static void ImpSourceFromFbx(FbxModel fbx, Dictionary<string, string> auto)
    {
        impSrcParts = new List<MdlFile.MeshPart>(); impSrcTex = new List<string>();
        foreach (var me in fbx.Meshes)
        {
            var v = new List<float>(); var uv = new List<float>(); var f = new List<int>();
            for (int pi = 0; pi < me.Polys.Count; pi++)
            {
                var poly = me.Polys[pi]; var puv = pi < me.PolyUv.Count ? me.PolyUv[pi] : null;
                int b = v.Count / 3;
                for (int k = 0; k < poly.Length; k++)
                {
                    int c = poly[k];
                    v.Add((float)me.Pos[c * 3]); v.Add((float)me.Pos[c * 3 + 1]); v.Add((float)me.Pos[c * 3 + 2]);
                    uv.Add(puv != null && puv.Length > k * 2 + 1 ? (float)puv[k * 2] : 0); uv.Add(puv != null && puv.Length > k * 2 + 1 ? 1 - (float)puv[k * 2 + 1] : 0);
                }
                for (int k = 1; k + 1 < poly.Length; k++) { f.Add(b); f.Add(b + k); f.Add(b + k + 1); }
            }
            impSrcParts.Add(new MdlFile.MeshPart { V = v.ToArray(), UV = uv.ToArray(), F = f.ToArray(), N = v.Count / 3 });
            string t; impSrcTex.Add(auto.TryGetValue(me.Name, out t) ? t : null);
        }
        impSrcJoints = fbx.Bones.Count > 1 ? fbx.Bones.SelectMany(b => new[] { (float)b.World[3], (float)b.World[7], (float)b.World[11] }).ToArray() : null;
    }

    static void ImpSourceFromObj(string file, List<string> groups, Dictionary<string, string> mats, bool byMat)
    {
        var ci = CultureInfo.InvariantCulture;
        var V = new List<float[]>(); var VT = new List<float[]>();
        var parts = groups.Select(g => new { V = new List<float>(), UV = new List<float>(), F = new List<int>(), Mat = new string[1] }).ToList();
        int cur = parts.Count > 0 ? 0 : -1; string mat = null;
        if (cur < 0) { parts.Add(new { V = new List<float>(), UV = new List<float>(), F = new List<int>(), Mat = new string[1] }); groups = new List<string> { "obj" }; cur = 0; }
        foreach (var raw in File.ReadLines(file))
        {
            var t = raw.Trim().Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
            if (t.Length == 0) continue;
            if (t[0] == "v" && t.Length >= 4) V.Add(new[] { float.Parse(t[1], ci), float.Parse(t[2], ci), float.Parse(t[3], ci) });
            else if (t[0] == "vt" && t.Length >= 3) VT.Add(new[] { float.Parse(t[1], ci), float.Parse(t[2], ci) });
            else if (!byMat && (t[0] == "o" || t[0] == "g") && t.Length > 1) { int i = groups.IndexOf(string.Join(" ", t.Skip(1))); if (i >= 0) cur = i; }
            else if (t[0] == "usemtl" && t.Length > 1) { mat = t[1]; if (byMat) { int i = groups.IndexOf(mat); if (i >= 0) cur = i; } }
            else if (t[0] == "f")
            {
                var p = parts[cur]; if (p.Mat[0] == null) p.Mat[0] = mat;
                var idx = new List<int>();
                for (int k = 1; k < t.Length; k++)
                {
                    var s = t[k].Split('/');
                    int vi = int.Parse(s[0]); vi = vi < 0 ? V.Count + vi : vi - 1;
                    int ti = s.Length > 1 && s[1] != "" ? int.Parse(s[1]) : 0; ti = ti < 0 ? VT.Count + ti : ti - 1;
                    if (vi < 0 || vi >= V.Count) continue;
                    idx.Add(p.V.Count / 3);
                    p.V.AddRange(V[vi]);
                    if (ti >= 0 && ti < VT.Count) { p.UV.Add(VT[ti][0]); p.UV.Add(1 - VT[ti][1]); } else { p.UV.Add(0); p.UV.Add(0); }
                }
                for (int k = 1; k + 1 < idx.Count; k++) { p.F.Add(idx[0]); p.F.Add(idx[k]); p.F.Add(idx[k + 1]); }
            }
        }
        impSrcParts = parts.Select(p => new MdlFile.MeshPart { V = p.V.ToArray(), UV = p.UV.ToArray(), F = p.F.ToArray(), N = p.V.Count / 3 }).ToList();
        impSrcTex = parts.Select(p => { string f; return p.Mat[0] != null && mats.TryGetValue(p.Mat[0], out f) ? f : null; }).ToList();
        impSrcJoints = null;
    }

    static Dictionary<string, MdlFile.Img> impTexCache = new Dictionary<string, MdlFile.Img>(StringComparer.OrdinalIgnoreCase);
    static MdlFile.Img ImpTexImage(string file)
    {
        if (file == null || file.StartsWith("flat:") || !File.Exists(file)) return null;
        MdlFile.Img img;
        if (impTexCache.TryGetValue(file, out img)) return img;
        try { int w, h; var px = PngIo.Load(file, out w, out h); img = MdlFile.Img.Make(px, w, h); } catch { img = null; }
        impTexCache[file] = img;
        return img;
    }

    static void ImpLoadGameResult()
    {
        string sw = Path.Combine(impOut, "SWAP");
        string mdv = Path.Combine(sw, "NIER030-EM-NIER030.MDV"), mdp = Path.Combine(sw, "NIER030.MDP");
        if (!File.Exists(mdv) || !File.Exists(mdp)) return;
        var vb = File.ReadAllBytes(mdv);
        var m = MdlFile.Load("NIER030", vb, File.ReadAllBytes(mdp));
        impGameParts = m.Parts();
        var tp = m.Textures();
        impGameImgs = new Dictionary<string, MdlFile.Img>(StringComparer.OrdinalIgnoreCase);
        foreach (var t in tp.Textures) if (t.Supported) try { impGameImgs[t.Name] = MdlFile.Img.Make(tp.Decode(t), t.Width, t.Height); } catch { }
        impGameMotions = new List<Motion>();
        try { impGameSkel = Anim.Load(vb, impGameMotions); } catch { impGameSkel = null; }
        impAnims.Items.Clear();
        impAnims.Items.Add(E("(pose de repouso)", "(rest pose)", "(pose de reposo)"));
        foreach (var mo in impGameMotions) impAnims.Items.Add(mo.Name.StartsWith("nier030_", StringComparison.OrdinalIgnoreCase) ? mo.Name.Substring(8) : mo.Name);
        int run = impGameMotions.FindIndex(mo => mo.Name.IndexOf("dash", StringComparison.OrdinalIgnoreCase) >= 0 || mo.Name.IndexOf("run", StringComparison.OrdinalIgnoreCase) >= 0);
        impAnims.SelectedIndex = run >= 0 ? run + 1 : 0;
        impAnims.Enabled = impGameSkel != null && impGameMotions.Count > 0;
        impShowGame.Enabled = true;
        impShowGame.Checked = true;
        ImpShowScene(false);
    }

    static bool impSceneShown;
    static int impLastMode = -1;  // 0 source, 1 game: the camera is framed again when it changes (other scales)
    static void ImpShowScene(bool keepCamera)
    {
        if (impView == null) return;
        int mode = impShowGame.Checked && impGameParts != null ? 1 : 0;
        keepCamera = (keepCamera || impSceneShown) && mode == impLastMode;
        impLastMode = mode;
        if (impShowGame.Checked && impGameParts != null)
        {
            var parts = impGameParts; float[] joints = null;
            int k = impAnims.SelectedIndex;
            if (impGameSkel != null && k >= 1 && k - 1 < impGameMotions.Count)
            {
                var mo = impGameMotions[k - 1];
                if (impAnimAt > mo.Frames - 1) impAnimAt = 0;
                var world = Anim.World(impGameSkel, mo, impAnimAt, false);
                parts = impGameParts.Select(q => Anim.Skin(q, Anim.SkinMatrices(impGameSkel, world))).ToList();
                if (impBones.Checked) joints = world.SelectMany(w => new[] { w[12], w[13], w[14] }).ToArray();
            }
            impView.Points = joints;
            impView.GhostTris = null; impView.Lines = null;
            impView.SetScene(parts, impGameImgs, keepCamera);
            impSceneShown = true;
            return;
        }
        if (impSrcParts.Count == 0) { impView.Clear(); return; }
        var shown = new List<MdlFile.MeshPart>(); var imgs = new Dictionary<string, MdlFile.Img>();
        int sel = impMeshes.SelectedIndex;
        for (int i = 0; i < impSrcParts.Count; i++)
        {
            if (i < impMeshes.Items.Count && !impMeshes.GetItemChecked(i)) continue;
            var p = impSrcParts[i]; string key;
            MdlFile.Img tex = impShowTex.Checked ? ImpTexImage(i < impSrcTex.Count ? impSrcTex[i] : null) : null;
            if (tex != null) { key = "t" + i; imgs[key] = tex; }
            else
            {
                var c = ImpPartColour(i); double dim = sel >= 0 && sel != i ? 0.35 : 1;
                key = "c" + i;
                imgs[key] = sel == i ? ImpSolid(255, 235, 60) : ImpSolid((int)(c.R * dim), (int)(c.G * dim), (int)(c.B * dim));
            }
            shown.Add(new MdlFile.MeshPart { Tex = key, V = ImpPlaced(i), UV = p.UV, F = p.F, N = p.N });
        }
        // the model's own joints only when it is not laid over the father (they are in the file's frame)
        impView.Points = impBones.Checked && impAlign == null ? impSrcJoints : null;
        impView.GhostTris = impAlign != null && impFatherBody.Checked ? impFatherTris : null;
        impView.Lines = impAlign != null && impFatherSkel.Checked ? impFatherLines : null;
        impView.SetScene(shown, imgs, keepCamera);
        impSceneShown = true;
    }

    class ImpMesh
    {
        public string Name, Follow, Xform, Shade; public int Tris;
        public override string ToString()
        {
            string s = Tris >= 0 ? Name + "   (" + Tris.ToString("N0", CultureInfo.CurrentCulture) + " tri)" : Name;
            if (Follow != null) s += "   → " + ImpFollowOptions().First(o => o[0] == Follow)[1];
            if (Shade != null) s += "   [" + ImpShadeOptions().First(o => o[0] == Shade)[1] + "]";
            if (Xform != null) s += "   ✎";
            return s;
        }
    }

    static void ImpBuild()
    {
        if (impBusy) return;
        if (string.IsNullOrEmpty(impFile.Text) || !File.Exists(impFile.Text)) { MessageBox.Show(E("Escolha o arquivo do modelo.", "Pick the model file.", "Elige el archivo del modelo."), form.Text); return; }
        if (string.IsNullOrEmpty(mediaBox.Text)) { MessageBox.Show(T("nomedia"), form.Text); return; }
        var map = new Dictionary<string, string>();
        // The meshes left out, by exact name (the default pattern when every one is ticked, so nothing changes).
        var off = impMeshes.Items.Cast<ImpMesh>().Where((m, i) => !impMeshes.GetItemChecked(i)).Select(m => Regex.Escape(m.Name)).ToList();
        if (impMeshes.Items.Count > 0 && off.Count == impMeshes.Items.Count) { MessageBox.Show(E("Marque pelo menos uma parte.", "Tick at least one part.", "Marca al menos una parte."), form.Text); return; }
        map["sem"] = off.Count > 0 ? "^(" + string.Join("|", off) + ")$" : "^$";
        foreach (ImpMesh m in impMeshes.Items) if (m.Follow != null) map["follow:" + m.Name] = m.Follow;
        foreach (ImpMesh m in impMeshes.Items) if (m.Shade != null) map["shade:" + m.Name] = m.Shade;
        if (impSplitLegs.Checked) map["splitlegs"] = "1";
        if (impSpread.Value != 0) map["spreadlegs"] = ((float)impSpread.Value / 100).ToString("0.####", CultureInfo.InvariantCulture);
        foreach (ImpMesh m in impMeshes.Items) if (m.Xform != null) map["xform:" + m.Name] = m.Xform;
        if (impMoveOf.SelectedIndex != 0 && impMoveOf.Text.Trim() != "") map["anims"] = impMoveOf.Text.Trim().ToUpperInvariant();
        if (impLegs.Value != 0) map["legs"] = ((float)impLegs.Value).ToString(CultureInfo.InvariantCulture);
        if (impArms.Value != 0) map["arms"] = ((float)impArms.Value).ToString(CultureInfo.InvariantCulture);
        foreach (DataGridViewRow r in impTex.Rows)
        {
            string key = r.Tag as string, t = Convert.ToString(r.Cells[2].Value);
            if (key != null && !string.IsNullOrWhiteSpace(t) && File.Exists(t)) map[key] = t;
        }
        string file = impFile.Text, media = mediaBox.Text, baseName = "NIER030";
        // One preview folder next to the editor (2026-11-16: a new one in %TEMP% a build, never deleted, filled the
        // user's drive C: 72 of them, 2.2 GB). Emptied before each build.
        impOut = Path.Combine(exeDir, "IMPORTAR-PREVIA");
        try { if (Directory.Exists(impOut)) Directory.Delete(impOut, true); } catch { }
        impBusy = true; impBuild.Enabled = false; impInstall.Enabled = false; form.Cursor = Cursors.WaitCursor;
        impPreview.Image = null; impAtlas.Image = null; impLog.Clear();
        ImpLogLine(E("Gerando… (um modelo grande leva de 30 s a alguns minutos)", "Building… (a big model takes 30 s to a few minutes)", "Generando… (un modelo grande tarda de 30 s a unos minutos)"));
        var started = DateTime.Now;
        var th = new System.Threading.Thread(() =>
        {
            Exception err = null;
            try { Outfit.Build(Program.OpenCpks(media), baseName, file, impOut, ImpLogLine, map); }
            catch (Exception ex) { err = ex; }
            form.BeginInvoke((Action)(() =>
            {
                impBusy = false; impBuild.Enabled = true; form.Cursor = Cursors.Default;
                if (err != null) { ImpLogLine(T("err", err.Message)); MessageBox.Show(T("err", err.Message), form.Text); return; }
                ImpLogLine(string.Format(E("Pronto em {0:0} s. Confira a prévia; se estiver boa, instale.", "Done in {0:0} s. Check the preview; if it looks right, install it.", "Listo en {0:0} s. Revisa la vista previa; si está bien, instálalo."), (DateTime.Now - started).TotalSeconds));
                impPreview.Image = ImpLoadImage(Path.Combine(impOut, "previa.png"));
                impAtlas.Image = ImpLoadImage(Path.Combine(impOut, "atlas.png"));
                impInstall.Enabled = Directory.Exists(Path.Combine(impOut, "SWAP"));
                try { ImpLoadGameResult(); } catch (Exception ex) { ImpLogLine(E("prévia 3D do resultado: ", "3D preview of the result: ", "vista 3D del resultado: ") + ex.Message); }
                string shot = Environment.GetEnvironmentVariable("NT_IMPORT_SHOT");
                if (!string.IsNullOrEmpty(shot))
                {
                    Application.DoEvents();
                    using (var bmp = new Bitmap(form.Width, form.Height)) { form.DrawToBitmap(bmp, new Rectangle(0, 0, form.Width, form.Height)); bmp.Save(shot); }
                    foreach (var mode in new[] { "jogo", "original" })
                    {
                        if (mode == "original") { impShowSrc.Checked = true; impMeshes.SelectedIndex = impMeshes.Items.Count > 1 ? 1 : -1; }
                        Application.DoEvents(); impView.Refresh(); Application.DoEvents();
                        using (var v = impView.Snapshot()) if (v != null) v.Save(Path.ChangeExtension(shot, null) + "-3d-" + mode + ".png");
                    }
                    form.Close();
                }
            }));
        });
        th.IsBackground = true; th.Start();
    }

    // Loaded into memory, so the file is not kept open (the folder is moved or deleted later).
    static Image ImpLoadImage(string path)
    {
        if (!File.Exists(path)) return null;
        using (var ms = new MemoryStream(File.ReadAllBytes(path))) return new Bitmap(Image.FromStream(ms));
    }

    // A body mod: a MODS folder that swaps the hero's models (SWAP\NIER0xx...).
    static bool ImpIsBodyMod(string dir)
    {
        string sw = Path.Combine(dir, "SWAP");
        return Directory.Exists(sw) && Directory.GetFiles(sw).Any(f => Path.GetFileName(f).StartsWith("NIER0", StringComparison.OrdinalIgnoreCase));
    }

    static void ImpInstall()
    {
        if (impOut == null || !Directory.Exists(impOut)) return;
        string name = Regex.Replace(impName.Text.Trim(), "[\\\\/:*?\"<>|]", "-");
        if (name == "") { MessageBox.Show(T("noname"), form.Text); return; }
        string mods = Path.Combine(exeDir, "MODS"), dest = Path.Combine(mods, name), off = Path.Combine(exeDir, "MODS-desligados");
        Directory.CreateDirectory(mods);
        // One body mod at a time: the others are moved aside (not deleted).
        var others = Directory.GetDirectories(mods).Where(d => !string.Equals(d, dest, StringComparison.OrdinalIgnoreCase) && ImpIsBodyMod(d)).ToList();
        string ask = string.Format(E("Instalar \"{0}\" na pasta MODS?", "Install \"{0}\" in the MODS folder?", "¿Instalar \"{0}\" en la carpeta MODS?"), name);
        if (Directory.Exists(dest)) ask += "\n\n" + E("Já existe um mod com esse nome: ele será substituído.", "A mod with that name exists: it will be replaced.", "Ya existe un mod con ese nombre: se reemplazará.");
        if (others.Count > 0)
            ask += "\n\n" + E("Só um mod de corpo pode ficar ligado. Estes vão para a pasta MODS-desligados (dá para voltar depois):",
                              "Only one body mod can be on. These go to the MODS-desligados folder (you can move them back):",
                              "Solo un mod de cuerpo puede estar activo. Estos van a la carpeta MODS-desligados (se pueden devolver):")
                   + "\n  " + string.Join("\n  ", others.Select(Path.GetFileName));
        ask += "\n\n" + E("Feche o jogo antes.", "Close the game first.", "Cierra el juego antes.");
        if (MessageBox.Show(ask, form.Text, MessageBoxButtons.OKCancel) != DialogResult.OK) return;
        try
        {
            foreach (var o in others)
            {
                Directory.CreateDirectory(off);
                string to = Path.Combine(off, Path.GetFileName(o));
                if (Directory.Exists(to)) Directory.Delete(to, true);
                Directory.Move(o, to);
                ImpLogLine(E("desligado: ", "turned off: ", "desactivado: ") + Path.GetFileName(o));
            }
            if (Directory.Exists(dest)) Directory.Delete(dest, true);
            ImpCopy(impOut, dest);
            ImpLogLine(E("Instalado em ", "Installed in ", "Instalado en ") + dest);
            MessageBox.Show(T("playasdone", dest), form.Text);
        }
        catch (Exception ex) { ImpLogLine(T("err", ex.Message)); MessageBox.Show(T("err", ex.Message), form.Text); }
    }

    static void ImpCopy(string from, string to)
    {
        Directory.CreateDirectory(to);
        foreach (var f in Directory.GetFiles(from)) File.Copy(f, Path.Combine(to, Path.GetFileName(f)), true);
        foreach (var d in Directory.GetDirectories(from)) ImpCopy(d, Path.Combine(to, Path.GetFileName(d)));
    }

    static void ApplyImportText()
    {
        if (tabs != null && tabs.TabPages.Count > 3) tabs.TabPages[3].Text = E("Importar modelo 3D", "Import 3D model", "Importar modelo 3D");
    }
}
