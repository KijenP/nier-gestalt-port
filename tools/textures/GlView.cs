// NierTextureTool - 3D view drawn by the graphics card (OpenGL 1.1 from opengl32.dll, part of
// Windows: no extra files). Each part becomes a display list, so turning a whole map around costs
// little. Two cameras: orbit (models: drag turns around the model) and free flight (maps: drag
// looks around, W/A/S/D/Q/E move, Shift faster).
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Windows.Forms;

public class GlView : Control
{
    // ---- Win32 / OpenGL entry points ----
    [StructLayout(LayoutKind.Sequential)]
    struct PFD
    {
        public ushort nSize, nVersion; public uint dwFlags; public byte iPixelType, cColorBits, cRedBits, cRedShift, cGreenBits, cGreenShift, cBlueBits, cBlueShift, cAlphaBits, cAlphaShift, cAccumBits, cAccumRedBits, cAccumGreenBits, cAccumBlueBits, cAccumAlphaBits, cDepthBits, cStencilBits, cAuxBuffers, iLayerType, bReserved;
        public uint dwLayerMask, dwVisibleMask, dwDamageMask;
    }
    [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr h);
    [DllImport("user32.dll")] static extern int ReleaseDC(IntPtr h, IntPtr dc);
    [DllImport("gdi32.dll")] static extern int ChoosePixelFormat(IntPtr dc, ref PFD p);
    [DllImport("gdi32.dll")] static extern bool SetPixelFormat(IntPtr dc, int f, ref PFD p);
    [DllImport("gdi32.dll")] static extern bool SwapBuffers(IntPtr dc);
    [DllImport("opengl32.dll")] static extern IntPtr wglCreateContext(IntPtr dc);
    [DllImport("opengl32.dll")] static extern bool wglMakeCurrent(IntPtr dc, IntPtr rc);
    [DllImport("opengl32.dll")] static extern bool wglDeleteContext(IntPtr rc);
    [DllImport("opengl32.dll")] static extern void glViewport(int x, int y, int w, int h);
    [DllImport("opengl32.dll")] static extern void glClearColor(float r, float g, float b, float a);
    [DllImport("opengl32.dll")] static extern void glClear(uint mask);
    [DllImport("opengl32.dll")] static extern void glEnable(uint cap);
    [DllImport("opengl32.dll")] static extern void glDisable(uint cap);
    [DllImport("opengl32.dll")] static extern void glMatrixMode(uint m);
    [DllImport("opengl32.dll")] static extern void glLoadMatrixf(float[] m);
    [DllImport("opengl32.dll")] static extern void glAlphaFunc(uint f, float r);
    [DllImport("opengl32.dll")] static extern void glGenTextures(int n, uint[] t);
    [DllImport("opengl32.dll")] static extern void glDeleteTextures(int n, uint[] t);
    [DllImport("opengl32.dll")] static extern void glBindTexture(uint target, uint t);
    [DllImport("opengl32.dll")] static extern void glTexImage2D(uint target, int level, int internalFormat, int w, int h, int border, uint format, uint type, byte[] px);
    [DllImport("opengl32.dll")] static extern void glTexParameteri(uint target, uint p, int v);
    [DllImport("opengl32.dll")] static extern void glPixelStorei(uint p, int v);
    [DllImport("opengl32.dll")] static extern uint glGenLists(int n);
    [DllImport("opengl32.dll")] static extern void glNewList(uint l, uint mode);
    [DllImport("opengl32.dll")] static extern void glEndList();
    [DllImport("opengl32.dll")] static extern void glCallList(uint l);
    [DllImport("opengl32.dll")] static extern void glDeleteLists(uint l, int n);
    [DllImport("opengl32.dll")] static extern void glEnableClientState(uint a);
    [DllImport("opengl32.dll")] static extern void glDisableClientState(uint a);
    [DllImport("opengl32.dll")] static extern void glVertexPointer(int size, uint type, int stride, IntPtr p);
    [DllImport("opengl32.dll")] static extern void glTexCoordPointer(int size, uint type, int stride, IntPtr p);
    [DllImport("opengl32.dll")] static extern void glDrawElements(uint mode, int count, uint type, IntPtr idx);
    [DllImport("opengl32.dll")] static extern void glNormalPointer(uint type, int stride, IntPtr p);
    [DllImport("opengl32.dll")] static extern void glLightfv(uint light, uint p, float[] v);
    [DllImport("opengl32.dll")] static extern void glLightModeli(uint p, int v);
    [DllImport("opengl32.dll")] static extern void glLoadIdentity();
    [DllImport("opengl32.dll")] static extern void glColor4f(float r, float g, float b, float a);
    [DllImport("opengl32.dll")] static extern void glLineWidth(float w);
    [DllImport("opengl32.dll")] static extern void glBegin(uint mode);
    [DllImport("opengl32.dll")] static extern void glEnd();
    [DllImport("opengl32.dll")] static extern void glVertex3f(float x, float y, float z);
    [DllImport("opengl32.dll")] static extern void glBlendFunc(uint s, uint d);
    [DllImport("opengl32.dll")] static extern void glPolygonMode(uint face, uint mode);
    [DllImport("opengl32.dll")] static extern void glDepthMask(byte flag);
    [DllImport("opengl32.dll")] static extern void glPolygonOffset(float factor, float units);
    [DllImport("opengl32.dll")] static extern void glReadPixels(int x, int y, int w, int h, uint format, uint type, byte[] px);
    [DllImport("opengl32.dll")] static extern void glFinish();
    [DllImport("opengl32.dll")] static extern void glPointSize(float s);

