// NieR model packs: <name>.MDV (tagged records: MESH, MATE, PRIM, VXBF, IXBF, TX2D...)
// + <name>.MDP (the raw buffers: indices, vertices, texture bytes).
// A "HEAP" block holds 0x20-byte records: tag, variant, ?, data offset, size,
// MDP offset, MDP size/count, ?. Names and 0x40-byte texture headers sit in the
// heap's string and header areas (same layout as the 2D packs' TX2D heap).
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

public class HeapRec
{
    public string Tag;
    public uint A, B, Off, Size, C, D, E;
    public int Pos;  // in the MDV
}

public class Heap
{
    public int Start;
    public int NameBase, HeaderBase, Count;
    public List<HeapRec> Recs = new List<HeapRec>();
}

public class MdlFile
{
    public string Name;
    public byte[] Vir, Pix;
    public List<Heap> Heaps = new List<Heap>();

    static bool Tag4(byte[] d, int o, string t) { return d[o] == t[0] && d[o + 1] == t[1] && d[o + 2] == t[2] && d[o + 3] == t[3]; }
    static string TagStr(byte[] d, int o) { return Encoding.ASCII.GetString(d, o, 4); }

    public static MdlFile Load(string name, byte[] vir, byte[] pix)
    {
        if (Lzo.IsContainer(pix)) pix = Lzo.Unpack(pix);
        var m = new MdlFile { Name = name, Vir = vir, Pix = pix };
        for (int o = 0; o + 0x40 < vir.Length; o += 0x10)
        {
            if (!Tag4(vir, o, "HEAP")) continue;
            var h = new Heap { Start = o };
            uint entries = Be.U32(vir, o + 8), nameBase = Be.U32(vir, o + 0xC), hdrBase = Be.U32(vir, o + 0x10), count = Be.U32(vir, o + 0x1C);
            if (entries != 0x20 || count == 0 || count > 0x4000 || o + entries + count * 0x20 > vir.Length) continue;
            h.NameBase = o + (int)nameBase; h.HeaderBase = o + (int)hdrBase; h.Count = (int)count;
            for (int i = 0; i < count; i++)
            {
                int e = o + (int)entries + i * 0x20;
                var r = new HeapRec { Tag = TagStr(vir, e), Pos = e, A = Be.U32(vir, e + 4), B = Be.U32(vir, e + 8), Off = Be.U32(vir, e + 0xC), Size = Be.U32(vir, e + 0x10), C = Be.U32(vir, e + 0x14), D = Be.U32(vir, e + 0x18), E = Be.U32(vir, e + 0x1C) };
                h.Recs.Add(r);
            }
            m.Heaps.Add(h);
        }
        return m;
    }

    public string NameAt(Heap h, uint off)
    {
        int s = h.NameBase + (int)off, e = s;
        if (s < 0 || s >= Vir.Length) return "";
        while (e < Vir.Length && Vir[e] != 0) e++;
        return Encoding.ASCII.GetString(Vir, s, e - s);
    }

    // The textures inside this model (TX2D records) as Tex2D, ready for Pack2D.Decode.
    public Pack2D Textures()
    {
        var p = new Pack2D { Name = Name, Vir = Vir, Pix = Pix };
        foreach (var h in Heaps)
            foreach (var r in h.Recs)
            {
                if (r.Tag != "TX2D") continue;
                var t = new Tex2D();
                t.Name = NameAt(h, r.A);
                t.EntryOffset = r.Pos;
                t.HeaderOffset = h.HeaderBase + (int)r.Off;
                t.DataOffset = (int)r.C; t.DataSize = (int)r.D;
                int f = t.HeaderOffset + 0x1C;
                if (f + 12 > Vir.Length) continue;
                t.F0 = Be.U32(Vir, f); t.F1 = Be.U32(Vir, f + 4); t.F2 = Be.U32(Vir, f + 8);
                t.F3 = Be.U32(Vir, f + 12); t.F4 = Be.U32(Vir, f + 16); t.F5 = Be.U32(Vir, f + 20);
                t.Format = (int)(t.F1 & 0x3F); t.Endian = (int)((t.F1 >> 6) & 3);
                t.Tiled = (t.F0 & 0x80000000u) != 0;
                t.Pitch = (int)((t.F0 >> 22) & 0x1FF) * 32;
                t.Width = (int)(t.F2 & 0x1FFF) + 1; t.Height = (int)((t.F2 >> 13) & 0x1FFF) + 1;
                p.Textures.Add(t);
            }
        return p;
    }

