// Binary FBX (7.x) reader: meshes (positions, polygons, UVs, materials and their textures), the skeleton
// (LimbNode / Null models, parent, rest pose from the Lcl values) and the skin (Cluster weights per control
// point). Enough for a character exported from Blender, Mixamo or a game extractor (Outfit.cs).
//
// Node record: end offset, property count, property bytes (u32, or u64 from version 7500), name length (u8),
// name, properties, children, then a null record. Properties: Y i16, C bool, I i32, F f32, D f64, L i64,
// S/R u32 length + bytes, f/d/l/i/b arrays (count, encoding 1 = zlib, byte length). Objects link by
// Connections: C "OO" child parent, C "OP" child parent property.
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;

public class FbxNode
{
    public string Name; public List<object> P = new List<object>(); public List<FbxNode> Kids = new List<FbxNode>();
    public FbxNode this[string n] { get { return Kids.FirstOrDefault(k => k.Name == n); } }
    public IEnumerable<FbxNode> All(string n) { return Kids.Where(k => k.Name == n); }
}

public class FbxModel
{
    public class Bone { public long Id; public string Name; public int Parent = -1; public double[] World; public double[] Local; public double[] Outer; }
    // An animation (AnimationStack), sampled at 30 frames a second: each bone's local matrix (its root bones: with the
    // world of the non-bone models above them, Bone.Outer) per frame.
    public class Take { public string Name; public int Frames; public double[][][] Local; }
    public List<Take> Takes = new List<Take>();
    public class Mesh
    {
        public string Name; public double[] World;
        public double[] Pos;                 // control points (x, y, z), in the scene
        public List<int[]> Polys = new List<int[]>();       // control point numbers
        public List<double[]> PolyUv = new List<double[]>(); // per polygon: u, v per corner
        public List<int> PolyMat = new List<int>();
        public List<string> MatTex = new List<string>();     // per material slot: texture file (or null)
        public List<List<string>> MatTexAll = new List<List<string>>();  // per slot: every texture file, colour-like first
        public List<Dictionary<int, double>> Weights;        // per control point: bone -> weight
    }
    public List<Bone> Bones = new List<Bone>();
    public List<Mesh> Meshes = new List<Mesh>();
    public int Version;

    // ---- parsing ----
    public static FbxModel Load(string path)
    {
        var d = File.ReadAllBytes(path);
        if (d.Length < 27 || Encoding.ASCII.GetString(d, 0, 18) != "Kaydara FBX Binary") throw new Exception("FBX binario esperado (exporte como binario)");
        var m = new FbxModel { Version = (int)BitConverter.ToUInt32(d, 23) };
        var root = new FbxNode { Name = "" };
        int pos = 27;
        while (true) { var n = ReadNode(d, ref pos, m.Version >= 7500); if (n == null) break; root.Kids.Add(n); }
        m.Build(root, Path.GetDirectoryName(Path.GetFullPath(path)));
        return m;
    }

    static FbxNode ReadNode(byte[] d, ref int pos, bool wide)
    {
        long end, count, plen;
        if (wide) { end = BitConverter.ToInt64(d, pos); count = BitConverter.ToInt64(d, pos + 8); plen = BitConverter.ToInt64(d, pos + 16); pos += 24; }
        else { end = BitConverter.ToUInt32(d, pos); count = BitConverter.ToUInt32(d, pos + 4); plen = BitConverter.ToUInt32(d, pos + 8); pos += 12; }
        int nlen = d[pos++];
        if (end == 0) return null;
        var n = new FbxNode { Name = Encoding.ASCII.GetString(d, pos, nlen) };
        pos += nlen;
        for (long i = 0; i < count; i++) n.P.Add(ReadProp(d, ref pos));
        while (pos < end)
        {
            var k = ReadNode(d, ref pos, wide);
            if (k == null) break;
            n.Kids.Add(k);
        }
        pos = (int)end;
        return n;
    }

    // (Skip/Take walked the file from its start for every block: a 20 MB FBX took minutes.)
    static byte[] Slice(byte[] d, int at, int len) { var r = new byte[len]; Buffer.BlockCopy(d, at, r, 0, len); return r; }

