// NieR model editor core: a model (.MDV header + .MDP data, unpacked) as editable vertex buffers and
// triangle lists, written back with any number of vertices and triangles.
//
// In the .MDV's HEAP each draw is a VXST record whose 0x40-byte header names, by header offset, its
// VXBO (bounds: sphere centre + radius first), IXBF (index buffer: 16-bit triangle strips, 0xFFFF
// restarts; header +0x1C = byte size), VXAR (vertex layout) and VXBF (vertex buffer; header +0x1C =
// the vertex fetch constant's size word: dwords << 2 | endian). VXST +0x10 = index count, +0x08 =
// primitive (6, strip). Buffers sit at the record's C (offset in the .MDP) with D bytes; several
// draws (the shadow variant) share them. Textures (TX2D) end the data; their mip addresses are
// relative to the texture, so they move freely.
//
// Writing: each index buffer becomes one strip per triangle (a, b, c, 0xFFFF: the first triangle of
// a strip keeps its winding), the buffers are laid out again one after the other, the textures after
// them, and every record, header and count follows. Positions are the first 3 big-endian floats of a
// vertex; the other attributes (normal, UV, bone weights and numbers) are copied with the vertex.
using System;
using System.Collections.Generic;
using System.Linq;

public class EditModel
{
    public class VBuf
    {
        public int Hdr;           // header offset (in the heap's header area) of its first record
        public List<int> Hdrs = new List<int>();  // every VXBF record on these bytes (the shadow draw's too)
        public int C, Stride;
        public byte[] Data;       // Count * Stride bytes
        public int Count { get { return Data.Length / Stride; } }
        public int UvOffset = -1; // half2 texture coordinate, when the layout has one
        public List<IBuf> Users = new List<IBuf>();
        public string Tex;        // the colour texture of its first drawn (not shadow) use
        public bool Drawn;        // used by a draw of the default variant
        public int Index;         // number in Buffers
        public List<MdlFile.VAttr> Layout; public string Sig;  // its vertex layout (VXAR), and as text to compare buffers
        public int Mat = -1;      // material of its first draw (MATE number)
    }
    public class IBuf
    {
        public int Hdr, C;
        public List<int> Hdrs = new List<int>();  // every IXBF record on these bytes
        public List<int> Orig; public byte[] OrigBytes;  // as read: written back as they were when unchanged
        public List<int> Tris = new List<int>();  // triangle list (vertex numbers in its VBuf)
        public VBuf V;
        public List<int> Streams = new List<int>(); // VXST header positions (absolute, in Mdv)
    }

    public byte[] Mdv, Pix;
    public string Name;
    public List<VBuf> Buffers = new List<VBuf>();
    public List<IBuf> Indices = new List<IBuf>();
    MdlFile md;
    Heap heap;

    static uint U(byte[] d, int o) { return Be.U32(d, o); }

    public static EditModel Load(string name, byte[] mdv, byte[] mdp)
    {
        var m = new EditModel { Name = name, Mdv = (byte[])mdv.Clone() };
        m.Pix = Lzo.IsContainer(mdp) ? Lzo.Unpack(mdp) : (byte[])mdp.Clone();
        m.md = MdlFile.Load(name, m.Mdv, m.Pix);
        if (m.md.Heaps.Count == 0) throw new Exception("modelo sem HEAP");
        m.heap = m.md.Heaps[0];
        var h = m.heap;
        var byOff = new Dictionary<uint, HeapRec>();
        foreach (var r in h.Recs) if (!byOff.ContainsKey(r.Off)) byOff[r.Off] = r;
        var vbs = new Dictionary<uint, VBuf>();  // by data offset: the draws (and the shadow draw) share it
        var ibs = new Dictionary<uint, IBuf>();
        var mats = m.md.MaterialTextures();
        var info = m.md.PartInfo();
        // The draws in the order the model lists them (the order Parts() and PartInfo() number them).
        var streams = h.Recs.Where(r => r.Tag == "VXST").ToList();
        var ixs = h.Recs.Where(r => r.Tag == "IXBF").ToList();
        var vas = h.Recs.Where(r => r.Tag == "VXAR").ToList();
        for (int k = 0; k < streams.Count; k++)
        {
            int s = h.HeaderBase + (int)streams[k].Off;
            uint ixOff = U(m.Mdv, s + 0x18), vaOff = U(m.Mdv, s + 0x1C), vbOff = U(m.Mdv, s + 0x2C);
            HeapRec ir, vr, ar;
            if (!byOff.TryGetValue(ixOff, out ir) || ir.Tag != "IXBF" || !byOff.TryGetValue(vbOff, out vr) || vr.Tag != "VXBF") continue;
            byOff.TryGetValue(vaOff, out ar);
            VBuf vb;
            if (!vbs.TryGetValue(vr.C, out vb))
            {
                vb = new VBuf { Hdr = (int)vbOff, C = (int)vr.C };
                vbs[vr.C] = vb;
            }
            if (!vb.Hdrs.Contains((int)vbOff)) vb.Hdrs.Add((int)vbOff);
            IBuf ib;
            if (!ibs.TryGetValue(ir.C, out ib))
            {
                ib = new IBuf { Hdr = (int)ixOff, C = (int)ir.C, V = vb };
                int count = (int)ir.D / 2, at = (int)ir.C;
                var idx = new int[count];
                for (int i = 0; i < count; i++) idx[i] = Be.U16(m.Pix, at + i * 2);
                int ra = -1, rb = -1, n = 0;
                foreach (int i in idx)
                {
                    if (i == 0xFFFF) { n = 0; continue; }
                    if (n >= 2 && ra != rb && rb != i && ra != i)
                    {
                        if ((n & 1) == 0) { ib.Tris.Add(ra); ib.Tris.Add(rb); ib.Tris.Add(i); }
                        else { ib.Tris.Add(rb); ib.Tris.Add(ra); ib.Tris.Add(i); }
                    }
                    ra = rb; rb = i; n++;
                }
                ib.Orig = new List<int>(ib.Tris);
                ib.OrigBytes = new byte[count * 2]; Array.Copy(m.Pix, at, ib.OrigBytes, 0, ib.OrigBytes.Length);
                ibs[ir.C] = ib;
                vb.Users.Add(ib);
            }
            ib.Streams.Add(s);
            if (!ib.Hdrs.Contains((int)ixOff)) ib.Hdrs.Add((int)ixOff);
            if (ar != null && vb.UvOffset < 0)
            {
                var uv = m.md.Layout(h, ar).FirstOrDefault(a => a.Usage == 5 && a.Index == 0 && (a.Format == 31 || a.Format == 32));
                if (uv != null) vb.UvOffset = uv.Offset;
            }
            if (ar != null && vb.Layout == null)
            {
                vb.Layout = m.md.Layout(h, ar);
                vb.Sig = string.Join(";", vb.Layout.Select(a => a.Usage + "." + a.Index + "@" + a.Offset + "f" + a.Format));
            }
            // Variant 0 (drawn) vs shadow: PartInfo numbers the draws as the IXBF records come.
            int gk = ixs.IndexOf(ir);
            if (vb.Mat < 0 && gk >= 0 && gk < info.Count) vb.Mat = info[gk].Item1;
            bool shadow = gk >= 0 && gk < info.Count && info[gk].Item2 != 0;
            if (!shadow)
            {
                vb.Drawn = true;
                if (vb.Tex == null && gk >= 0 && gk < info.Count && info[gk].Item1 >= 0 && info[gk].Item1 < mats.Count && mats[info[gk].Item1].Count > 0)
                    vb.Tex = MdlFile.ColorTexture(mats[info[gk].Item1]);
            }
        }
        // Vertex count: the highest number any of its index buffers uses, + 1 (the buffer may be padded).
        foreach (var vb in vbs.Values)
        {
            int bytes = vb.Hdrs.Max(x => (int)byOff[(uint)x].D);
            int max = 0;
            foreach (var ib in vb.Users) foreach (int i in ib.Tris) if (i > max) max = i;
            int count = max + 1;
            vb.Stride = Math.Max(12, bytes / Math.Max(1, count));
            vb.Data = new byte[count * vb.Stride];
            Array.Copy(m.Pix, vb.C, vb.Data, 0, Math.Min(vb.Data.Length, m.Pix.Length - vb.C));
        }
        m.Buffers = vbs.Values.OrderBy(v => v.C).ToList();
        for (int i = 0; i < m.Buffers.Count; i++) m.Buffers[i].Index = i;
        m.Indices = ibs.Values.OrderBy(v => v.C).ToList();
        return m;
    }