    static float BeFloat(byte[] d, int o)
    {
        return BitConverter.ToSingle(new[] { d[o + 3], d[o + 2], d[o + 1], d[o] }, 0);
    }

    static float Half(int h)
    {
        int s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
        double v;
        if (e == 0) v = m / 1024.0 * Math.Pow(2, -14);
        else if (e == 31) v = m == 0 ? double.PositiveInfinity : double.NaN;
        else v = (1 + m / 1024.0) * Math.Pow(2, e - 15);
        return (float)(s == 1 ? -v : v);
    }

    // OBJ text for every IXBF/VXBF pair (matched by variant), triangle strips with
    // 0xFFFF restarts turned into triangles. Positions are float3 at the start of
    // each vertex; the stride is vertex-buffer bytes / (highest index + 1).
    public List<KeyValuePair<string, string>> ExportObj()
    {
        var res = new List<KeyValuePair<string, string>>();
        ParseMesh();
        var mats = MaterialTextures();
        var partMat = PartMaterials();
        var mtl = new StringBuilder();
        var seenTex = new HashSet<string>();
        int gbase = 0;
        foreach (var h in Heaps)
        {
            var ix = h.Recs.Where(r => r.Tag == "IXBF").ToList();
            var vx = h.Recs.Where(r => r.Tag == "VXBF").ToList();
            var va = h.Recs.Where(r => r.Tag == "VXAR").ToList();
            int gb = gbase; gbase += Math.Min(ix.Count, vx.Count);
            for (int k = 0; k < ix.Count && k < vx.Count; k++)
            {
                var I = ix[k]; var V = vx[k];
                int count = (int)I.D / 2, ioff = (int)I.C;  // D is the byte size of the u16 indices
                if (ioff + count * 2 > Pix.Length || count <= 0) continue;
                var idx = new int[count];
                int max = 0;
                for (int i = 0; i < count; i++) { idx[i] = Be.U16(Pix, ioff + i * 2); if (idx[i] != 0xFFFF && idx[i] > max) max = idx[i]; }
                int vcount = max + 1, vbytes = (int)V.D, voff = (int)V.C;
                int stride = vbytes / vcount;
                if (voff + vbytes > Pix.Length || stride < 12) continue;
                // texture coordinates (first TEXCOORD, 16-bit floats)
                VAttr uv = null;
                if (k < va.Count) uv = Layout(h, va[k]).FirstOrDefault(a => a.Usage == 5 && a.Index == 0 && (a.Format == 31 || a.Format == 32));
                string tex = null;
                int gk = gb + k;
                if (gk < partMat.Count && partMat[gk] >= 0 && partMat[gk] < mats.Count && mats[partMat[gk]].Count > 0) tex = mats[partMat[gk]][0];
                var sb = new StringBuilder();
                var ci = CultureInfo.InvariantCulture;
                sb.AppendLine("# " + Name + " part " + k + " (" + vcount + " vertices, stride " + stride + ", material " + (gb + k < partMat.Count ? partMat[gb + k] : -1) + ", texture " + tex + ", layout " + (k < va.Count ? string.Join(";", Layout(h, va[k]).Select(a => a.Usage + "." + a.Index + "@" + a.Offset + "f" + a.Format)) : "none") + ")");
                if (tex != null) { sb.AppendLine("mtllib " + Name + ".mtl"); sb.AppendLine("usemtl " + tex); if (seenTex.Add(tex)) mtl.AppendLine("newmtl " + tex + "\nmap_Kd " + tex + ".png\n"); }
                for (int v = 0; v < vcount; v++)
                {
                    int o = voff + v * stride;
                    sb.AppendLine("v " + BeFloat(Pix, o).ToString("0.######", ci) + " " + BeFloat(Pix, o + 4).ToString("0.######", ci) + " " + BeFloat(Pix, o + 8).ToString("0.######", ci));
                }
                if (uv != null)
                    for (int v = 0; v < vcount; v++)
                    {
                        int o = voff + v * stride + uv.Offset;
                        sb.AppendLine("vt " + Half(Be.U16(Pix, o)).ToString("0.#####", ci) + " " + (1 - Half(Be.U16(Pix, o + 2))).ToString("0.#####", ci));
                    }
                var run = new List<int>();
                Action flush = () =>
                {
                    for (int t = 2; t < run.Count; t++)
                    {
                        int a = run[t - 2] + 1, b2 = run[t - 1] + 1, c = run[t] + 1;
                        if (a == b2 || b2 == c || a == c) continue;
                        if ((t & 1) != 0) { int s = a; a = b2; b2 = s; }
                        if (uv != null) sb.AppendLine("f " + a + "/" + a + " " + b2 + "/" + b2 + " " + c + "/" + c);
                        else sb.AppendLine("f " + a + " " + b2 + " " + c);
                    }
                    run.Clear();
                };
                foreach (int i in idx) { if (i == 0xFFFF) flush(); else run.Add(i); }
                flush();
                res.Add(new KeyValuePair<string, string>(Name + "_" + k + ".obj", sb.ToString()));
            }
        }
        if (mtl.Length > 0) res.Add(new KeyValuePair<string, string>(Name + ".mtl", mtl.ToString()));
        return res;
    }
    // BI/BW: 4 bone numbers and weights per vertex (null when the part has none), for the animations (Anim.cs).
    public class MeshPart { public string Tex; public float[] V, UV; public int[] F; public int N; public int VOff, Stride; public byte[] BI; public float[] BW; }