    static object ReadProp(byte[] d, ref int pos)
    {
        char t = (char)d[pos++];
        switch (t)
        {
            case 'Y': pos += 2; return (long)BitConverter.ToInt16(d, pos - 2);
            case 'C': pos += 1; return (long)d[pos - 1];
            case 'I': pos += 4; return (long)BitConverter.ToInt32(d, pos - 4);
            case 'F': pos += 4; return (double)BitConverter.ToSingle(d, pos - 4);
            case 'D': pos += 8; return BitConverter.ToDouble(d, pos - 8);
            case 'L': pos += 8; return BitConverter.ToInt64(d, pos - 8);
            case 'S': case 'R':
                {
                    int len = (int)BitConverter.ToUInt32(d, pos); pos += 4;
                    object r = t == 'S' ? (object)Encoding.UTF8.GetString(d, pos, len) : Slice(d, pos, len);
                    pos += len; return r;
                }
            default:
                {
                    int count = (int)BitConverter.ToUInt32(d, pos), enc = (int)BitConverter.ToUInt32(d, pos + 4), blen = (int)BitConverter.ToUInt32(d, pos + 8);
                    pos += 12;
                    byte[] raw;
                    if (enc == 1)
                    {
                        using (var ms = new MemoryStream(d, pos + 2, blen - 2))
                        using (var z = new DeflateStream(ms, CompressionMode.Decompress))
                        using (var o = new MemoryStream()) { z.CopyTo(o); raw = o.ToArray(); }
                    }
                    else raw = Slice(d, pos, blen);
                    pos += blen;
                    switch (t)
                    {
                        case 'f': return Enumerable.Range(0, count).Select(i => (double)BitConverter.ToSingle(raw, i * 4)).ToArray();
                        case 'd': return Enumerable.Range(0, count).Select(i => BitConverter.ToDouble(raw, i * 8)).ToArray();
                        case 'l': return Enumerable.Range(0, count).Select(i => BitConverter.ToInt64(raw, i * 8)).ToArray();
                        case 'i': return Enumerable.Range(0, count).Select(i => BitConverter.ToInt32(raw, i * 4)).ToArray();
                        case 'b': return raw;
                        default: throw new Exception("FBX: propriedade desconhecida " + t);
                    }
                }
        }
    }

