// NierTextureTool - the "Models" tab: pick a model (.MDV/.MDP), turn it around, change its textures.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Windows.Forms;

static partial class Gui
{
    static List<PackInfo> allModels = new List<PackInfo>();
    static ListBox modelList;
    static TextBox modelSearch;
    static ComboBox modelKind;
    static GlView preview;
    static FlowLayoutPanel modelCards;
    static Label modelInfo;
    static Button exportObj, importObj, resetView, playAs;
    // The player's models (the father of Gestalt, his outfits): a body mod puts the chosen body on all of them.
    static readonly string[] kPlayerModels = { "NIER030", "NIER031", "NIER040" };
    static MdlFile curModel;
    static Pack2D curModelTex;
    static List<MdlFile.MeshPart> curParts;
    static bool sceneFresh;  // a new model: the camera starts over

    // Filter of the model list: directories of the game's packs.
    static readonly string[][] kKindDirs = { new[] { "CHARA" }, new[] { "BG", "SKY" }, new[] { "OBJ" }, new[] { "WEAPON" }, new[] { "EVENT" }, null };
    static bool IsMap(PackInfo p) { return p.Dir.Equals("BG", StringComparison.OrdinalIgnoreCase) || p.Dir.Equals("SKY", StringComparison.OrdinalIgnoreCase); }

    static void BuildModelTab(TabPage page)
    {
        var left = new Panel { Dock = DockStyle.Left, Width = 250 };
        modelSearch = new TextBox { Dock = DockStyle.Top };
        modelKind = new ComboBox { Dock = DockStyle.Top, DropDownStyle = ComboBoxStyle.DropDownList };
        modelList = new ListBox { Dock = DockStyle.Fill, IntegralHeight = false };
        left.Controls.Add(modelList); left.Controls.Add(modelKind); left.Controls.Add(modelSearch);

        modelCards = new FlowLayoutPanel { Dock = DockStyle.Right, Width = 340, AutoScroll = true, Padding = new Padding(4), BackColor = Color.FromArgb(34, 34, 38) };
        var bottom = new Panel { Dock = DockStyle.Bottom, Height = 26, BackColor = Color.FromArgb(24, 24, 28) };
        modelInfo = new Label { Dock = DockStyle.Fill, ForeColor = Color.Silver, TextAlign = ContentAlignment.MiddleLeft };
        exportObj = new Button { Dock = DockStyle.Right, Width = 130, FlatStyle = FlatStyle.Flat, ForeColor = Color.White };
        resetView = new Button { Dock = DockStyle.Right, Width = 110, FlatStyle = FlatStyle.Flat, ForeColor = Color.White };
        playAs = new Button { Dock = DockStyle.Right, Width = 170, FlatStyle = FlatStyle.Flat, ForeColor = Color.White };
        importObj = new Button { Dock = DockStyle.Right, Width = 130, FlatStyle = FlatStyle.Flat, ForeColor = Color.White };
        openEditor = new Button { Dock = DockStyle.Right, Width = 130, FlatStyle = FlatStyle.Flat, ForeColor = Color.White, Font = new Font(SystemFonts.DefaultFont, FontStyle.Bold) };
        openEditor.Click += (s, e) => OpenEditor();
        bottom.Controls.Add(modelInfo); bottom.Controls.Add(resetView); bottom.Controls.Add(importObj); bottom.Controls.Add(exportObj); bottom.Controls.Add(openEditor); bottom.Controls.Add(playAs);
        importObj.Click += (s, e) => ImportObj();
        playAs.Click += (s, e) => PlayAs();
        preview = new GlView { Dock = DockStyle.Fill, BackColor = Color.FromArgb(30, 30, 34) };
        BuildAnimBar();
        page.Controls.Add(preview); page.Controls.Add(animBar); page.Controls.Add(bottom); page.Controls.Add(modelCards); page.Controls.Add(left);
        BuildAnimEditor(page);

        modelSearch.TextChanged += (s, e) => FillModels();
        modelKind.SelectedIndexChanged += (s, e) => FillModels();
        modelList.SelectedIndexChanged += (s, e) => ShowModel();
        exportObj.Click += (s, e) => ExportObj();
        resetView.Click += (s, e) => { preview.ResetCamera(); preview.Focus(); };
    }