    const uint GL_COLOR_BUFFER_BIT = 0x4000, GL_DEPTH_BUFFER_BIT = 0x100, GL_DEPTH_TEST = 0x0B71, GL_ALPHA_TEST = 0x0BC0, GL_GREATER = 0x0204;
    const uint GL_TEXTURE_2D = 0x0DE1, GL_RGBA = 0x1908, GL_UNSIGNED_BYTE = 0x1401, GL_FLOAT = 0x1406, GL_UNSIGNED_INT = 0x1405, GL_TRIANGLES = 4;
    const uint GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803;
    const int GL_LINEAR = 0x2601, GL_LINEAR_MIPMAP_LINEAR = 0x2703, GL_REPEAT = 0x2901;
    const uint GL_PROJECTION = 0x1701, GL_MODELVIEW = 0x1700, GL_COMPILE = 0x1300, GL_VERTEX_ARRAY = 0x8074, GL_TEXTURE_COORD_ARRAY = 0x8078, GL_NORMAL_ARRAY = 0x8075;
    const uint GL_UNPACK_ALIGNMENT = 0x0CF5, GL_PACK_ALIGNMENT = 0x0D05;

    IntPtr dc, rc;
    public bool Ready { get; private set; }

    // ---- scene ----
    class Item { public uint Tex; public bool Shade; public IntPtr V, N, T, F; public int Count; public List<IntPtr> Mem = new List<IntPtr>(); public float[] Lo = new float[3], Hi = new float[3]; public int Key = -1; public bool Hidden; }
    bool hasShade;  // the scene has a Shade's black body (a light backdrop then)
    readonly List<Item> items = new List<Item>();
    // The map's collision (C toggles it): its triangles in translucent red with their edges, over the map.
    uint collisionList;
    public bool ShowCollision = Environment.GetEnvironmentVariable("NT_COLLISION") != null;  // NT_COLLISION: on from the start (tests)
    public int CollisionTriangles { get; private set; }
    public void SetCollision(float[] tris)
    {
        if (!Ready) return;
        Current();
        if (collisionList != 0) { glDeleteLists(collisionList, 1); collisionList = 0; }
        CollisionTriangles = 0;
        if (tris == null || tris.Length < 9) { Invalidate(); return; }
        collisionList = glGenLists(1);
        glNewList(collisionList, GL_COMPILE);
        for (int pass = 0; pass < 2; pass++)
        {
            glPolygonMode(0x0408 /* GL_FRONT_AND_BACK */, pass == 0 ? 0x1B02u /* GL_FILL */ : 0x1B01u /* GL_LINE */);
            if (pass == 1) glColor4f(1f, 0.6f, 0.25f, 0.18f);
            glBegin(GL_TRIANGLES);
            for (int i = 0; i + 8 < tris.Length; i += 9)
            {
                if (pass == 0)
                {   // walls (steep faces: what stops the player) strong, floors faint
                    float ux = tris[i + 3] - tris[i], uy = tris[i + 4] - tris[i + 1], uz = tris[i + 5] - tris[i + 2];
                    float vx = tris[i + 6] - tris[i], vy = tris[i + 7] - tris[i + 1], vz = tris[i + 8] - tris[i + 2];
                    float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
                    float l = (float)Math.Sqrt(nx * nx + ny * ny + nz * nz);
                    bool wall = l > 1e-9f && Math.Abs(ny / l) < 0.5f;
                    if (wall) glColor4f(1f, 0.12f, 0.08f, 0.55f); else glColor4f(0.2f, 0.6f, 1f, 0.12f);
                }
                glVertex3f(tris[i], tris[i + 1], tris[i + 2]); glVertex3f(tris[i + 3], tris[i + 4], tris[i + 5]); glVertex3f(tris[i + 6], tris[i + 7], tris[i + 8]);
            }
            glEnd();
        }
        glPolygonMode(0x0408, 0x1B02);
        glEndList();
        CollisionTriangles = tris.Length / 9;
        Invalidate();
    }
    readonly Dictionary<string, uint> texIds = new Dictionary<string, uint>(StringComparer.OrdinalIgnoreCase);
    float[] lo = { 0, 0, 0 }, hi = { 1, 1, 1 };
    public int Triangles { get; private set; }

