// NierTextureTool - the animation editor (2026-11-18), a panel of the 3D Models tab.
//
// Animations are edited in the master skeleton's terms (Humanoid.cs): each bone's turn in the model's space away from
// the common T-pose, the hips' place in hip heights. So an animation shown here plays the same on any body (the
// father's, Kaine's, an imported one): the joints turn, the bones keep the body's own lengths (as VRChat and such).
// The model chosen in the tab shows it; its joints are drawn and picked with a click.
//
// Sources: the model's own animation (the bar below), a .hanim, a BVH (Blender, Mixamo, mocap) or an FBX's first
// animation. Saved as .hanim (for the import's "Movimento de:" folder: a file named as the father's animation,
// "nier030_01_walk-lo_01.hanim", replaces it) or BVH (on the shown model's T-pose and bone lengths).
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Windows.Forms;

static partial class Gui
{
    static Panel aePanel;
    static Button aeToggle;
    static ListBox aeBones;
    static NumericUpDown aeRx, aeRy, aeRz, aeReach;
    static CheckBox aeAll, aeKids, aeShowJoints;
    static Label aeInfo;
    static Humanoid.Clip aeClip;
    static readonly List<Humanoid.Clip> aeUndo = new List<Humanoid.Clip>();
    static int[] aeJoints;  // master bone -> joint of the shown model