    // ---- vertices ----
    static float F(byte[] d, int o) { return BitConverter.ToSingle(new[] { d[o + 3], d[o + 2], d[o + 1], d[o] }, 0); }
    static void PutF(byte[] d, int o, float v) { var b = BitConverter.GetBytes(v); d[o] = b[3]; d[o + 1] = b[2]; d[o + 2] = b[1]; d[o + 3] = b[0]; }
    static float Half(int h)
    {
        int s = (h >> 15) & 1, e = (h >> 10) & 0x1F, mm = h & 0x3FF;
        double v = e == 0 ? mm / 1024.0 * Math.Pow(2, -14) : e == 31 ? 0 : (1 + mm / 1024.0) * Math.Pow(2, e - 15);
        return (float)(s == 1 ? -v : v);
    }
    public float[] Pos(VBuf b, int v) { int o = v * b.Stride; return new[] { F(b.Data, o), F(b.Data, o + 4), F(b.Data, o + 8) }; }
    public void SetPos(VBuf b, int v, float x, float y, float z) { int o = v * b.Stride; PutF(b.Data, o, x); PutF(b.Data, o + 4, y); PutF(b.Data, o + 8, z); }
    public float[] Uv(VBuf b, int v)
    {
        if (b.UvOffset < 0 || b.UvOffset + 4 > b.Stride) return new float[] { 0, 0 };
        int o = v * b.Stride + b.UvOffset;
        return new[] { Half(Be.U16(b.Data, o)), Half(Be.U16(b.Data, o + 2)) };
    }

    // Triangles drawn with the buffer (its first index buffer: the others are the same polygons).
    public List<int> Tris(VBuf b) { return b.Users.Count > 0 ? b.Users[0].Tris : new List<int>(); }

    // The buffer as a part for the viewer.
    public MdlFile.MeshPart Part(VBuf b)
    {
        int n = b.Count;
        var pt = new MdlFile.MeshPart { Tex = b.Tex, N = n, V = new float[n * 3], UV = new float[n * 2], VOff = b.C, Stride = b.Stride };
        for (int v = 0; v < n; v++)
        {
            var p = Pos(b, v); pt.V[v * 3] = p[0]; pt.V[v * 3 + 1] = p[1]; pt.V[v * 3 + 2] = p[2];
            var t = Uv(b, v); pt.UV[v * 2] = t[0]; pt.UV[v * 2 + 1] = t[1];
        }
        pt.F = Tris(b).ToArray();
        return pt;
    }

    // ---- edits (every index buffer of the vertex buffer follows) ----

    // Removes the vertices (and every triangle using one of them).
    public void DeleteVertices(VBuf b, HashSet<int> gone)
    {
        if (gone.Count == 0) return;
        var map = new int[b.Count]; int k = 0;
        for (int v = 0; v < b.Count; v++) map[v] = gone.Contains(v) ? -1 : k++;
        var data = new byte[k * b.Stride];
        for (int v = 0; v < b.Count; v++) if (map[v] >= 0) Array.Copy(b.Data, v * b.Stride, data, map[v] * b.Stride, b.Stride);
        b.Data = data;
        foreach (var ib in b.Users)
        {
            var t = new List<int>(ib.Tris.Count);
            for (int i = 0; i + 2 < ib.Tris.Count; i += 3)
            {
                int a = map[ib.Tris[i]], c = map[ib.Tris[i + 1]], d = map[ib.Tris[i + 2]];
                if (a < 0 || c < 0 || d < 0) continue;
                t.Add(a); t.Add(c); t.Add(d);
            }
            ib.Tris = t;
        }
    }

    // Copies the vertices (all their attributes) and the triangles lying wholly among them; returns the copies.
    public List<int> Duplicate(VBuf b, HashSet<int> sel)
    {
        var order = sel.OrderBy(x => x).ToList();
        var map = new Dictionary<int, int>();
        int start = b.Count;
        var data = new byte[(start + order.Count) * b.Stride];
        Array.Copy(b.Data, data, b.Data.Length);
        for (int i = 0; i < order.Count; i++) { Array.Copy(b.Data, order[i] * b.Stride, data, (start + i) * b.Stride, b.Stride); map[order[i]] = start + i; }
        b.Data = data;
        foreach (var ib in b.Users)
        {
            int n = ib.Tris.Count;
            for (int i = 0; i + 2 < n; i += 3)
            {
                int a, c, d;
                if (map.TryGetValue(ib.Tris[i], out a) && map.TryGetValue(ib.Tris[i + 1], out c) && map.TryGetValue(ib.Tris[i + 2], out d))
                { ib.Tris.Add(a); ib.Tris.Add(c); ib.Tris.Add(d); }
            }
        }
        return map.Values.OrderBy(x => x).ToList();
    }

    // A new triangle between three vertices (both sides drawn? the game culls back faces: one side, in
    // the order given).
    public void AddTriangle(VBuf b, int a, int c, int d)
    {
        foreach (var ib in b.Users) { ib.Tris.Add(a); ib.Tris.Add(c); ib.Tris.Add(d); }
    }

    // Removes the triangles whose three vertices are all selected (the vertices stay).
    public int DeleteFaces(VBuf b, HashSet<int> sel)
    {
        int n = 0;
        foreach (var ib in b.Users)
        {
            var t = new List<int>(ib.Tris.Count);
            for (int i = 0; i + 2 < ib.Tris.Count; i += 3)
            {
                if (sel.Contains(ib.Tris[i]) && sel.Contains(ib.Tris[i + 1]) && sel.Contains(ib.Tris[i + 2])) { if (ib == b.Users[0]) n++; continue; }
                t.Add(ib.Tris[i]); t.Add(ib.Tris[i + 1]); t.Add(ib.Tris[i + 2]);
            }
            ib.Tris = t;
        }
        return n;
    }

    // ---- more mesh operations (as the BT3 editor's): faces are numbers in Tris(b) (triangle f = 3f..3f+2) ----

    // After changing the first index buffer: the others that drew the same triangles follow.
    void Sync(VBuf b, List<List<int>> before)
    {
        for (int u = 1; u < b.Users.Count; u++)
            if (before != null && u < before.Count && before[u].SequenceEqual(before[0])) b.Users[u].Tris = new List<int>(b.Users[0].Tris);
    }
    List<List<int>> Before(VBuf b) { return b.Users.Select(u => new List<int>(u.Tris)).ToList(); }