    static void RunTest()
    {
        tabs.SelectedIndex = 1;
        modelKind.SelectedIndex = kKindDirs.Length - 1;  // everything
        // NT_REPRO=<model>|<mod dir>: that model shown and a body mod made from it first (as "Play with this body").
        var repro = Environment.GetEnvironmentVariable("NT_REPRO");
        if (!string.IsNullOrEmpty(repro) && repro.Contains("|"))
        {
            var rp = repro.Split('|');
            modelSearch.Text = rp[0];
            if (modelList.Items.Count > 0) modelList.SelectedIndex = 0;
            Application.DoEvents();
            Directory.CreateDirectory(rp[1]);
            var pairs = string.Join(",", kPlayerModels.Select(m => m + "=" + rp[0]));
            Program.Swap(Program.OpenCpks(mediaBox.Text), pairs, rp[1], "CHARA");
            Application.DoEvents();
        }
        modelSearch.Text = TestModel;
        if (modelList.Items.Count > 0) modelList.SelectedIndex = 0;
        Application.DoEvents();
        // NT_ANIM=<part of an animation's name>:<frame>: the model posed there for the screenshot.
        var na = Environment.GetEnvironmentVariable("NT_ANIM");
        if (!string.IsNullOrEmpty(na) && animList.Items.Count > 1)
        {
            var sp = na.Split(':');
            for (int i = 1; i < animList.Items.Count; i++)
                if (animList.Items[i].ToString().IndexOf(sp[0], StringComparison.OrdinalIgnoreCase) >= 0) { animList.SelectedIndex = i; break; }
            SetPlaying(false);
            float fr; if (sp.Length > 1 && float.TryParse(sp[1], out fr)) animAt = fr;
            ShowPose();
            Application.DoEvents();
            // NT_ANIMSCREEN=<seconds>: plays as the user does (timer, paint) and grabs the window from the SCREEN
            // every 15 ms; the drawn area of each grab logged to <shot>.txt and the biggest saved as <shot>-max.png.
            int secs;
            if (int.TryParse(Environment.GetEnvironmentVariable("NT_ANIMSCREEN") ?? "", out secs))
            {
                if (Environment.GetEnvironmentVariable("NT_VISITEDITOR") != null) { OpenEditor(); Application.DoEvents(); tabs.SelectedIndex = 1; Application.DoEvents(); }
                form.TopMost = true; form.Activate();
                SetPlaying(true);
                var log = new System.Text.StringBuilder();
                var sw = System.Diagnostics.Stopwatch.StartNew();
                int worst = 0, n = 0; Bitmap worstBmp = null;
                var r = preview.RectangleToScreen(preview.ClientRectangle);
                while (sw.ElapsedMilliseconds < secs * 1000)
                {
                    Application.DoEvents();
                    var b = new Bitmap(r.Width, r.Height);
                    using (var g = Graphics.FromImage(b)) g.CopyFromScreen(r.Location, Point.Empty, r.Size);
                    int x0 = b.Width, x1 = -1, y0 = b.Height, y1 = -1;
                    for (int y = 0; y < b.Height; y += 3)
                        for (int x = 0; x < b.Width; x += 3)
                        {
                            var c = b.GetPixel(x, y);
                            if (Math.Abs(c.R - 30) + Math.Abs(c.G - 30) + Math.Abs(c.B - 34) < 24) continue;
                            if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
                        }
                    int area = x1 < 0 ? 0 : (x1 - x0) * (y1 - y0);
                    log.AppendLine(sw.ElapsedMilliseconds + " " + (int)animAt + " " + area);
                    if (area > worst) { worst = area; if (worstBmp != null) worstBmp.Dispose(); worstBmp = b; } else b.Dispose();
                    n++;
                    System.Threading.Thread.Sleep(15);
                }
                SetPlaying(false);
                if (worstBmp != null) { worstBmp.Save(TestShot + "-max.png"); worstBmp.Dispose(); }
                File.WriteAllText(TestShot + ".txt", n + " capturas, maior " + worst + "\r\n" + log);
            }
            // NT_ANIMSTRESS=<passes>: every frame with garbage collections around it; the drawn area of each
            // snapshot logged (a model drawn as stretched vertices fills far more of the view).
            int passes;
            if (int.TryParse(Environment.GetEnvironmentVariable("NT_ANIMSTRESS") ?? "", out passes) && CurMotion() != null)
            {
                var log = new System.Text.StringBuilder();
                int worst = 0, best = int.MaxValue;
                for (int pass = 0; pass < passes; pass++)
                    for (int f = 0; f < CurMotion().Frames; f++)
                    {
                        animAt = f;
                        GC.Collect(); ShowPose(); GC.Collect(); GC.WaitForPendingFinalizers(); GC.Collect();
                        using (var b = preview.Snapshot())
                        {
                            int x0 = b.Width, x1 = -1, y0 = b.Height, y1 = -1;
                            for (int y = 0; y < b.Height; y += 2)
                                for (int x = 0; x < b.Width; x += 2)
                                {
                                    var c = b.GetPixel(x, y);
                                    if (Math.Abs(c.R - 30) + Math.Abs(c.G - 30) + Math.Abs(c.B - 34) < 24) continue;
                                    if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
                                }
                            int area = x1 < 0 ? 0 : (x1 - x0) * (y1 - y0);
                            worst = Math.Max(worst, area); best = Math.Min(best, area);
                            log.AppendLine(pass + " " + f + " " + area);
                        }
                    }
                File.WriteAllText(TestShot + ".txt", "menor " + best + " maior " + worst + "\r\n" + log);
            }
        }
        // NT_GUI_EDIT=1: the 3D Editor instead (all of the first part selected, then the screenshot).
        GlView shot = preview;
        if (Environment.GetEnvironmentVariable("NT_GUI_EDIT") != null)
        {
            OpenEditor(); Application.DoEvents();
            if (Environment.GetEnvironmentVariable("NT_GUI_EDIT") == "4") EdMapSelfTest();
            if (Environment.GetEnvironmentVariable("NT_GUI_EDIT") == "5") { edView.Snapshot().Dispose(); EdClick(new Point(edView.ClientSize.Width / 2, edView.ClientSize.Height / 2)); Application.DoEvents(); goto shotNow; }
            EdSelectAll(true); if (Environment.GetEnvironmentVariable("NT_GUI_EDIT") == "2") edTabs.SelectedIndex = 1; if (Environment.GetEnvironmentVariable("NT_GUI_EDIT") == "3") { edView.Wire = true; EdSetMode('F'); edFaces.Clear(); edActive = 6; edParts.SelectedIndex = 6; edBrush = 10; Application.DoEvents(); edView.Snapshot().Dispose(); var bb = EdBuf(); var t0 = edModel.Tris(bb); var p0 = edModel.Pos(bb, t0[t0.Count / 2]); float qx, qy, qz; edView.Project(p0[0], p0[1], p0[2], out qx, out qy, out qz); EdClick(new Point((int)qx, (int)qy)); } Application.DoEvents();
            shotNow:
            shot = edView;
        }
        using (var bmp = new Bitmap(form.Width, form.Height))
        {
            form.DrawToBitmap(bmp, new Rectangle(0, 0, form.Width, form.Height));
            using (var g = Graphics.FromImage(bmp))
            using (var view = shot.Snapshot())
            {
                var at = form.PointToClient(shot.PointToScreen(Point.Empty));
                var frame = form.PointToScreen(Point.Empty);
                at.Offset(frame.X - form.Left, frame.Y - form.Top);
                g.DrawImage(view, at);
            }
            bmp.Save(TestShot);
        }
        Application.Exit();
    }