    // The vertex buffers as ExportObj numbers them: (part k of its heap = <Name>_k.obj, offset of the
    // first vertex in Pix, vertex count, stride); positions are the first 3 big-endian floats of each.
    public List<Tuple<int, int, int, int>> VertexParts()
    {
        var res = new List<Tuple<int, int, int, int>>();
        foreach (var h in Heaps)
        {
            var ix = h.Recs.Where(r => r.Tag == "IXBF").ToList();
            var vx = h.Recs.Where(r => r.Tag == "VXBF").ToList();
            for (int k = 0; k < ix.Count && k < vx.Count; k++)
            {
                var I = ix[k]; var V = vx[k];
                int count = (int)I.D / 2, ioff = (int)I.C;
                if (ioff + count * 2 > Pix.Length || count <= 0) continue;
                int max = 0;
                for (int i = 0; i < count; i++) { int x = Be.U16(Pix, ioff + i * 2); if (x != 0xFFFF && x > max) max = x; }
                int vcount = max + 1, vbytes = (int)V.D, voff = (int)V.C, stride = vbytes / vcount;
                if (voff + vbytes > Pix.Length || stride < 12) continue;
                res.Add(Tuple.Create(k, voff, vcount, stride));
            }
        }
        return res;
    }

    // A texture with its mip chain (RGBA, top row first).
    public class Img
    {
        public int Levels; public byte[][] Px; public int[] W, H;
        public static Img Make(byte[] rgba, int w, int h)
        {
            var list = new List<byte[]> { rgba }; var ws = new List<int> { w }; var hs = new List<int> { h };
            while (w > 1 && h > 1 && list.Count < 12)
            {
                int nw = w / 2, nh = h / 2; var src = list[list.Count - 1]; var dst = new byte[nw * nh * 4];
                for (int y = 0; y < nh; y++)
                    for (int x = 0; x < nw; x++)
                        for (int c = 0; c < 4; c++)
                        {
                            int a = ((2 * y) * w + 2 * x) * 4 + c, b = a + 4, d = a + w * 4, e = d + 4;
                            dst[(y * nw + x) * 4 + c] = (byte)((src[a] + src[b] + src[d] + src[e] + 2) >> 2);
                        }
                list.Add(dst); ws.Add(nw); hs.Add(nh); w = nw; h = nh;
            }
            return new Img { Levels = list.Count, Px = list.ToArray(), W = ws.ToArray(), H = hs.ToArray() };
        }
    }