    // Polygon reduction (the BT3 map editor's local LOD; 2026-11-07): the vertices of the faces are gathered in cells of
    // 'cell' units, each cell's vertices merged into one at their average (its UV the one nearest that average, so a
    // texture seam keeps its sides apart: a cell holds one merged vertex per UV island it touches); faces that collapse go.
    // Vertices left without faces are removed. Returns the number of faces before and after.
    public Tuple<int, int> Decimate(VBuf b, HashSet<int> faces, float cell)
    {
        var before = Before(b);
        var t = Tris(b); int n0 = t.Count / 3;
        if (cell <= 0 || faces.Count == 0) return Tuple.Create(n0, n0);
        var verts = VertsOfFaces(b, faces);
        // Cell key + a coarse UV key (a quarter of the texture), so islands far apart in the texture do not merge.
        var groups = new Dictionary<string, List<int>>();
        foreach (int v in verts)
        {
            var p = Pos(b, v); var uv = Uv(b, v);
            string k = Math.Floor(p[0] / cell) + "," + Math.Floor(p[1] / cell) + "," + Math.Floor(p[2] / cell) + "|" + Math.Floor(uv[0] * 4) + "," + Math.Floor(uv[1] * 4);
            List<int> l; if (!groups.TryGetValue(k, out l)) groups[k] = l = new List<int>(); l.Add(v);
        }
        var to = new Dictionary<int, int>();
        foreach (var g in groups.Values)
        {
            float x = 0, y = 0, z = 0;
            foreach (int v in g) { var p = Pos(b, v); x += p[0]; y += p[1]; z += p[2]; }
            x /= g.Count; y /= g.Count; z /= g.Count;
            int keep = g[0]; float best = float.MaxValue;
            foreach (int v in g) { var p = Pos(b, v); float d = (p[0] - x) * (p[0] - x) + (p[1] - y) * (p[1] - y) + (p[2] - z) * (p[2] - z); if (d < best) { best = d; keep = v; } }
            SetPos(b, keep, x, y, z);
            foreach (int v in g) to[v] = keep;
        }
        var nt = new List<int>(t.Count);
        for (int f = 0; f * 3 + 2 < t.Count; f++)
        {
            int a = t[f * 3], c = t[f * 3 + 1], d = t[f * 3 + 2];
            if (faces.Contains(f)) { int m; if (to.TryGetValue(a, out m)) a = m; if (to.TryGetValue(c, out m)) c = m; if (to.TryGetValue(d, out m)) d = m; if (a == c || c == d || a == d) continue; }
            nt.Add(a); nt.Add(c); nt.Add(d);
        }
        // Vertices of other faces may have moved to their cell's average only if they were merged: the faces outside
        // the selection that used a merged-away vertex use its keeper too (no holes at the border).
        for (int i = 0; i < nt.Count; i++) { int m; if (to.TryGetValue(nt[i], out m)) nt[i] = m; }
        b.Users[0].Tris = nt;
        Sync(b, before);
        var used = new HashSet<int>(nt);
        DeleteVertices(b, new HashSet<int>(verts.Where(v => !used.Contains(v))));
        return Tuple.Create(n0, Tris(b).Count / 3);
    }

    // A box or a plane (size s, centre c) as new geometry of the buffer, with planar UVs; returns the new vertices.
    public List<int> AddPrimitive(VBuf b, string kind, float[] c, float s)
    {
        float h = s / 2;
        var pos = new List<float>(); var uv = new List<float>(); var tris = new List<int>();
        Action<float[][], float[][]> quad = (corners, uvs) =>
        {
            int k = pos.Count / 3;
            for (int i = 0; i < 4; i++) { pos.Add(c[0] + corners[i][0]); pos.Add(c[1] + corners[i][1]); pos.Add(c[2] + corners[i][2]); uv.Add(uvs[i][0]); uv.Add(uvs[i][1]); }
            tris.AddRange(new[] { k, k + 1, k + 2, k, k + 2, k + 3 });
        };
        var U = new[] { new[] { 0f, 0f }, new[] { 1f, 0f }, new[] { 1f, 1f }, new[] { 0f, 1f } };
        if (kind == "plane")
            quad(new[] { new[] { -h, 0, -h }, new[] { -h, 0, h }, new[] { h, 0, h }, new[] { h, 0, -h } }, U);
        else
        {
            quad(new[] { new[] { -h, h, -h }, new[] { -h, h, h }, new[] { h, h, h }, new[] { h, h, -h } }, U);      // top
            quad(new[] { new[] { -h, -h, -h }, new[] { h, -h, -h }, new[] { h, -h, h }, new[] { -h, -h, h } }, U);  // bottom
            quad(new[] { new[] { -h, -h, h }, new[] { h, -h, h }, new[] { h, h, h }, new[] { -h, h, h } }, U);      // +z
            quad(new[] { new[] { h, -h, -h }, new[] { -h, -h, -h }, new[] { -h, h, -h }, new[] { h, h, -h } }, U);  // -z
            quad(new[] { new[] { h, -h, h }, new[] { h, -h, -h }, new[] { h, h, -h }, new[] { h, h, h } }, U);      // +x
            quad(new[] { new[] { -h, -h, -h }, new[] { -h, -h, h }, new[] { -h, h, h }, new[] { -h, h, -h } }, U);  // -x
        }
        return AddGeometry(b, pos.ToArray(), uv.ToArray(), tris.ToArray());
    }

    public int FaceCount(VBuf b) { return Tris(b).Count / 3; }
    public HashSet<int> VertsOfFaces(VBuf b, IEnumerable<int> faces)
    {
        var t = Tris(b); var s = new HashSet<int>();
        foreach (int f in faces) { s.Add(t[f * 3]); s.Add(t[f * 3 + 1]); s.Add(t[f * 3 + 2]); }
        return s;
    }
    public HashSet<int> FacesOfVerts(VBuf b, HashSet<int> verts, bool all)
    {
        var t = Tris(b); var s = new HashSet<int>();
        for (int f = 0; f * 3 + 2 < t.Count; f++)
        {
            int n = (verts.Contains(t[f * 3]) ? 1 : 0) + (verts.Contains(t[f * 3 + 1]) ? 1 : 0) + (verts.Contains(t[f * 3 + 2]) ? 1 : 0);
            if (all ? n == 3 : n > 0) s.Add(f);
        }
        return s;
    }

    public void DeleteFaceSet(VBuf b, HashSet<int> faces)
    {
        var before = Before(b); var t = Tris(b); var k = new List<int>(t.Count);
        for (int f = 0; f * 3 + 2 < t.Count; f++) if (!faces.Contains(f)) { k.Add(t[f * 3]); k.Add(t[f * 3 + 1]); k.Add(t[f * 3 + 2]); }
        b.Users[0].Tris = k; Sync(b, before);
    }

    public void FlipFaces(VBuf b, HashSet<int> faces)
    {
        var before = Before(b); var t = Tris(b);
        for (int f = 0; f * 3 + 2 < t.Count; f++) if (faces == null || faces.Contains(f)) { int x = t[f * 3 + 1]; t[f * 3 + 1] = t[f * 3 + 2]; t[f * 3 + 2] = x; }
        Sync(b, before);
    }

    public void Mirror(VBuf b, HashSet<int> verts, int axis, float pivot)
    {
        foreach (int v in verts) { var p = Pos(b, v); p[axis] = 2 * pivot - p[axis]; SetPos(b, v, p[0], p[1], p[2]); }
        FlipFaces(b, FacesOfVerts(b, verts, true));
    }