    static void ApplyModelText()
    {
        int k = modelKind.SelectedIndex;
        modelKind.Items.Clear();
        foreach (var key in new[] { "kchara", "kmap", "kobj", "kweapon", "kevent", "kall" }) modelKind.Items.Add(T(key));
        modelKind.SelectedIndex = k < 0 ? 0 : k;
        exportObj.Text = T("exportobj"); importObj.Text = T("importobj"); resetView.Text = T("resetview"); playAs.Text = T("playas");
        modelInfo.Text = curModel == null ? T("mdlhint") : curModel.Name;
        ApplyAnimText();
    }

    static void FillModels()
    {
        string q = modelSearch.Text.Trim();
        var dirs = modelKind.SelectedIndex >= 0 ? kKindDirs[modelKind.SelectedIndex] : kKindDirs[0];
        var shown = allModels.Where(p => (dirs == null || dirs.Any(d => p.Dir.Equals(d, StringComparison.OrdinalIgnoreCase))) && (q == "" || p.Key.IndexOf(q, StringComparison.OrdinalIgnoreCase) >= 0)).ToList();
        modelList.BeginUpdate();
        modelList.Items.Clear();
        foreach (var p in shown) modelList.Items.Add(p);
        modelList.EndUpdate();
        modelList.DisplayMember = "Key";
    }