    // Textured parts of the default variant (shadow-caster variants are left out), read straight from the buffers.
    public List<MeshPart> Parts()
    {
        var res = new List<MeshPart>();
        ParseMesh();
        var mats = MaterialTextures();
        var info = PartInfo();
        int gbase = 0;
        foreach (var h in Heaps)
        {
            var ix = h.Recs.Where(r => r.Tag == "IXBF").ToList();
            var vx = h.Recs.Where(r => r.Tag == "VXBF").ToList();
            var va = h.Recs.Where(r => r.Tag == "VXAR").ToList();
            int gb = gbase; gbase += Math.Min(ix.Count, vx.Count);
            for (int k = 0; k < ix.Count && k < vx.Count; k++)
            {
                int gk = gb + k;
                if (gk < info.Count && info[gk].Item2 != 0) continue;
                string tex = null;
                if (gk < info.Count && info[gk].Item1 >= 0 && info[gk].Item1 < mats.Count && mats[info[gk].Item1].Count > 0) tex = ColorTexture(mats[info[gk].Item1]);
                if (tex == null || k >= va.Count) continue;
                var lay = Layout(h, va[k]);
                var uv = lay.FirstOrDefault(a => a.Usage == 5 && a.Index == 0 && (a.Format == 31 || a.Format == 32));
                if (uv == null) continue;
                var bw = lay.FirstOrDefault(a => a.Usage == 1 && a.Format == 6);
                var bi = lay.FirstOrDefault(a => a.Usage == 2 && a.Format == 6);
                var I = ix[k]; var V = vx[k];
                int count = (int)I.D / 2, ioff = (int)I.C;
                if (count <= 0 || ioff + count * 2 > Pix.Length) continue;
                var idx = new int[count];
                int max = 0;
                for (int i = 0; i < count; i++) { idx[i] = Be.U16(Pix, ioff + i * 2); if (idx[i] != 0xFFFF && idx[i] > max) max = idx[i]; }
                int vcount = max + 1, vbytes = (int)V.D, voff = (int)V.C;
                int stride = vbytes / vcount;
                if (voff + vbytes > Pix.Length || stride < 12 || uv.Offset + 4 > stride) continue;
                var pt = new MeshPart { Tex = tex, N = vcount, V = new float[vcount * 3], UV = new float[vcount * 2], VOff = voff, Stride = stride };
                for (int v = 0; v < vcount; v++)
                {
                    int o = voff + v * stride;
                    pt.V[v * 3] = BeFloat(Pix, o); pt.V[v * 3 + 1] = BeFloat(Pix, o + 4); pt.V[v * 3 + 2] = BeFloat(Pix, o + 8);
                    int q = o + uv.Offset;
                    pt.UV[v * 2] = Half(Be.U16(Pix, q)); pt.UV[v * 2 + 1] = Half(Be.U16(Pix, q + 2));
                }
                if (bw != null && bi != null && bw.Offset + 4 <= stride && bi.Offset + 4 <= stride)
                {
                    pt.BI = new byte[vcount * 4]; pt.BW = new float[vcount * 4];
                    for (int v = 0; v < vcount; v++)
                        for (int j = 0; j < 4; j++)
                        {
                            int o = voff + v * stride;
                            pt.BI[v * 4 + j] = Pix[o + bi.Offset + j];
                            pt.BW[v * 4 + j] = Pix[o + bw.Offset + j] / 255f;
                        }
                }
                var faces = new List<int>(count * 3);
                int ra = -1, rb = -1, n = 0;  // strip walk: the last two indices of the current run
                foreach (int i in idx)
                {
                    if (i == 0xFFFF) { n = 0; continue; }
                    if (n >= 2 && ra != rb && rb != i && ra != i)
                    {
                        if ((n & 1) == 0) { faces.Add(ra); faces.Add(rb); faces.Add(i); }
                        else { faces.Add(rb); faces.Add(ra); faces.Add(i); }
                    }
                    ra = rb; rb = i; n++;
                }
                pt.F = faces.ToArray();
                res.Add(pt);
            }
        }
        return res;
    }

