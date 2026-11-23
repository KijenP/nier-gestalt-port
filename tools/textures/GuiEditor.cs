// NierTextureTool - the "3D Editor" tab: a model of the game edited piece by piece, vertex by vertex or
// face by face (the same tools as the BT3 character editor), saved as a mod.
//
// Opened from the 3D Models tab ("Edit model..."). Toolbar: Piece / Vertex / Face mode, undo / redo,
// wireframe, solid, x-ray, vertices, camera (front, back, side, top, all, selection), save. Side panel:
// Pieces (list: click = edit it, double click = show / hide; duplicate, mirror, delete), Edit (pivot,
// step, X symmetry, selection all / none / invert / grow / linked, position, nudges, rotate, scale,
// flatten; mesh: delete, duplicate, face of 3 vertices, flip, subdivide, detach, smooth, inflate, weld),
// Import (OBJ from Blender & co, pieces of another NieR model, OBJ export). In the view: click selects
// (Shift adds), Ctrl + drag a rectangle; G move, S scale, R rotate with the mouse (X / Y / Z one axis,
// click or Enter confirms, Esc cancels), Delete, Shift+D, F, A, Ctrl+Z / Ctrl+Y.
// "Save to the game" writes the model (any number of vertices and faces, EditModel.Build) as
// MODS\editado-<model>.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Windows.Forms;

static partial class Gui
{
    static GlView edView;
    static CheckedListBox edParts;
    static Label edInfo;
    static Button openEditor;
    static EditModel edModel;
    static PackInfo edPack;
    static int edActive = -1;
    static HashSet<int> edSel = new HashSet<int>();    // vertices of the active piece
    static HashSet<int> edFaces = new HashSet<int>();  // faces of the active piece (Face mode)
    static List<object> edUndoStack = new List<object>(), edRedoStack = new List<object>();
    static bool edShowVerts = true, edDirty, edSym, edSolid;
    static char edSelMode = 'V';                        // 'P' piece, 'V' vertex, 'F' face
    static int edBrush = 1;                             // selection size: a click takes this many rings (the picked one + its neighbours)
    static string edPivot = "sel";
    static double edStep = 0.05;
    // A mouse transform in progress: 'G' move, 'S' scale, 'R' rotate; axis -1 free, 0/1/2 X/Y/Z.
    static char edMode; static int edAxis = -1; static double edDx, edDy;
    static Dictionary<int, float[]> edStart; static float[] edPivotAt;
    static TabPage edPage;
    static FlowLayoutPanel edToolbar, edPanel;
    static TabControl edTabs;
    static readonly List<Action> edTextUpdates = new List<Action>();
    // Import from another model.
    static EditModel edOther; static PackInfo edOtherPack; static CheckedListBox edOtherParts; static ComboBox edOtherBox;

    static string E(string pt, string en, string es) { return ui == "pt" ? pt : ui == "es" ? es : en; }

    // ---- building the tab ----
    static Button EdBtn(Control parent, Func<string> text, Action click, int width = 0)
    {
        var b = new Button { FlatStyle = FlatStyle.Flat, Height = 25, AutoSize = width == 0, Width = width > 0 ? width : 60, Margin = new Padding(2) };
        b.Click += (s, e) => { try { click(); } catch (Exception ex) { MessageBox.Show(T("err", ex.Message), form.Text); } edView.Focus(); };
        edTextUpdates.Add(() => b.Text = text());
        parent.Controls.Add(b);
        return b;
    }
    static CheckBox EdChk(Control parent, Func<string> text, Action<bool> set, bool on = false)
    {
        var c = new CheckBox { AutoSize = true, Checked = on, Margin = new Padding(4, 6, 4, 2) };
        c.CheckedChanged += (s, e) => { set(c.Checked); edView.Focus(); };
        edTextUpdates.Add(() => c.Text = text());
        parent.Controls.Add(c);
        return c;
    }
    static Label EdLbl(Control parent, Func<string> text, bool head = false)
    {
        var l = new Label { AutoSize = true, Margin = new Padding(3, head ? 10 : 6, 3, 2) };
        if (head) l.Font = new Font(SystemFonts.DefaultFont, FontStyle.Bold);
        edTextUpdates.Add(() => l.Text = text());
        parent.Controls.Add(l);
        return l;
    }
    static NumericUpDown EdNum(Control parent, decimal value, decimal min, decimal max, int dec, decimal inc, int width = 70)
    {
        var n = new NumericUpDown { Minimum = min, Maximum = max, DecimalPlaces = dec, Increment = inc, Value = value, Width = width, Margin = new Padding(2) };
        parent.Controls.Add(n);
        return n;
    }
    static FlowLayoutPanel EdRow(Control parent)
    {
        var r = new FlowLayoutPanel { AutoSize = true, WrapContents = true, FlowDirection = FlowDirection.LeftToRight, Margin = new Padding(0), MaximumSize = new Size(300, 0) };
        parent.Controls.Add(r);
        return r;
    }
    static void EdBreak(Control c) { var p = c.Parent as FlowLayoutPanel; if (p != null) p.SetFlowBreak(c, true); }

    static void BuildEditorTab(TabPage page)
    {
        edPage = page;
        edView = new GlView { Dock = DockStyle.Fill, BackColor = Color.FromArgb(30, 30, 34) };
        edInfo = new Label { Dock = DockStyle.Bottom, Height = 40, ForeColor = Color.Silver, BackColor = Color.FromArgb(24, 24, 28), Padding = new Padding(6, 4, 6, 4) };
        edToolbar = new FlowLayoutPanel { Dock = DockStyle.Top, AutoSize = true, WrapContents = true, Padding = new Padding(2), BackColor = Color.FromArgb(40, 40, 46), ForeColor = Color.White };
        var side = new Panel { Dock = DockStyle.Left, Width = 320 };
        edTabs = new TabControl { Dock = DockStyle.Fill };
        side.Controls.Add(edTabs);
        page.Controls.Add(edView); page.Controls.Add(edInfo); page.Controls.Add(edToolbar); page.Controls.Add(side);

        // Toolbar.
        var tb = edToolbar;
        // The simple way to pick what to edit: on, a click on an object selects it (Piece mode); T toggles it.
        edObjBtn = EdBtn(tb, () => E("Selecionar objeto (T)", "Select object (T)", "Seleccionar objeto (T)"), EdToggleObjectSelect);
        edObjBtn.Font = new Font(SystemFonts.DefaultFont, FontStyle.Bold);
        EdBtn(tb, () => E("Mover (G)", "Move (G)", "Mover (G)"), () => EdBegin('G'));
        EdBtn(tb, () => E("Girar (R)", "Rotate (R)", "Girar (R)"), () => EdBegin('R'));
        EdBtn(tb, () => E("Escalar (S/B)", "Scale (S/B)", "Escalar (S/B)"), () => EdBegin('S'));
        EdBtn(tb, () => E("Peça", "Piece", "Pieza"), () => EdSetMode('P'));
        EdBtn(tb, () => E("Vértice", "Vertex", "Vértice"), () => EdSetMode('V'));
        EdBtn(tb, () => E("Face", "Face", "Cara"), () => EdSetMode('F'));
        EdBtn(tb, () => E("Desfazer", "Undo", "Deshacer"), EdUndo); EdBtn(tb, () => E("Refazer", "Redo", "Rehacer"), EdRedo);
        EdChk(tb, () => E("Aramado", "Wireframe", "Alambre"), v => { edView.Wire = v; edView.Invalidate(); });
        EdChk(tb, () => E("Sólido", "Solid", "Sólido"), v => { edSolid = v; EdRefresh(); });
        EdChk(tb, () => E("Através", "X-ray", "A través"), v => { edView.XRay = v; edView.Invalidate(); });
        EdChk(tb, () => E("Vértices", "Vertices", "Vértices"), v => { edShowVerts = v; EdPoints(); }, true);
        // Selection size: a click selects the face (or vertex) and the rings of neighbours around it.
        EdLbl(tb, () => E("Tamanho da seleção", "Selection size", "Tamaño de selección")).ForeColor = Color.White;
        var brush = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 55, Margin = new Padding(2, 3, 2, 2) };
        foreach (var x in new[] { 1, 2, 3, 5, 10, 20, 50 }) brush.Items.Add(x);
        brush.SelectedIndex = 0;
        brush.SelectedIndexChanged += (s, e) => { edBrush = (int)brush.SelectedItem; edView.Focus(); };
        tb.Controls.Add(brush);
        EdBtn(tb, () => E("Frente", "Front", "Frente"), () => edView.SetView(Math.PI, -0.05));
        EdBtn(tb, () => E("Costas", "Back", "Espalda"), () => edView.SetView(0, -0.05));
        EdBtn(tb, () => E("Lado", "Side", "Lado"), () => edView.SetView(Math.PI / 2, -0.05));
        EdBtn(tb, () => E("Topo", "Top", "Arriba"), () => edView.SetView(Math.PI, -1.5));
        EdBtn(tb, () => E("Ver tudo", "View all", "Ver todo"), () => edView.ResetCamera());
        EdBtn(tb, () => E("Ver seleção", "View selection", "Ver selección"), EdFrameSel);
        var save = EdBtn(tb, () => E("Salvar no jogo (mod)...", "Save to the game (mod)...", "Guardar en el juego (mod)..."), EdSave);
        save.Font = new Font(SystemFonts.DefaultFont, FontStyle.Bold);
        var play = EdBtn(tb, () => E("Jogar com este corpo...", "Play with this body...", "Jugar con este cuerpo..."), EdPlayAs);
        play.Font = new Font(SystemFonts.DefaultFont, FontStyle.Bold);
        edPlayBtn = play;  // hidden for maps and anything not a character (OpenEditor)

        // Pieces.
        var tp = new TabPage(); edTextUpdates.Add(() => tp.Text = E("Peças", "Pieces", "Piezas")); edTabs.TabPages.Add(tp);
        var pPanel = new FlowLayoutPanel { Dock = DockStyle.Bottom, AutoSize = true, FlowDirection = FlowDirection.LeftToRight, WrapContents = true, Padding = new Padding(2) };
        edParts = new CheckedListBox { Dock = DockStyle.Fill, CheckOnClick = false, IntegralHeight = false };
        tp.Controls.Add(edParts); tp.Controls.Add(pPanel);
        EdBtn(pPanel, () => E("Mostrar tudo", "Show all", "Mostrar todo"), () => { for (int i = 0; i < edParts.Items.Count; i++) edParts.SetItemChecked(i, true); });
        EdBtn(pPanel, () => E("Isolar", "Isolate", "Aislar"), () => { for (int i = 0; i < edParts.Items.Count; i++) edParts.SetItemChecked(i, i == edActive); });
        EdBtn(pPanel, () => E("Ocultar", "Hide", "Ocultar"), () => { if (edActive >= 0) edParts.SetItemChecked(edActive, false); });
        EdBtn(pPanel, () => E("Duplicar peça", "Duplicate piece", "Duplicar pieza"), () => { EdSelectPiece(); EdDuplicate(); });
        EdBtn(pPanel, () => E("Apagar peça", "Delete piece", "Borrar pieza"), () => { EdSelectPiece(); EdDelete(false); });
        foreach (int ax in new[] { 0, 1, 2 }) { int a = ax; EdBtn(pPanel, () => E("Espelhar ", "Mirror ", "Espejar ") + "XYZ"[a], () => { EdSelectPiece(); EdMirror(a, false); }); }
        EdBtn(pPanel, () => E("Cópia espelhada X", "Mirrored copy X", "Copia espejada X"), () => { EdMirror(0, true); });
        // Visibility only (no rebuild), once for a burst of changes ("Show all", the opening).
        edParts.ItemCheck += (s, e) =>
        {
            if (edLoading || edVisPending) return;
            edVisPending = true;
            form.BeginInvoke((Action)(() => { edVisPending = false; EdSyncVisibility(); }));
        };
        edParts.SelectedIndexChanged += (s, e) =>
        {
            int i = edParts.SelectedIndex;
            if (i != edActive)
            {
                bool multi = edPieces.Count > 1 && edPieces.Contains(i);
                edActive = i; edSel.Clear(); edFaces.Clear();
                if (!multi) { edPieces.Clear(); if (i >= 0) edPieces.Add(i); }
                if (edSelMode == 'P') EdSelectPiece();
                EdPoints();
            }
        };
        edParts.DoubleClick += (s, e) => { int i = edParts.SelectedIndex; if (i >= 0) edParts.SetItemChecked(i, !edParts.GetItemChecked(i)); };