    static void ShowModel()
    {
        var p = modelList.SelectedItem as PackInfo;
        if (p == null) return;
        form.Cursor = Cursors.WaitCursor;
        try
        {
            curModel = MdlFile.Load(p.Name, p.Cpk.Read(p.V), p.Cpk.Read(p.P));
            curModelTex = curModel.Textures();
            curParts = curModel.Parts();
            current = p;
            LoadAnims(p);
            LoadShared();
            preview.Fly = IsMap(p);
            // Maps are not edited for now (GuiEditor.MapEditing): no editor, no OBJ import for them.
            openEditor.Visible = importObj.Visible = !IsMap(p) || MapEditing;
            sceneFresh = true;
            modelCards.SuspendLayout();
            foreach (Control c in modelCards.Controls) c.Dispose();
            modelCards.Controls.Clear();
            foreach (var t in curModelTex.Textures) { var card = MakeCard(p, curModelTex, t); card.Width = 300; modelCards.Controls.Add(card); }
            modelCards.ResumeLayout();
            var lost = curParts.Select(q => q.Tex).Where(n => !curLib.ContainsKey(n) && !curModelTex.Textures.Any(t => string.Equals(t.Name, n, StringComparison.OrdinalIgnoreCase))).Distinct().ToList();
            RenderPreview(false);
            // A map's collision (CGOB records): drawn over it with C, so its invisible walls show.
            preview.SetCollision(IsMap(p) ? Program.CollisionTris(p.Cpk.Read(p.V)) : null);
            modelInfo.Text = p.Key + "  —  " + T("mdlstats", curParts.Count, curModelTex.Textures.Count, preview.Triangles.ToString("N0")) + "  —  " + T(IsMap(p) ? "flyhint" : "orbithint") + (lost.Count > 0 ? "  —  " + T("noshared", string.Join(", ", lost)) : "");
        }
        catch (Exception e) { status.Text = T("err", e.Message); }
        form.Cursor = Cursors.Default;
    }

    // Called by the texture cards when a texture is replaced or restored.
    static void TextureChanged()
    {
        if (tabs != null && tabs.SelectedIndex == 1 && curModel != null) RenderPreview(false);
    }

    // Images used by the current model, by texture name: own textures (re-read when the project PNG changes) and shared ones.
    static Dictionary<string, Tuple<long, MdlFile.Img>> imgCache = new Dictionary<string, Tuple<long, MdlFile.Img>>();
    static Dictionary<string, PackInfo> libIndex;
    static Dictionary<string, MdlFile.Img> curLib = new Dictionary<string, MdlFile.Img>();

    static void BuildLib()
    {
        if (libIndex != null) return;
        libIndex = new Dictionary<string, PackInfo>(StringComparer.OrdinalIgnoreCase);
        foreach (var p in allModels)
            try
            {
                var m = MdlFile.LoadHeader(p.Name, p.Cpk.Read(p.V));
                foreach (var t in m.Textures().Textures) if (!libIndex.ContainsKey(t.Name)) libIndex[t.Name] = p;
            }
            catch { }
    }

    // Textures the parts use that are not inside this model: fetched from the model that holds them.
    static void LoadShared()
    {
        curLib = new Dictionary<string, MdlFile.Img>();
        var own = new HashSet<string>(curModelTex.Textures.Select(t => t.Name), StringComparer.OrdinalIgnoreCase);
        var need = curParts.Select(p => p.Tex).Where(n => !own.Contains(n)).Distinct().ToList();
        if (need.Count == 0) return;
        BuildLib();
        foreach (var n in need)
        {
            string key = "lib/" + n;
            Tuple<long, MdlFile.Img> c;
            if (imgCache.TryGetValue(key, out c)) { if (c.Item2 != null) curLib[n] = c.Item2; continue; }
            MdlFile.Img img = null;
            PackInfo o;
            if (libIndex.TryGetValue(n, out o))
                try
                {
                    var m = MdlFile.Load(o.Name, o.Cpk.Read(o.V), o.Cpk.Read(o.P));
                    var tp = m.Textures();
                    var t = tp.Textures.FirstOrDefault(x => string.Equals(x.Name, n, StringComparison.OrdinalIgnoreCase));
                    if (t != null && t.Supported) img = MdlFile.Img.Make(tp.Decode(t), t.Width, t.Height);
                }
                catch { }
            imgCache[key] = Tuple.Create(0L, img);
            if (img != null) curLib[n] = img;
        }
    }