    // A new vertex copying v (bones, normal, everything), at another place and texture coordinate.
    int CloneVertex(VBuf b, int v, float[] pos, float[] uv)
    {
        int n = b.Count;
        var data = new byte[(n + 1) * b.Stride];
        Array.Copy(b.Data, data, b.Data.Length);
        Array.Copy(b.Data, v * b.Stride, data, n * b.Stride, b.Stride);
        b.Data = data;
        SetPos(b, n, pos[0], pos[1], pos[2]);
        if (uv != null) SetUv(b, n, uv[0], uv[1]);
        return n;
    }
    // Many new vertices at once (one copy of the buffer).
    void Grow(VBuf b, int extra)
    {
        var data = new byte[(b.Count + extra) * b.Stride];
        Array.Copy(b.Data, data, b.Data.Length);
        b.Data = data;
    }
    static ushort ToHalf(float f)
    {
        int bits = BitConverter.ToInt32(BitConverter.GetBytes(f), 0);
        int s = (bits >> 16) & 0x8000, e = ((bits >> 23) & 0xFF) - 127 + 15, m = bits & 0x7FFFFF;
        if (e <= 0) return (ushort)s;
        if (e >= 31) return (ushort)(s | 0x7BFF);
        return (ushort)(s | e << 10 | m >> 13);
    }
    public void SetUv(VBuf b, int v, float u, float w)
    {
        if (b.UvOffset < 0 || b.UvOffset + 4 > b.Stride) return;
        int o = v * b.Stride + b.UvOffset; ushort hu = ToHalf(u), hv = ToHalf(w);
        b.Data[o] = (byte)(hu >> 8); b.Data[o + 1] = (byte)hu; b.Data[o + 2] = (byte)(hv >> 8); b.Data[o + 3] = (byte)hv;
    }

    // 1 triangle -> 4 (new vertices at the middle of each edge, copies of the edge's first end).
    public void Subdivide(VBuf b, HashSet<int> faces)
    {
        var before = Before(b); var t = Tris(b);
        var mid = new Dictionary<long, int>(); var nt = new List<int>(t.Count * 2);
        Func<int, int, int> mp = (x, y) =>
        {
            long key = x < y ? (long)x << 32 | (uint)y : (long)y << 32 | (uint)x;
            int m;
            if (mid.TryGetValue(key, out m)) return m;
            var px = Pos(b, x); var py = Pos(b, y); var ux = Uv(b, x); var uy = Uv(b, y);
            m = CloneVertex(b, x, new[] { (px[0] + py[0]) / 2, (px[1] + py[1]) / 2, (px[2] + py[2]) / 2 }, new[] { (ux[0] + uy[0]) / 2, (ux[1] + uy[1]) / 2 });
            mid[key] = m; return m;
        };
        for (int f = 0; f * 3 + 2 < t.Count; f++)
        {
            int a = t[f * 3], c = t[f * 3 + 1], d = t[f * 3 + 2];
            if (faces != null && !faces.Contains(f)) { nt.Add(a); nt.Add(c); nt.Add(d); continue; }
            int ab = mp(a, c), bc = mp(c, d), ca = mp(d, a);
            nt.AddRange(new[] { a, ab, ca, ab, c, bc, ca, bc, d, ab, bc, ca });
        }
        b.Users[0].Tris = nt; Sync(b, before);
    }

    // The selected faces get vertices of their own where they touch the others (moved apart freely).
    public void Detach(VBuf b, HashSet<int> faces)
    {
        var before = Before(b); var t = Tris(b);
        var inside = new HashSet<int>(); var outside = new HashSet<int>();
        for (int f = 0; f * 3 + 2 < t.Count; f++) for (int c = 0; c < 3; c++) (faces.Contains(f) ? inside : outside).Add(t[f * 3 + c]);
        var map = new Dictionary<int, int>();
        foreach (int v in inside.Where(outside.Contains).ToList()) map[v] = CloneVertex(b, v, Pos(b, v), Uv(b, v));
        foreach (int f in faces) for (int c = 0; c < 3; c++) { int m; if (map.TryGetValue(t[f * 3 + c], out m)) t[f * 3 + c] = m; }
        Sync(b, before);
    }

    // Vertices at the same place (and texture coordinate, and the same other bytes) become one.
    public int Weld(VBuf b, float tol)
    {
        double q = 1 / Math.Max(tol, 1e-6);
        var seen = new Dictionary<string, int>(); var map = new int[b.Count]; int merged = 0;
        for (int v = 0; v < b.Count; v++)
        {
            var p = Pos(b, v); var uv = Uv(b, v);
            string key = Math.Round(p[0] * q) + "," + Math.Round(p[1] * q) + "," + Math.Round(p[2] * q) + "," + Math.Round(uv[0] * 1000) + "," + Math.Round(uv[1] * 1000);
            int s;
            if (seen.TryGetValue(key, out s)) { map[v] = s; merged++; } else { seen[key] = v; map[v] = v; }
        }
        foreach (var ib in b.Users)
        {
            var k = new List<int>(ib.Tris.Count);
            for (int i = 0; i + 2 < ib.Tris.Count; i += 3)
            {
                int a = map[ib.Tris[i]], c = map[ib.Tris[i + 1]], d = map[ib.Tris[i + 2]];
                if (a == c || c == d || a == d) continue;
                k.Add(a); k.Add(c); k.Add(d);
            }
            ib.Tris = k;
        }
        // Vertices no face uses any more go.
        var used = new HashSet<int>(); foreach (var ib in b.Users) foreach (int i in ib.Tris) used.Add(i);
        var gone = new HashSet<int>(Enumerable.Range(0, b.Count).Where(v => !used.Contains(v)));
        DeleteVertices(b, gone);
        return merged;
    }

    // Vertices at the same place count as one (a seam of the texture is still one surface).
    Dictionary<string, List<int>> Groups(VBuf b, out int[] gid, out List<List<int>> lists)
    {
        var g = new Dictionary<string, List<int>>(); gid = new int[b.Count]; lists = new List<List<int>>();
        for (int v = 0; v < b.Count; v++)
        {
            var p = Pos(b, v); string k = Math.Round(p[0] * 1e5) + "," + Math.Round(p[1] * 1e5) + "," + Math.Round(p[2] * 1e5);
            List<int> l; if (!g.TryGetValue(k, out l)) { l = new List<int>(); g[k] = l; lists.Add(l); }
            l.Add(v);
        }
        for (int i = 0; i < lists.Count; i++) foreach (int v in lists[i]) gid[v] = i;
        return g;
    }

    // Each selected vertex moves toward the middle of its neighbours (sanding the shape).
    public void Smooth(VBuf b, HashSet<int> verts, int iter, float factor)
    {
        int[] gid; List<List<int>> lists; Groups(b, out gid, out lists);
        var nb = new HashSet<int>[lists.Count]; for (int i = 0; i < nb.Length; i++) nb[i] = new HashSet<int>();
        var t = Tris(b);
        for (int f = 0; f * 3 + 2 < t.Count; f++) for (int c = 0; c < 3; c++) { int x = gid[t[f * 3 + c]], y = gid[t[f * 3 + (c + 1) % 3]]; if (x != y) { nb[x].Add(y); nb[y].Add(x); } }
        var sel = new HashSet<int>(verts.Select(v => gid[v]));
        for (int it = 0; it < iter; it++)
        {
            var np = new Dictionary<int, float[]>();
            foreach (int a in sel)
            {
                if (nb[a].Count == 0) continue;
                var p0 = Pos(b, lists[a][0]); float x = 0, y = 0, z = 0;
                foreach (int o in nb[a]) { var q = Pos(b, lists[o][0]); x += q[0]; y += q[1]; z += q[2]; }
                int m = nb[a].Count;
                np[a] = new[] { p0[0] + (x / m - p0[0]) * factor, p0[1] + (y / m - p0[1]) * factor, p0[2] + (z / m - p0[2]) * factor };
            }
            foreach (var kv in np) foreach (int v in lists[kv.Key]) SetPos(b, v, kv.Value[0], kv.Value[1], kv.Value[2]);
        }
    }