    // ---- camera ----
    public bool Fly;            // free flight (maps) instead of orbit (models)
    double yaw = 0.6, pitch = -0.25, dist = 3;
    double px, py, pz;          // fly: eye; orbit: the point looked at
    double extent = 1;
    Point last; MouseButtons held;
    readonly HashSet<Keys> keys = new HashSet<Keys>();
    readonly Timer tick = new Timer { Interval = 16 };
    DateTime lastTick = DateTime.Now;
    public event Action Moved;
    // ---- editor hooks (GuiEditor) ----
    public float[] Points, SelPoints;            // vertices drawn as dots (xyz), and the selected ones
    public float[] SelTris;                      // selected faces (9 floats each), drawn over the model
    // NieR (2026-11-16, the import tab): a see-through body over everything (9 floats a triangle, GhostColor) and lines
    // (6 floats each, LineColor) on top of it all: the father's body and skeleton the new model is fitted to.
    public float[] GhostTris, Lines;
    public float[] GhostColor = { 1f, 0.45f, 0.75f, 0.22f }, LineColor = { 1f, 0.3f, 0.6f, 1f };
    public bool Wire, Solid, XRay;               // wireframe over it; no textures; selection seen through the model
    public void SetView(double yawTo, double pitchTo) { yaw = yawTo; pitch = pitchTo; Invalidate(); }
    // The camera looks at the box (a selection) from where it is.
    public void Frame(float[] lo3, float[] hi3)
    {
        px = (lo3[0] + hi3[0]) / 2; py = (lo3[1] + hi3[1]) / 2; pz = (lo3[2] + hi3[2]) / 2;
        double e = Math.Max(0.02, Math.Max(hi3[0] - lo3[0], Math.Max(hi3[1] - lo3[1], hi3[2] - lo3[2])));
        if (!Fly) dist = e * 1.8;
        else { double fx = Math.Sin(yaw) * Math.Cos(pitch), fy = Math.Sin(pitch), fz = Math.Cos(yaw) * Math.Cos(pitch); px -= fx * e * 1.8; py -= fy * e * 1.8; pz -= fz * e * 1.8; }
        Invalidate();
    }
    public Action<Point> Clicked;                // a left click without dragging
    public Action<Rectangle, bool> Boxed;        // Ctrl + left drag: a rectangle (and whether Shift was held)
    public Func<KeyEventArgs, bool> KeyHook;     // true: the key is the editor's (no camera move)
    public Func<int, int, bool> MoveHook;        // mouse move (dx, dy): true while the editor uses it
    // Left-button painting (the editor's sculpt brush): DragHook(point, first) at the press (true = a stroke starts,
    // no camera) and at each move while held; StrokeEnd at the release.
    public Func<Point, bool, bool> DragHook;
    public Action StrokeEnd;
    bool painting;
    float[] lastProj, lastView; int lastW = 1, lastH = 1;
    Point downAt; bool boxing; Rectangle box;
    // A point in the window (pixels from the top left) and its depth; false behind the camera.
    public bool Project(float x, float y, float z, out float sx, out float sy, out float depth)
    {
        sx = sy = depth = 0;
        if (lastProj == null) return false;
        var e = new float[4]; var w = new[] { x, y, z, 1f };
        for (int i = 0; i < 4; i++) { float s = 0; for (int j = 0; j < 4; j++) s += lastView[j * 4 + i] * w[j]; e[i] = s; }
        var c = new float[4];
        for (int i = 0; i < 4; i++) { float s = 0; for (int j = 0; j < 4; j++) s += lastProj[j * 4 + i] * e[j]; c[i] = s; }
        if (c[3] <= 1e-6f) return false;
        sx = (c[0] / c[3] * 0.5f + 0.5f) * lastW; sy = (1 - (c[1] / c[3] * 0.5f + 0.5f)) * lastH; depth = c[3];
        return true;
    }
    // The camera's right and up directions, and the world size of one pixel at the point looked at.
    public void Axes(out double[] right, out double[] up, out double pixel)
    {
        double fx = Math.Sin(yaw) * Math.Cos(pitch), fy = Math.Sin(pitch), fz = Math.Cos(yaw) * Math.Cos(pitch);
        double rx = -fz, rz = fx; double l = Math.Sqrt(rx * rx + rz * rz); if (l < 1e-9) { rx = 1; rz = 0; l = 1; } rx /= l; rz /= l;
        right = new[] { rx, 0, rz };
        up = new[] { right[1] * fz - right[2] * fy, right[2] * fx - right[0] * fz, right[0] * fy - right[1] * fx };
        pixel = (Fly ? extent * 0.05 : dist) * 2 * Math.Tan(0.45) / Math.Max(1, ClientSize.Height);
    }