    static MdlFile.Img OwnImg(Tex2D t)
    {
        string f = ProjectFile(current, t), key = current.Key + "/" + t.Name;
        long stamp = 0;
        try { if (File.Exists(f)) stamp = File.GetLastWriteTimeUtc(f).Ticks; } catch { }
        Tuple<long, MdlFile.Img> c;
        if (imgCache.TryGetValue(key, out c) && c.Item1 == stamp) return c.Item2;
        MdlFile.Img img = null;
        try
        {
            if (stamp != 0) { int w, h; var rgba = PngIo.Load(f, out w, out h); img = MdlFile.Img.Make(rgba, w, h); }
            else img = MdlFile.Img.Make(curModelTex.Decode(t), t.Width, t.Height);
        }
        catch { }
        imgCache[key] = Tuple.Create(stamp, img);
        return img;
    }

    static void RenderPreview(bool fast)
    {
        if (curModel == null || curParts == null) return;
        var imgs = new Dictionary<string, MdlFile.Img>(curLib, StringComparer.OrdinalIgnoreCase);
        var used = new HashSet<string>(curParts.Select(p => p.Tex), StringComparer.OrdinalIgnoreCase);
        foreach (var t in curModelTex.Textures)
        {
            if (!t.Supported || !used.Contains(t.Name)) continue;
            var img = OwnImg(t);
            if (img != null) imgs[t.Name] = img;
        }
        curImgs = imgs;
        var motion = CurMotion();
        if (motion != null && !sceneFresh) { ShowPose(); return; }
        preview.SetScene(curParts, imgs, !sceneFresh);
        sceneFresh = false;
    }

    // ---- Animations (Anim.cs): the model posed by its own animations, played in the view ----
    static Panel animBar;
    static ComboBox animList;
    static Button animPlay;
    static TrackBar animFrame;
    static Label animInfo;
    static CheckBox animMove;
    static Timer animTimer;
    static Skeleton curSkel;
    static List<Motion> curMotions = new List<Motion>();
    static Dictionary<string, MdlFile.Img> curImgs;
    static float animAt;
    static bool animPlaying, animSetting;

    static void BuildAnimBar()
    {
        animBar = new Panel { Dock = DockStyle.Bottom, Height = 30, BackColor = Color.FromArgb(28, 28, 32), Padding = new Padding(4, 3, 4, 3), Visible = false };
        animList = new ComboBox { Dock = DockStyle.Left, Width = 330, DropDownStyle = ComboBoxStyle.DropDownList, MaxDropDownItems = 30 };
        animPlay = new Button { Dock = DockStyle.Left, Width = 90, FlatStyle = FlatStyle.Flat, ForeColor = Color.White };
        animMove = new CheckBox { Dock = DockStyle.Left, Width = 150, ForeColor = Color.Silver, Padding = new Padding(8, 0, 0, 0) };
        animFrame = new TrackBar { Dock = DockStyle.Fill, TickStyle = TickStyle.None, Minimum = 0, Maximum = 1, AutoSize = false, Height = 24 };
        animInfo = new Label { Dock = DockStyle.Right, Width = 110, ForeColor = Color.Silver, TextAlign = ContentAlignment.MiddleRight };
        animBar.Controls.Add(animFrame); animBar.Controls.Add(animInfo); animBar.Controls.Add(animMove); animBar.Controls.Add(animPlay); animBar.Controls.Add(animList);
        animTimer = new Timer { Interval = 33 };  // the game's 30 frames a second
        animList.SelectedIndexChanged += (s, e) =>
        {
            if (animSetting) return;
            var m = CurMotion();
            animAt = 0;
            animSetting = true;
            animFrame.Maximum = m != null ? Math.Max(1, m.Frames - 1) : 1;
            animFrame.Value = 0;
            animSetting = false;
            SetPlaying(m != null || aeClip != null);
            AeFrames();
            ShowPose();
        };
        animPlay.Click += (s, e) => { SetPlaying(!animPlaying && (CurMotion() != null || aeClip != null)); preview.Focus(); };
        animFrame.ValueChanged += (s, e) => { if (animSetting) return; SetPlaying(false); animAt = animFrame.Value; ShowPose(); };
        animMove.CheckedChanged += (s, e) => ShowPose();
        animTimer.Tick += (s, e) =>
        {
            var m = CurMotion();
            if ((m == null && aeClip == null) || !animPlaying) return;
            animAt += 1;
            if (animAt > CurFrames() - 1) animAt = 0;
            animSetting = true;
            animFrame.Value = Math.Min(animFrame.Maximum, (int)animAt);
            animSetting = false;
            ShowPose();
        };
        ApplyAnimText();
    }