    static void BuildAnimEditor(TabPage page)
    {
        aeToggle = new Button { Dock = DockStyle.Right, Width = 120, FlatStyle = FlatStyle.Flat, ForeColor = Color.White };
        animBar.Controls.Add(aeToggle);
        aeToggle.Click += (s, e) => { aePanel.Visible = !aePanel.Visible; ApplyAnimEdText(); };

        aePanel = new Panel { Dock = DockStyle.Right, Width = 300, Visible = false, BackColor = Color.FromArgb(36, 36, 42), ForeColor = Color.Gainsboro, AutoScroll = true };
        int y = 8;
        Func<string, Control> label = t => { var l = new Label { Left = 8, Top = y, Width = 280, Height = 18, Text = t }; aePanel.Controls.Add(l); y += 20; return l; };
        Func<string, int, int, Button> button = (t, x, w) => { var b = new Button { Left = x, Top = y, Width = w, Height = 26, Text = t, FlatStyle = FlatStyle.Flat }; aePanel.Controls.Add(b); return b; };

        var title = label(E("Editor de animação (esqueleto mestre)", "Animation editor (master skeleton)", "Editor de animación (esqueleto maestro)"));
        title.Font = new Font(title.Font, FontStyle.Bold);
        var fromCur = button(E("Pegar a animação atual", "Take the current animation", "Tomar la animación actual"), 8, 280); y += 30;
        var imp = button(E("Importar...", "Import...", "Importar..."), 8, 136);
        var exp = button(E("Exportar...", "Export...", "Exportar..."), 152, 136); y += 34;
        label(E("Ossos (clique numa junta no visor):", "Bones (click a joint in the view):", "Huesos (clic en una articulación):"));
        aeBones = new ListBox { Left = 8, Top = y, Width = 280, Height = 170, BackColor = Color.FromArgb(28, 28, 32), ForeColor = Color.Gainsboro, BorderStyle = BorderStyle.FixedSingle };
        aePanel.Controls.Add(aeBones); y += 176;
        label(E("Girar (graus, eixos do mundo X Y Z):", "Turn (degrees, world axes X Y Z):", "Girar (grados, ejes del mundo X Y Z):"));
        Func<int, NumericUpDown> num = x => { var n = new NumericUpDown { Left = x, Top = y, Width = 84, Minimum = -180, Maximum = 180, DecimalPlaces = 1, Increment = 5 }; aePanel.Controls.Add(n); return n; };
        aeRx = num(8); aeRy = num(104); aeRz = num(200); y += 30;
        label(E("Suavizar (quadros para cada lado):", "Blend (frames each side):", "Suavizar (cuadros a cada lado):"));
        aeReach = new NumericUpDown { Left = 8, Top = y, Width = 84, Minimum = 0, Maximum = 120, Value = 4 };
        aePanel.Controls.Add(aeReach);
        aeAll = new CheckBox { Left = 104, Top = y, Width = 184, Text = E("em todos os quadros", "on every frame", "en todos los cuadros") };
        aePanel.Controls.Add(aeAll); y += 28;
        aeKids = new CheckBox { Left = 8, Top = y, Width = 280, Checked = true, Text = E("levar junto o que pende do osso", "carry what hangs from the bone", "llevar lo que cuelga del hueso") };
        aePanel.Controls.Add(aeKids); y += 26;
        var apply = button(E("Aplicar giro", "Apply turn", "Aplicar giro"), 8, 136);
        var undo = button(E("Desfazer", "Undo", "Deshacer"), 152, 136); y += 34;
        aeShowJoints = new CheckBox { Left = 8, Top = y, Width = 280, Checked = true, Text = E("mostrar as juntas", "show the joints", "mostrar las articulaciones") };
        aePanel.Controls.Add(aeShowJoints); y += 26;
        var close = button(E("Parar de editar", "Stop editing", "Dejar de editar"), 8, 280); y += 34;
        aeInfo = new Label { Left = 8, Top = y, Width = 280, Height = 120, ForeColor = Color.Silver };
        aePanel.Controls.Add(aeInfo);
        page.Controls.Add(aePanel);

        fromCur.Click += (s, e) =>
        {
            var m = CurMotion();
            if (curSkel == null || m == null) { aeInfo.Text = E("Escolha um modelo e uma animação na barra de baixo.", "Pick a model and an animation in the bar below.", "Elige un modelo y una animación abajo."); return; }
            var c = Humanoid.FromMotion(curSkel, m, m.Name);
            if (c == null) { aeInfo.Text = E("Este esqueleto não tem os ossos mestres: ", "This skeleton lacks the master bones: ", "Este esqueleto no tiene los huesos maestros: ") + Humanoid.Missing(Humanoid.Map(curSkel)); return; }
            AeStart(c);
        };
        imp.Click += (s, e) =>
        {
            using (var d = new OpenFileDialog { Filter = E("Animações", "Animations", "Animaciones") + " (*.hanim;*.bvh;*.fbx)|*.hanim;*.bvh;*.fbx" })
            {
                if (d.ShowDialog() != DialogResult.OK) return;
                try
                {
                    Humanoid.Clip c;
                    string ext = Path.GetExtension(d.FileName).ToLowerInvariant();
                    if (ext == ".hanim") c = Humanoid.Read(d.FileName);
                    else if (ext == ".bvh") c = Humanoid.ReadBvh(d.FileName);
                    else
                    {
                        var fm = FbxModel.Load(d.FileName);
                        if (fm.Takes.Count == 0) throw new Exception(E("o FBX não tem animação", "the FBX has no animation", "el FBX no tiene animación"));
                        var sk = Humanoid.SkeletonOf(fm);
                        c = Humanoid.FromMotion(sk, Humanoid.MotionOf(sk, fm.Takes[0]), Path.GetFileNameWithoutExtension(d.FileName));
                        if (c == null) throw new Exception(E("faltam ossos: ", "missing bones: ", "faltan huesos: ") + Humanoid.Missing(Humanoid.Map(sk)));
                    }
                    AeStart(c);
                }
                catch (Exception ex) { aeInfo.Text = ex.Message; }
            }
        };
        exp.Click += (s, e) =>
        {
            if (aeClip == null) return;
            using (var d = new SaveFileDialog { Filter = E("Animação mestre", "Master animation", "Animación maestra") + " (*.hanim)|*.hanim|BVH (*.bvh)|*.bvh", FileName = aeClip.Name.Replace('^', '-') })
            {
                if (d.ShowDialog() != DialogResult.OK) return;
                try
                {
                    if (d.FileName.EndsWith(".bvh", StringComparison.OrdinalIgnoreCase))
                    {
                        if (curSkel == null) throw new Exception(E("escolha um modelo (dá as medidas do BVH)", "pick a model (it gives the BVH its sizes)", "elige un modelo"));
                        Humanoid.WriteBvh(aeClip, curSkel, d.FileName);
                    }
                    else Humanoid.Write(aeClip, d.FileName);
                    aeInfo.Text = E("Salvo: ", "Saved: ", "Guardado: ") + d.FileName;
                }
                catch (Exception ex) { aeInfo.Text = ex.Message; }
            }
        };
        apply.Click += (s, e) =>
        {
            int b = AeBone();
            if (aeClip == null || b < 0) return;
            aeUndo.Add(Humanoid.Copy(aeClip)); if (aeUndo.Count > 30) aeUndo.RemoveAt(0);
            Humanoid.Turn(aeClip, b, aeKids.Checked, Anim.Euler((float)aeRx.Value, (float)aeRy.Value, (float)aeRz.Value), (int)animAt, (int)aeReach.Value, aeAll.Checked);
            ShowPose();
        };
        undo.Click += (s, e) => { if (aeUndo.Count == 0) return; aeClip = aeUndo[aeUndo.Count - 1]; aeUndo.RemoveAt(aeUndo.Count - 1); ShowPose(); };
        close.Click += (s, e) => { aeClip = null; aeUndo.Clear(); preview.Lines = preview.Points = preview.SelPoints = null; preview.Clicked = null; AeFrames(); ShowPose(); };
        aeBones.SelectedIndexChanged += (s, e) => ShowPose();
        aeShowJoints.CheckedChanged += (s, e) => ShowPose();
        ApplyAnimEdText();
    }