    // Software render (orthographic, z-buffer, mip-mapped nearest texel). tex: name -> image.
    public static byte[] Render(List<MeshPart> parts, Dictionary<string, Img> tex, int W, int H, double yaw, double pitch, double zoom, double panX = 0, double panY = 0)
    {
        var pix = new byte[W * H * 4];
        for (int i = 0; i < pix.Length; i += 4) { pix[i] = 30; pix[i + 1] = 30; pix[i + 2] = 34; pix[i + 3] = 255; }
        if (parts.Count == 0) return pix;
        float[] lo = { 1e9f, 1e9f, 1e9f }, hi = { -1e9f, -1e9f, -1e9f };
        foreach (var pt in parts) for (int i = 0; i < pt.V.Length; i++) { int a = i % 3; lo[a] = Math.Min(lo[a], pt.V[i]); hi[a] = Math.Max(hi[a], pt.V[i]); }
        double cx0 = (lo[0] + hi[0]) / 2, cy = (lo[1] + hi[1]) / 2, cz0 = (lo[2] + hi[2]) / 2;
        double ext = Math.Max(hi[1] - lo[1], Math.Max(hi[0] - lo[0], hi[2] - lo[2]));
        double s = H * 0.9 / Math.Max(0.001, ext) * zoom;
        double cyaw = Math.Cos(yaw), syaw = Math.Sin(yaw), cp = Math.Cos(pitch), sp = Math.Sin(pitch);
        var zb = new float[W * H];
        for (int i = 0; i < zb.Length; i++) zb[i] = -1e9f;
        foreach (var pt in parts)
        {
            Img img = null;
            tex.TryGetValue(pt.Tex, out img);
            var sx = new float[pt.N]; var sy = new float[pt.N]; var sz = new float[pt.N];
            for (int i = 0; i < pt.N; i++)
            {
                double vx = pt.V[i * 3], vy = pt.V[i * 3 + 1], vz = pt.V[i * 3 + 2];
                vx -= cx0; vz -= cz0;
                double x = vx * cyaw + vz * syaw, z = -vx * syaw + vz * cyaw, y = vy - cy;
                double y2 = y * cp - z * sp, z2 = y * sp + z * cp;
                sx[i] = (float)(W / 2 + x * s + panX * W); sy[i] = (float)(H / 2 - y2 * s + panY * H); sz[i] = (float)z2;
            }
            var F = pt.F; var UV = pt.UV;
            for (int fi = 0; fi + 2 < F.Length; fi += 3)
            {
                int i0 = F[fi], i1 = F[fi + 1], i2 = F[fi + 2];
                float ax = sx[i0], ay = sy[i0], bx = sx[i1], by = sy[i1], cx = sx[i2], cy2 = sy[i2];
                int minx = (int)Math.Max(0, Math.Floor(Math.Min(ax, Math.Min(bx, cx)))), maxx = (int)Math.Min(W - 1, Math.Ceiling(Math.Max(ax, Math.Max(bx, cx))));
                int miny = (int)Math.Max(0, Math.Floor(Math.Min(ay, Math.Min(by, cy2)))), maxy = (int)Math.Min(H - 1, Math.Ceiling(Math.Max(ay, Math.Max(by, cy2))));
                if (minx > maxx || miny > maxy) continue;
                float den = (by - cy2) * (ax - cx) + (cx - bx) * (ay - cy2);
                if (Math.Abs(den) < 1e-6f) continue;
                float u0 = UV[i0 * 2], v0 = UV[i0 * 2 + 1], u1 = UV[i1 * 2], v1 = UV[i1 * 2 + 1], u2 = UV[i2 * 2], v2 = UV[i2 * 2 + 1];
                int lvl = 0; byte[] lp = null; int lw = 0, lh = 0;
                if (img != null)
                {
                    double texelArea = Math.Abs((u1 - u0) * (v2 - v0) - (u2 - u0) * (v1 - v0)) * img.W[0] * img.H[0];
                    double ratio = texelArea / (Math.Abs(den) * 0.5);
                    if (ratio > 1) lvl = Math.Min(img.Levels - 1, (int)Math.Round(0.5 * Math.Log(ratio, 2)));
                    lp = img.Px[lvl]; lw = img.W[lvl]; lh = img.H[lvl];
                }
                float dw0 = (by - cy2) / den, dw1 = (cy2 - ay) / den;
                float shade = 1;
                {   // flat light for untextured parts: facing the viewer is brighter
                    float ex1 = bx - ax, ey1 = by - ay, ez1 = sz[i1] - sz[i0], ex2 = cx - ax, ey2 = cy2 - ay, ez2 = sz[i2] - sz[i0];
                    float nx = ey1 * ez2 - ez1 * ey2, ny = ez1 * ex2 - ex1 * ez2, nz = ex1 * ey2 - ey1 * ex2;
                    float nl = (float)Math.Sqrt(nx * nx + ny * ny + nz * nz);
                    shade = nl < 1e-9f ? 1 : 0.3f + 0.7f * Math.Abs(nz) / nl;
                }
                float z0 = sz[i0], z1 = sz[i1], z2 = sz[i2];
                for (int py = miny; py <= maxy; py++)
                {
                    float w0 = ((by - cy2) * (minx - cx) + (cx - bx) * (py - cy2)) / den, w1 = ((cy2 - ay) * (minx - cx) + (ax - cx) * (py - cy2)) / den;
                    for (int px = minx; px <= maxx; px++, w0 += dw0, w1 += dw1)
                    {
                        float w2 = 1 - w0 - w1;
                        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                        float z = w0 * z0 + w1 * z1 + w2 * z2;
                        int o = py * W + px;
                        if (z < zb[o]) continue;
                        byte r = (byte)(170 * shade), g = (byte)(170 * shade), b = (byte)(180 * shade);
                        if (lp != null)
                        {
                            float u = w0 * u0 + w1 * u1 + w2 * u2, v = w0 * v0 + w1 * v1 + w2 * v2;
                            int tx = (int)Math.Floor((u - Math.Floor(u)) * lw), ty = (int)Math.Floor((v - Math.Floor(v)) * lh);
                            if (tx >= lw) tx = lw - 1; if (ty >= lh) ty = lh - 1;
                            int q = (ty * lw + tx) * 4;
                            if (lp[q + 3] < 64) continue;  // cut-out (hair, lashes)
                            r = lp[q]; g = lp[q + 1]; b = lp[q + 2];
                        }
                        zb[o] = z;
                        pix[o * 4] = r; pix[o * 4 + 1] = g; pix[o * 4 + 2] = b; pix[o * 4 + 3] = 255;
                    }
                }
            }
        }
        return pix;
    }