    static void ApplyAnimText()
    {
        if (animPlay == null) return;
        animPlay.Text = T(animPlaying ? "animpause" : "animplay");
        animMove.Text = T("animmove");
        if (animList.Items.Count > 0) { animSetting = true; int k = animList.SelectedIndex; animList.Items[0] = T("animrest"); animList.SelectedIndex = k; animSetting = false; }
    }

    static void SetPlaying(bool on)
    {
        animPlaying = on;
        if (on) animTimer.Start(); else animTimer.Stop();
        ApplyAnimText();
    }

    static Motion CurMotion()
    {
        int k = animList == null ? 0 : animList.SelectedIndex;
        return k >= 1 && k - 1 < curMotions.Count ? curMotions[k - 1] : null;
    }

    static void LoadAnims(PackInfo p)
    {
        SetPlaying(false);
        curSkel = null;
        curMotions = new List<Motion>();
        try { if (!IsMap(p)) curSkel = Anim.Load(p.Cpk.Read(p.V), curMotions); } catch { curSkel = null; }
        bool any = curSkel != null && curMotions.Count > 0 && curParts.Any(q => q.BI != null);
        animSetting = true;
        animList.BeginUpdate();
        animList.Items.Clear();
        if (any)
        {
            animList.Items.Add(T("animrest"));
            string prefix = p.Name.ToLowerInvariant() + "_";
            foreach (var m in curMotions) animList.Items.Add(m.Name.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) ? m.Name.Substring(prefix.Length) : m.Name);
            animList.SelectedIndex = 0;
        }
        animList.EndUpdate();
        animSetting = false;
        animInfo.Text = any ? T("animcount", curMotions.Count) : "";
        animBar.Visible = any;
    }

    // The current frame of the chosen animation on the model (the textures stay uploaded).
    static void ShowPose()
    {
        if (curModel == null || curParts == null || curImgs == null) return;
        var m = CurMotion();
        // the animation editor's clip on this model (GuiAnimEd.cs)
        var edWorld = AePose();
        if (edWorld != null)
        {
            var em = Anim.SkinMatrices(curSkel, edWorld);
            preview.SetScene(curParts.Select(q => Anim.Skin(q, em)).ToList(), curImgs, true, null, true);
            animInfo.Text = ((int)animAt + 1) + " / " + aeClip.Frames;
            return;
        }
        if (m == null || curSkel == null) { preview.SetScene(curParts, curImgs, true, null, true); return; }
        var mats = Anim.SkinMatrices(curSkel, Anim.World(curSkel, m, animAt, animMove.Checked));
        var posed = curParts.Select(q => Anim.Skin(q, mats)).ToList();
        preview.SetScene(posed, curImgs, true, null, true);
        animInfo.Text = ((int)animAt + 1) + " / " + m.Frames;
    }

    // A mod (MODS\corpo-<model>) where the player's models get the chosen character's body on their own
    // skeleton and animations (Program.Swap).
    static void PlayAs()
    {
        var p = modelList.SelectedItem as PackInfo;
        if (p == null || !p.Dir.Equals("CHARA", StringComparison.OrdinalIgnoreCase)) { MessageBox.Show(T("playasno"), form.Text); return; }
        string name = "corpo-" + p.Name.ToLowerInvariant();
        string dir = Path.Combine(exeDir, "MODS", name);
        if (MessageBox.Show(T("playasask", name, p.Name), form.Text, MessageBoxButtons.OKCancel) != DialogResult.OK) return;
        form.Cursor = Cursors.WaitCursor;
        status.Text = T("loading");
        Application.DoEvents();
        try
        {
            if (Directory.Exists(dir)) Directory.Delete(dir, true);
            Directory.CreateDirectory(dir);
            var pairs = string.Join(",", kPlayerModels.Where(m => !m.Equals(p.Name, StringComparison.OrdinalIgnoreCase)).Select(m => m + "=" + p.Name));
            Program.Swap(Program.OpenCpks(mediaBox.Text), pairs, dir, "CHARA");
            form.Cursor = Cursors.Default;
            status.Text = T("playasdone", dir).Replace("\n", "  ");
            MessageBox.Show(T("playasdone", dir), form.Text);
        }
        catch (Exception e) { form.Cursor = Cursors.Default; status.Text = T("err", e.Message); MessageBox.Show(T("err", e.Message)); }
    }

    // The model edited in Blender (or any 3D program): the OBJ written by ExportObj, its parts ("o partN")
    // with the same vertices in the same order, moved. Their new positions go into a copy of the model's
    // data (.MDP), served by a mod in MODS\editado-<model>. Vertices can't be added or removed (the game's
    // buffers, bones and textures stay as they are).
    static void ImportObj()
    {
        var p = modelList.SelectedItem as PackInfo;
        if (curModel == null || curParts == null || p == null) return;
        string file;
        using (var dlg = new OpenFileDialog { Filter = "Wavefront OBJ|*.obj", Title = T("importobj") })
        {
            if (dlg.ShowDialog(form) != DialogResult.OK) return;
            file = dlg.FileName;
        }
        var ci = System.Globalization.CultureInfo.InvariantCulture;
        var groups = new SortedDictionary<int, List<float[]>>();
        List<float[]> cur = null;
        int order = 0;
        foreach (var raw in File.ReadLines(file))
        {
            var line = raw.Trim();
            if (line.StartsWith("o ") || line.StartsWith("g "))
            {
                var m = System.Text.RegularExpressions.Regex.Match(line, @"part(\d+)");
                int idx = m.Success ? int.Parse(m.Groups[1].Value) : order;
                order++;
                if (!groups.TryGetValue(idx, out cur)) groups[idx] = cur = new List<float[]>();
                continue;
            }
            if (!line.StartsWith("v ")) continue;
            var q = line.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
            if (q.Length < 4) continue;
            if (cur == null) groups[0] = cur = new List<float[]>();
            cur.Add(new[] { float.Parse(q[1], ci), float.Parse(q[2], ci), float.Parse(q[3], ci) });
        }
        // OBJ vertex numbers run over the whole file; parts are matched by name (or order) and count.
        byte[] pix = (byte[])curModel.Pix.Clone();
        int done = 0;
        var bad = new List<string>();
        foreach (var g in groups)
        {
            if (g.Key < 0 || g.Key >= curParts.Count) { bad.Add("part" + g.Key); continue; }
            var pt = curParts[g.Key];
            if (g.Value.Count != pt.N) { bad.Add("part" + g.Key + " (" + g.Value.Count + "/" + pt.N + ")"); continue; }
            for (int i = 0; i < pt.N; i++)
                for (int k = 0; k < 3; k++)
                {
                    var b = BitConverter.GetBytes(g.Value[i][k]);
                    int o = pt.VOff + i * pt.Stride + k * 4;
                    pix[o] = b[3]; pix[o + 1] = b[2]; pix[o + 2] = b[1]; pix[o + 3] = b[0];
                }
            done++;
        }
        if (done == 0) { MessageBox.Show(T("importnone", string.Join(", ", bad)), form.Text); return; }
        string name = "editado-" + p.Name.ToLowerInvariant();
        string dir = Path.Combine(exeDir, "MODS", name);
        if (MessageBox.Show(T("importask", name, done, curParts.Count) + (bad.Count > 0 ? "\n\n" + T("importbad", string.Join(", ", bad)) : ""),
                            form.Text, MessageBoxButtons.OKCancel) != DialogResult.OK) return;
        try
        {
            if (Directory.Exists(dir)) Directory.Delete(dir, true);
            Directory.CreateDirectory(dir);
            CpkEntry mdv, mdp;
            var cpk = Program.FindModel(Program.OpenCpks(mediaBox.Text), p.Dir + "/" + p.Name, out mdv, out mdp);
            if (cpk == null) { MessageBox.Show(T("err", p.Name), form.Text); return; }
            Program.WriteMdpMod(cpk, mdp, curModel.Name, pix, dir, s => { });
            // The preview shows the edit at once.
            for (int i = 0; i < curParts.Count; i++)
            {
                var pt = curParts[i];
                for (int v = 0; v < pt.N; v++)
                    for (int k = 0; k < 3; k++)
                    {
                        int o = pt.VOff + v * pt.Stride + k * 4;
                        pt.V[v * 3 + k] = BitConverter.ToSingle(new[] { pix[o + 3], pix[o + 2], pix[o + 1], pix[o] }, 0);
                    }
            }
            curModel.Pix = pix;
            RenderPreview(false);
            status.Text = T("importdone", dir).Replace("\n", "  ");
            MessageBox.Show(T("importdone", dir), form.Text);
        }
        catch (Exception e) { status.Text = T("err", e.Message); MessageBox.Show(T("err", e.Message)); }
    }

    // The model as one Wavefront OBJ (+ MTL and the textures as PNG), the parts the game draws.
    static void ExportObj()
    {
        if (curModel == null || curParts == null) return;
        using (var dlg = new FolderBrowserDialog { Description = T("exportobj") })
        {
            if (dlg.ShowDialog(form) != DialogResult.OK) return;
            string dir = Path.Combine(dlg.SelectedPath, curModel.Name);
            Directory.CreateDirectory(dir);
            var ci = System.Globalization.CultureInfo.InvariantCulture;
            var obj = new System.Text.StringBuilder(); var mtl = new System.Text.StringBuilder();
            obj.AppendLine("# " + current.Key + " (NieR), exported by NierTextureTool");
            obj.AppendLine("mtllib " + curModel.Name + ".mtl");
            int vbase = 1, n = 0;
            var mats = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var pt in curParts)
            {
                obj.AppendLine("o part" + (n++));
                for (int i = 0; i < pt.N; i++) obj.AppendLine("v " + pt.V[i * 3].ToString("0.######", ci) + " " + pt.V[i * 3 + 1].ToString("0.######", ci) + " " + pt.V[i * 3 + 2].ToString("0.######", ci));
                for (int i = 0; i < pt.N; i++) obj.AppendLine("vt " + pt.UV[i * 2].ToString("0.#####", ci) + " " + (1 - pt.UV[i * 2 + 1]).ToString("0.#####", ci));
                if (pt.Tex != null) { obj.AppendLine("usemtl " + Program.Safe(pt.Tex)); mats.Add(pt.Tex); }
                for (int f = 0; f + 2 < pt.F.Length; f += 3)
                {
                    int a = pt.F[f] + vbase, b = pt.F[f + 1] + vbase, c = pt.F[f + 2] + vbase;
                    obj.AppendLine("f " + a + "/" + a + " " + b + "/" + b + " " + c + "/" + c);
                }
                vbase += pt.N;
            }
            foreach (var m in mats)
            {
                mtl.AppendLine("newmtl " + Program.Safe(m)); mtl.AppendLine("map_Kd " + Program.Safe(m) + ".png"); mtl.AppendLine();
                MdlFile.Img img = null;
                var own = curModelTex.Textures.FirstOrDefault(t => string.Equals(t.Name, m, StringComparison.OrdinalIgnoreCase));
                if (own != null && own.Supported) img = OwnImg(own); else curLib.TryGetValue(m, out img);
                if (img != null) PngIo.Save(Path.Combine(dir, Program.Safe(m) + ".png"), img.Px[0], img.W[0], img.H[0]);
            }
            File.WriteAllText(Path.Combine(dir, curModel.Name + ".obj"), obj.ToString());
            File.WriteAllText(Path.Combine(dir, curModel.Name + ".mtl"), mtl.ToString());
            status.Text = T("exported", dir);
        }
    }
}