    static void ApplyAnimEdText()
    {
        if (aeToggle == null) return;
        aeToggle.Text = aePanel.Visible ? E("Fechar editor", "Close editor", "Cerrar editor") : E("Editar animação", "Edit animation", "Editar animación");
    }

    static int AeBone()
    {
        var s = aeBones.SelectedItem as string;
        return s == null ? -1 : Array.IndexOf(Humanoid.Bones, s);
    }

    static void AeStart(Humanoid.Clip c)
    {
        aeClip = c;
        aeUndo.Clear();
        aeBones.Items.Clear();
        for (int b = 0; b < Humanoid.Bones.Length; b++) if (c.Rot[b] != null) aeBones.Items.Add(Humanoid.Bones[b]);
        aeInfo.Text = c.Name + ": " + c.Frames + E(" quadros. O modelo do visor toca com as próprias medidas.", " frames. The model in the view plays it with its own sizes.", " cuadros.");
        preview.Clicked = AePick;
        animAt = 0;
        AeFrames();
        ShowPose();
    }

    // The bar's frames: the clip's while editing.
    static void AeFrames()
    {
        var m = CurMotion();
        animSetting = true;
        animFrame.Maximum = aeClip != null ? Math.Max(1, aeClip.Frames - 1) : m != null ? Math.Max(1, m.Frames - 1) : 1;
        animFrame.Value = Math.Min(animFrame.Maximum, (int)animAt);
        animSetting = false;
    }
    static int CurFrames() { var m = CurMotion(); return aeClip != null ? aeClip.Frames : m != null ? m.Frames : 0; }

    // The pose while editing: the clip on the shown model (its other joints from its own animation), and the joints.
    static float[][] AePose()
    {
        if (aeClip == null || curSkel == null) return null;
        var m = CurMotion();
        var one = Humanoid.PoseAt(aeClip, animAt, curSkel, m, m != null ? Math.Min(animAt, m.Frames - 1) : 0);
        if (one == null) { aeInfo.Text = E("Este modelo não tem os ossos mestres.", "This model lacks the master bones.", "Este modelo no tiene los huesos maestros."); return null; }
        var world = Anim.World(curSkel, one, 0, false);
        aeJoints = Humanoid.JointsOf(curSkel);
        if (aeShowJoints.Checked && aeJoints != null)
        {
            var pts = new List<float>(); var lines = new List<float>(); var sel = new List<float>();
            int pick = AeBone();
            for (int b = 0; b < Humanoid.Bones.Length; b++)
            {
                int j = aeJoints[b];
                if (j < 0) continue;
                pts.AddRange(new[] { world[j][12], world[j][13], world[j][14] });
                if (b == pick) sel.AddRange(new[] { world[j][12], world[j][13], world[j][14] });
                int p = Humanoid.ParentOf[b];
                while (p >= 0 && aeJoints[p] < 0) p = Humanoid.ParentOf[p];
                if (p >= 0) { int pj = aeJoints[p]; lines.AddRange(new[] { world[pj][12], world[pj][13], world[pj][14], world[j][12], world[j][13], world[j][14] }); }
            }
            preview.Points = pts.ToArray(); preview.Lines = lines.ToArray(); preview.SelPoints = sel.ToArray();
        }
        else preview.Points = preview.Lines = preview.SelPoints = null;
        return world;
    }

    // A click: the nearest drawn joint (within 20 pixels) is chosen.
    static void AePick(Point at)
    {
        if (aeClip == null || aeJoints == null || curSkel == null) return;
        var m = CurMotion();
        var one = Humanoid.PoseAt(aeClip, animAt, curSkel, m, m != null ? Math.Min(animAt, m.Frames - 1) : 0);
        if (one == null) return;
        var world = Anim.World(curSkel, one, 0, false);
        int best = -1; float bd = 20 * 20;
        for (int b = 0; b < Humanoid.Bones.Length; b++)
        {
            int j = aeJoints[b];
            float sx, sy, dp;
            if (j < 0 || aeClip.Rot[b] == null || !preview.Project(world[j][12], world[j][13], world[j][14], out sx, out sy, out dp)) continue;
            float d = (sx - at.X) * (sx - at.X) + (sy - at.Y) * (sy - at.Y);
            if (d < bd) { bd = d; best = b; }
        }
        if (best >= 0) aeBones.SelectedItem = Humanoid.Bones[best];
    }
}