    // Surface normals from the triangles (the file's own are packed): one per vertex, seams joined.
    public float[] FaceNormals(VBuf b)
    {
        int[] gid; List<List<int>> lists; Groups(b, out gid, out lists);
        var gn = new double[lists.Count * 3]; var t = Tris(b);
        for (int f = 0; f * 3 + 2 < t.Count; f++)
        {
            var a = Pos(b, t[f * 3]); var c = Pos(b, t[f * 3 + 1]); var d = Pos(b, t[f * 3 + 2]);
            double ux = c[0] - a[0], uy = c[1] - a[1], uz = c[2] - a[2], vx = d[0] - a[0], vy = d[1] - a[1], vz = d[2] - a[2];
            double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
            for (int k = 0; k < 3; k++) { int g = gid[t[f * 3 + k]]; gn[g * 3] += nx; gn[g * 3 + 1] += ny; gn[g * 3 + 2] += nz; }
        }
        var n = new float[b.Count * 3];
        for (int v = 0; v < b.Count; v++)
        {
            int g = gid[v]; double l = Math.Sqrt(gn[g * 3] * gn[g * 3] + gn[g * 3 + 1] * gn[g * 3 + 1] + gn[g * 3 + 2] * gn[g * 3 + 2]);
            if (l > 1e-12) { n[v * 3] = (float)(gn[g * 3] / l); n[v * 3 + 1] = (float)(gn[g * 3 + 1] / l); n[v * 3 + 2] = (float)(gn[g * 3 + 2] / l); }
        }
        return n;
    }