    // Header only (names, layout): no .MDP needed, used to index the textures shared between models.
    public static MdlFile LoadHeader(string name, byte[] vir) { return Load(name, vir, new byte[0x40]); }

    // All parts' triangles as 3D points (x,y,z per vertex, 3 vertices per triangle).
    public List<float[]> Triangles()
    {
        var tris = new List<float[]>();
        foreach (var kv in ExportObj())
        {
            if (!kv.Key.EndsWith(".obj")) continue;
            var v = new List<float[]>();
            foreach (var line in kv.Value.Split('\n'))
            {
                var p = line.Trim().Split(' ');
                if (p[0] == "v") v.Add(new[] { float.Parse(p[1], CultureInfo.InvariantCulture), float.Parse(p[2], CultureInfo.InvariantCulture), float.Parse(p[3], CultureInfo.InvariantCulture) });
                else if (p[0] == "f")
                {
                    var t = new float[9];
                    bool ok = true;
                    for (int k = 0; k < 3 && ok; k++) { int vi = int.Parse(p[k + 1].Split('/')[0]) - 1; if (vi < 0 || vi >= v.Count) { ok = false; break; } var q = v[vi]; t[k * 3] = q[0]; t[k * 3 + 1] = q[1]; t[k * 3 + 2] = q[2]; }
                    if (ok) tris.Add(t);
                }
            }
        }
        return tris;
    }

    // ---- the mesh section: nested records [tag][size][fields...] before the HEAP ----
    public class Rec { public string Tag; public int Pos, Size; public uint[] F; }
    public List<Rec> Records = new List<Rec>();
    public List<string> Strings = new List<string>();