        // Edit.
        var te = new TabPage { AutoScroll = true }; edTextUpdates.Add(() => te.Text = E("Editar", "Edit", "Editar")); edTabs.TabPages.Add(te);
        edPanel = new FlowLayoutPanel { Dock = DockStyle.Top, AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, Padding = new Padding(2) };
        te.Controls.Add(edPanel);
        var ep = edPanel;
        var r = EdRow(ep);
        EdLbl(r, () => E("Pivô", "Pivot", "Pivote"));
        var piv = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 160 }; r.Controls.Add(piv);
        edTextUpdates.Add(() => { int k = Math.Max(0, piv.SelectedIndex); piv.Items.Clear(); piv.Items.Add(E("Centro da seleção", "Selection centre", "Centro de la selección")); piv.Items.Add(E("Origem (0,0,0)", "Origin (0,0,0)", "Origen (0,0,0)")); piv.SelectedIndex = k; });
        piv.SelectedIndexChanged += (s, e) => edPivot = piv.SelectedIndex == 1 ? "world" : "sel";
        r = EdRow(ep);
        EdLbl(r, () => E("Passo", "Step", "Paso"));
        var stp = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 70 }; r.Controls.Add(stp);
        foreach (var x in new[] { "0.005", "0.01", "0.05", "0.1", "0.5", "1" }) stp.Items.Add(x);
        stp.SelectedIndex = 2; stp.SelectedIndexChanged += (s, e) => edStep = double.Parse((string)stp.SelectedItem, CultureInfo.InvariantCulture);
        EdChk(r, () => E("Simetria X", "X symmetry", "Simetría X"), v => edSym = v);

        EdLbl(ep, () => E("Seleção", "Selection", "Selección"), true);
        r = EdRow(ep);
        EdBtn(r, () => E("Tudo", "All", "Todo"), () => EdSelectAll(true));
        EdBtn(r, () => E("Nada", "None", "Nada"), () => { edSel.Clear(); edFaces.Clear(); EdPoints(); });
        EdBtn(r, () => E("Inverter", "Invert", "Invertir"), EdInvert);
        EdBtn(r, () => E("Expandir", "Grow", "Expandir"), () => EdSelOp(1));
        EdBtn(r, () => E("Ligado", "Linked", "Conectado"), () => EdSelOp(2));

        EdLbl(ep, () => E("Posição (centro da seleção)", "Position (selection centre)", "Posición (centro de la selección)"), true);
        r = EdRow(ep);
        var pos = new NumericUpDown[3];
        for (int i = 0; i < 3; i++) { r.Controls.Add(new Label { Text = "XYZ"[i].ToString(), AutoSize = true, Margin = new Padding(2, 6, 0, 0) }); pos[i] = EdNum(r, 0, -1000, 1000, 3, 0.01m, 72); }
        EdBtn(r, () => E("Aplicar", "Apply", "Aplicar"), () =>
        {
            var c = EdCenter(); if (c == null) return;
            var d = new double[] { (double)pos[0].Value - c[0], (double)pos[1].Value - c[1], (double)pos[2].Value - c[2] };
            EdTransform(p => new[] { p[0] + d[0], p[1] + d[1], p[2] + d[2] });
        });
        EdBtn(r, () => E("Ler", "Read", "Leer"), () => { var c = EdCenter(); if (c == null) return; for (int i = 0; i < 3; i++) pos[i].Value = (decimal)Math.Max(-1000, Math.Min(1000, Math.Round(c[i], 3))); });
        r = EdRow(ep);
        for (int i = 0; i < 3; i++)
            foreach (int sg in new[] { -1, 1 })
            {
                int ax = i, sign = sg;
                EdBtn(r, () => "XYZ"[ax] + (sign < 0 ? "−" : "+"), () => EdTransform(p => { var q = (double[])p.Clone(); q[ax] += sign * edStep; return q; }), 38);
            }

        EdLbl(ep, () => E("Girar", "Rotate", "Girar"), true);
        r = EdRow(ep);
        var ang = EdNum(r, 15, -360, 360, 1, 1, 60);
        for (int i = 0; i < 3; i++)
            foreach (int sg in new[] { 1, -1 })
            {
                int ax = i, sign = sg;
                EdBtn(r, () => (sign > 0 ? "↻" : "↺") + "XYZ"[ax], () => EdRotate(ax, sign * (double)ang.Value * Math.PI / 180), 40);
            }

        EdLbl(ep, () => E("Escalar (%)", "Scale (%)", "Escalar (%)"), true);
        r = EdRow(ep);
        var sc = new NumericUpDown[3];
        for (int i = 0; i < 3; i++) { r.Controls.Add(new Label { Text = "XYZ"[i].ToString(), AutoSize = true, Margin = new Padding(2, 6, 0, 0) }); sc[i] = EdNum(r, 100, -1000, 1000, 1, 5, 62); }
        EdBtn(r, () => E("Aplicar", "Apply", "Aplicar"), () => EdScale(new[] { (double)sc[0].Value / 100, (double)sc[1].Value / 100, (double)sc[2].Value / 100 }));
        r = EdRow(ep);
        EdBtn(r, () => "×0,9", () => EdScale(new[] { .9, .9, .9 }));
        EdBtn(r, () => "×1,1", () => EdScale(new[] { 1.1, 1.1, 1.1 }));
        for (int i = 0; i < 3; i++) { int ax = i; EdBtn(r, () => E("Achatar ", "Flatten ", "Aplanar ") + "XYZ"[ax], () => { var s = new[] { 1.0, 1.0, 1.0 }; s[ax] = 0; EdScale(s); }); }

        EdLbl(ep, () => E("Malha", "Mesh", "Malla"), true);
        r = EdRow(ep);
        EdBtn(r, () => E("Apagar", "Delete", "Borrar"), () => EdDelete(false));
        EdBtn(r, () => E("Apagar só faces", "Delete faces only", "Borrar solo caras"), () => EdDelete(true));
        EdBtn(r, () => E("Duplicar", "Duplicate", "Duplicar"), EdDuplicate);
        EdBtn(r, () => E("Face de 3 vértices", "Face of 3 vertices", "Cara de 3 vértices"), EdFace);
        EdBtn(r, () => E("Inverter faces", "Flip faces", "Invertir caras"), () => EdMesh(b => edModel.FlipFaces(b, EdFaceSet()), false));
        EdBtn(r, () => E("Subdividir", "Subdivide", "Subdividir"), () => EdMesh(b => edModel.Subdivide(b, EdFaceSet()), true));
        EdBtn(r, () => E("Desconectar", "Detach", "Desconectar"), () => EdMesh(b => edModel.Detach(b, EdFaceSet()), true));
        r = EdRow(ep);
        var it = EdNum(r, 3, 1, 50, 0, 1, 45); var fa = EdNum(r, 0.5m, 0, 1, 2, 0.1m, 52);
        EdBtn(r, () => E("Alisar (passos, força)", "Smooth (steps, strength)", "Suavizar (pasos, fuerza)"), () => EdMesh(b => edModel.Smooth(b, EdVerts(), (int)it.Value, (float)fa.Value), false));
        r = EdRow(ep);
        var inf = EdNum(r, 0.01m, -1, 1, 3, 0.005m, 62);
        EdBtn(r, () => E("Inflar (− encolhe)", "Inflate (− shrinks)", "Inflar (− encoge)"), () => EdMesh(b => edModel.Inflate(b, EdVerts(), (float)inf.Value), false));
        r = EdRow(ep);
        var tol = EdNum(r, 0.0001m, 0.00001m, 1, 5, 0.0001m, 75);
        EdBtn(r, () => E("Soldar vértices iguais", "Weld equal vertices", "Soldar vértices iguales"), () => EdMesh(b => { int n = edModel.Weld(b, (float)tol.Value); edInfo.Text = n + E(" vértices soldados", " vertices welded", " vértices soldados"); }, true));

        // Import / export.
        var ti = new TabPage { AutoScroll = true }; edTextUpdates.Add(() => ti.Text = E("Importar", "Import", "Importar")); edTabs.TabPages.Add(ti);
        var ip = new FlowLayoutPanel { Dock = DockStyle.Top, AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, Padding = new Padding(2) };
        ti.Controls.Add(ip);
        EdLbl(ip, () => E("Exportar", "Export", "Exportar"), true);
        r = EdRow(ip);
        EdBtn(r, () => E("OBJ da peça", "OBJ of the piece", "OBJ de la pieza"), () => EdExportObj(false));
        EdBtn(r, () => E("OBJ de tudo", "OBJ of everything", "OBJ de todo"), () => EdExportObj(true));
        EdLbl(ip, () => E("Importar OBJ (Blender etc.) na peça", "Import OBJ (Blender...) into the piece", "Importar OBJ (Blender...) en la pieza"), true);
        r = EdRow(ip);
        r.Controls.Add(new Label { Text = E("Escala", "Scale", "Escala"), AutoSize = true, Margin = new Padding(2, 6, 0, 0) });
        var os = EdNum(r, 1, 0.0001m, 1000, 4, 0.1m, 70);
        var oz = EdChk(r, () => E("Z para cima", "Z up", "Z arriba"), v => { });
        var of = EdChk(r, () => E("Inverter faces", "Flip faces", "Invertir caras"), v => { });
        r = EdRow(ip);
        var od = new NumericUpDown[3];
        for (int i = 0; i < 3; i++) { r.Controls.Add(new Label { Text = "Δ" + "XYZ"[i], AutoSize = true, Margin = new Padding(2, 6, 0, 0) }); od[i] = EdNum(r, 0, -100, 100, 3, 0.05m, 62); }
        r = EdRow(ip);
        EdBtn(r, () => E("Escolher .obj e importar...", "Pick .obj and import...", "Elegir .obj e importar..."), () => EdImportObj((float)os.Value, oz.Checked, of.Checked, new[] { (float)od[0].Value, (float)od[1].Value, (float)od[2].Value }));
        EdLbl(ip, () => E("Peças de outro modelo do NieR", "Pieces of another NieR model", "Piezas de otro modelo de NieR"), true);
        edOtherBox = new ComboBox { Width = 290, DropDownStyle = ComboBoxStyle.DropDown, AutoCompleteMode = AutoCompleteMode.SuggestAppend, AutoCompleteSource = AutoCompleteSource.ListItems };
        ip.Controls.Add(edOtherBox);
        r = EdRow(ip);
        EdBtn(r, () => E("Abrir modelo", "Open model", "Abrir modelo"), EdOpenOther);
        edOtherParts = new CheckedListBox { Width = 290, Height = 170, CheckOnClick = true, IntegralHeight = false };
        ip.Controls.Add(edOtherParts);
        r = EdRow(ip);
        EdBtn(r, () => E("Trazer marcadas para a peça", "Bring the ticked into the piece", "Traer las marcadas a la pieza"), () => EdImportOther((float)os.Value, new[] { (float)od[0].Value, (float)od[1].Value, (float)od[2].Value }));
        EdLbl(ip, () => E("As peças trazidas ficam na peça selecionada: seguem os ossos mais próximos dela e usam a textura dela (ajuste as UVs ou troque a textura na aba Imagens).",
                         "Brought pieces go into the selected piece: they follow its nearest bones and use its texture.",
                         "Las piezas traídas quedan en la pieza seleccionada: siguen sus huesos más cercanos y usan su textura.")).MaximumSize = new Size(290, 0);
        edTabs.SelectedIndexChanged += (s, e) => { if (edTabs.SelectedIndex == 2 && edOtherBox.Items.Count == 0) foreach (var p in allModels) edOtherBox.Items.Add(p.Key); };

        if (MapEditing) BuildMapTab();

        edView.Clicked = EdClick;
        edView.Boxed = EdBox;
        edView.DragHook = EdSculptDrag;
        edView.StrokeEnd = EdSculptEnd;
        edView.KeyHook = EdKey;
        edView.MoveHook = EdMouse;
    }

    // ---- the "Map" tab: the BT3 map editor's general tools, for NieR's maps (2026-11-07) ----
    // Objects: Shift+click in Piece mode selects several pieces, "Select nearby" adds the pieces around (a whole tree);
    // G/S/R, delete and duplicate then act on all of them. Relief: a brush that raises, lowers, smooths or flattens the
    // ground under the mouse (every shown piece, one undo per stroke). Create polygon: a plane or a box in the active
    // piece (its texture and bones). Polygon reduction: the selected faces merged in cells of a size (local LOD).
    static bool edSculpt; static char edSculptMode = 'U'; static double edSculptRadius = 3, edSculptStrength = 0.1;
    static Dictionary<int, Tuple<int, byte[], List<List<int>>>> edStroke;   // the pieces a stroke changed, as before it
    static Dictionary<int, float[]> edStrokeBox;                              // their x/z bounds (lo x, lo z, hi x, hi z)
    static float edFlatY;

    static void BuildMapTab()
    {
        var tm = new TabPage { AutoScroll = true }; edTextUpdates.Add(() => tm.Text = E("Mapa", "Map", "Mapa")); edTabs.TabPages.Add(tm);
        var mp = new FlowLayoutPanel { Dock = DockStyle.Top, AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, Padding = new Padding(2) };
        tm.Controls.Add(mp);

        EdLbl(mp, () => E("Objetos", "Objects", "Objetos"), true);
        EdLbl(mp, () => E("Modo Peça: Shift+clique soma/tira peças. Mover, girar, escalar, duplicar e apagar valem para todas.",
                         "Piece mode: Shift+click adds/removes pieces. Move, rotate, scale, duplicate and delete act on all of them.",
                         "Modo Pieza: Shift+clic suma/quita piezas. Mover, girar, escalar, duplicar y borrar valen para todas.")).MaximumSize = new Size(290, 0);
        var r = EdRow(mp);
        var near = EdNum(r, 5, 0.1m, 10000, 1, 1, 70);
        EdBtn(r, () => E("Selecionar próximos", "Select nearby", "Seleccionar cercanos"), () => EdSelectNearby((double)near.Value));
        EdBtn(r, () => E("Limpar", "Clear", "Limpiar"), () => { edPieces.Clear(); if (edActive >= 0) edPieces.Add(edActive); EdPoints(); });

        EdLbl(mp, () => E("Relevo (montanhas e crateras)", "Relief (mountains and craters)", "Relieve (montañas y cráteres)"), true);
        EdChk(mp, () => E("Esculpir (botão esquerdo pinta)", "Sculpt (left button paints)", "Esculpir (botón izquierdo pinta)"), v => { edSculpt = v; EdStatus(); });
        EdLbl(mp, () => E("Câmera: botão direito, roda, WASD.", "Camera: right button, wheel, WASD.", "Cámara: botón derecho, rueda, WASD."));
        r = EdRow(mp);
        var smode = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 110, Margin = new Padding(2) }; r.Controls.Add(smode);
        edTextUpdates.Add(() => { int k = Math.Max(0, smode.SelectedIndex); smode.Items.Clear(); smode.Items.Add(E("Subir", "Raise", "Subir")); smode.Items.Add(E("Descer", "Lower", "Bajar")); smode.Items.Add(E("Alisar", "Smooth", "Suavizar")); smode.Items.Add(E("Nivelar", "Flatten", "Nivelar")); smode.SelectedIndex = k; });
        smode.SelectedIndexChanged += (s, e) => edSculptMode = "UDSF"[Math.Max(0, smode.SelectedIndex)];
        r = EdRow(mp);
        r.Controls.Add(new Label { Text = E("Raio", "Radius", "Radio"), AutoSize = true, Margin = new Padding(2, 6, 0, 0) });
        var srad = EdNum(r, 3, 0.05m, 1000, 2, 0.5m, 65); srad.ValueChanged += (s, e) => edSculptRadius = (double)srad.Value;
        r.Controls.Add(new Label { Text = E("Força", "Strength", "Fuerza"), AutoSize = true, Margin = new Padding(2, 6, 0, 0) });
        var sstr = EdNum(r, 0.1m, 0.001m, 100, 3, 0.05m, 65); sstr.ValueChanged += (s, e) => edSculptStrength = (double)sstr.Value;

        EdLbl(mp, () => E("Criar polígono (na peça ativa)", "Create polygon (in the active piece)", "Crear polígono (en la pieza activa)"), true);
        r = EdRow(mp);
        var prim = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 80, Margin = new Padding(2) }; r.Controls.Add(prim);
        edTextUpdates.Add(() => { int k = Math.Max(0, prim.SelectedIndex); prim.Items.Clear(); prim.Items.Add(E("Plano", "Plane", "Plano")); prim.Items.Add(E("Caixa", "Box", "Caja")); prim.SelectedIndex = k; });
        var psize = EdNum(r, 2, 0.01m, 10000, 2, 0.5m, 65);
        EdBtn(r, () => E("Criar", "Create", "Crear"), () => EdCreatePrimitive(prim.SelectedIndex == 1 ? "box" : "plane", (float)psize.Value));

        EdLbl(mp, () => E("Redução de polígonos (LOD local)", "Polygon reduction (local LOD)", "Reducción de polígonos (LOD local)"), true);
        EdLbl(mp, () => E("Junta os vértices da seleção (ou da peça) em células do tamanho escolhido. Desfazer com Ctrl+Z.",
                         "Merges the selection's (or the piece's) vertices in cells of the chosen size. Undo with Ctrl+Z.",
                         "Junta los vértices de la selección (o de la pieza) en celdas del tamaño elegido. Deshacer con Ctrl+Z.")).MaximumSize = new Size(290, 0);
        r = EdRow(mp);
        r.Controls.Add(new Label { Text = E("Célula", "Cell", "Celda"), AutoSize = true, Margin = new Padding(2, 6, 0, 0) });
        var cell = EdNum(r, 0.5m, 0.001m, 1000, 3, 0.1m, 70);
        EdBtn(r, () => E("Reduzir", "Reduce", "Reducir"), () => EdDecimate((float)cell.Value));
    }

    // The point of the shown pieces under the mouse (null: none), from the nearest face hit.
    static float[] EdPickPoint(Point at)
    {
        float[] best = null; float bestZ = float.MaxValue;
        for (int i = 0; i < edModel.Buffers.Count; i++)
        {
            if (i >= edParts.Items.Count || !edParts.GetItemChecked(i)) continue;
            var b = edModel.Buffers[i]; var t = edModel.Tris(b);
            var sx = new float[b.Count]; var sy = new float[b.Count]; var sz = new float[b.Count]; var ok = new bool[b.Count];
            for (int v = 0; v < b.Count; v++) { var p = edModel.Pos(b, v); ok[v] = edView.Project(p[0], p[1], p[2], out sx[v], out sy[v], out sz[v]); }
            for (int f = 0; f * 3 + 2 < t.Count; f++)
            {
                int a = t[f * 3], c = t[f * 3 + 1], d = t[f * 3 + 2];
                if (!ok[a] || !ok[c] || !ok[d]) continue;
                float den = (sy[c] - sy[d]) * (sx[a] - sx[d]) + (sx[d] - sx[c]) * (sy[a] - sy[d]);
                if (Math.Abs(den) < 1e-6f) continue;
                float l1 = ((sy[c] - sy[d]) * (at.X - sx[d]) + (sx[d] - sx[c]) * (at.Y - sy[d])) / den;
                float l2 = ((sy[d] - sy[a]) * (at.X - sx[d]) + (sx[a] - sx[d]) * (at.Y - sy[d])) / den;
                float l3 = 1 - l1 - l2;
                if (l1 < 0 || l2 < 0 || l3 < 0) continue;
                float z = l1 * sz[a] + l2 * sz[c] + l3 * sz[d];
                if (z >= bestZ) continue;
                bestZ = z;
                var pa = edModel.Pos(b, a); var pc = edModel.Pos(b, c); var pd = edModel.Pos(b, d);
                best = new[] { l1 * pa[0] + l2 * pc[0] + l3 * pd[0], l1 * pa[1] + l2 * pc[1] + l3 * pd[1], l1 * pa[2] + l2 * pc[2] + l3 * pd[2] };
            }
        }
        return best;
    }

    static bool EdSculptDrag(Point at, bool first)
    {
        if (!edSculpt || edModel == null || edMode != '\0') return false;
        var hit = EdPickPoint(at);
        if (first)
        {
            edStroke = new Dictionary<int, Tuple<int, byte[], List<List<int>>>>();
            edStrokeBox = new Dictionary<int, float[]>();
            for (int i = 0; i < edModel.Buffers.Count; i++)
            {
                if (i >= edParts.Items.Count || !edParts.GetItemChecked(i)) continue;
                var b = edModel.Buffers[i]; if (b.Count == 0) continue;
                var box = new[] { float.MaxValue, float.MaxValue, float.MinValue, float.MinValue };
                for (int v = 0; v < b.Count; v++) { var p = edModel.Pos(b, v); box[0] = Math.Min(box[0], p[0]); box[1] = Math.Min(box[1], p[2]); box[2] = Math.Max(box[2], p[0]); box[3] = Math.Max(box[3], p[2]); }
                edStrokeBox[i] = box;
            }
            if (hit != null) edFlatY = hit[1];
        }
        if (hit != null) EdSculptAt(hit);
        if (edApplyClock.ElapsedMilliseconds >= 40) { edApplyClock.Restart(); EdRefresh(); }
        return true;
    }

    // One dab of the brush at the point: the vertices within the radius (on x/z) move on y, more at the centre.
    static void EdSculptAt(float[] at)
    {
        double r = edSculptRadius, s = edSculptStrength;
        var hits = new List<Tuple<int, int, double>>();  // piece, vertex, weight
        double sumY = 0, sumW = 0;
        foreach (var kv in edStrokeBox)
        {
            var box = kv.Value;
            if (at[0] + r < box[0] || at[0] - r > box[2] || at[2] + r < box[1] || at[2] - r > box[3]) continue;
            var b = edModel.Buffers[kv.Key];
            for (int v = 0; v < b.Count; v++)
            {
                var p = edModel.Pos(b, v);
                double dx = p[0] - at[0], dz = p[2] - at[2], d2 = dx * dx + dz * dz;
                if (d2 > r * r) continue;
                double w = 1 - Math.Sqrt(d2) / r; w = w * w * (3 - 2 * w);
                hits.Add(Tuple.Create(kv.Key, v, w));
                sumY += p[1] * w; sumW += w;
            }
        }
        if (hits.Count == 0) return;
        double avgY = sumY / sumW;
        foreach (var h in hits)
        {
            if (!edStroke.ContainsKey(h.Item1)) edStroke[h.Item1] = (Tuple<int, byte[], List<List<int>>>)edModel.Snapshot(h.Item1);
            var b = edModel.Buffers[h.Item1]; var p = edModel.Pos(b, h.Item2);
            double y = p[1], w = h.Item3;
            if (edSculptMode == 'U') y += s * w;
            else if (edSculptMode == 'D') y -= s * w;
            else if (edSculptMode == 'S') y += (avgY - y) * Math.Min(1, s * w * 4);
            else y += (edFlatY - y) * Math.Min(1, s * w * 4);
            edModel.SetPos(b, h.Item2, p[0], (float)y, p[2]);
            edDirtyBufs.Add(h.Item1);
        }
    }

    static void EdSculptEnd()
    {
        if (edStroke != null && edStroke.Count > 0)
        {
            edUndoStack.Add(edStroke.Values.ToList());
            long bytes = edUndoStack.Sum(x => EditModel.SnapshotBytes(x));
            while (edUndoStack.Count > 1 && (edUndoStack.Count > 40 || bytes > 400L << 20)) { bytes -= EditModel.SnapshotBytes(edUndoStack[0]); edUndoStack.RemoveAt(0); }
            edRedoStack.Clear();
            edDirty = true;
        }
        edStroke = null; edStrokeBox = null;
        EdRefresh();
    }

    static void EdCreatePrimitive(string kind, float size)
    {
        var b = EdBuf(); if (b == null) { MessageBox.Show(E("Escolha uma peça primeiro.", "Pick a piece first.", "Elige una pieza primero."), form.Text); return; }
        // At the ground under the middle of the view, else at the selection, else at the piece's centre.
        var c = EdPickPoint(new Point(edView.ClientSize.Width / 2, edView.ClientSize.Height / 2)) ?? EdCenter() ?? EdBufCenter(edActive);
        if (kind == "box") c = new[] { c[0], c[1] + size / 2, c[2] };
        EdPush();
        var made = edModel.AddPrimitive(b, kind, c, size);
        if (edSelMode == 'P') edSelMode = 'V';
        edSel = new HashSet<int>(made);
        edFaces = edModel.FacesOfVerts(b, edSel, true);
        EdRefresh();
    }

    static void EdDecimate(float cell)
    {
        var b = EdBuf(); if (b == null) return;
        var targets = EdTargets();
        if (targets.Count == 0) { EdSelectPiece(); targets = EdTargets(); }
        if (targets.Count == 0) return;
        EdPushMany(targets.Keys);
        int before = 0, after = 0;
        foreach (var kv in targets)
        {
            var pb = edModel.Buffers[kv.Key];
            var faces = edModel.FacesOfVerts(pb, kv.Value, true);
            var res = edModel.Decimate(pb, faces, cell);
            before += res.Item1; after += res.Item2;
            edDirtyBufs.Add(kv.Key);
        }
        edSel.Clear(); edFaces.Clear();
        EdRefresh();
        edInfo.Text = E("Faces: ", "Faces: ", "Caras: ") + before + " → " + after + "\n" + edInfo.Text.Split('\n').Last();
    }

    // NT_GUI_EDIT=4 with --gui-test: the Map tab's tools on the opened map, timed, into nt-maptest.txt beside the picture
    // (a sculpt stroke across the middle of the view, a multi-piece move, a reduction, a box, then undo of the box).
    static void EdMapSelfTest()
    {
        var log = new System.Text.StringBuilder(); var sw = System.Diagnostics.Stopwatch.StartNew();
        Action<string> L = s => { log.AppendLine(sw.ElapsedMilliseconds + " ms  " + s); sw.Restart(); };
        edTabs.SelectedIndex = 3; Application.DoEvents(); edView.Snapshot().Dispose();
        int w = edView.ClientSize.Width, h = edView.ClientSize.Height;
        var hit = EdPickPoint(new Point(w / 2, h * 2 / 3)); L("pick " + (hit == null ? "none" : string.Join(",", hit.Select(x => x.ToString("0.00")))));
        edSculpt = true; edSculptMode = 'U'; edSculptRadius = 8; edSculptStrength = 1.5;
        EdSculptDrag(new Point(w / 3, h * 2 / 3), true);
        for (int i = 0; i <= 10; i++) EdSculptDrag(new Point(w / 3 + i * w / 30, h * 2 / 3), false);
        int touched = edStroke == null ? 0 : edStroke.Count;
        EdSculptEnd(); L("sculpt stroke: " + touched + " pieces, undo " + edUndoStack.Count);
        edSculpt = false;
        EdSetMode('P'); edPieces.Clear();
        int a = edModel.Buffers.FindIndex(b => b.Drawn && b.Count > 100);
        edActive = a; edParts.SelectedIndex = a; EdSelectNearby(20); L("nearby: " + edPieces.Count + " pieces");
        EdTransform(p => new[] { p[0], p[1] + 3, p[2] }); L("multi move up 3");
        EdSetMode('V'); EdSelectPiece();
        var bb = EdBuf(); int f0 = edModel.FaceCount(bb);
        EdDecimate(1.0f); L("reduce piece " + edActive + ": " + f0 + " -> " + edModel.FaceCount(bb) + " faces");
        int c0 = EdBuf().Count;
        EdCreatePrimitive("box", 4); L("box: vertices " + c0 + " -> " + EdBuf().Count);
        EdUndo(); L("undo box: vertices " + EdBuf().Count);
        long mem = System.Diagnostics.Process.GetCurrentProcess().WorkingSet64 >> 20; L("memory " + mem + " MB, undo steps " + edUndoStack.Count);
        try { File.WriteAllText(Path.Combine(Path.GetDirectoryName(TestShot), "nt-maptest.txt"), log.ToString()); } catch { }
        EdSetMode('P');
    }

    static void ApplyEditorText()
    {
        if (edView == null) return;
        foreach (var a in edTextUpdates) a();
        if (openEditor != null) openEditor.Text = E("Editar modelo...", "Edit model...", "Editar modelo...");
        if (edPage != null) edPage.Text = E("Editor 3D", "3D Editor", "Editor 3D");
        EdStatus();
    }

    // ---- opening ----
    // 2026-11-14: editing maps (BG/SKY) is hidden until after the release: a map edit crashed the game, and it is a
    // luxury next to the rest. The code stays; NT_MAP_EDIT=1 brings it back for development.
    static readonly bool MapEditing = Environment.GetEnvironmentVariable("NT_MAP_EDIT") == "1";

    static void OpenEditor()
    {
        var p = modelList.SelectedItem as PackInfo;
        if (p == null || curModel == null) { MessageBox.Show(T("mdlhint"), form.Text); return; }
        if (IsMap(p) && !MapEditing) return;
        if (edDirty && edPack != null && MessageBox.Show(E("Descartar as mudanças em " + edPack.Name + "?", "Discard the changes to " + edPack.Name + "?", "¿Descartar los cambios en " + edPack.Name + "?"), form.Text, MessageBoxButtons.OKCancel) != DialogResult.OK) return;
        try
        {
            form.Cursor = Cursors.WaitCursor;
            edModel = EditModel.Load(p.Name, p.Cpk.Read(p.V), p.Cpk.Read(p.P));
            edPack = p; edSel.Clear(); edFaces.Clear(); edUndoStack.Clear(); edRedoStack.Clear(); edDirty = false; edMode = '\0';
            edLoading = true;
            edParts.BeginUpdate();
            edParts.Items.Clear();
            foreach (var b in edModel.Buffers) edParts.Items.Add(EdLabel(b), b.Drawn);
            edParts.EndUpdate();
            edActive = edModel.Buffers.FindIndex(b => b.Drawn);
            if (edActive >= 0) edParts.SelectedIndex = edActive;
            edLoading = false;
            edView.Fly = IsMap(p);
            // "Play with this body" swaps the player's body: a map (or an object) there crashes the game.
            if (edPlayBtn != null) edPlayBtn.Visible = !IsMap(p) && EdIsCharacter(p);
            tabs.SelectedTab = edPage;
            edImgCache = null;
            edPieces.Clear();
            EdRefresh(false);
            EdSetMode(IsMap(p) ? 'P' : 'V');
            edView.Focus();
        }
        catch (Exception e) { MessageBox.Show(T("err", e.Message), form.Text); }
        form.Cursor = Cursors.Default;
    }

    static bool EdIsCharacter(PackInfo p) { return p != null && p.Dir.Equals("CHARA", StringComparison.OrdinalIgnoreCase); }

    static string EdLabel(EditModel.VBuf b)
    {
        return (b.Index + 1) + ". " + (b.Tex ?? (b.Drawn ? E("(sem textura)", "(no texture)", "(sin textura)") : E("(sombra)", "(shadow)", "(sombra)"))) + "  " + b.Count + "v " + edModel.Tris(b).Count / 3 + "f";
    }

    static Dictionary<string, MdlFile.Img> EdImages()
    {
        var imgs = new Dictionary<string, MdlFile.Img>(StringComparer.OrdinalIgnoreCase);
        if (edSolid) return imgs;
        foreach (var kv in curLib) imgs[kv.Key] = kv.Value;
        if (curModelTex != null && current == edPack)
            foreach (var t in curModelTex.Textures)
                if (t.Supported) { var img = OwnImg(t); if (img != null) imgs[t.Name] = img; }
        return imgs;
    }

    // 2026-11-07 (maps were very slow to open and an edit crashed the app, explorer.exe too): every refresh rebuilt
    // the whole scene and re-uploaded every texture, at each mouse move of G/S/R and at each of the hundreds of list
    // items filled at the opening. Now the scene is built once (every piece, the hidden ones hidden), an edit rebuilds
    // the pieces it changed (the active one and edDirtyBufs), and showing / hiding a piece only flips it.
    static readonly HashSet<int> edDirtyBufs = new HashSet<int>();
    static Dictionary<string, MdlFile.Img> edImgCache;
    static bool edVisPending, edLoading;
    static Button edPlayBtn;
    static readonly System.Diagnostics.Stopwatch edApplyClock = System.Diagnostics.Stopwatch.StartNew();

    static void EdRefresh() { EdRefreshChanged(); }
    static void EdRefresh(bool keepCamera)
    {
        if (edModel == null) return;
        edImgCache = EdImages();
        var parts = new List<MdlFile.MeshPart>(); var keys = new List<int>();
        for (int i = 0; i < edModel.Buffers.Count; i++) { parts.Add(edModel.Part(edModel.Buffers[i])); keys.Add(i); }
        edView.SetScene(parts, edImgCache, keepCamera, keys, keepCamera);
        edDirtyBufs.Clear();
        EdSyncVisibility();
        EdLabels();
        EdPoints();
    }

    // The pieces an edit changed, built again (and the labels); the camera, the textures and the other pieces stay.
    static void EdRefreshChanged()
    {
        if (edModel == null) return;
        if (edImgCache == null) { EdRefresh(true); return; }
        if (edActive >= 0) edDirtyBufs.Add(edActive);
        foreach (int i in edDirtyBufs)
            if (i >= 0 && i < edModel.Buffers.Count) edView.ReplacePart(i, edModel.Part(edModel.Buffers[i]), edImgCache);
        edDirtyBufs.Clear();
        EdLabels();
        EdPoints();
    }

    static void EdLabels()
    {
        edParts.BeginUpdate();
        for (int i = 0; i < edModel.Buffers.Count && i < edParts.Items.Count; i++)
        {
            string label = EdLabel(edModel.Buffers[i]);
            if ((string)edParts.Items[i] != label) edParts.Items[i] = label;
        }
        edParts.EndUpdate();
    }

    static void EdSyncVisibility()
    {
        if (edModel == null) return;
        for (int i = 0; i < edModel.Buffers.Count; i++) edView.SetHidden(i, !(i < edParts.Items.Count && edParts.GetItemChecked(i)));
    }

    static EditModel.VBuf EdBuf() { return edModel != null && edActive >= 0 && edActive < edModel.Buffers.Count ? edModel.Buffers[edActive] : null; }

    // The vertices an edit acts on (Face mode: those of the selected faces).
    static HashSet<int> EdVerts()
    {
        var b = EdBuf(); if (b == null) return new HashSet<int>();
        if (edSelMode == 'F') return edModel.VertsOfFaces(b, edFaces.Where(f => f < edModel.FaceCount(b)));
        return new HashSet<int>(edSel.Where(v => v < b.Count));
    }
    // The faces an edit acts on (Vertex mode: those wholly selected).
    static HashSet<int> EdFaceSet()
    {
        var b = EdBuf(); if (b == null) return new HashSet<int>();
        if (edSelMode == 'F') return new HashSet<int>(edFaces.Where(f => f < edModel.FaceCount(b)));
        return edModel.FacesOfVerts(b, EdVerts(), true);
    }

    static void EdPoints()
    {
        var b = EdBuf();
        if (b == null || edView == null) { if (edView != null) { edView.Points = edView.SelPoints = edView.SelTris = null; edView.Invalidate(); } EdStatus(); return; }
        if (edShowVerts && edSelMode != 'F')
        {
            var pts = new float[b.Count * 3];
            for (int v = 0; v < b.Count; v++) { var p = edModel.Pos(b, v); pts[v * 3] = p[0]; pts[v * 3 + 1] = p[1]; pts[v * 3 + 2] = p[2]; }
            edView.Points = pts;
        }
        else edView.Points = null;
        var sel = new List<float>();
        if (edSelMode != 'F') foreach (int v in edSel) if (v < b.Count) sel.AddRange(edModel.Pos(b, v));
        edView.SelPoints = sel.ToArray();
        var tris = new List<float>(); var t = edModel.Tris(b);
        foreach (int f in EdFaceSet()) for (int c = 0; c < 3; c++) tris.AddRange(edModel.Pos(b, t[f * 3 + c]));
        EdPiecesTris(tris);
        edView.SelTris = tris.ToArray();
        edView.Invalidate();
        EdStatus();
    }

    static void EdStatus()
    {
        if (edInfo == null) return;
        string mode = edMode == 'G' ? E("MOVER", "MOVE", "MOVER") : edMode == 'S' ? E("ESCALAR", "SCALE", "ESCALAR") : edMode == 'R' ? E("GIRAR", "ROTATE", "GIRAR") : null;
        string axis = edAxis < 0 ? "" : " (" + "XYZ"[edAxis] + ")";
        string selMode = edSelMode == 'P' ? E("peças", "pieces", "piezas") : edSelMode == 'F' ? E("faces", "faces", "caras") : E("vértices", "vertices", "vértices");
        int count = edSelMode == 'F' ? edFaces.Count : edSel.Count;
        string head = edModel == null ? E("Abra um modelo na aba Modelos 3D e clique em \"Editar modelo...\".", "Open a model in the 3D Models tab and click \"Edit model...\".", "Abre un modelo en la pestaña Modelos 3D y pulsa \"Editar modelo...\".")
            : edPack.Key + (edDirty ? " *" : "") + "  —  " + E("modo: ", "mode: ", "modo: ") + selMode + "  —  " + E("selecionados: ", "selected: ", "seleccionados: ") + count + (mode != null ? "  —  " + mode + axis : "");
        if (edModel != null && edSelMode == 'P' && EdBuf() != null)
            head += "  —  " + (edPieces.Count > 1 ? edPieces.Count + E(" peças", " pieces", " piezas") : EdLabel(EdBuf()));
        if (edModel != null && mode == null && edView.Fly)
        {
            edInfo.Text = head + "\n" + E("T: selecionar objeto (clique nele) | G mover | R girar | B escalar | Delete apagar | Shift+D duplicar | Ctrl+A tudo | Ctrl+Z/Y | WASD/Q/E + arrastar: câmera",
                "T: select object (click it) | G move | R rotate | B scale | Delete | Shift+D duplicate | Ctrl+A all | Ctrl+Z/Y | WASD/Q/E + drag: camera",
                "T: seleccionar objeto (clic en él) | G mover | R girar | B escalar | Supr borrar | Shift+D duplicar | Ctrl+A todo | Ctrl+Z/Y | WASD/Q/E + arrastrar: cámara");
            return;
        }
        edInfo.Text = head + "\n" + (mode != null
            ? E("mouse: ajustar | X/Y/Z: um eixo | clique ou Enter: confirmar | Esc: cancelar", "mouse: adjust | X/Y/Z: one axis | click or Enter: confirm | Esc: cancel", "ratón: ajustar | X/Y/Z: un eje | clic o Intro: confirmar | Esc: cancelar")
            : E("clique: selecionar (Shift soma) | Ctrl+arrastar: retângulo | G mover | S escalar | R girar | Delete apagar | Shift+D duplicar | F face | A tudo | Ctrl+Z/Y | arrastar: câmera",
                "click: select (Shift adds) | Ctrl+drag: rectangle | G move | S scale | R rotate | Delete | Shift+D duplicate | F face | A all | Ctrl+Z/Y | drag: camera",
                "clic: seleccionar (Shift suma) | Ctrl+arrastrar: rectángulo | G mover | S escalar | R girar | Supr borrar | Shift+D duplicar | F cara | A todo | Ctrl+Z/Y | arrastrar: cámara"));
    }

    // ---- selection ----
    static Button edObjBtn;
    static char edModeBeforeObj = 'V';
    static void EdToggleObjectSelect()
    {
        if (edSelMode == 'P') EdSetMode(edModeBeforeObj == 'P' ? 'V' : edModeBeforeObj);
        else { edModeBeforeObj = edSelMode; EdSetMode('P'); }
    }

    static void EdSetMode(char m)
    {
        if (edObjBtn != null) { edObjBtn.BackColor = m == 'P' ? Color.FromArgb(40, 110, 200) : SystemColors.Control; edObjBtn.ForeColor = m == 'P' ? Color.White : SystemColors.ControlText; }
        var b = EdBuf();
        if (b != null && m != edSelMode)
        {
            if (m == 'F') edFaces = EdFaceSet();
            else if (edSelMode == 'F') edSel = EdVerts();
        }
        edSelMode = m;
        if (m == 'P') EdSelectPiece();
        EdPoints();
    }

    static void EdSelectPiece()
    {
        var b = EdBuf(); if (b == null) return;
        edSel = new HashSet<int>(Enumerable.Range(0, b.Count));
        edFaces = new HashSet<int>(Enumerable.Range(0, edModel.FaceCount(b)));
    }

    // The face under the point (of the visible pieces): (piece, face), nearest first.
    static Tuple<int, int> EdPickFace(Point at, bool activeOnly)
    {
        Tuple<int, int> best = null; float bestZ = float.MaxValue;
        for (int i = 0; i < edModel.Buffers.Count; i++)
        {
            if (activeOnly && i != edActive) continue;
            if (i >= edParts.Items.Count || !edParts.GetItemChecked(i)) continue;
            var b = edModel.Buffers[i]; var t = edModel.Tris(b);
            var sx = new float[b.Count]; var sy = new float[b.Count]; var sz = new float[b.Count]; var ok = new bool[b.Count];
            for (int v = 0; v < b.Count; v++) { var p = edModel.Pos(b, v); ok[v] = edView.Project(p[0], p[1], p[2], out sx[v], out sy[v], out sz[v]); }
            for (int f = 0; f * 3 + 2 < t.Count; f++)
            {
                int a = t[f * 3], c = t[f * 3 + 1], d = t[f * 3 + 2];
                if (!ok[a] || !ok[c] || !ok[d]) continue;
                float d1 = (at.X - sx[c]) * (sy[a] - sy[c]) - (sx[a] - sx[c]) * (at.Y - sy[c]);
                float d2 = (at.X - sx[d]) * (sy[c] - sy[d]) - (sx[c] - sx[d]) * (at.Y - sy[d]);
                float d3 = (at.X - sx[a]) * (sy[d] - sy[a]) - (sx[d] - sx[a]) * (at.Y - sy[a]);
                bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
                if (neg && pos) continue;
                float z = (sz[a] + sz[c] + sz[d]) / 3;
                if (z < bestZ) { bestZ = z; best = Tuple.Create(i, f); }
            }
        }
        return best;
    }

    static void EdClick(Point at)
    {
        if (edMode != '\0') { EdConfirm(); return; }
        if (edModel == null) return;
        bool add = (Control.ModifierKeys & Keys.Shift) != 0;
        if (edSelMode == 'P')
        {
            var hit = EdPickFace(at, false);
            if (hit == null) { if (!add) { edPieces.Clear(); edSel.Clear(); edFaces.Clear(); EdPoints(); } return; }
            if (!add) edPieces.Clear();
            if (add && edPieces.Contains(hit.Item1) && edPieces.Count > 1)
            {
                edPieces.Remove(hit.Item1);
                if (hit.Item1 == edActive) { edActive = edPieces.First(); edParts.SelectedIndex = edActive; }
                EdSelectPiece(); EdPoints(); return;
            }
            if (add && edActive >= 0) edPieces.Add(edActive);
            edPieces.Add(hit.Item1);
            edActive = hit.Item1; edParts.SelectedIndex = edActive; EdSelectPiece(); EdPoints(); return;
        }
        if (edSelMode == 'F')
        {
            var hit = EdPickFace(at, true);
            if (!add) edFaces.Clear();
            if (hit != null)
            {
                var ring = EdRing(EdBuf(), null, hit.Item2);
                bool remove = add && edFaces.Contains(hit.Item2);
                foreach (int f in ring) { if (remove) edFaces.Remove(f); else edFaces.Add(f); }
            }
            EdPoints(); return;
        }
        var b = EdBuf(); if (b == null) return;
        int best = -1; float bestD = 12 * 12, bestZ = float.MaxValue;
        for (int v = 0; v < b.Count; v++)
        {
            var p = edModel.Pos(b, v);
            float sx, sy, z;
            if (!edView.Project(p[0], p[1], p[2], out sx, out sy, out z)) continue;
            float d = (sx - at.X) * (sx - at.X) + (sy - at.Y) * (sy - at.Y);
            if (d > 12 * 12) continue;
            if (best < 0 || d < bestD - 9 || (Math.Abs(d - bestD) <= 9 && z < bestZ)) { bestD = d; bestZ = z; best = v; }
        }
        if (!add) edSel.Clear();
        if (best >= 0)
        {
            var ring = EdRing(b, best, -1);
            bool remove = add && edSel.Contains(best);
            foreach (int v in ring) { if (remove) edSel.Remove(v); else edSel.Add(v); }
        }
        EdPoints();
    }

    // The picked vertex (or face) and edBrush - 1 rings of neighbours around it: each ring adds what
    // shares a vertex (or an edge's corner) with the ones before, across texture seams too.
    static HashSet<int> EdRing(EditModel.VBuf b, int? vertex, int face)
    {
        if (b == null) return new HashSet<int>();
        if (vertex.HasValue)
        {
            var vs = edModel.SamePlace(b, new HashSet<int> { vertex.Value });
            for (int i = 1; i < edBrush; i++) vs = edModel.Grow(b, vs);
            return vs;
        }
        var fs = new HashSet<int> { face };
        for (int i = 1; i < edBrush; i++) fs = edModel.FacesOfVerts(b, edModel.SamePlace(b, edModel.VertsOfFaces(b, fs)), false);
        return fs;
    }

    static void EdBox(Rectangle r, bool add)
    {
        var b = EdBuf(); if (b == null) return;
        var inside = new HashSet<int>();
        for (int v = 0; v < b.Count; v++)
        {
            var p = edModel.Pos(b, v);
            float sx, sy, z;
            if (edView.Project(p[0], p[1], p[2], out sx, out sy, out z) && r.Contains((int)sx, (int)sy)) inside.Add(v);
        }
        if (edSelMode == 'F') { if (!add) edFaces.Clear(); foreach (int f in edModel.FacesOfVerts(b, inside, true)) edFaces.Add(f); }
        else { if (!add) edSel.Clear(); foreach (int v in inside) edSel.Add(v); }
        EdPoints();
    }

    static void EdSelectAll(bool all)
    {
        var b = EdBuf(); if (b == null) return;
        bool any = edSelMode == 'F' ? edFaces.Count > 0 : edSel.Count > 0;
        edSel.Clear(); edFaces.Clear();
        if (all || !any) EdSelectPiece();
        if (edSelMode == 'F') edSel.Clear(); else if (edSelMode == 'V') edFaces.Clear();
        EdPoints();
    }

    static void EdInvert()
    {
        var b = EdBuf(); if (b == null) return;
        if (edSelMode == 'F') edFaces = new HashSet<int>(Enumerable.Range(0, edModel.FaceCount(b)).Where(f => !edFaces.Contains(f)));
        else edSel = new HashSet<int>(Enumerable.Range(0, b.Count).Where(v => !edSel.Contains(v)));
        EdPoints();
    }

    static void EdSelOp(int op)
    {
        var b = EdBuf(); if (b == null) return;
        var v = EdVerts(); if (v.Count == 0) return;
        var r = op == 1 ? edModel.Grow(b, v) : edModel.Linked(b, v);
        if (edSelMode == 'F') edFaces = edModel.FacesOfVerts(b, r, true); else edSel = r;
        EdPoints();
    }

    static float[] EdCenter()
    {
        var targets = EdTargets(); if (targets.Count == 0) return null;
        var c = new double[3]; long n = 0;
        foreach (var kv in targets)
        {
            var b = edModel.Buffers[kv.Key];
            foreach (int v in kv.Value) { var p = edModel.Pos(b, v); c[0] += p[0]; c[1] += p[1]; c[2] += p[2]; n++; }
        }
        if (n == 0) return null;
        return new[] { (float)(c[0] / n), (float)(c[1] / n), (float)(c[2] / n) };
    }
    static float[] EdPivotPoint() { return edPivot == "world" ? new float[3] : (EdCenter() ?? new float[3]); }

    static void EdFrameSel()
    {
        var b = EdBuf(); var vs = EdVerts(); if (b == null) return;
        if (vs.Count == 0) vs = new HashSet<int>(Enumerable.Range(0, b.Count));
        var lo = new[] { float.MaxValue, float.MaxValue, float.MaxValue }; var hi = new[] { float.MinValue, float.MinValue, float.MinValue };
        foreach (int v in vs) { var p = edModel.Pos(b, v); for (int k = 0; k < 3; k++) { lo[k] = Math.Min(lo[k], p[k]); hi[k] = Math.Max(hi[k], p[k]); } }
        edView.Frame(lo, hi);
    }

    // ---- edits ----
    // The active piece only (EditModel.Snapshot(int)); at most 40 steps and 400 MB.
    static void EdPush()
    {
        edUndoStack.Add(edActive >= 0 ? edModel.Snapshot(edActive) : edModel.Snapshot());
        long bytes = edUndoStack.Sum(s => EditModel.SnapshotBytes(s));
        while (edUndoStack.Count > 1 && (edUndoStack.Count > 40 || bytes > 400L << 20)) { bytes -= EditModel.SnapshotBytes(edUndoStack[0]); edUndoStack.RemoveAt(0); }
        edRedoStack.Clear();
        edDirty = true;
    }
    static void EdPushMany(IEnumerable<int> buffers)
    {
        var list = buffers.ToList();
        if (list.Count <= 1) { EdPush(); return; }
        edUndoStack.Add(edModel.Snapshot(list));
        long bytes = edUndoStack.Sum(s => EditModel.SnapshotBytes(s));
        while (edUndoStack.Count > 1 && (edUndoStack.Count > 40 || bytes > 400L << 20)) { bytes -= EditModel.SnapshotBytes(edUndoStack[0]); edUndoStack.RemoveAt(0); }
        edRedoStack.Clear();
        edDirty = true;
    }
    static object EdSnapshotLike(object other)
    {
        var bs = EditModel.SnapshotBuffers(other);
        return bs == null ? edModel.Snapshot() : bs.Count == 1 ? edModel.Snapshot(bs[0]) : edModel.Snapshot(bs);
    }
    static void EdMarkDirty(object snap)
    {
        var bs = EditModel.SnapshotBuffers(snap);
        if (bs != null) foreach (int i in bs) edDirtyBufs.Add(i); else for (int i = 0; i < edModel.Buffers.Count; i++) edDirtyBufs.Add(i);
    }

    // ---- several pieces at once (the BT3 map editor's multi-selection; 2026-11-07) ----
    // Piece mode: the pieces selected (Shift+click adds or removes one; "Select nearby" adds those around).
    static readonly HashSet<int> edPieces = new HashSet<int>();
    // What an edit acts on: piece -> its vertices (every vertex of each selected piece in Piece mode).
    static Dictionary<int, HashSet<int>> EdTargets()
    {
        var r = new Dictionary<int, HashSet<int>>();
        if (edModel == null) return r;
        if (edSelMode == 'P' && edPieces.Count > 1)
        {
            foreach (int i in edPieces) if (i >= 0 && i < edModel.Buffers.Count) r[i] = new HashSet<int>(Enumerable.Range(0, edModel.Buffers[i].Count));
            return r;
        }
        var b = EdBuf(); if (b == null) return r;
        var vs = EdVerts(); if (vs.Count > 0) r[edActive] = vs;
        return r;
    }
    static float[] EdBufCenter(int i)
    {
        var b = edModel.Buffers[i]; var c = new float[3]; if (b.Count == 0) return c;
        var lo = new[] { float.MaxValue, float.MaxValue, float.MaxValue }; var hi = new[] { float.MinValue, float.MinValue, float.MinValue };
        for (int v = 0; v < b.Count; v++) { var p = edModel.Pos(b, v); for (int k = 0; k < 3; k++) { lo[k] = Math.Min(lo[k], p[k]); hi[k] = Math.Max(hi[k], p[k]); } }
        for (int k = 0; k < 3; k++) c[k] = (lo[k] + hi[k]) / 2;
        return c;
    }
    // Adds every shown piece whose centre is within 'radius' of the active piece's (a whole tree at once).
    static void EdSelectNearby(double radius)
    {
        if (edModel == null || edActive < 0) return;
        var c = EdBufCenter(edActive);
        if (edSelMode != 'P') { edSelMode = 'P'; }
        edPieces.Add(edActive);
        for (int i = 0; i < edModel.Buffers.Count; i++)
        {
            if (i >= edParts.Items.Count || !edParts.GetItemChecked(i)) continue;
            var d = EdBufCenter(i);
            double dx = d[0] - c[0], dy = d[1] - c[1], dz = d[2] - c[2];
            if (dx * dx + dy * dy + dz * dz <= radius * radius) edPieces.Add(i);
        }
        EdSelectPiece(); EdPoints();
    }
    static void EdPiecesTris(List<float> tris)
    {
        if (edSelMode != 'P' || edPieces.Count <= 1) return;
        foreach (int i in edPieces)
        {
            if (i == edActive || i >= edModel.Buffers.Count) continue;
            var b = edModel.Buffers[i]; var t = edModel.Tris(b);
            for (int k = 0; k + 2 < t.Count; k += 3) for (int c = 0; c < 3; c++) tris.AddRange(edModel.Pos(b, t[k + c]));
        }
    }
    static void EdAfterTopology() { var b = EdBuf(); if (b == null) return; edSel.RemoveWhere(v => v >= b.Count); edFaces.RemoveWhere(f => f >= edModel.FaceCount(b)); }

    static void EdUndo()
    {
        if (edModel == null || edUndoStack.Count == 0) return;
        var top = edUndoStack[edUndoStack.Count - 1];
        edRedoStack.Add(EdSnapshotLike(top));
        edModel.Restore(top); EdMarkDirty(top);
        edUndoStack.RemoveAt(edUndoStack.Count - 1);
        EdAfterTopology(); EdRefresh();
    }
    static void EdRedo()
    {
        if (edModel == null || edRedoStack.Count == 0) return;
        var top = edRedoStack[edRedoStack.Count - 1];
        edUndoStack.Add(EdSnapshotLike(top));
        edModel.Restore(top); EdMarkDirty(top);
        edRedoStack.RemoveAt(edRedoStack.Count - 1);
        EdAfterTopology(); EdRefresh();
    }

    // A mesh operation on the active piece (topology: the selection is cleared when it changes).
    static void EdMesh(Action<EditModel.VBuf> op, bool topology)
    {
        var b = EdBuf(); if (b == null) return;
        if (EdVerts().Count == 0) { MessageBox.Show(E("Selecione algo primeiro.", "Select something first.", "Selecciona algo primero."), form.Text); return; }
        EdPush();
        op(b);
        if (topology) { edSel.Clear(); edFaces.Clear(); }
        EdAfterTopology(); EdRefresh();
    }

    static void EdDuplicate()
    {
        if (edSelMode == 'P' && edPieces.Count > 1)
        {   // each selected piece gets a copy of its geometry (in itself), then all the copies move together
            var targets = EdTargets();
            EdPushMany(targets.Keys);
            var made = new Dictionary<int, HashSet<int>>();
            foreach (var kv in targets) { made[kv.Key] = new HashSet<int>(edModel.Duplicate(edModel.Buffers[kv.Key], kv.Value)); edDirtyBufs.Add(kv.Key); }
            EdRefresh();
            edMode = 'G'; edAxis = -1; edDx = edDy = 0;
            edStart = new Dictionary<int, float[]>(); edStartOthers = new Dictionary<int, Dictionary<int, float[]>>();
            foreach (var kv in made)
            {
                var bb = edModel.Buffers[kv.Key];
                var start = kv.Value.ToDictionary(v => v, v => edModel.Pos(bb, v));
                if (kv.Key == edActive) edStart = start; else edStartOthers[kv.Key] = start;
            }
            edPivotAt = EdPivotPoint();
            EdStatus();
            return;
        }
        var b = EdBuf(); var vs = EdVerts(); if (b == null || vs.Count == 0) return;
        EdPush();
        var copies = edModel.Duplicate(b, vs);
        edSel = new HashSet<int>(copies);
        if (edSelMode == 'F') edFaces = edModel.FacesOfVerts(b, edSel, true);
        else if (edSelMode == 'P') edSelMode = 'V';
        EdRefresh();
        EdBegin('G');
    }

    static void EdFace()
    {
        var b = EdBuf(); if (b == null) return;
        if (edSelMode != 'V' || edSel.Count != 3) { MessageBox.Show(E("No modo Vértice, selecione exatamente 3 vértices.", "In Vertex mode, select exactly 3 vertices.", "En modo Vértice, selecciona exactamente 3 vértices."), form.Text); return; }
        EdPush();
        var v = edSel.ToList();
        var a = edModel.Pos(b, v[0]); var c = edModel.Pos(b, v[1]); var d = edModel.Pos(b, v[2]);
        float ax, ay, cx, cy, dx, dy, z;
        edView.Project(a[0], a[1], a[2], out ax, out ay, out z); edView.Project(c[0], c[1], c[2], out cx, out cy, out z); edView.Project(d[0], d[1], d[2], out dx, out dy, out z);
        float cross = (cx - ax) * (dy - ay) - (cy - ay) * (dx - ax);
        if (cross < 0) edModel.AddTriangle(b, v[0], v[1], v[2]); else edModel.AddTriangle(b, v[0], v[2], v[1]);
        EdRefresh();
    }

    static void EdDelete(bool facesOnly)
    {
        if (edSelMode == 'P' && edPieces.Count > 1)
        {
            var targets = EdTargets();
            EdPushMany(targets.Keys);
            foreach (var kv in targets)
            {
                var pb = edModel.Buffers[kv.Key];
                if (facesOnly) edModel.DeleteFaceSet(pb, new HashSet<int>(Enumerable.Range(0, edModel.FaceCount(pb))));
                else edModel.DeleteVertices(pb, kv.Value);
                edDirtyBufs.Add(kv.Key);
            }
            edSel.Clear(); edFaces.Clear();
            EdRefresh();
            return;
        }
        var b = EdBuf(); if (b == null) return;
        var vs = EdVerts(); if (vs.Count == 0) return;
        EdPush();
        if (edSelMode == 'F' || facesOnly)
        {
            edModel.DeleteFaceSet(b, EdFaceSet());
            // Vertices no face uses any more go too (Face mode).
            if (!facesOnly)
            {
                var used = new HashSet<int>(edModel.Tris(b));
                edModel.DeleteVertices(b, new HashSet<int>(vs.Where(v => !used.Contains(v))));
            }
        }
        else edModel.DeleteVertices(b, vs);
        edSel.Clear(); edFaces.Clear();
        EdRefresh();
    }

    static void EdMirror(int axis, bool copy)
    {
        var b = EdBuf(); if (b == null) return;
        if (copy) { if (EdVerts().Count == 0) EdSelectPiece(); EdPush(); edSel = new HashSet<int>(edModel.Duplicate(b, EdVerts())); edFaces = edModel.FacesOfVerts(b, edSel, true); }
        else { if (EdVerts().Count == 0) return; EdPush(); }
        edModel.Mirror(b, EdVerts(), axis, edPivot == "world" || copy ? 0f : EdPivotPoint()[axis]);
        EdRefresh();
    }

    // Moves every selected vertex through f (and, with X symmetry, its mirror twin through f's mirror).
    static void EdTransform(Func<double[], double[]> f)
    {
        var targets = EdTargets(); if (targets.Count == 0) return;
        EdPushMany(targets.Keys);
        foreach (var kv in targets)
        {
            var b = edModel.Buffers[kv.Key];
            EdApplyTo(b, kv.Value.ToDictionary(v => v, v => edModel.Pos(b, v)), f);
            edDirtyBufs.Add(kv.Key);
        }
        EdRefresh();
    }
    static void EdApplyTo(EditModel.VBuf b, Dictionary<int, float[]> start, Func<double[], double[]> f)
    {
        Dictionary<string, List<int>> twins = null;
        if (edSym)
        {
            twins = new Dictionary<string, List<int>>();
            for (int v = 0; v < b.Count; v++) { var p = edModel.Pos(b, v); string k = Key(p[0], p[1], p[2]); List<int> l; if (!twins.TryGetValue(k, out l)) twins[k] = l = new List<int>(); l.Add(v); }
        }
        var mirrored = new Dictionary<int, double[]>();
        foreach (var kv in start)
        {
            var p = kv.Value; var q = f(new double[] { p[0], p[1], p[2] });
            edModel.SetPos(b, kv.Key, (float)q[0], (float)q[1], (float)q[2]);
            List<int> l;
            if (twins != null && Math.Abs(p[0]) > 1e-5 && twins.TryGetValue(Key(-p[0], p[1], p[2]), out l))
                foreach (int t in l) if (!start.ContainsKey(t)) mirrored[t] = new[] { -q[0], q[1], q[2] };
        }
        foreach (var kv in mirrored) edModel.SetPos(b, kv.Key, (float)kv.Value[0], (float)kv.Value[1], (float)kv.Value[2]);
    }
    static string Key(double x, double y, double z) { return Math.Round(x * 1e4) + "," + Math.Round(y * 1e4) + "," + Math.Round(z * 1e4); }

    static double[] Rot(double[] p, float[] c, int ax, double ang)
    {
        int i = (ax + 1) % 3, j = (ax + 2) % 3; var q = (double[])p.Clone();
        double a = p[i] - c[i], d = p[j] - c[j];
        q[i] = c[i] + a * Math.Cos(ang) - d * Math.Sin(ang);
        q[j] = c[j] + a * Math.Sin(ang) + d * Math.Cos(ang);
        return q;
    }
    static void EdRotate(int ax, double ang) { var c = EdPivotPoint(); EdTransform(p => Rot(p, c, ax, ang)); }
    static void EdScale(double[] s) { var c = EdPivotPoint(); EdTransform(p => new[] { c[0] + (p[0] - c[0]) * s[0], c[1] + (p[1] - c[1]) * s[1], c[2] + (p[2] - c[2]) * s[2] }); }

    // ---- mouse transforms ----
    // The other pieces of a multi-piece G/S/R (edStart holds the active one's).
    static Dictionary<int, Dictionary<int, float[]>> edStartOthers;
    static void EdBegin(char mode)
    {
        var targets = EdTargets(); if (targets.Count == 0) return;
        if (edMode == '\0') EdPushMany(targets.Keys);
        edMode = mode; edAxis = -1; edDx = edDy = 0;
        edStart = null; edStartOthers = new Dictionary<int, Dictionary<int, float[]>>();
        foreach (var kv in targets)
        {
            var bb = edModel.Buffers[kv.Key];
            var start = kv.Value.ToDictionary(v => v, v => edModel.Pos(bb, v));
            if (kv.Key == edActive) edStart = start; else edStartOthers[kv.Key] = start;
        }
        if (edStart == null) edStart = new Dictionary<int, float[]>();
        edPivotAt = EdPivotPoint();
        EdStatus();
    }

    static void EdApply()
    {
        var b = EdBuf(); if (b == null || edStart == null) return;
        Func<double[], double[]> f = EdMouseTransform();
        EdApplyTo(b, edStart, f);
        if (edStartOthers != null)
            foreach (var kv in edStartOthers) { EdApplyTo(edModel.Buffers[kv.Key], kv.Value, f); edDirtyBufs.Add(kv.Key); }
        // The mouse sends many moves a frame: the pieces are rebuilt at most every 33 ms (and at the confirm).
        if (edApplyClock.ElapsedMilliseconds >= 33) { edApplyClock.Restart(); EdRefresh(); }
    }

    static Func<double[], double[]> EdMouseTransform()
    {
        double[] right, up; double pix;
        edView.Axes(out right, out up, out pix);
        Func<double[], double[]> f;
        if (edMode == 'G')
        {
            var d = new[] { (right[0] * edDx - up[0] * edDy) * pix, (right[1] * edDx - up[1] * edDy) * pix, (right[2] * edDx - up[2] * edDy) * pix };
            if (edAxis >= 0) { double sx = right[edAxis], sy = up[edAxis], l2 = sx * sx + sy * sy; double along = l2 > 1e-6 ? (edDx * sx - edDy * sy) * pix / Math.Sqrt(l2) : 0; d = new double[3]; d[edAxis] = along; }
            f = p => new[] { p[0] + d[0], p[1] + d[1], p[2] + d[2] };
        }
        else if (edMode == 'S')
        {
            double s = Math.Max(0.001, 1 + edDx * 0.005); var c = edPivotAt;
            f = p => { var q = (double[])p.Clone(); for (int k = 0; k < 3; k++) if (edAxis < 0 || edAxis == k) q[k] = c[k] + (p[k] - c[k]) * s; return q; };
        }
        else
        {
            double ang = edDx * 0.01; int ax = edAxis < 0 ? 1 : edAxis; var c = edPivotAt;
            f = p => Rot(p, c, ax, ang);
        }
        return f;
    }

    static void EdConfirm() { edMode = '\0'; edStart = null; edStartOthers = null; EdRefresh(); EdStatus(); }
    static void EdCancel()
    {
        var b = EdBuf();
        if (b != null && edUndoStack.Count > 0) { var top = edUndoStack[edUndoStack.Count - 1]; edModel.Restore(top); EdMarkDirty(top); edUndoStack.RemoveAt(edUndoStack.Count - 1); }
        edMode = '\0'; edStart = null; edStartOthers = null; EdRefresh();
    }

    static bool EdMouse(int dx, int dy)
    {
        if (edMode == '\0') return false;
        edDx += dx; edDy += dy;
        EdApply();
        return true;
    }

    static bool EdKey(KeyEventArgs e)
    {
        if (edModel == null) return false;
        Keys k = e.KeyCode;
        if (edMode != '\0')
        {
            if (k == Keys.Escape) { EdCancel(); return true; }
            if (k == Keys.Enter || k == Keys.Return) { EdConfirm(); return true; }
            if (k == Keys.X || k == Keys.Y || k == Keys.Z) { int a = k == Keys.X ? 0 : k == Keys.Y ? 1 : 2; edAxis = edAxis == a ? -1 : a; EdApply(); EdStatus(); return true; }
            return true;
        }
        if (e.Control && k == Keys.Z) { EdUndo(); return true; }
        if (e.Control && k == Keys.Y) { EdRedo(); return true; }
        if (e.Control && k == Keys.A) { EdSelectAll(false); return true; }
        if (e.Shift && k == Keys.D) { EdDuplicate(); return true; }
        if (k == Keys.T) { EdToggleObjectSelect(); return true; }
        // Maps fly with W/A/S/D/Q/E: those stay the camera's (scale is B there, select all Ctrl+A).
        if (edView.Fly && (k == Keys.W || k == Keys.A || k == Keys.S || k == Keys.D || k == Keys.Q || k == Keys.E)) return false;
        if (k == Keys.B) { EdBegin('S'); return true; }
        if (k == Keys.G || k == Keys.S || k == Keys.R) { EdBegin(k == Keys.G ? 'G' : k == Keys.S ? 'S' : 'R'); return true; }
        if (k == Keys.A) { EdSelectAll(false); return true; }
        if (k == Keys.F) { EdFace(); return true; }
        if (k == Keys.D1) { EdSetMode('V'); return true; }
        if (k == Keys.D2) { EdSetMode('F'); return true; }
        if (k == Keys.D3) { EdSetMode('P'); return true; }
        if (k == Keys.Delete) { EdDelete(e.Shift); return true; }
        if (k == Keys.Home) { edView.ResetCamera(); return true; }
        return false;  // W/A/S/D... the camera (fly on maps)
    }

    // ---- import / export ----
    static void EdExportObj(bool all)
    {
        if (edModel == null) return;
        using (var dlg = new SaveFileDialog { Filter = "Wavefront OBJ|*.obj", FileName = edModel.Name + (all ? "" : "_" + (edActive + 1)) + ".obj" })
        {
            if (dlg.ShowDialog(form) != DialogResult.OK) return;
            var ci = CultureInfo.InvariantCulture; var sb = new System.Text.StringBuilder();
            sb.AppendLine("# " + edPack.Key + " (NieR), NierTextureTool 3D Editor");
            int vbase = 1;
            for (int i = 0; i < edModel.Buffers.Count; i++)
            {
                if (!all && i != edActive) continue;
                var b = edModel.Buffers[i];
                sb.AppendLine("o part" + i);
                for (int v = 0; v < b.Count; v++) { var p = edModel.Pos(b, v); sb.AppendLine("v " + p[0].ToString("0.######", ci) + " " + p[1].ToString("0.######", ci) + " " + p[2].ToString("0.######", ci)); }
                for (int v = 0; v < b.Count; v++) { var t = edModel.Uv(b, v); sb.AppendLine("vt " + t[0].ToString("0.#####", ci) + " " + (1 - t[1]).ToString("0.#####", ci)); }
                var tr = edModel.Tris(b);
                for (int f = 0; f + 2 < tr.Count; f += 3) { int a = tr[f] + vbase, c = tr[f + 1] + vbase, d = tr[f + 2] + vbase; sb.AppendLine("f " + a + "/" + a + " " + c + "/" + c + " " + d + "/" + d); }
                vbase += b.Count;
            }
            File.WriteAllText(dlg.FileName, sb.ToString());
            status.Text = T("exported", dlg.FileName);
        }
    }

    // An OBJ's polygons (triangulated), each (v, vt) pair one vertex.
    static void ParseObj(string file, float scale, bool zUp, bool flip, float[] d, out float[] pos, out float[] uv, out int[] tris)
    {
        var ci = CultureInfo.InvariantCulture;
        var V = new List<float[]>(); var VT = new List<float[]>();
        var map = new Dictionary<string, int>(); var P = new List<float>(); var U = new List<float>(); var F = new List<int>();
        foreach (var raw in File.ReadLines(file))
        {
            var l = raw.Trim(); if (l.Length == 0 || l[0] == '#') continue;
            var t = l.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
            if (t[0] == "v" && t.Length >= 4) V.Add(new[] { float.Parse(t[1], ci), float.Parse(t[2], ci), float.Parse(t[3], ci) });
            else if (t[0] == "vt" && t.Length >= 3) VT.Add(new[] { float.Parse(t[1], ci), float.Parse(t[2], ci) });
            else if (t[0] == "f" && t.Length >= 4)
            {
                var idx = new List<int>();
                for (int k = 1; k < t.Length; k++)
                {
                    var s = t[k].Split('/');
                    int vi = int.Parse(s[0]); vi = vi < 0 ? V.Count + vi : vi - 1;
                    int ti = s.Length > 1 && s[1] != "" ? int.Parse(s[1]) : 0; ti = ti < 0 ? VT.Count + ti : ti - 1;
                    string key = vi + "/" + ti; int at;
                    if (!map.TryGetValue(key, out at))
                    {
                        at = P.Count / 3; map[key] = at;
                        var p = V[vi]; float x = p[0], y = p[1], z = p[2];
                        if (zUp) { float yy = z, zz = -y; y = yy; z = zz; }
                        P.Add(x * scale + d[0]); P.Add(y * scale + d[1]); P.Add(z * scale + d[2]);
                        if (ti >= 0 && ti < VT.Count) { U.Add(VT[ti][0]); U.Add(1 - VT[ti][1]); } else { U.Add(0); U.Add(0); }
                    }
                    idx.Add(at);
                }
                for (int k = 1; k + 1 < idx.Count; k++) { if (flip) { F.Add(idx[0]); F.Add(idx[k + 1]); F.Add(idx[k]); } else { F.Add(idx[0]); F.Add(idx[k]); F.Add(idx[k + 1]); } }
            }
        }
        pos = P.ToArray(); uv = U.ToArray(); tris = F.ToArray();
    }

    static void EdImportObj(float scale, bool zUp, bool flip, float[] d)
    {
        var b = EdBuf(); if (b == null) { MessageBox.Show(E("Escolha a peça de destino na aba Peças.", "Pick the target piece in the Pieces tab.", "Elige la pieza de destino en la pestaña Piezas."), form.Text); return; }
        string file;
        using (var dlg = new OpenFileDialog { Filter = "Wavefront OBJ|*.obj" }) { if (dlg.ShowDialog(form) != DialogResult.OK) return; file = dlg.FileName; }
        float[] pos, uv; int[] tris;
        ParseObj(file, scale, zUp, flip, d, out pos, out uv, out tris);
        if (tris.Length == 0) { MessageBox.Show(E("O OBJ não tem faces.", "The OBJ has no faces.", "El OBJ no tiene caras."), form.Text); return; }
        if (b.Count + pos.Length / 3 >= 0xFFFF) { MessageBox.Show(E("Vértices demais para uma peça (máximo 65534).", "Too many vertices for one piece (65534 at most).", "Demasiados vértices para una pieza (máximo 65534)."), form.Text); return; }
        EdPush();
        var added = edModel.AddGeometry(b, pos, uv, tris);
        edSelMode = 'V'; edSel = new HashSet<int>(added); edFaces.Clear();
        EdRefresh();
        edInfo.Text = added.Count + E(" vértices e ", " vertices and ", " vértices y ") + tris.Length / 3 + E(" faces importados.", " faces imported.", " caras importadas.");
    }

    static void EdOpenOther()
    {
        string key = edOtherBox.Text.Trim();
        var p = allModels.FirstOrDefault(m => string.Equals(m.Key, key, StringComparison.OrdinalIgnoreCase)) ?? allModels.FirstOrDefault(m => m.Key.IndexOf(key, StringComparison.OrdinalIgnoreCase) >= 0);
        if (p == null) { MessageBox.Show(E("Modelo não encontrado.", "Model not found.", "Modelo no encontrado."), form.Text); return; }
        form.Cursor = Cursors.WaitCursor;
        try
        {
            edOther = EditModel.Load(p.Name, p.Cpk.Read(p.V), p.Cpk.Read(p.P)); edOtherPack = p;
            edOtherParts.Items.Clear();
            foreach (var b in edOther.Buffers) if (b.Drawn) edOtherParts.Items.Add((b.Index + 1) + ". " + (b.Tex ?? "?") + "  " + edOther.Tris(b).Count / 3 + "f");
            edOtherBox.Text = p.Key;
        }
        finally { form.Cursor = Cursors.Default; }
    }

    static void EdImportOther(float scale, float[] d)
    {
        var b = EdBuf(); if (b == null || edOther == null) return;
        var drawn = edOther.Buffers.Where(x => x.Drawn).ToList();
        int total = 0;
        EdPush();
        var sel = new HashSet<int>();
        for (int i = 0; i < edOtherParts.Items.Count && i < drawn.Count; i++)
        {
            if (!edOtherParts.GetItemChecked(i)) continue;
            var o = drawn[i]; int n = o.Count;
            if (b.Count + n >= 0xFFFF) { MessageBox.Show(E("Vértices demais para uma peça (máximo 65534).", "Too many vertices for one piece (65534 at most).", "Demasiados vértices para una pieza (máximo 65534)."), form.Text); break; }
            var pos = new float[n * 3]; var uv = new float[n * 2];
            for (int v = 0; v < n; v++) { var p = edOther.Pos(o, v); var t = edOther.Uv(o, v); pos[v * 3] = p[0] * scale + d[0]; pos[v * 3 + 1] = p[1] * scale + d[1]; pos[v * 3 + 2] = p[2] * scale + d[2]; uv[v * 2] = t[0]; uv[v * 2 + 1] = t[1]; }
            foreach (int v in edModel.AddGeometry(b, pos, uv, edOther.Tris(o).ToArray())) sel.Add(v);
            total += n;
        }
        if (total == 0) { edUndoStack.RemoveAt(edUndoStack.Count - 1); return; }
        edSelMode = 'V'; edSel = sel; edFaces.Clear();
        EdRefresh();
    }

    // The edited character as the player's body (every outfit of the father), as "Play with this body" in
    // the 3D Models tab does with the disc's: the edited files are the swap's source.
    static void EdPlayAs()
    {
        if (edModel == null || edPack == null) return;
        if (!edPack.Dir.Equals("CHARA", StringComparison.OrdinalIgnoreCase)) { MessageBox.Show(T("playasno"), form.Text); return; }
        string name = "corpo-" + edPack.Name.ToLowerInvariant();
        string dir = Path.Combine(exeDir, "MODS", name);
        if (MessageBox.Show(T("playasask", name, edPack.Name) + "\n\n" + E("Usa a versão editada (o que está no editor agora).", "Uses the edited version (what is in the editor now).", "Usa la versión editada (lo que está en el editor ahora)."),
                            form.Text, MessageBoxButtons.OKCancel) != DialogResult.OK) return;
        form.Cursor = Cursors.WaitCursor;
        try
        {
            var built = edModel.Build(0, s => { });
            Program.SwapSource.Clear();
            Program.SwapSource[edPack.V.Path.ToUpperInvariant()] = built.Item1;
            Program.SwapSource[edPack.P.Path.ToUpperInvariant()] = built.Item2;
            if (Directory.Exists(dir)) Directory.Delete(dir, true);
            Directory.CreateDirectory(dir);
            var pairs = string.Join(",", kPlayerModels.Where(m => !m.Equals(edPack.Name, StringComparison.OrdinalIgnoreCase)).Select(m => m + "=" + edPack.Name));
            Program.Swap(Program.OpenCpks(mediaBox.Text), pairs, dir, "CHARA");
            form.Cursor = Cursors.Default;
            MessageBox.Show(T("playasdone", dir), form.Text);
        }
        catch (Exception e) { form.Cursor = Cursors.Default; MessageBox.Show(T("err", e.Message), form.Text); }
        finally { Program.SwapSource.Clear(); }
    }

    // ---- saving ----
    static void EdSave()
    {
        if (edModel == null || edPack == null) return;
        string name = "editado-" + edPack.Name.ToLowerInvariant();
        string dir = Path.Combine(exeDir, "MODS", name);
        if (MessageBox.Show(E("Criar o mod \"" + name + "\" na pasta MODS com este modelo editado?\n\nFeche o jogo antes. Para voltar ao normal, apague essa pasta.",
                              "Create the mod \"" + name + "\" in the MODS folder with this edited model?\n\nClose the game first. To undo, delete that folder.",
                              "¿Crear el mod \"" + name + "\" en la carpeta MODS con este modelo editado?\n\nCierra el juego antes. Para deshacerlo, borra esa carpeta."),
                            form.Text, MessageBoxButtons.OKCancel) != DialogResult.OK) return;
        try
        {
            form.Cursor = Cursors.WaitCursor;
            var log = new System.Text.StringBuilder();
            var mdp = edPack.Cpk.Read(edPack.P);
            int limit = Lzo.IsContainer(mdp) ? (int)Be.U32(mdp, 0x10) : mdp.Length;
            var built = edModel.Build(limit, s => log.AppendLine(s));
            if (Directory.Exists(dir)) Directory.Delete(dir, true);
            Directory.CreateDirectory(Path.Combine(dir, "SWAP"));
            var lines = new List<string>();
            var tail = new Dictionary<string, long>();
            string relV = "SWAP\\" + edPack.Name + "-EDIT.MDV", relP = "SWAP\\" + edPack.Name + "-EDIT.MDP";
            File.WriteAllBytes(Path.Combine(dir, relV), built.Item1);
            File.WriteAllBytes(Path.Combine(dir, relP), built.Item2);
            Program.ServeFile(edPack.Cpk, edPack.V, built.Item1.Length, built.Item1.Length, relV, lines, tail, s => log.AppendLine(s));
            Program.ServeFile(edPack.Cpk, edPack.P, built.Item2.Length, built.Item2.Length, relP, lines, tail, s => log.AppendLine(s));
            File.WriteAllLines(Path.Combine(dir, "swap.txt"), lines);
            edDirty = false;
            form.Cursor = Cursors.Default;
            EdStatus();
            MessageBox.Show(E("Mod criado em ", "Mod created in ", "Mod creado en ") + dir + (log.Length > 0 ? "\n\n" + log : ""), form.Text);
        }
        catch (Exception e) { form.Cursor = Cursors.Default; MessageBox.Show(T("err", e.Message), form.Text); }
    }
}