    // ---- Packed normals and tangents (2026-11-17) ----
    // A new vertex is a copy of the nearest base vertex (AddSkinned): its normal and tangent were that vertex's, any
    // direction over the new body (imports looked metallic and mottled: the sky's and the ground's light in patches).
    // The layout packs them in one dword: normal (usage 3) as format 16 or 17 (three fields of 10 / 11 bits), the
    // tangent (usage 6) as format 7 (2_10_10_10). Field order (low bits first) and signedness: NormalLayouts, the
    // first that matches the base's own normals (NormalCheck measures it).
    public static readonly int[][] NormalLayouts = { new[] { 11, 11, 10 }, new[] { 10, 11, 11 } };
    public static int NormalLayout = 0;     // index in NormalLayouts for format 16 (17: the other one)
    public static bool NormalHighFirst = false;  // x in the high bits
    static int[] Widths(int format)
    {
        if (format == 7) return new[] { 10, 10, 10 };
        int i = format == 16 ? NormalLayout : 1 - NormalLayout;
        return NormalLayouts[i];
    }
    public static float[] Unpack3(uint w, int format)
    {
        var wd = Widths(format); var r = new float[3]; int shift = 0;
        for (int k = 0; k < 3; k++)
        {
            int c = NormalHighFirst ? 2 - k : k;
            int bits = wd[c]; uint raw = (w >> shift) & ((1u << bits) - 1); shift += bits;
            int sv = (int)(raw << (32 - bits)) >> (32 - bits);  // signed
            r[c] = Math.Max(-1f, sv / (float)((1 << (bits - 1)) - 1));
        }
        return r;
    }
    public static uint Pack3(float[] n, int format, uint keepHigh = 0)
    {
        var wd = Widths(format); uint w = 0; int shift = 0;
        for (int k = 0; k < 3; k++)
        {
            int c = NormalHighFirst ? 2 - k : k;
            int bits = wd[c]; int max = (1 << (bits - 1)) - 1;
            int sv = (int)Math.Round(Math.Max(-1, Math.Min(1, n[c])) * max);
            w |= ((uint)sv & ((1u << bits) - 1)) << shift; shift += bits;
        }
        if (format == 7) w |= keepHigh & 0xC0000000u;  // the 2-bit w (the tangent's handedness) as it was
        return w;
    }
    // How well each way of reading the normals matches the triangles' normals in buffer b (mean dot, the share
    // pointing the same way): picks NormalLayout / NormalHighFirst. Returns a report.
    public string NormalCheck(VBuf b)
    {
        var na = b.Layout == null ? null : b.Layout.FirstOrDefault(a => a.Usage == 3);
        if (na == null) return "sem normal";
        var fn = FaceNormals(b);
        var sb = new System.Text.StringBuilder("normal formato " + na.Format + " em +" + na.Offset + ":");
        double best = -2; int bl = 0; bool bh = false;
        foreach (bool hi in new[] { false, true })
            for (int l = 0; l < NormalLayouts.Length; l++)
            {
                NormalLayout = l; NormalHighFirst = hi;
                double dot = 0; int n = 0;
                for (int v = 0; v < b.Count; v++)
                {
                    float fl = fn[v * 3] * fn[v * 3] + fn[v * 3 + 1] * fn[v * 3 + 1] + fn[v * 3 + 2] * fn[v * 3 + 2]; if (fl < 0.5f) continue;
                    var q = Unpack3(Be.U32(b.Data, v * b.Stride + na.Offset), na.Format);
                    double ql = Math.Sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]); if (ql < 1e-3) continue;
                    dot += (q[0] * fn[v * 3] + q[1] * fn[v * 3 + 1] + q[2] * fn[v * 3 + 2]) / ql; n++;
                }
                double m = n > 0 ? dot / n : 0;
                sb.Append(string.Format(" [{0}{1}] {2:F2}", string.Join("/", NormalLayouts[l]), hi ? " alto" : "", m));
                if (m > best) { best = m; bl = l; bh = hi; }
            }
        NormalLayout = bl; NormalHighFirst = bh;
        return sb.ToString();
    }
    // The vertices' normals from the triangles and tangents from the texture coordinates, written packed.
    public int SetNormals(VBuf b, ICollection<int> verts)
    {
        var na = b.Layout == null ? null : b.Layout.FirstOrDefault(a => a.Usage == 3);
        if (na == null || (na.Format != 16 && na.Format != 17)) return 0;
        var ta = b.Layout.FirstOrDefault(a => a.Usage == 6 && a.Format == 7);
        var fn = FaceNormals(b);
        float[] tan = null;
        if (ta != null && b.UvOffset >= 0)
        {
            tan = new float[b.Count * 3]; var t = Tris(b);
            for (int f = 0; f * 3 + 2 < t.Count; f++)
            {
                int i0 = t[f * 3], i1 = t[f * 3 + 1], i2 = t[f * 3 + 2];
                var p0 = Pos(b, i0); var p1 = Pos(b, i1); var p2 = Pos(b, i2); var u0 = Uv(b, i0); var u1 = Uv(b, i1); var u2 = Uv(b, i2);
                float du1 = u1[0] - u0[0], dv1 = u1[1] - u0[1], du2 = u2[0] - u0[0], dv2 = u2[1] - u0[1];
                float r = du1 * dv2 - du2 * dv1; if (Math.Abs(r) < 1e-12f) continue; r = 1 / r;
                for (int k = 0; k < 3; k++)
                {
                    float e1 = p1[k] - p0[k], e2 = p2[k] - p0[k], tk = (e1 * dv2 - e2 * dv1) * r;
                    tan[i0 * 3 + k] += tk; tan[i1 * 3 + k] += tk; tan[i2 * 3 + k] += tk;
                }
            }
        }
        int done = 0;
        foreach (int v in verts)
        {
            var n = new[] { fn[v * 3], fn[v * 3 + 1], fn[v * 3 + 2] };
            if (n[0] * n[0] + n[1] * n[1] + n[2] * n[2] < 0.5f) continue;
            int o = v * b.Stride;
            Be.Put32(b.Data, o + na.Offset, Pack3(n, na.Format));
            if (tan != null)
            {
                float tx = tan[v * 3], ty = tan[v * 3 + 1], tz = tan[v * 3 + 2];
                float d = tx * n[0] + ty * n[1] + tz * n[2]; tx -= n[0] * d; ty -= n[1] * d; tz -= n[2] * d;  // across the surface
                float l = (float)Math.Sqrt(tx * tx + ty * ty + tz * tz);
                if (l > 1e-6f) Be.Put32(b.Data, o + ta.Offset, Pack3(new[] { tx / l, ty / l, tz / l }, 7, Be.U32(b.Data, o + ta.Offset)));
            }
            done++;
        }
        return done;
    }

    // The texture coordinate's 3rd and 4th values (format 32, four halves; the first two are the UV): what the base
    // holds there (2026-11-17, looking for the mottled colours on imports).
    public string UvExtraReport(VBuf b)
    {
        var ua = b.Layout == null ? null : b.Layout.FirstOrDefault(a => a.Usage == 5 && a.Index == 0);
        if (ua == null) return "sem UV";
        if (ua.Format != 32) return "UV formato " + ua.Format + " (sem 3o/4o valor)";
        float[] mn = { float.MaxValue, float.MaxValue }, mx = { float.MinValue, float.MinValue }; double[] sum = new double[2];
        var hist = new Dictionary<string, int>();
        for (int v = 0; v < b.Count; v++)
            for (int k = 0; k < 2; k++)
            {
                float f = Half(b.Data, v * b.Stride + ua.Offset + 4 + k * 2);
                mn[k] = Math.Min(mn[k], f); mx[k] = Math.Max(mx[k], f); sum[k] += f;
                if (k == 0) { string key = f.ToString("F2") + "," + Half(b.Data, v * b.Stride + ua.Offset + 6).ToString("F2"); int c; hist.TryGetValue(key, out c); hist[key] = c + 1; }
            }
        return string.Format("UV 3o/4o valor: z {0:F3}..{1:F3} media {2:F3}; w {3:F3}..{4:F3} media {5:F3}; mais comuns {6}", mn[0], mx[0], sum[0] / b.Count,
                             mn[1], mx[1], sum[1] / b.Count, string.Join(" ", hist.OrderByDescending(kv => kv.Value).Take(5).Select(kv => kv.Key + "x" + kv.Value)));
    }
    public static float Half(byte[] d, int o)
    {
        int h = d[o] << 8 | d[o + 1];
        int s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
        float f = e == 0 ? m / 1024f * (float)Math.Pow(2, -14) : e == 31 ? float.PositiveInfinity : (1 + m / 1024f) * (float)Math.Pow(2, e - 15);
        return s == 1 ? -f : f;
    }

    // Pushes the vertices along the surface's normal (negative: shrinks).
    public void Inflate(VBuf b, HashSet<int> verts, float d)
    {
        var n = FaceNormals(b);
        foreach (int v in verts) { var p = Pos(b, v); SetPos(b, v, p[0] + n[v * 3] * d, p[1] + n[v * 3 + 1] * d, p[2] + n[v * 3 + 2] * d); }
    }

    // Vertices whose place is shared with the given ones (both sides of a texture seam).
    public HashSet<int> SamePlace(VBuf b, HashSet<int> verts)
    {
        int[] gid; List<List<int>> lists; Groups(b, out gid, out lists);
        var s = new HashSet<int>();
        foreach (int g in verts.Select(v => gid[v]).Distinct()) foreach (int v in lists[g]) s.Add(v);
        return s;
    }
    // Grows the selection by the faces touching it.
    public HashSet<int> Grow(VBuf b, HashSet<int> verts)
    {
        var at = SamePlace(b, verts); return VertsOfFaces(b, FacesOfVerts(b, at, false));
    }
    // Everything joined to the selection by faces (a whole piece: a sleeve, a buckle).
    public HashSet<int> Linked(VBuf b, HashSet<int> verts)
    {
        int[] gid; List<List<int>> lists; Groups(b, out gid, out lists);
        var par = Enumerable.Range(0, lists.Count).ToArray();
        Func<int, int> find = null; find = a => { while (par[a] != a) { par[a] = par[par[a]]; a = par[a]; } return a; };
        var t = Tris(b);
        for (int f = 0; f * 3 + 2 < t.Count; f++) { int a = find(gid[t[f * 3]]); par[find(gid[t[f * 3 + 1]])] = a; par[find(gid[t[f * 3 + 2]])] = a; }
        var roots = new HashSet<int>(verts.Select(v => find(gid[v])));
        return new HashSet<int>(Enumerable.Range(0, b.Count).Where(v => roots.Contains(find(gid[v]))));
    }

    // New polygons in the part (an OBJ, or a part of another model): each vertex copies the part's
    // nearest one (its bones and other attributes), with its own place and texture coordinate.
    public List<int> AddGeometry(VBuf b, float[] pos, float[] uv, int[] tris)
    {
        int n0 = b.Count, n = pos.Length / 3;
        if (n0 == 0) throw new Exception("parte vazia");
        var src = new float[n0 * 3]; for (int v = 0; v < n0; v++) { var p = Pos(b, v); src[v * 3] = p[0]; src[v * 3 + 1] = p[1]; src[v * 3 + 2] = p[2]; }
        Grow(b, n);
        var res = new List<int>(n);
        for (int i = 0; i < n; i++)
        {
            float x = pos[i * 3], y = pos[i * 3 + 1], z = pos[i * 3 + 2];
            int best = 0; float bd = float.MaxValue;
            for (int v = 0; v < n0; v++) { float dx = src[v * 3] - x, dy = src[v * 3 + 1] - y, dz = src[v * 3 + 2] - z, d = dx * dx + dy * dy + dz * dz; if (d < bd) { bd = d; best = v; } }
            int at = n0 + i;
            Array.Copy(b.Data, best * b.Stride, b.Data, at * b.Stride, b.Stride);
            SetPos(b, at, x, y, z);
            if (uv != null) SetUv(b, at, uv[i * 2], uv[i * 2 + 1]);
            res.Add(at);
        }
        foreach (var ib in b.Users) for (int i = 0; i + 2 < tris.Length; i += 3) { ib.Tris.Add(n0 + tris[i]); ib.Tris.Add(n0 + tris[i + 1]); ib.Tris.Add(n0 + tris[i + 2]); }
        return res;
    }

    // A model from outside the game (Program "outfit"): new polygons in b, each vertex taking the bones and other
    // attributes of the nearest vertex in any of the sources (buffers of the same layout, the whole body), so a
    // hand gets the hand's bones even when the target part is the coat.
    public List<int> AddSkinned(VBuf b, List<VBuf> sources, float[] pos, float[] uv, int[] tris, Func<VBuf, int, bool> usable = null)
    {
        var src = new List<Tuple<VBuf, int, float, float, float>>();
        foreach (var s in sources)
            for (int v = 0; v < s.Count; v++) { if (usable != null && !usable(s, v)) continue; var p = Pos(s, v); src.Add(Tuple.Create(s, v, p[0], p[1], p[2])); }
        if (src.Count == 0) throw new Exception("sem vertices de origem");
        int n0 = b.Count, n = pos.Length / 3;
        Grow(b, n);
        var res = new List<int>(n);
        for (int i = 0; i < n; i++)
        {
            float x = pos[i * 3], y = pos[i * 3 + 1], z = pos[i * 3 + 2];
            Tuple<VBuf, int, float, float, float> best = null; float bd = float.MaxValue;
            foreach (var t in src) { float dx = t.Item3 - x, dy = t.Item4 - y, dz = t.Item5 - z, d = dx * dx + dy * dy + dz * dz; if (d < bd) { bd = d; best = t; } }
            int at = n0 + i;
            Array.Copy(best.Item1.Data, best.Item2 * best.Item1.Stride, b.Data, at * b.Stride, b.Stride);
            SetPos(b, at, x, y, z);
            if (uv != null) SetUv(b, at, uv[i * 2], uv[i * 2 + 1]);
            res.Add(at);
        }
        foreach (var ib in b.Users) for (int i = 0; i + 2 < tris.Length; i += 3) { ib.Tris.Add(n0 + tris[i]); ib.Tris.Add(n0 + tris[i + 1]); ib.Tris.Add(n0 + tris[i + 2]); }
        return res;
    }

    // A vertex's bones: up to 4 joints (skeleton numbers) and weights; written as the layout's 8-bit
    // weights (summing to 255) and bone numbers (joint - 1).
    public void SetBones(VBuf b, int v, int[] joints, float[] w)
    {
        var wa = b.Layout == null ? null : b.Layout.FirstOrDefault(a => a.Usage == 1);
        var ia = b.Layout == null ? null : b.Layout.FirstOrDefault(a => a.Usage == 2);
        if (wa == null || ia == null) return;
        float sum = 0; for (int k = 0; k < joints.Length && k < 4; k++) sum += w[k];
        var q = new int[4]; int total = 0, big = 0;
        for (int k = 0; k < 4; k++)
        {
            q[k] = k < joints.Length && sum > 0 ? (int)Math.Round(w[k] / sum * 255) : 0;
            total += q[k]; if (q[k] > q[big]) big = k;
        }
        q[big] += 255 - total;
        int o = v * b.Stride;
        for (int k = 0; k < 4; k++)
        {
            b.Data[o + wa.Offset + k] = (byte)q[k];
            b.Data[o + ia.Offset + k] = (byte)(k < joints.Length && q[k] > 0 ? joints[k] - 1 : 0);
        }
    }

    // The technique name of the buffer's material ("ALL-4weight", "ALL_LOD_L-2weight", ...): MATE word 2 names it.
    public string MaterialName(VBuf b)
    {
        var mates = md.Records.Where(r => r.Tag == "MATE").ToList();
        if (b.Mat < 0 || b.Mat >= mates.Count || mates[b.Mat].F.Length < 3) return "";
        int s = (int)mates[b.Mat].F[2];
        return s >= 0 && s < md.Strings.Count ? md.Strings[s] : "";
    }
    // How many bones a vertex may have under the buffer's material ("-3weight": 3), 4 when it does not say.
    public int MaxWeights(VBuf b)
    {
        var m = System.Text.RegularExpressions.Regex.Match(MaterialName(b), "(\\d)weight");
        return m.Success ? Math.Max(1, Math.Min(4, int.Parse(m.Groups[1].Value))) : 4;
    }

    // Every triangle of the buffer gone (its vertices stay: nothing is drawn).
    public void ClearTris(VBuf b) { foreach (var ib in b.Users) ib.Tris.Clear(); }

    public Tex2D Texture(string name) { return md.Textures().Textures.FirstOrDefault(t => string.Equals(t.Name, name, StringComparison.OrdinalIgnoreCase)); }
    // Diagnostics: a texture's fields, and how many bytes of its data a decode + encode changes.
    public string TextureCheck(string name)
    {
        var p = md.Textures(); var t = p.Textures.First(x => string.Equals(x.Name, name, StringComparison.OrdinalIgnoreCase));
        var before = (byte[])p.Pix.Clone();
        p.Encode(t, p.Decode(t));
        int diff = 0, first = -1; for (int i = 0; i < before.Length; i++) if (before[i] != p.Pix[i]) { diff++; if (first < 0) first = i; }
        string raw = BitConverter.ToString(before, t.DataOffset, 32);
        string rawAfter = BitConverter.ToString(p.Pix, t.DataOffset, 32);
        Array.Copy(before, p.Pix, before.Length);
        return "off 0x" + t.DataOffset.ToString("X") + " size 0x" + t.DataSize.ToString("X") + " pix 0x" + p.Pix.Length.ToString("X") + " tiled " + t.Tiled + " endian " + t.Endian + " pitch " + t.Pitch + " F4 0x" + t.F4.ToString("X8") + " F5 0x" + t.F5.ToString("X8") + " mudou " + diff + " bytes (primeiro 0x" + first.ToString("X") + ")\n    antes  " + raw + "\n    depois " + rawAfter;
    }
    // A texture's base level as RGBA.
    public byte[] TexturePixels(string name) { var p = md.Textures(); return p.Decode(p.Textures.First(t => string.Equals(t.Name, name, StringComparison.OrdinalIgnoreCase))); }
    // New pixels for a texture of the model (same size), base level only: its mip levels are switched off (they
    // would still show the old picture far away).
    public void PaintTexture(string name, byte[] rgba)
    {
        var p = md.Textures();
        var t = p.Textures.First(x => string.Equals(x.Name, name, StringComparison.OrdinalIgnoreCase));
        p.Encode(t, rgba);
        int f4 = t.HeaderOffset + 0x1C + 16;
        if (Environment.GetEnvironmentVariable("NT_KEEPMIP") != "1") Be.Put32(Mdv, f4, Be.U32(Mdv, f4) & ~(0xFu << 6));
    }

    // Undo: a copy of the editable state.
    public object Snapshot()
    {
        return Buffers.Select(b => Tuple.Create((byte[])b.Data.Clone(), b.Users.Select(u => new List<int>(u.Tris)).ToList())).ToList();
    }
    // One buffer only (2026-11-07): a map has hundreds of buffers and every undo step cloned them all (hundreds of MB
    // an edit, 40 steps: memory ran out and Windows' explorer went down with the app). Edits change the active piece.
    public object Snapshot(int only)
    {
        var b = Buffers[only];
        return Tuple.Create(only, (byte[])b.Data.Clone(), b.Users.Select(u => new List<int>(u.Tris)).ToList());
    }
    // Several buffers (a multi-piece edit, a sculpt stroke).
    public object Snapshot(IEnumerable<int> buffers)
    {
        return buffers.Distinct().Where(i => i >= 0 && i < Buffers.Count).Select(i => (Tuple<int, byte[], List<List<int>>>)Snapshot(i)).ToList();
    }
    // The buffers a snapshot is of (null: the whole model).
    public static List<int> SnapshotBuffers(object snap)
    {
        var one = snap as Tuple<int, byte[], List<List<int>>>;
        if (one != null) return new List<int> { one.Item1 };
        var many = snap as List<Tuple<int, byte[], List<List<int>>>>;
        return many != null ? many.Select(t => t.Item1).ToList() : null;
    }
    public static long SnapshotBytes(object snap)
    {
        var many = snap as List<Tuple<int, byte[], List<List<int>>>>;
        if (many != null) return many.Sum(t => SnapshotBytes(t));
        var one = snap as Tuple<int, byte[], List<List<int>>>;
        if (one != null) return one.Item2.Length + one.Item3.Sum(l => (long)l.Count * 4);
        var all = snap as List<Tuple<byte[], List<List<int>>>>;
        return all == null ? 0 : all.Sum(t => t.Item1.Length + t.Item2.Sum(l => (long)l.Count * 4));
    }
    // The buffer a one-buffer snapshot is of (-1: a whole-model one).
    public static int SnapshotBuffer(object snap) { var one = snap as Tuple<int, byte[], List<List<int>>>; return one != null ? one.Item1 : -1; }
    public void Restore(object snap)
    {
        var many = snap as List<Tuple<int, byte[], List<List<int>>>>;
        if (many != null) { foreach (var t in many) Restore(t); return; }
        var one = snap as Tuple<int, byte[], List<List<int>>>;
        if (one != null)
        {
            if (one.Item1 < Buffers.Count)
            {
                var b = Buffers[one.Item1];
                b.Data = one.Item2;
                for (int u = 0; u < b.Users.Count && u < one.Item3.Count; u++) b.Users[u].Tris = one.Item3[u];
            }
            return;
        }
        var s = (List<Tuple<byte[], List<List<int>>>>)snap;
        for (int i = 0; i < Buffers.Count && i < s.Count; i++)
        {
            Buffers[i].Data = s[i].Item1;
            for (int u = 0; u < Buffers[i].Users.Count && u < s[i].Item2.Count; u++) Buffers[i].Users[u].Tris = s[i].Item2[u];
        }
    }

    // ---- writing ----

    // The edited model: (.MDV, .MDP unpacked). limit > 0: the data must fit in that many bytes (a
    // player model: where the game unpacks it); the biggest textures lose their top level until it does.
    public Tuple<byte[], byte[]> Build(int limit, Action<string> log)
    {
        var mdv = (byte[])Mdv.Clone();
        var h = heap;
        foreach (var b in Buffers) if (b.Count >= 0xFFFF) throw new Exception("parte com " + b.Count + " vertices (maximo 65534)");
        var texs = md.Textures().Textures.OrderBy(t => t.DataOffset).ToList();
        int texStart = texs.Count > 0 ? texs[0].DataOffset : Pix.Length;
        var outp = new List<byte>(Pix.Length);
        Func<int, int> align = a => { while (outp.Count % a != 0) outp.Add(0); return outp.Count; };
        var newC = new Dictionary<int, Tuple<int, int>>();  // header offset -> (C, D)
        if (Environment.GetEnvironmentVariable("NT_EDITDBG") != null)
        {
            long a = 0, bb = 0, va = 0, vb = 0;
            foreach (var r in h.Recs) { if (r.Tag == "IXBF") a += r.D; if (r.Tag == "VXBF") va += r.D; }
            foreach (var ib in Indices) bb += Strips(ib.Tris).Count * 2;
            foreach (var b in Buffers) vb += b.Data.Length;
            log("   indices " + a + " -> " + bb + ", vertices " + va + " -> " + vb + ", texturas em " + texStart + " de " + Pix.Length);
        }
        foreach (var ib in Indices)
        {
            int at = align(0x80);
            if (ib.Tris.SequenceEqual(ib.Orig)) outp.AddRange(ib.OrigBytes); else foreach (var x in Strips(ib.Tris)) { outp.Add((byte)(x >> 8)); outp.Add((byte)x); }
            foreach (int hd in ib.Hdrs) newC[hd] = Tuple.Create(at, outp.Count - at);
        }
        foreach (var b in Buffers)
        {
            int at = align(0x80);
            outp.AddRange(b.Data);
            foreach (int hd in b.Hdrs) newC[hd] = Tuple.Create(at, b.Data.Length);
        }
        // Anything else between the buffers and the textures (not expected) keeps its bytes, moved.
        int otherFrom = 0;
        foreach (var r in h.Recs) if ((r.Tag == "IXBF" || r.Tag == "VXBF") && r.C + r.D > otherFrom) otherFrom = (int)(r.C + r.D);
        int shift = 0;
        if (otherFrom < texStart)
        {
            int at = align(0x80);
            shift = at - otherFrom;
            for (int i = otherFrom; i < texStart; i++) outp.Add(Pix[i]);
        }
        align(0x1000);
        int texBase = outp.Count;
        for (int i = texStart; i < Pix.Length; i++) outp.Add(Pix[i]);
        var pix = outp.ToArray();
        int texShift = texBase - texStart;
        foreach (var r in h.Recs)
        {
            Tuple<int, int> cd;
            if ((r.Tag == "IXBF" || r.Tag == "VXBF") && newC.TryGetValue((int)r.Off, out cd))
            {
                Be.Put32(mdv, r.Pos + 0x14, (uint)cd.Item1); Be.Put32(mdv, r.Pos + 0x18, (uint)cd.Item2);
                int hdr = h.HeaderBase + (int)r.Off;
                if (r.Tag == "IXBF") Be.Put32(mdv, hdr + 0x1C, (uint)cd.Item2);
                else { uint w = Be.U32(mdv, hdr + 0x1C); Be.Put32(mdv, hdr + 0x1C, (w & 0xFC000003u) | ((uint)(cd.Item2 / 4) << 2)); }
            }
            // The textures and what follows them (the shaders' microcode and constants: VXSH, PXSH, SHCO) moved as one
            // block; before 2026-11-14 only the textures followed, and an edited model's shaders read other data.
            else if (r.Tag == "TX2D" || (r.C != 0xFFFFFFFF && r.D != 0 && r.C >= texStart)) Be.Put32(mdv, r.Pos + 0x14, (uint)(r.C + texShift));
            else if (r.C != 0xFFFFFFFF && r.D != 0 && r.C >= otherFrom && r.C < texStart) Be.Put32(mdv, r.Pos + 0x14, (uint)(r.C + shift));
        }
        // Index counts and bounds of each draw.
        foreach (var ib in Indices)
        {
            int count = ib.Tris.SequenceEqual(ib.Orig) ? ib.OrigBytes.Length / 2 : Strips(ib.Tris).Count;
            foreach (int s in ib.Streams)
            {
                Be.Put32(mdv, s + 0x10, (uint)count);
                int bo = h.HeaderBase + (int)Be.U32(mdv, s + 0x04);
                GrowBounds(mdv, bo, ib.V);
            }
        }
        if (limit > 0 && pix.Length > limit) pix = Program.FitMdpPublic(mdv, pix, limit, log);
        return Tuple.Create(mdv, pix);
    }

    // Triangle strips again: a triangle continues the strip when it is the strip's next one (its last two
    // vertices, in the winding the strip's parity gives), else a 0xFFFF restart begins a new one. The
    // triangles come in the order they were read, so an untouched model gets its strips back.
    static List<int> Strips(List<int> tris)
    {
        var s = new List<int>(tris.Count + tris.Count / 3);
        int runStart = 0;  // index in s where the current strip begins
        for (int i = 0; i + 2 < tris.Count; i += 3)
        {
            int a = tris[i], b = tris[i + 1], c = tris[i + 2];
            int m = s.Count - runStart - 1;  // the strip has m + 1 vertices
            if (s.Count > 0 && m >= 2)
            {
                int p = s[s.Count - 2], q = s[s.Count - 1];
                bool even = ((m + 1) & 1) == 0;
                if ((even && a == p && b == q) || (!even && a == q && b == p)) { s.Add(c); continue; }
            }
            if (s.Count > 0) s.Add(0xFFFF);
            runStart = s.Count;
            s.Add(a); s.Add(b); s.Add(c);
        }
        return s;
    }

    // VXBO starts with a bounding sphere (centre, radius): it grows to hold the edited vertices, so the
    // game does not cull the part while it is on screen.
    void GrowBounds(byte[] mdv, int at, VBuf b)
    {
        if (at < 0 || at + 16 > mdv.Length) return;
        float cx = F(mdv, at), cy = F(mdv, at + 4), cz = F(mdv, at + 8), r = F(mdv, at + 12);
        if (float.IsNaN(r) || r <= 0 || r > 1e5f) return;
        float need = r;
        for (int v = 0; v < b.Count; v++)
        {
            var p = Pos(b, v);
            float dx = p[0] - cx, dy = p[1] - cy, dz = p[2] - cz;
            float d = (float)Math.Sqrt(dx * dx + dy * dy + dz * dz);
            if (d > need) need = d;
        }
        if (need > r) PutF(mdv, at + 12, need * 1.01f);
    }
}