    void Walk(int p, int end)
    {
        while (p + 8 <= end && p + 8 <= Vir.Length)
        {
            string tag = TagStr(Vir, p);
            int size = (int)Be.U32(Vir, p + 4);
            tag = tag.TrimEnd('\0');
            bool ascii = tag.Length >= 3 && tag.All(c => (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'));
            if (!ascii || size < 0 || p + 8 + size > end) break;
            var r = new Rec { Tag = tag, Pos = p, Size = size };
            int n = Math.Max(0, size / 4);
            r.F = new uint[n];
            for (int i = 0; i < n; i++) r.F[i] = Be.U32(Vir, p + 8 + i * 4);
            Records.Add(r);
            if (tag == "MESH") Walk(p + 8, p + 8 + size);
            if (tag == "STRB") Walk(p + 12, p + 8 + size);
            if (tag == "STRL")
            {
                int s = p + 8, e = p + 8 + size;
                while (s < e)
                {
                    int q = s; while (q < e && Vir[q] != 0) q++;
                    Strings.Add(Encoding.ASCII.GetString(Vir, s, q - s));
                    s = q + 1;
                }
            }
            p += 8 + size;
        }
    }

    public void ParseMesh()
    {
        Records.Clear(); Strings.Clear();
        for (int o = 0; o + 16 < Vir.Length; o += 0x10)
            if (Tag4(Vir, o, "MESH")) { Walk(o, Math.Min(Vir.Length, o + 8 + (int)Be.U32(Vir, o + 4))); break; }
    }

    string Str(uint i) { return i < Strings.Count ? Strings[(int)i] : "#" + i.ToString("X"); }

    // texture names of each SAMP (material k = k-th SAMP): sampler slot -> texture
    public List<List<string>> MaterialTextures()
    {
        if (Records.Count == 0) ParseMesh();
        var res = new List<List<string>>();
        foreach (var r in Records.Where(r => r.Tag == "SAMP"))
        {
            var tex = new List<string>();
            for (int i = 0; i + 3 < r.F.Length; i++)
                if (r.F[i] == 0x53535456) { tex.Add(Str(r.F[i + 3])); i += 4; }
            res.Add(tex);
        }
        return res;
    }

    // The colour texture of a material: names end in _A (colour), _N (normal), _G (gloss), _C...; maps
    // also list light maps (UV_n) and water/noise layers. The first _A one, else the first that is not
    // one of the others.
    public static string ColorTexture(List<string> tex)
    {
        Func<string, bool> isA = n => { string u = n.ToUpperInvariant(); return u.Contains("_A_") || u.EndsWith("_A") || u.Contains("_A.") || u.EndsWith("_A_DDS") || u.EndsWith("_A_PSD") || u.EndsWith("_A_TGA"); };
        Func<string, bool> aux = n => { string u = n.ToUpperInvariant(); return u.StartsWith("UV_") || u.Contains("_N_") || u.EndsWith("_N") || u.Contains("_G_") || u.Contains("NOIZ") || u.Contains("SHADOW"); };
        foreach (var n in tex) if (isA(n)) return n;
        foreach (var n in tex) if (!aux(n)) return n;
        return tex.Count > 0 ? tex[0] : null;
    }

    // material index of each part (IXBF order): the PRIMs of the VARIs in order
    public List<int> PartMaterials() { return PartInfo().Select(t => t.Item1).ToList(); }

    // (material, variant) of each part; variant 0 is the one drawn, the others cast shadows
    public List<Tuple<int, int>> PartInfo()
    {
        if (Records.Count == 0) ParseMesh();
        var res = new List<Tuple<int, int>>();
        int vi = 0;
        foreach (var v in Records.Where(r => r.Tag == "VARI"))
        {
            for (int i = 0; i + 7 < v.F.Length; i++)
                if (v.F[i] == 0x5052494D) { res.Add(Tuple.Create((int)v.F[i + 7], vi)); i += 8; }
            vi++;
        }
        return res;
    }

    public class VAttr { public int Offset, Format, Usage, Index; }

    // vertex layout from the VXAR record: triples [offset][format][usage<<16 | index<<8]
    public List<VAttr> Layout(Heap h, HeapRec vxar)
    {
        var res = new List<VAttr>();
        int pos = h.HeaderBase + (int)vxar.Off, end = Math.Min(Vir.Length - 12, pos + (int)vxar.Size);
        int cur = 0;
        // Header of 14 words (word 6 = element count), then the elements as triples. Characters and
        // maps differ in the low byte of the usage word (0x78 on map geometry).
        if (pos + 14 * 4 <= end && Be.U32(Vir, pos) == 0x00100005u)
        {
            int n = (int)Be.U32(Vir, pos + 24);
            for (int i = 0, q = pos + 56; i < n && q + 12 <= end + 12; i++, q += 12)
            {
                uint fmt = Be.U32(Vir, q), use = Be.U32(Vir, q + 4), next = Be.U32(Vir, q + 8);
                res.Add(new VAttr { Offset = cur, Format = (int)(fmt & 0x3F), Usage = (int)((use >> 16) & 0xFF), Index = (int)((use >> 8) & 0xFF) });
                cur = (int)next;
            }
            return res;
        }
        for (int q = pos; q < end; q += 4)
        {
            // element = [format][usage<<16 | index<<8][offset of the next element]
            uint fmt = Be.U32(Vir, q), use = Be.U32(Vir, q + 4), next = Be.U32(Vir, q + 8);
            bool ok = (fmt & 0xFF000000u) == 0 && ((fmt >> 16) & 0xFF) >= 0x10 && ((fmt >> 16) & 0xFF) <= 0x3F && (use & 0xFF000000u) == 0 && ((use >> 16) & 0xFF) < 0x10 && next < 0x100;
            if (!ok) { if (res.Count > 0) break; continue; }
            res.Add(new VAttr { Offset = cur, Format = (int)(fmt & 0x3F), Usage = (int)((use >> 16) & 0xFF), Index = (int)((use >> 8) & 0xFF) });
            cur = (int)next;
            q += 8;
        }
        return res;
    }
    public string DumpMaterials()
    {
        ParseMesh();
        var sb = new StringBuilder();
        sb.AppendLine(Records.Count + " records, " + Strings.Count + " strings");
        foreach (var g in Records.GroupBy(r => r.Tag)) sb.Append(g.Key + "x" + g.Count() + " ");
        sb.AppendLine();
        foreach (var r in Records)
        {
            if (r.Tag != "MATE" && r.Tag != "SAMP" && r.Tag != "SSTV" && r.Tag != "PRIM" && r.Tag != "VARI") continue;
            sb.Append(r.Tag + " @" + r.Pos.ToString("X") + ":");
            foreach (var f in r.F) sb.Append(" " + (f < Strings.Count ? "[" + f + "]" + Strings[(int)f] : f.ToString("X")));
            sb.AppendLine();
        }
        return sb.ToString();
    }

    public string Summary()
    {
        var sb = new StringBuilder();
        sb.AppendLine(Name + ": " + Heaps.Count + " heap(s), MDV " + Vir.Length + " bytes, MDP " + Pix.Length + " bytes");
        foreach (var h in Heaps)
        {
            var g = h.Recs.GroupBy(r => r.Tag).Select(x => x.Key + "x" + x.Count());
            sb.AppendLine("  heap @" + h.Start.ToString("X") + ": " + string.Join(" ", g));
            if (Environment.GetEnvironmentVariable("NT_RECS") != null)
                foreach (var r in h.Recs.Where(r => Environment.GetEnvironmentVariable("NT_RECS") == "all" || r.Tag == "IXBF" || r.Tag == "VXBF" || r.Tag == "VXST" || r.Tag == "VXAR" || r.Tag == "VXBO"))
                {
                    if (r.Tag == "VXBO") { int p0 = h.HeaderBase + (int)r.Off; var hx = new StringBuilder(); for (int q = 0; q < (int)r.Size && p0 + q < Vir.Length; q += 4) hx.Append(Be.U32(Vir, p0 + q).ToString("X") + " "); sb.AppendLine("    VXBODATA " + hx); }
                    if (r.Tag == "VXAR") { int p0 = h.HeaderBase + (int)r.Off; var hx = new StringBuilder(); for (int q = 0; q < (int)r.Size && p0 + q < Vir.Length; q++) { hx.Append(Vir[p0 + q].ToString("X2")); if ((q & 3) == 3) hx.Append(' '); } sb.AppendLine("    VXARDATA " + hx); }
                    sb.AppendLine("    " + r.Tag + " A=" + r.A.ToString("X") + " B=" + r.B.ToString("X") + " off=" + r.Off.ToString("X") + " size=" + r.Size.ToString("X") + " C=" + r.C.ToString("X") + " D=" + r.D.ToString("X") + " E=" + r.E.ToString("X"));
                }
        }
        if (Environment.GetEnvironmentVariable("NT_BIDX") != null) sb.Append(BoneIndexStats());
        var tex = Textures();
        foreach (var t in tex.Textures) sb.AppendLine("  tex " + t.Name + " " + t.Width + "x" + t.Height + " " + t.FormatName);
        return sb.ToString();
    }

    // Per part: the distinct values of the skinning-index attribute (usage 2) and their range.
    public string BoneIndexStats()
    {
        var sb = new StringBuilder();
        int part = 0;
        foreach (var h in Heaps)
        {
            var ix = h.Recs.Where(r => r.Tag == "IXBF").ToList();
            var vx = h.Recs.Where(r => r.Tag == "VXBF").ToList();
            var va = h.Recs.Where(r => r.Tag == "VXAR").ToList();
            for (int k = 0; k < ix.Count && k < vx.Count && k < va.Count; k++, part++)
            {
                var lay = Layout(h, va[k]);
                var bi = lay.FirstOrDefault(a => a.Usage == 2); var bw = lay.FirstOrDefault(a => a.Usage == 1);
                if (bi == null) { sb.AppendLine("  part " + part + ": sem indices"); continue; }
                int count = (int)ix[k].D / 2, ioff = (int)ix[k].C, max = 0;
                for (int i = 0; i < count; i++) { int x = Be.U16(Pix, ioff + i * 2); if (x != 0xFFFF && x > max) max = x; }
                int vcount = max + 1, stride = (int)vx[k].D / vcount, voff = (int)vx[k].C;
                var seen = new SortedSet<int>();
                for (int v = 0; v < vcount; v++)
                    for (int j = 0; j < 4; j++)
                    {
                        int w = bw != null ? Pix[voff + v * stride + bw.Offset + j] : 1;
                        if (w != 0) seen.Add(Pix[voff + v * stride + bi.Offset + j]);
                    }
                sb.AppendLine("  part " + part + " (" + vcount + " v, stride " + stride + ", fmt " + bi.Format + "/" + (bw != null ? bw.Format : -1) + "): " + string.Join(",", seen));
                if (Environment.GetEnvironmentVariable("NT_BIDX") == "v")
                {
                    sb.AppendLine("    layout: " + string.Join(" ", lay.Select(a => a.Usage + "." + a.Index + "@" + a.Offset + "/f" + a.Format)));
                    for (int v = 0; v < vcount; v += Math.Max(1, vcount / 4))
                    {
                        int o = voff + v * stride;
                        var hx = new StringBuilder();
                        for (int q = 0; q < stride; q++) { hx.Append(Pix[o + q].ToString("X2")); if ((q & 3) == 3) hx.Append(' '); }
                        sb.AppendLine("    v" + v + ": " + BeFloat(Pix, o).ToString("F3") + "," + BeFloat(Pix, o + 4).ToString("F3") + "," + BeFloat(Pix, o + 8).ToString("F3") + "  " + hx);
                    }
                }
            }
        }
        return sb.ToString();
    }
}