    // ---- matrices: 4x4 row-major, column vectors (p' = M p), translation in [3], [7], [11] ----
    public static double[] Mul(double[] a, double[] b)
    {
        var r = new double[16];
        for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { double s = 0; for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j]; r[i * 4 + j] = s; }
        return r;
    }
    public static double[] Ident() { return new double[] { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 }; }
    static double[] Tr(double[] t) { var m = Ident(); m[3] = t[0]; m[7] = t[1]; m[11] = t[2]; return m; }
    static double[] Sc(double[] s) { var m = Ident(); m[0] = s[0]; m[5] = s[1]; m[10] = s[2]; return m; }
    // Euler XYZ in degrees (FBX's default order: X first): Rz * Ry * Rx.
    static double[] Rot(double[] e)
    {
        double x = e[0] * Math.PI / 180, y = e[1] * Math.PI / 180, z = e[2] * Math.PI / 180;
        var rx = new double[] { 1, 0, 0, 0, 0, Math.Cos(x), -Math.Sin(x), 0, 0, Math.Sin(x), Math.Cos(x), 0, 0, 0, 0, 1 };
        var ry = new double[] { Math.Cos(y), 0, Math.Sin(y), 0, 0, 1, 0, 0, -Math.Sin(y), 0, Math.Cos(y), 0, 0, 0, 0, 1 };
        var rz = new double[] { Math.Cos(z), -Math.Sin(z), 0, 0, Math.Sin(z), Math.Cos(z), 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        return Mul(rz, Mul(ry, rx));
    }
    public static double[] Apply(double[] m, double x, double y, double z)
    {
        return new[] { m[0] * x + m[1] * y + m[2] * z + m[3], m[4] * x + m[5] * y + m[6] * z + m[7], m[8] * x + m[9] * y + m[10] * z + m[11] };
    }

    static double[] Prop3(FbxNode obj, string name, double[] def)
    {
        var p70 = obj["Properties70"]; if (p70 == null) return def;
        foreach (var p in p70.All("P"))
            if (p.P.Count >= 7 && (string)p.P[0] == name) return new[] { Convert.ToDouble(p.P[4]), Convert.ToDouble(p.P[5]), Convert.ToDouble(p.P[6]) };
        return def;
    }
    static string Prop(FbxNode obj, string name)
    {
        var p70 = obj["Properties70"]; if (p70 == null) return null;
        foreach (var p in p70.All("P")) if (p.P.Count >= 5 && (string)p.P[0] == name) return p.P[4] as string;
        return null;
    }
    static string NameOf(FbxNode o) { var s = o.P.Count > 1 ? o.P[1] as string ?? "" : ""; int z = s.IndexOf('\0'); return z >= 0 ? s.Substring(0, z) : s; }
    static string ClassOf(FbxNode o) { return o.P.Count > 2 ? o.P[2] as string ?? "" : ""; }

    // Local transform: T * Rpre * R * Rpost^-1 * S (pivots and offsets left out: exporters of game rigs keep them 0).
    static double[] LocalOf(FbxNode o) { return LocalWith(o, null, null); }
    static double[] LocalWith(FbxNode o, double[] tAt, double[] rAt)
    {
        var t = tAt ?? Prop3(o, "Lcl Translation", new double[3]); var r = rAt ?? Prop3(o, "Lcl Rotation", new double[3]);
        var s = Prop3(o, "Lcl Scaling", new double[] { 1, 1, 1 }); var pre = Prop3(o, "PreRotation", new double[3]);
        var post = Prop3(o, "PostRotation", new double[3]);
        var rpost = Rot(post); var rpostInv = new double[16];
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) rpostInv[i * 4 + j] = rpost[j * 4 + i];
        rpostInv[15] = 1;
        return Mul(Tr(t), Mul(Rot(pre), Mul(Rot(r), Mul(rpostInv, Sc(s)))));
    }

    void Build(FbxNode root, string dir)
    {
        var objs = new Dictionary<long, FbxNode>();
        var objects = root["Objects"]; if (objects == null) throw new Exception("FBX sem Objects");
        foreach (var o in objects.Kids) if (o.P.Count > 0 && o.P[0] is long) objs[(long)o.P[0]] = o;
        var parentOf = new Dictionary<long, List<long>>();  // child -> parents (OO and OP)
        var linkProp = new Dictionary<Tuple<long, long>, string>();  // (child, parent) -> property of an OP link
        var childrenOf = new Dictionary<long, List<long>>();
        var conns = root["Connections"];
        if (conns != null)
            foreach (var c in conns.All("C"))
            {
                if (c.P.Count < 3) continue;
                long ch = Convert.ToInt64(c.P[1]), pa = Convert.ToInt64(c.P[2]);
                if (!parentOf.ContainsKey(ch)) parentOf[ch] = new List<long>(); parentOf[ch].Add(pa);
                if (c.P.Count >= 4 && c.P[3] is string) linkProp[Tuple.Create(ch, pa)] = (string)c.P[3];
                if (!childrenOf.ContainsKey(pa)) childrenOf[pa] = new List<long>(); childrenOf[pa].Add(ch);
            }
        Func<long, IEnumerable<FbxNode>> kids = id => childrenOf.ContainsKey(id) ? childrenOf[id].Where(objs.ContainsKey).Select(k => objs[k]) : Enumerable.Empty<FbxNode>();
        Func<long, IEnumerable<FbxNode>> parents = id => parentOf.ContainsKey(id) ? parentOf[id].Where(objs.ContainsKey).Select(k => objs[k]) : Enumerable.Empty<FbxNode>();

        // Every model's world transform.
        var world = new Dictionary<long, double[]>();
        Func<long, double[]> W = null;
        W = id =>
        {
            double[] w; if (world.TryGetValue(id, out w)) return w;
            var o = objs[id];
            var par = parents(id).FirstOrDefault(p => p.Name == "Model");
            w = LocalOf(o);
            if (par != null) w = Mul(W((long)par.P[0]), w);
            world[id] = w; return w;
        };
        // Bones: LimbNode models, and Null / Root models with a LimbNode under them.
        var models = objs.Values.Where(o => o.Name == "Model").ToList();
        var boneIx = new Dictionary<long, int>();
        foreach (var o in models)
        {
            string cls = ClassOf(o);
            bool limb = cls == "LimbNode" || cls == "Root" || (cls == "Null" && kids((long)o.P[0]).Any(k => k.Name == "Model" && ClassOf(k) == "LimbNode"));
            if (!limb) continue;
            boneIx[(long)o.P[0]] = Bones.Count;
            Bones.Add(new Bone { Id = (long)o.P[0], Name = NameOf(o), World = W((long)o.P[0]), Local = LocalOf(o) });
        }
        foreach (var b in Bones)
        {
            var par = parents(b.Id).FirstOrDefault(p => p.Name == "Model" && boneIx.ContainsKey((long)p.P[0]));
            if (par != null) b.Parent = boneIx[(long)par.P[0]];
            else { var outer = parents(b.Id).FirstOrDefault(p => p.Name == "Model"); b.Outer = outer != null ? W((long)outer.P[0]) : Ident(); }
        }
        // Animations (2026-11-18): AnimationStack > AnimationLayer > AnimationCurveNode (linked to a bone's "Lcl Rotation"
        // or "Lcl Translation") > AnimationCurve ("d|X", "d|Y", "d|Z": KeyTime in 1/46186158000 s, KeyValueFloat), read
        // as straight lines between the keys (exporters bake a key a frame).
        const double tick = 46186158000.0, fps = 30;
        foreach (var stack in objs.Values.Where(o => o.Name == "AnimationStack"))
        {
            var ch = new Dictionary<int, Tuple<long[], double[]>[]>();  // bone -> 6 channels (rotation x y z, translation x y z)
            long t0 = long.MaxValue, t1 = long.MinValue;
            foreach (var layer in kids((long)stack.P[0]).Where(k => k.Name == "AnimationLayer"))
                foreach (var cn in kids((long)layer.P[0]).Where(k => k.Name == "AnimationCurveNode"))
                {
                    long cid = (long)cn.P[0];
                    int bi = -1, baseCh = -1;
                    foreach (var pa in parentOf.ContainsKey(cid) ? parentOf[cid] : new List<long>())
                    {
                        string pr;
                        if (!boneIx.ContainsKey(pa) || !linkProp.TryGetValue(Tuple.Create(cid, pa), out pr)) continue;
                        if (pr == "Lcl Rotation") { bi = boneIx[pa]; baseCh = 0; }
                        else if (pr == "Lcl Translation") { bi = boneIx[pa]; baseCh = 3; }
                    }
                    if (bi < 0) continue;
                    if (!ch.ContainsKey(bi)) ch[bi] = new Tuple<long[], double[]>[6];
                    foreach (var cv in kids(cid).Where(k => k.Name == "AnimationCurve"))
                    {
                        string pr;
                        if (!linkProp.TryGetValue(Tuple.Create((long)cv.P[0], cid), out pr) || pr.Length < 3) continue;
                        int axis = pr.EndsWith("X") ? 0 : pr.EndsWith("Y") ? 1 : pr.EndsWith("Z") ? 2 : -1;
                        var kt = cv["KeyTime"] != null ? cv["KeyTime"].P[0] as long[] : null;
                        var kv = cv["KeyValueFloat"] != null ? cv["KeyValueFloat"].P[0] as double[] : null;
                        if (axis < 0 || kt == null || kv == null || kt.Length == 0 || kv.Length < kt.Length) continue;
                        ch[bi][baseCh + axis] = Tuple.Create(kt, kv);
                        t0 = Math.Min(t0, kt[0]); t1 = Math.Max(t1, kt[kt.Length - 1]);
                    }
                }
            if (ch.Count == 0 || t1 < t0) continue;
            int frames = Math.Max(1, (int)Math.Round((t1 - t0) / tick * fps) + 1);
            var take = new Take { Name = NameOf(stack), Frames = frames, Local = new double[frames][][] };
            Func<Tuple<long[], double[]>, long, double> eval = (c, t) =>
            {
                var k = c.Item1; var v = c.Item2;
                if (t <= k[0]) return v[0];
                for (int i = 1; i < k.Length; i++)
                    if (t <= k[i]) return v[i - 1] + (v[i] - v[i - 1]) * (t - k[i - 1]) / Math.Max(1.0, k[i] - k[i - 1]);
                return v[k.Length - 1];
            };
            for (int f = 0; f < frames; f++)
            {
                long t = t0 + (long)Math.Round(f * tick / fps);
                take.Local[f] = new double[Bones.Count][];
                for (int b = 0; b < Bones.Count; b++)
                {
                    var o = objs[Bones[b].Id];
                    Tuple<long[], double[]>[] c;
                    double[] r = null, tr = null;
                    if (ch.TryGetValue(b, out c))
                    {
                        var r0 = Prop3(o, "Lcl Rotation", new double[3]); var t0v = Prop3(o, "Lcl Translation", new double[3]);
                        r = new double[3]; tr = new double[3];
                        for (int a = 0; a < 3; a++) { r[a] = c[a] != null ? eval(c[a], t) : r0[a]; tr[a] = c[3 + a] != null ? eval(c[3 + a], t) : t0v[a]; }
                    }
                    var l = LocalWith(o, tr, r);
                    take.Local[f][b] = Bones[b].Parent < 0 ? Mul(Bones[b].Outer, l) : l;
                }
            }
            Takes.Add(take);
        }
        // Meshes.
        foreach (var g in objs.Values.Where(o => o.Name == "Geometry" && ClassOf(o) == "Mesh"))
        {
            long gid = (long)g.P[0];
            var model = parents(gid).FirstOrDefault(p => p.Name == "Model");
            var mesh = new Mesh { Name = NameOf(g) };
            mesh.World = model != null ? W((long)model.P[0]) : Ident();
            if (model != null)
            {   // geometric transform: the mesh's own offset, not passed to children
                var gt = Prop3(model, "GeometricTranslation", new double[3]); var gr = Prop3(model, "GeometricRotation", new double[3]); var gs = Prop3(model, "GeometricScaling", new double[] { 1, 1, 1 });
                mesh.World = Mul(mesh.World, Mul(Tr(gt), Mul(Rot(gr), Sc(gs))));
            }
            var v = g["Vertices"].P[0] as double[];
            mesh.Pos = new double[v.Length];
            for (int i = 0; i + 2 < v.Length; i += 3) { var p = Apply(mesh.World, v[i], v[i + 1], v[i + 2]); mesh.Pos[i] = p[0]; mesh.Pos[i + 1] = p[1]; mesh.Pos[i + 2] = p[2]; }
            var pvi = g["PolygonVertexIndex"].P[0] as int[];
            var cornerOf = new List<int>();  // polygon-vertex number of each corner
            var cur = new List<int>(); var curC = new List<int>();
            var polyCorners = new List<int[]>();
            for (int i = 0; i < pvi.Length; i++)
            {
                int x = pvi[i]; bool last = x < 0; if (last) x = ~x;
                cur.Add(x); curC.Add(i);
                if (last) { mesh.Polys.Add(cur.ToArray()); polyCorners.Add(curC.ToArray()); cur.Clear(); curC.Clear(); }
            }
            // UV (first layer)
            var luv = g["LayerElementUV"];
            double[] uv = luv != null && luv["UV"] != null ? luv["UV"].P[0] as double[] : null;
            int[] uvi = luv != null && luv["UVIndex"] != null ? luv["UVIndex"].P[0] as int[] : null;
            string uvMap = luv != null && luv["MappingInformationType"] != null ? (string)luv["MappingInformationType"].P[0] : "";
            for (int pi = 0; pi < mesh.Polys.Count; pi++)
            {
                var poly = mesh.Polys[pi]; var corners = polyCorners[pi];
                var res = new double[poly.Length * 2];
                for (int k = 0; k < poly.Length; k++)
                {
                    if (uv == null) continue;
                    int ix = uvMap == "ByControlPoint" || uvMap == "ByVertice" || uvMap == "ByVertex" ? poly[k] : corners[k];
                    if (uvi != null) ix = ix < uvi.Length ? uvi[ix] : 0;
                    if (ix >= 0 && ix * 2 + 1 < uv.Length) { res[k * 2] = uv[ix * 2]; res[k * 2 + 1] = uv[ix * 2 + 1]; }
                }
                mesh.PolyUv.Add(res);
            }
            // materials: the model's material connections, in order
            var lm = g["LayerElementMaterial"];
            int[] mats = lm != null && lm["Materials"] != null ? lm["Materials"].P[0] as int[] : null;
            string mmap = lm != null && lm["MappingInformationType"] != null ? (string)lm["MappingInformationType"].P[0] : "AllSame";
            for (int pi = 0; pi < mesh.Polys.Count; pi++) mesh.PolyMat.Add(mats == null || mats.Length == 0 ? 0 : mmap == "AllSame" ? mats[0] : mats[Math.Min(pi, mats.Length - 1)]);
            if (model != null)
                foreach (var mat in kids((long)model.P[0]).Where(k => k.Name == "Material"))
                {
                    string file = null;
                    // The colour map: the texture linked as DiffuseColor (a material also links normal, specular...
                    // maps: 2B's legs came out blue with the first one); else the first that is not such a map.
                    long mid = (long)mat.P[0];
                    Func<FbxNode, string> propOf = tx => { string pr; return linkProp.TryGetValue(Tuple.Create((long)tx.P[0], mid), out pr) ? pr : ""; };
                    var texs = kids(mid).Where(k => k.Name == "Texture").ToList();
                    texs = texs.OrderBy(tx => propOf(tx).IndexOf("Diffuse", StringComparison.OrdinalIgnoreCase) >= 0 ? 0
                        : System.Text.RegularExpressions.Regex.IsMatch(propOf(tx), "normal|bump|spec|shin|emiss|transp|reflect|height|rough|metal", System.Text.RegularExpressions.RegexOptions.IgnoreCase) ? 2 : 1).ToList();
                    var all = new List<string>();
                    foreach (var tex in texs)
                    {
                        string f = tex["RelativeFilename"] != null ? tex["RelativeFilename"].P[0] as string : null;
                        if (f == null && tex["FileName"] != null) f = tex["FileName"].P[0] as string;
                        if (f == null) f = Prop(tex, "FileName") ?? Prop(tex, "RelativeFilename");
                        if (f != null) all.Add(Path.IsPathRooted(f) ? f : Path.Combine(dir, f));
                    }
                    mesh.MatTexAll.Add(all);
                    foreach (var tex in texs)
                    {
                        string f = null;
                        if (tex["RelativeFilename"] != null) f = tex["RelativeFilename"].P[0] as string;
                        if ((f == null || !File.Exists(Path.Combine(dir, f))) && tex["FileName"] != null) f = tex["FileName"].P[0] as string;
                        if (f == null) f = Prop(tex, "FileName") ?? Prop(tex, "RelativeFilename");
                        if (f != null)
                        {
                            string full = Path.IsPathRooted(f) ? f : Path.Combine(dir, f);
                            if (!File.Exists(full)) full = Path.Combine(dir, Path.GetFileName(f));
                            file = full; break;
                        }
                    }
                    mesh.MatTex.Add(file);
                }
            // skin
            int npts = mesh.Pos.Length / 3;
            foreach (var skin in kids(gid).Where(k => k.Name == "Deformer" && ClassOf(k) == "Skin"))
                foreach (var cl in kids((long)skin.P[0]).Where(k => k.Name == "Deformer" && ClassOf(k) == "Cluster"))
                {
                    var bone = kids((long)cl.P[0]).FirstOrDefault(k => k.Name == "Model" && boneIx.ContainsKey((long)k.P[0]));
                    if (bone == null || cl["Indexes"] == null || cl["Weights"] == null) continue;
                    int bi = boneIx[(long)bone.P[0]];
                    var ix = cl["Indexes"].P[0] as int[]; var w = cl["Weights"].P[0] as double[];
                    if (mesh.Weights == null) mesh.Weights = Enumerable.Range(0, npts).Select(_ => new Dictionary<int, double>()).ToList();
                    for (int i = 0; i < ix.Length && i < w.Length; i++)
                        if (ix[i] >= 0 && ix[i] < npts && w[i] > 0) { double o; mesh.Weights[ix[i]].TryGetValue(bi, out o); mesh.Weights[ix[i]][bi] = o + w[i]; }
                }
            // a mesh without skin under a bone follows that bone
            if (mesh.Weights == null && model != null)
            {
                var pb = parents((long)model.P[0]).FirstOrDefault(p => p.Name == "Model" && boneIx.ContainsKey((long)p.P[0]));
                if (pb != null) { int bi = boneIx[(long)pb.P[0]]; mesh.Weights = Enumerable.Range(0, npts).Select(_ => new Dictionary<int, double> { { bi, 1.0 } }).ToList(); }
            }
            Meshes.Add(mesh);
        }
    }
}