    public GlView()
    {
        SetStyle(ControlStyles.Opaque | ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.Selectable, true);
        DoubleBuffered = false;
        TabStop = true;
        tick.Tick += (s, e) => Step();
    }

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        try
        {
            dc = GetDC(Handle);
            var p = new PFD { nSize = (ushort)Marshal.SizeOf(typeof(PFD)), nVersion = 1, dwFlags = 0x4 | 0x20 | 0x1, iPixelType = 0, cColorBits = 32, cDepthBits = 24 };  // window | opengl | double buffer
            int f = ChoosePixelFormat(dc, ref p);
            if (f == 0 || !SetPixelFormat(dc, f, ref p)) return;
            rc = wglCreateContext(dc);
            if (rc == IntPtr.Zero || !wglMakeCurrent(dc, rc)) return;
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glPixelStorei(GL_PACK_ALIGNMENT, 1);
            Ready = true;
        }
        catch { Ready = false; }
    }

    protected override void OnHandleDestroyed(EventArgs e)
    {
        if (rc != IntPtr.Zero) { wglMakeCurrent(dc, rc); Clear(); if (collisionList != 0) glDeleteLists(collisionList, 1); collisionList = 0; wglMakeCurrent(IntPtr.Zero, IntPtr.Zero); wglDeleteContext(rc); rc = IntPtr.Zero; }
        if (dc != IntPtr.Zero) { ReleaseDC(Handle, dc); dc = IntPtr.Zero; }
        Ready = false;
        base.OnHandleDestroyed(e);
    }

    void Current() { if (Ready) wglMakeCurrent(dc, rc); }

    public void Clear()
    {
        if (!Ready) return;
        Current();
        foreach (var it in items) DeleteItem(it);
        items.Clear();
        if (texIds.Count > 0) { var ids = new uint[texIds.Count]; texIds.Values.CopyTo(ids, 0); glDeleteTextures(ids.Length, ids); }
        texIds.Clear();
        Triangles = 0;
    }

    uint Texture(string name, MdlFile.Img img)
    {
        uint id;
        if (texIds.TryGetValue(name, out id)) return id;
        var t = new uint[1]; glGenTextures(1, t); id = t[0];
        glBindTexture(GL_TEXTURE_2D, id);
        for (int l = 0; l < img.Levels; l++) glTexImage2D(GL_TEXTURE_2D, l, (int)GL_RGBA, img.W[l], img.H[l], 0, GL_RGBA, GL_UNSIGNED_BYTE, img.Px[l]);
        // The chain stops at 2 texels on the short side: tell GL where it ends, or the texture is incomplete.
        glTexParameteri(GL_TEXTURE_2D, 0x813D /* GL_TEXTURE_MAX_LEVEL */, img.Levels - 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, img.Levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        texIds[name] = id;
        return id;
    }

    // New scene. keepCamera: same model, only its textures changed.
    public void SetScene(List<MdlFile.MeshPart> parts, Dictionary<string, MdlFile.Img> tex, bool keepCamera)
    {
        SetScene(parts, tex, keepCamera, null, false);
    }

    // keys: an id per part (the 3D Editor's piece number), for ReplacePart / SetHidden. keepTextures: the textures
    // stay uploaded (2026-11-07: the editor re-uploaded a map's every texture at each edit, a driver crash at last).
    public void SetScene(List<MdlFile.MeshPart> parts, Dictionary<string, MdlFile.Img> tex, bool keepCamera, List<int> keys, bool keepTextures)
    {
        if (!Ready) return;
        if (keepTextures) ClearLists(); else Clear();
        Current();
        hasShade = false;
        lo = new[] { 1e30f, 1e30f, 1e30f }; hi = new[] { -1e30f, -1e30f, -1e30f };
        for (int pi = 0; pi < parts.Count; pi++)
        {
            var it = BuildItem(parts[pi], tex);
            if (it == null) continue;
            it.Key = keys != null && pi < keys.Count ? keys[pi] : -1;
            for (int a = 0; a < 3; a++) { lo[a] = Math.Min(lo[a], it.Lo[a]); hi[a] = Math.Max(hi[a], it.Hi[a]); }
            items.Add(it);
            Triangles += parts[pi].F.Length / 3;
        }
        if (items.Count == 0) { lo = new[] { -1f, -1f, -1f }; hi = new[] { 1f, 1f, 1f }; }
        if (!keepCamera) ResetCamera();
        Invalidate();
    }

    void ClearLists()
    {
        if (!Ready) return;
        Current();
        foreach (var it in items) DeleteItem(it);
        items.Clear();
        Triangles = 0;
    }

    // One part built again (the edited piece); the rest of the scene and the camera stay.
    public void ReplacePart(int key, MdlFile.MeshPart pt, Dictionary<string, MdlFile.Img> tex)
    {
        if (!Ready) return;
        Current();
        int at = items.FindIndex(i => i.Key == key);
        bool hidden = at >= 0 && items[at].Hidden;
        if (at >= 0)
        {
            DeleteItem(items[at]);
            items.RemoveAt(at);
        }
        var it = BuildItem(pt, tex);
        if (it != null) { it.Key = key; it.Hidden = hidden; if (at >= 0 && at <= items.Count) items.Insert(at, it); else items.Add(it); }
        Invalidate();
    }

    public void SetHidden(int key, bool hidden)
    {
        foreach (var it in items) if (it.Key == key) it.Hidden = hidden;
        Invalidate();
    }

    // The drawn geometry lives in memory of our own (never moved) and goes to the GPU at each draw. Display lists
    // (until 2026-11-15) now and then came out of the driver garbled: the model a tangle of stretched polygons
    // until it was picked again, while the data itself was right (its OBJ export, taken from the same window, was
    // whole). Nothing compiled is kept now, so nothing can be kept wrong.
    void DeleteItem(Item it)
    {
        foreach (var p in it.Mem) Marshal.FreeHGlobal(p);
        it.Mem.Clear();
        it.V = it.N = it.T = it.F = IntPtr.Zero;
        it.Count = 0;
    }

    static IntPtr Copy(Item it, float[] a)
    {
        var p = Marshal.AllocHGlobal(Math.Max(4, a.Length * 4)); Marshal.Copy(a, 0, p, a.Length); it.Mem.Add(p); return p;
    }
    static IntPtr Copy(Item it, int[] a)
    {
        var p = Marshal.AllocHGlobal(Math.Max(4, a.Length * 4)); Marshal.Copy(a, 0, p, a.Length); it.Mem.Add(p); return p;
    }

    Item BuildItem(MdlFile.MeshPart pt, Dictionary<string, MdlFile.Img> tex)
    {
        if (pt.F.Length == 0) return null;
        var it = new Item();
        for (int a = 0; a < 3; a++) { it.Lo[a] = 1e30f; it.Hi[a] = -1e30f; }
        for (int i = 0; i < pt.V.Length; i++) { int a = i % 3; float v = pt.V[i]; if (float.IsNaN(v) || Math.Abs(v) > 1e6f) continue; if (v < it.Lo[a]) it.Lo[a] = v; if (v > it.Hi[a]) it.Hi[a] = v; }
        MdlFile.Img img;
        if (pt.Tex != null && tex.TryGetValue(pt.Tex, out img) && img != null) it.Tex = Texture(pt.Tex, img);
        // Shades ("mamono", the game's monsters): their body texture is no file of the disc (the game's shader
        // draws them black with a glow), so a part asking for a MAMONO texture that is not there is black.
        else if (pt.Tex != null && pt.Tex.IndexOf("MAMONO", StringComparison.OrdinalIgnoreCase) >= 0) { it.Shade = true; hasShade = true; }
        it.V = Copy(it, pt.V); it.N = Copy(it, Normals(pt)); it.T = Copy(it, pt.UV); it.F = Copy(it, pt.F);
        it.Count = pt.F.Length;
        return it;
    }

    // One item: textured (or its plain colour), or for the wireframe only its triangles.
    void DrawItem(Item it, bool wire)
    {
        if (it.Count == 0) return;
        glEnableClientState(GL_VERTEX_ARRAY); glVertexPointer(3, GL_FLOAT, 0, it.V);
        if (!wire)
        {
            if (it.Tex != 0) { glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, it.Tex); glColor4f(1, 1, 1, 1); }
            else if (it.Shade) { glDisable(GL_TEXTURE_2D); glColor4f(0.07f, 0.07f, 0.08f, 1); }
            else { glDisable(GL_TEXTURE_2D); glColor4f(0.62f, 0.62f, 0.66f, 1); }
            glEnableClientState(GL_NORMAL_ARRAY); glNormalPointer(GL_FLOAT, 0, it.N);
            if (it.Tex != 0) { glEnableClientState(GL_TEXTURE_COORD_ARRAY); glTexCoordPointer(2, GL_FLOAT, 0, it.T); }
        }
        glDrawElements(GL_TRIANGLES, it.Count, GL_UNSIGNED_INT, it.F);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY); glDisableClientState(GL_NORMAL_ARRAY); glDisableClientState(GL_VERTEX_ARRAY);
    }

    // Smooth vertex normals from the triangles (the files' own normals are packed formats; this is enough to shade).
    static float[] Normals(MdlFile.MeshPart pt)
    {
        var n = new float[pt.V.Length];
        var V = pt.V; var F = pt.F;
        for (int f = 0; f + 2 < F.Length; f += 3)
        {
            int a = F[f] * 3, b = F[f + 1] * 3, c = F[f + 2] * 3;
            if (a + 2 >= V.Length || b + 2 >= V.Length || c + 2 >= V.Length) continue;
            float ux = V[b] - V[a], uy = V[b + 1] - V[a + 1], uz = V[b + 2] - V[a + 2];
            float vx = V[c] - V[a], vy = V[c + 1] - V[a + 1], vz = V[c + 2] - V[a + 2];
            float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
            foreach (int k in new[] { a, b, c }) { n[k] += nx; n[k + 1] += ny; n[k + 2] += nz; }
        }
        for (int i = 0; i + 2 < n.Length; i += 3)
        {
            float l = (float)Math.Sqrt(n[i] * n[i] + n[i + 1] * n[i + 1] + n[i + 2] * n[i + 2]);
            if (l > 1e-12f) { n[i] /= l; n[i + 1] /= l; n[i + 2] /= l; } else n[i + 1] = 1;
        }
        return n;
    }

    public void ResetCamera()
    {
        extent = Math.Max(0.01, Math.Max(hi[0] - lo[0], Math.Max(hi[1] - lo[1], hi[2] - lo[2])));
        double cx = (lo[0] + hi[0]) / 2, cy = (lo[1] + hi[1]) / 2, cz = (lo[2] + hi[2]) / 2;
        yaw = 0.6; pitch = -0.25;
        if (Fly)
        {   // above one end of the map, looking across it
            pitch = -0.35; yaw = 0.8;
            px = cx - Math.Sin(yaw) * extent * 0.35; pz = cz - Math.Cos(yaw) * extent * 0.35; py = cy + extent * 0.15;
        }
        else { yaw = Math.PI - 0.5; px = cx; py = cy; pz = cz; dist = extent * 1.6; }  // models face +Z: start in front
        Invalidate();
    }

    // ---- input ----
    protected override bool IsInputKey(Keys k) { return true; }
    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e); Focus(); last = e.Location; held = e.Button; downAt = e.Location;
        boxing = Boxed != null && e.Button == MouseButtons.Left && (ModifierKeys & Keys.Control) != 0; box = Rectangle.Empty;
        painting = !boxing && e.Button == MouseButtons.Left && DragHook != null && DragHook(e.Location, true);
    }
    protected override void OnMouseUp(MouseEventArgs e)
    {
        base.OnMouseUp(e);
        if (painting) { painting = false; held = MouseButtons.None; if (StrokeEnd != null) StrokeEnd(); return; }
        var was = held; held = MouseButtons.None;
        if (boxing) { boxing = false; var r = box; box = Rectangle.Empty; Invalidate(); if (r.Width > 2 && r.Height > 2 && Boxed != null) Boxed(r, (ModifierKeys & Keys.Shift) != 0); return; }
        if (was == MouseButtons.Left && Clicked != null && Math.Abs(e.X - downAt.X) + Math.Abs(e.Y - downAt.Y) < 5) Clicked(e.Location);
    }
    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        int dx = e.X - last.X, dy = e.Y - last.Y;
        if (painting) { last = e.Location; DragHook(e.Location, false); return; }
        if (MoveHook != null && MoveHook(dx, dy)) { last = e.Location; return; }
        if (boxing) { box = Rectangle.FromLTRB(Math.Min(downAt.X, e.X), Math.Min(downAt.Y, e.Y), Math.Max(downAt.X, e.X), Math.Max(downAt.Y, e.Y)); Invalidate(); return; }
        if (held == MouseButtons.None) { last = e.Location; return; }
        last = e.Location;
        if (held == MouseButtons.Left) { yaw -= dx * 0.008; pitch = Math.Max(-1.5, Math.Min(1.5, pitch - dy * 0.008)); }
        else
        {   // right / middle: slide sideways and up/down
            double s = (Fly ? extent * 0.0015 : dist * 0.0015);
            double rx = Math.Cos(yaw), rz = -Math.Sin(yaw);
            px += rx * dx * s; pz += rz * dx * s; py += dy * s;
        }
        Invalidate(); if (Moved != null) Moved();
    }
    protected override void OnMouseWheel(MouseEventArgs e)
    {
        base.OnMouseWheel(e);
        if (Fly) { double f = Math.Cos(pitch), step = extent * 0.03 * Math.Sign(e.Delta); px += Math.Sin(yaw) * f * step; pz += Math.Cos(yaw) * f * step; py += Math.Sin(pitch) * step; }
        else dist = Math.Max(extent * 0.05, Math.Min(extent * 20, dist * (e.Delta > 0 ? 0.88 : 1.14)));
        Invalidate(); if (Moved != null) Moved();
    }
    protected override void OnKeyDown(KeyEventArgs e) { base.OnKeyDown(e); if (KeyHook != null && KeyHook(e)) { Invalidate(); return; } keys.Add(e.KeyCode); if (e.KeyCode == Keys.R) ResetCamera(); if (e.KeyCode == Keys.C) { ShowCollision = !ShowCollision; Invalidate(); } if (!tick.Enabled) { lastTick = DateTime.Now; tick.Start(); } }
    protected override void OnKeyUp(KeyEventArgs e) { base.OnKeyUp(e); keys.Remove(e.KeyCode); }
    protected override void OnLostFocus(EventArgs e) { base.OnLostFocus(e); keys.Clear(); }

    void Step()
    {
        double dt = Math.Min(0.1, (DateTime.Now - lastTick).TotalSeconds); lastTick = DateTime.Now;
        if (keys.Count == 0) { tick.Stop(); return; }
        double speed = extent * (keys.Contains(Keys.ShiftKey) ? 0.6 : 0.15) * dt;
        double fx = Math.Sin(yaw) * Math.Cos(pitch), fy = Math.Sin(pitch), fz = Math.Cos(yaw) * Math.Cos(pitch);
        double rx = Math.Cos(yaw), rz = -Math.Sin(yaw);
        double mx = 0, my = 0, mz = 0;
        if (keys.Contains(Keys.W)) { mx += fx; my += fy; mz += fz; }
        if (keys.Contains(Keys.S)) { mx -= fx; my -= fy; mz -= fz; }
        if (keys.Contains(Keys.D)) { mx -= rx; mz -= rz; }
        if (keys.Contains(Keys.A)) { mx += rx; mz += rz; }
        if (keys.Contains(Keys.E) || keys.Contains(Keys.Space)) my += 1;
        // Ctrl descends only in the viewer: in the 3D Editor (Boxed set) it is the rectangle's key.
        if (keys.Contains(Keys.Q) || (Boxed == null && keys.Contains(Keys.ControlKey))) my -= 1;
        if (mx == 0 && my == 0 && mz == 0) return;
        if (!Fly) speed *= 0.5;
        px += mx * speed; py += my * speed; pz += mz * speed;
        Invalidate(); if (Moved != null) Moved();
    }

    // ---- drawing ----
    static float[] Perspective(double fovy, double aspect, double near, double far)
    {
        double f = 1 / Math.Tan(fovy / 2);
        return new[] { (float)(f / aspect), 0, 0, 0, 0, (float)f, 0, 0, 0, 0, (float)((far + near) / (near - far)), -1, 0, 0, (float)(2 * far * near / (near - far)), 0 };
    }

    static float[] LookAt(double ex, double ey, double ez, double fx, double fy, double fz)
    {
        double l = Math.Sqrt(fx * fx + fy * fy + fz * fz); fx /= l; fy /= l; fz /= l;
        double sx = fy * 0 - fz * 1, sy = fz * 0 - fx * 0, sz = fx * 1 - fy * 0;  // f x up(0,1,0)
        l = Math.Sqrt(sx * sx + sy * sy + sz * sz); if (l < 1e-9) { sx = 1; sy = 0; sz = 0; l = 1; } sx /= l; sy /= l; sz /= l;
        double ux = sy * fz - sz * fy, uy = sz * fx - sx * fz, uz = sx * fy - sy * fx;
        return new[] {
            (float)sx, (float)ux, (float)-fx, 0,
            (float)sy, (float)uy, (float)-fy, 0,
            (float)sz, (float)uz, (float)-fz, 0,
            (float)-(sx * ex + sy * ey + sz * ez), (float)-(ux * ex + uy * ey + uz * ez), (float)(fx * ex + fy * ey + fz * ez), 1 };
    }

    void Draw(int w, int h)
    {
        glViewport(0, 0, w, h);
        if (hasShade) glClearColor(0.62f, 0.62f, 0.60f, 1);  // a black Shade on the dark backdrop vanished
        else glClearColor(0.12f, 0.12f, 0.14f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_ALPHA_TEST); glAlphaFunc(GL_GREATER, 0.25f);  // cut-outs: leaves, hair, fences
        // One light from above and behind the viewer's left shoulder, set in eye space, lighting both sides.
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        glEnable(0x0B50 /* GL_LIGHTING */); glEnable(0x4000 /* GL_LIGHT0 */); glEnable(0x0B57 /* GL_COLOR_MATERIAL */); glEnable(0x0BA1 /* GL_NORMALIZE */);
        glLightModeli(0x0B52 /* GL_LIGHT_MODEL_TWO_SIDE */, 1);
        glLightfv(0x4000, 0x1203 /* GL_POSITION */, new[] { -0.4f, 0.8f, 0.6f, 0f });
        glLightfv(0x4000, 0x1200 /* GL_AMBIENT */, new[] { 0.42f, 0.42f, 0.44f, 1f });
        glLightfv(0x4000, 0x1201 /* GL_DIFFUSE */, new[] { 0.7f, 0.7f, 0.68f, 1f });
        double fx = Math.Sin(yaw) * Math.Cos(pitch), fy = Math.Sin(pitch), fz = Math.Cos(yaw) * Math.Cos(pitch);
        double ex = px, ey = py, ez = pz;
        if (!Fly) { ex = px - fx * dist; ey = py - fy * dist; ez = pz - fz * dist; }
        double near = Math.Max(extent * (Fly ? 0.0005 : 0.002), 0.001), far = extent * 8 + (Fly ? 0 : dist);
        glMatrixMode(GL_PROJECTION); glLoadMatrixf(Perspective(0.9, Math.Max(1, w) / (double)Math.Max(1, h), near, far));
        glMatrixMode(GL_MODELVIEW); glLoadMatrixf(LookAt(ex, ey, ez, fx, fy, fz));
        foreach (var it in items) if (!it.Hidden) DrawItem(it, false);
        if (Wire)
        {   // the edges over the surface
            glDisable(0x0B50); glDisable(GL_TEXTURE_2D); glDisable(GL_ALPHA_TEST);
            glEnable(0x8038 /* GL_POLYGON_OFFSET_LINE */); glPolygonOffset(-1, -1);
            glPolygonMode(0x0408, 0x1B01); glColor4f(0.1f, 0.1f, 0.12f, 1);
            foreach (var it in items) if (!it.Hidden) DrawItem(it, true);
            glPolygonMode(0x0408, 0x1B02); glDisable(0x8038);
            glEnable(0x0B50); glEnable(GL_ALPHA_TEST);
        }
        if (SelTris != null && SelTris.Length >= 9)
        {
            glDisable(0x0B50); glDisable(GL_TEXTURE_2D); glDisable(GL_ALPHA_TEST);
            glEnable(0x0BE2); glBlendFunc(0x0302, 0x0303);
            glEnable(0x8037); glPolygonOffset(-1, -2); glDepthMask(0);
            if (XRay) glDisable(GL_DEPTH_TEST);
            glColor4f(1f, 0.5f, 0.1f, 0.45f);
            glBegin(GL_TRIANGLES); for (int i = 0; i + 8 < SelTris.Length; i += 9) { glVertex3f(SelTris[i], SelTris[i + 1], SelTris[i + 2]); glVertex3f(SelTris[i + 3], SelTris[i + 4], SelTris[i + 5]); glVertex3f(SelTris[i + 6], SelTris[i + 7], SelTris[i + 8]); } glEnd();
            glEnable(GL_DEPTH_TEST); glDepthMask(1); glDisable(0x8037); glDisable(0x0BE2);
        }
        if (GhostTris != null && GhostTris.Length >= 9)
        {
            glDisable(0x0B50); glDisable(GL_TEXTURE_2D); glDisable(GL_ALPHA_TEST);
            glEnable(0x0BE2); glBlendFunc(0x0302, 0x0303); glDepthMask(0); glDisable(GL_DEPTH_TEST);
            glColor4f(GhostColor[0], GhostColor[1], GhostColor[2], GhostColor[3]);
            glBegin(GL_TRIANGLES); for (int i = 0; i + 8 < GhostTris.Length; i += 9) { glVertex3f(GhostTris[i], GhostTris[i + 1], GhostTris[i + 2]); glVertex3f(GhostTris[i + 3], GhostTris[i + 4], GhostTris[i + 5]); glVertex3f(GhostTris[i + 6], GhostTris[i + 7], GhostTris[i + 8]); } glEnd();
            glEnable(GL_DEPTH_TEST); glDepthMask(1); glDisable(0x0BE2);
        }
        if (Lines != null && Lines.Length >= 6)
        {
            glDisable(0x0B50); glDisable(GL_TEXTURE_2D); glDisable(GL_ALPHA_TEST); glDisable(GL_DEPTH_TEST);
            glLineWidth(2.5f); glColor4f(LineColor[0], LineColor[1], LineColor[2], LineColor[3]);
            glBegin(1 /* GL_LINES */); for (int i = 0; i + 5 < Lines.Length; i += 6) { glVertex3f(Lines[i], Lines[i + 1], Lines[i + 2]); glVertex3f(Lines[i + 3], Lines[i + 4], Lines[i + 5]); } glEnd();
            glLineWidth(1); glEnable(GL_DEPTH_TEST);
        }
        lastProj = Perspective(0.9, Math.Max(1, w) / (double)Math.Max(1, h), near, far); lastView = LookAt(ex, ey, ez, fx, fy, fz); lastW = w; lastH = h;
        if (Points != null || SelPoints != null || !box.IsEmpty)
        {
            glDisable(0x0B50 /* GL_LIGHTING */); glDisable(GL_TEXTURE_2D); glDisable(GL_ALPHA_TEST);
            if (XRay) glDisable(GL_DEPTH_TEST);
            if (Points != null) { glPointSize(3); glColor4f(0.2f, 0.85f, 1f, 1f); glBegin(0 /* GL_POINTS */); for (int i = 0; i + 2 < Points.Length; i += 3) glVertex3f(Points[i], Points[i + 1], Points[i + 2]); glEnd(); }
            glDisable(GL_DEPTH_TEST);
            if (SelPoints != null) { glPointSize(6); glColor4f(1f, 0.55f, 0.1f, 1f); glBegin(0); for (int i = 0; i + 2 < SelPoints.Length; i += 3) glVertex3f(SelPoints[i], SelPoints[i + 1], SelPoints[i + 2]); glEnd(); }
            if (!box.IsEmpty)
            {   // the selection rectangle, in window pixels
                glMatrixMode(GL_PROJECTION); glLoadMatrixf(new float[] { 2f / w, 0, 0, 0, 0, -2f / h, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1 });
                glMatrixMode(GL_MODELVIEW); glLoadIdentity();
                glColor4f(1f, 0.8f, 0.2f, 1f);
                glBegin(2 /* GL_LINE_LOOP */); glVertex3f(box.Left, box.Top, 0); glVertex3f(box.Right, box.Top, 0); glVertex3f(box.Right, box.Bottom, 0); glVertex3f(box.Left, box.Bottom, 0); glEnd();
            }
            glEnable(GL_DEPTH_TEST);
        }
        if (ShowCollision && collisionList != 0)
        {   // over the map, seen through it a little (depth test on, no depth writes, pulled forward)
            glDisable(0x0B50 /* GL_LIGHTING */); glDisable(GL_TEXTURE_2D); glDisable(GL_ALPHA_TEST);
            glEnable(0x0BE2 /* GL_BLEND */); glBlendFunc(0x0302 /* GL_SRC_ALPHA */, 0x0303 /* GL_ONE_MINUS_SRC_ALPHA */);
            glEnable(0x8037 /* GL_POLYGON_OFFSET_FILL */); glPolygonOffset(-1, -2);
            glDepthMask(0);
            glCallList(collisionList);
            glDepthMask(1);
            glDisable(0x8037); glDisable(0x0BE2);
        }
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        if (!Ready) { e.Graphics.Clear(Color.FromArgb(30, 30, 34)); TextRenderer.DrawText(e.Graphics, "OpenGL indisponível", Font, new Point(10, 10), Color.Silver); return; }
        Current();
        Draw(ClientSize.Width, ClientSize.Height);
        SwapBuffers(dc);
    }

    protected override void OnResize(EventArgs e) { base.OnResize(e); Invalidate(); }

    // The current view as an image (for --gui-test and "save picture").
    public Bitmap Snapshot()
    {
        int w = Math.Max(1, ClientSize.Width), h = Math.Max(1, ClientSize.Height);
        var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb);
        if (!Ready) return bmp;
        Current();
        Draw(w, h); glFinish();
        var px = new byte[w * h * 4];
        glReadPixels(0, 0, w, h, 0x80E1 /* GL_BGRA */, GL_UNSIGNED_BYTE, px);
        var data = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
        for (int y = 0; y < h; y++) Marshal.Copy(px, (h - 1 - y) * w * 4, data.Scan0 + y * data.Stride, w * 4);
        bmp.UnlockBits(data);
        return bmp;
    }
}