// NierTextureTool - a model's skeleton (CJF) and animations (CMF), posed and skinned for the 3D view.
//
// Formats (2026-11-15, NOTAS.md "Animacoes do Nier"):
// - A model .MDV (unpacked) is a KPKy of 3; top[2] is a KPKy whose item 1 is the joint list (CJF) and the
//   rest the animations (CMF).
// - CJF: "CJF\1" + name, @0x24 joint count, @0x2C names (36 bytes: hash + 32 chars), @0x34 parents (u16, -1 none),
//   @0x38 rest pose in model space (4x4, rows, row vectors: world = local * parent), @0x3C its inverse,
//   @0x40 48 bytes per joint: local rest (quaternion x y z w, translation x y z, 0, scale x y z, 0).
// - CMF: "CMF\2", @0x04 u16 frames, @0x06 u16 joints, @0x0C name, @0x94 offset of the rotation table, @0x98 of the
//   translation table (absolute; per joint a u32 relative to the table, 0 = the joint keeps its rest value).
//   A track: u16 n, n x u16 key frame (the last = frames - 1), aligned to 4, then n values: rotations 4 x int16
//   / 32767 (quaternion x y z w, replacing the local rotation), translations 3 floats (replacing the local one).
// - A vertex's bone numbers (usage 2, 8_8_8_8) pair byte by byte with its weights (usage 1, 8_8_8_8 /255);
//   bone number b is joint b + 1 of the CJF (joint 0 = N_ALL, the model's origin).
using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;

public class Skeleton
{
    public string[] Names;
    public int[] Parent;
    public float[][] Local;    // 12 floats: quaternion, translation, 0, scale, 0
    public float[][] RestWorld, InvRest;  // 16 floats, rows
    public int[] Order;        // parents before their children

    public static Skeleton Read(byte[] c)
    {
        if (c == null || c.Length < 0x44 || c[0] != 'C' || c[1] != 'J' || c[2] != 'F') return null;
        int n = (int)Be.U32(c, 0x24);
        int names = (int)Be.U32(c, 0x2C), par = (int)Be.U32(c, 0x34), rest = (int)Be.U32(c, 0x38), inv = (int)Be.U32(c, 0x3C), loc = (int)Be.U32(c, 0x40);
        if (n <= 0 || n > 1024 || loc + 48 * n > c.Length || inv + 64 * n > c.Length) return null;
        var s = new Skeleton { Names = new string[n], Parent = new int[n], Local = new float[n][], RestWorld = new float[n][], InvRest = new float[n][] };
        for (int j = 0; j < n; j++)
        {
            int o = names + 36 * j + 4, e = o;
            while (e < o + 32 && e < c.Length && c[e] != 0) e++;
            s.Names[j] = Encoding.ASCII.GetString(c, o, e - o);
            s.Parent[j] = (short)Be.U16(c, par + 2 * j);
            s.Local[j] = Floats(c, loc + 48 * j, 12);
            s.RestWorld[j] = Floats(c, rest + 64 * j, 16);
            s.InvRest[j] = Floats(c, inv + 64 * j, 16);
        }
        var order = new List<int>(); var done = new bool[n];
        for (int pass = 0; pass < n && order.Count < n; pass++)
            for (int j = 0; j < n; j++)
                if (!done[j] && (s.Parent[j] < 0 || s.Parent[j] >= n || done[s.Parent[j]])) { done[j] = true; order.Add(j); }
        s.Order = order.ToArray();
        return s;
    }

    internal static float[] Floats(byte[] d, int at, int count)
    {
        var f = new float[count];
        for (int i = 0; i < count; i++) f[i] = BitConverter.ToSingle(BitConverter.GetBytes(Be.U32(d, at + 4 * i)), 0);
        return f;
    }
}

public class Motion
{
    public class Track { public int[] Keys; public float[][] Vals; }
    public string Name;
    public int Frames;
    public Dictionary<int, Track> Rot = new Dictionary<int, Track>(), Pos = new Dictionary<int, Track>();

    public static Motion Read(byte[] d)
    {
        if (d == null || d.Length < 0xA0 || d[0] != 'C' || d[1] != 'M' || d[2] != 'F') return null;
        var m = new Motion { Frames = Be.U16(d, 4) };
        int joints = Be.U16(d, 6);
        int e = 0xC; while (e < 0x30 && d[e] != 0) e++;
        m.Name = Encoding.ASCII.GetString(d, 0xC, e - 0xC);
        Tracks(d, (int)Be.U32(d, 0x94), joints, 4, m.Rot);
        Tracks(d, (int)Be.U32(d, 0x98), joints, 3, m.Pos);
        return m;
    }

    static void Tracks(byte[] d, int table, int joints, int width, Dictionary<int, Track> into)
    {
        if (table <= 0 || table + 4 * joints > d.Length) return;
        for (int j = 0; j < joints; j++)
        {
            int o = (int)Be.U32(d, table + 4 * j);
            if (o == 0) continue;
            int a = table + o;
            if (a + 2 > d.Length) continue;
            int n = Be.U16(d, a);
            int q = (a + 2 + 2 * n + 3) & ~3;
            if (n == 0 || q + n * width * (width == 4 ? 2 : 4) > d.Length) continue;
            var t = new Track { Keys = new int[n], Vals = new float[n][] };
            for (int k = 0; k < n; k++)
            {
                t.Keys[k] = Be.U16(d, a + 2 + 2 * k);
                var v = new float[width];
                for (int c = 0; c < width; c++)
                    v[c] = width == 4 ? (short)Be.U16(d, q + 8 * k + 2 * c) / 32767f
                                      : BitConverter.ToSingle(BitConverter.GetBytes(Be.U32(d, q + 12 * k + 4 * c)), 0);
                t.Vals[k] = v;
            }
            into[j] = t;
        }
    }
}

public static class Anim
{
    // The skeleton and animations of a model .MDV (unpacked); null when it has none.
    public static Skeleton Load(byte[] mdv, List<Motion> motions)
    {
        var top = Program.KpkItems(mdv, 0, mdv.Length);
        if (top == null || top.Length < 3 || top[2] == null) return null;
        var items = Program.KpkItems(top[2], 0, top[2].Length);
        if (items == null) return null;
        Skeleton s = null;
        // [1] the CJF, [2] a KPKy of the CMFs (looked through one level deep either way).
        Action<byte[][], int> walk = null;
        walk = (list, depth) =>
        {
            foreach (var it in list)
            {
                if (it == null || it.Length < 4) continue;
                if (it[0] == 'C' && it[1] == 'J' && it[2] == 'F') { if (s == null) s = Skeleton.Read(it); }
                else if (it[0] == 'C' && it[1] == 'M' && it[2] == 'F') { var m = Motion.Read(it); if (m != null && m.Frames > 0) motions.Add(m); }
                else if (depth < 2 && it[0] == 'K' && it[1] == 'P' && it[2] == 'K') { var sub = Program.KpkItems(it, 0, it.Length); if (sub != null) walk(sub, depth + 1); }
            }
        };
        walk(items, 0);
        return s;
    }

    internal static float[] Sample(Motion.Track t, float f)
    {
        var k = t.Keys;
        if (f <= k[0]) return t.Vals[0];
        for (int i = 1; i < k.Length; i++)
            if (f <= k[i])
            {
                float u = (f - k[i - 1]) / Math.Max(1, k[i] - k[i - 1]);
                var a = t.Vals[i - 1]; var b = t.Vals[i];
                var r = new float[a.Length];
                float sign = 1;
                if (a.Length == 4) { float dot = 0; for (int c = 0; c < 4; c++) dot += a[c] * b[c]; if (dot < 0) sign = -1; }
                for (int c = 0; c < a.Length; c++) r[c] = a[c] + (sign * b[c] - a[c]) * u;
                if (a.Length == 4)
                {
                    float l = (float)Math.Sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2] + r[3] * r[3]);
                    if (l > 0) for (int c = 0; c < 4; c++) r[c] /= l;
                }
                return r;
            }
        return t.Vals[k.Length - 1];
    }

    // Local matrix (rows, row vectors): scale, then rotation, then translation.
    static float[] LocalMatrix(float[] q, float[] t, float[] s)
    {
        float x = q[0], y = q[1], z = q[2], w = q[3];
        // Rows of the row-vector rotation (the transpose of the usual column-vector matrix).
        var m = new float[16];
        m[0] = (1 - 2 * (y * y + z * z)) * s[0]; m[1] = 2 * (x * y + w * z) * s[0]; m[2] = 2 * (x * z - w * y) * s[0];
        m[4] = 2 * (x * y - w * z) * s[1]; m[5] = (1 - 2 * (x * x + z * z)) * s[1]; m[6] = 2 * (y * z + w * x) * s[1];
        m[8] = 2 * (x * z + w * y) * s[2]; m[9] = 2 * (y * z - w * x) * s[2]; m[10] = (1 - 2 * (x * x + y * y)) * s[2];
        m[12] = t[0]; m[13] = t[1]; m[14] = t[2]; m[15] = 1;
        return m;
    }

    internal static float[] Mul(float[] a, float[] b)  // a * b, rows
    {
        var r = new float[16];
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
        return r;
    }

    // The joints' model-space matrices at a frame (motion null: the rest pose). rootMotion off keeps N_ALL in place.
    public static float[][] World(Skeleton s, Motion m, float frame, bool rootMotion)
    {
        int n = s.Names.Length;
        var world = new float[n][];
        foreach (int j in s.Order)
        {
            var l = s.Local[j];
            float[] q = { l[0], l[1], l[2], l[3] }, t = { l[4], l[5], l[6] }, sc = { l[8], l[9], l[10] };
            if (sc[0] == 0 && sc[1] == 0 && sc[2] == 0) sc = new float[] { 1, 1, 1 };
            Motion.Track tr;
            if (m != null && m.Rot.TryGetValue(j, out tr)) q = Sample(tr, frame);
            if (m != null && m.Pos.TryGetValue(j, out tr) && (rootMotion || s.Parent[j] >= 0)) t = Sample(tr, frame);
            var local = LocalMatrix(q, t, sc);
            int p = s.Parent[j];
            world[j] = p >= 0 && p < n && world[p] != null ? Mul(local, world[p]) : local;
        }
        return world;
    }

    // Skinning matrices: the inverse rest pose, then the posed joint.
    public static float[][] SkinMatrices(Skeleton s, float[][] world)
    {
        var r = new float[world.Length][];
        for (int j = 0; j < world.Length; j++) r[j] = Mul(s.InvRest[j], world[j]);
        return r;
    }

    // A copy of the part with its vertices moved by their bones (parts without bone data stay as they are).
    public static MdlFile.MeshPart Skin(MdlFile.MeshPart p, float[][] mats)
    {
        if (p.BI == null || p.BW == null) return p;
        var q = new MdlFile.MeshPart { Tex = p.Tex, UV = p.UV, F = p.F, N = p.N, VOff = p.VOff, Stride = p.Stride, BI = p.BI, BW = p.BW, V = new float[p.V.Length] };
        for (int v = 0; v < p.N; v++)
        {
            float x = p.V[v * 3], y = p.V[v * 3 + 1], z = p.V[v * 3 + 2], ox = 0, oy = 0, oz = 0, tw = 0;
            for (int k = 0; k < 4; k++)
            {
                float w = p.BW[v * 4 + k];
                if (w <= 0) continue;
                int j = p.BI[v * 4 + k] + 1;
                if (j >= mats.Length) continue;
                var m = mats[j];
                ox += w * (x * m[0] + y * m[4] + z * m[8] + m[12]);
                oy += w * (x * m[1] + y * m[5] + z * m[9] + m[13]);
                oz += w * (x * m[2] + y * m[6] + z * m[10] + m[14]);
                tw += w;
            }
            if (tw > 0) { q.V[v * 3] = ox / tw; q.V[v * 3 + 1] = oy / tw; q.V[v * 3 + 2] = oz / tw; }
            else { q.V[v * 3] = x; q.V[v * 3 + 1] = y; q.V[v * 3 + 2] = z; }
        }
        return q;
    }

    // ---- Another character's animations on this skeleton (a body swap that moves like the body's owner) ----

    internal static float[] QMul(float[] a, float[] b)  // column convention: (a * b) v = a (b v)
    {
        return new[] {
            a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2] };
    }
    internal static float[] QInv(float[] q) { return new[] { -q[0], -q[1], -q[2], q[3] }; }
    internal static float[] QNorm(float[] q)
    {
        float l = (float)Math.Sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        return l > 0 ? new[] { q[0] / l, q[1] / l, q[2] / l, q[3] / l } : new float[] { 0, 0, 0, 1 };
    }

    // World rotations (world = parent * local, which rebuilds the CJF's rest pose) from a joint's local rotation.
    internal static float[][] WorldRots(Skeleton s, Func<int, float[]> local)
    {
        var w = new float[s.Names.Length][];
        foreach (int j in s.Order)
        {
            int p = s.Parent[j];
            w[j] = p >= 0 && p < w.Length && w[p] != null ? QNorm(QMul(w[p], local(j))) : local(j);
        }
        return w;
    }

    internal static float[] LocalRot(Skeleton s, Motion m, int j, float f)
    {
        Motion.Track t;
        if (m != null && m.Rot.TryGetValue(j, out t)) return Sample(t, f);
        var l = s.Local[j];
        return new[] { l[0], l[1], l[2], l[3] };
    }

    static float Angle(float[] a, float[] b)
    {
        float d = Math.Abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
        return d >= 1 ? 0 : 2 * (float)Math.Acos(d);
    }

    // The keys a dense track (one value a frame) needs: a frame is left out while the line between the kept
    // ones misses it by less than tol (radians for rotations, units for positions).
    static List<int> Reduce(float[][] v, bool rot, float tol)
    {
        var keys = new List<int> { 0 };
        int a = 0;
        for (int b = 2; b < v.Length; b++)
        {
            bool ok = true;
            for (int i = a + 1; i < b && ok; i++)
            {
                float u = (float)(i - a) / (b - a);
                var e = new float[v[a].Length];
                float sign = 1;
                if (rot) { float dot = 0; for (int c = 0; c < 4; c++) dot += v[a][c] * v[b][c]; if (dot < 0) sign = -1; }
                for (int c = 0; c < e.Length; c++) e[c] = v[a][c] + (sign * v[b][c] - v[a][c]) * u;
                float err;
                if (rot) err = Angle(QNorm(e), v[i]);
                else { err = 0; for (int c = 0; c < e.Length; c++) err = Math.Max(err, Math.Abs(e[c] - v[i][c])); }
                ok = err < tol;
            }
            if (!ok) { keys.Add(b - 1); a = b - 1; }
        }
        if (v.Length > 1) keys.Add(v.Length - 1);
        return keys;
    }

    static void Track(List<byte> o, Motion.Track t, bool rot)
    {
        int n = t.Keys.Length;
        Action<uint> u16 = x => { o.Add((byte)(x >> 8)); o.Add((byte)x); };
        u16((uint)n);
        foreach (int k in t.Keys) u16((uint)k);
        while ((o.Count & 3) != 0) o.Add(0);
        foreach (var v in t.Vals)
            for (int c = 0; c < (rot ? 4 : 3); c++)
            {
                if (rot) { int q = (int)Math.Round(Math.Max(-1f, Math.Min(1f, v[c])) * 32767f); u16((uint)(ushort)(short)q); }
                else { var b = BitConverter.GetBytes(v[c]); o.Add(b[3]); o.Add(b[2]); o.Add(b[1]); o.Add(b[0]); }
            }
        while ((o.Count & 3) != 0) o.Add(0);
    }

    internal static Motion.Track Dense(float[][] v, bool rot, float tol)
    {
        var keys = Reduce(v, rot, tol);
        return new Motion.Track { Keys = keys.ToArray(), Vals = keys.Select(k => v[k]).ToArray() };
    }

    // A TARGET animation (its file tgtCmf, on the TARGET skeleton tgt) rebuilt with the SOURCE character's
    // animation of the same name: every joint both skeletons have turns, in the world, as the SOURCE's
    // turns away from its own rest pose (so a different rest pose, an A-pose against a T-pose, does not
    // bend it); joints only the TARGET has keep its own animation; the root's and the hips' movement are
    // the SOURCE's. The TARGET's length in frames is kept (the game times its actions by it): the
    // SOURCE's animation is stretched to it.
    //
    // srcFrameOf: the SOURCE frame for each TARGET frame (null: stretched evenly); attacks pass one that puts
    // the SOURCE's strike where the TARGET's is (the game's hit frames are the TARGET's: AttackWarp).
    // dual: the SOURCE holds two swords and the TARGET one (Program.DualWield): the TARGET's RIGHTWEAPON (the
    // blade the game hits with; the SOURCE's own blades are held by its hands then) follows, frame by
    // frame, whichever SOURCE blade moves faster: a strike of the left sword hits too.
    public static byte[] Convert(byte[] tgtCmf, Skeleton tgt, Motion tm, Skeleton src, Motion sm, out int moved)
    {
        return Convert(tgtCmf, tgt, tm, src, sm, null, false, out moved);
    }

    internal static int Joint(Skeleton s, string name) { return Array.FindIndex(s.Names, x => string.Equals(x, name, StringComparison.OrdinalIgnoreCase)); }

    // Speed of a blade's tip (joint + its X axis * 0.8: the blades lie along X) at each of the given frames.
    internal static float[] TipSpeeds(Skeleton s, Motion m, int joint, float[] at)
    {
        var sp = new float[at.Length];
        float[] last = null;
        for (int i = 0; i < at.Length; i++)
        {
            var w = World(s, m, at[i], true)[joint];
            var p = new[] { w[12] + w[0] * 0.8f, w[13] + w[1] * 0.8f, w[14] + w[2] * 0.8f };
            if (last != null) sp[i] = (float)Math.Sqrt((p[0] - last[0]) * (p[0] - last[0]) + (p[1] - last[1]) * (p[1] - last[1]) + (p[2] - last[2]) * (p[2] - last[2]));
            last = p;
        }
        return sp;
    }

    static int Peak(float[] sp) { int k = 0; for (int i = 1; i < sp.Length; i++) if (sp[i] > sp[k]) k = i; return k; }

    // For an attack: the SOURCE frame to show at each TARGET frame, so that the SOURCE's fastest blade moment
    // falls on the TARGET's (its right blade's fastest: inside the game's hit frames); before and after it
    // stretched evenly. Null when either has no blade.
    // hitCenter: the middle of the TARGET's hit frames (the game's attack table, Program.HitWindows); the TARGET
    // blade's own fastest moment is no guide (the father draws his sword from his back in the first frames).
    public static float[] AttackWarp(Skeleton tgt, Motion tm, Skeleton src, Motion sm, int hitCenter)
    {
        int sr = Joint(src, "RIGHTWEAPON"), sl = Joint(src, "LEFTWEAPON");
        if (hitCenter < 0 || (sr < 0 && sl < 0) || tm.Frames < 3 || sm.Frames < 3) return null;
        var sf = Enumerable.Range(0, sm.Frames).Select(i => (float)i).ToArray();
        int pt = hitCenter;
        var ss = new float[sm.Frames];
        foreach (int j in new[] { sr, sl }) if (j >= 0) { var v = TipSpeeds(src, sm, j, sf); for (int i = 0; i < ss.Length; i++) ss[i] = Math.Max(ss[i], v[i]); }
        int ps = Peak(ss);
        int te = tm.Frames - 1, se = sm.Frames - 1;
        if (pt <= 0 || pt >= te || ps <= 0 || ps >= se) return null;
        var map = new float[tm.Frames];
        for (int f = 0; f < tm.Frames; f++) map[f] = f <= pt ? f * (float)ps / pt : ps + (f - pt) * (float)(se - ps) / (te - pt);
        return map;
    }

    public static byte[] Convert(byte[] tgtCmf, Skeleton tgt, Motion tm, Skeleton src, Motion sm, float[] srcFrameOf, bool dual, out int moved)
    {
        int n = tgt.Names.Length, frames = Math.Max(1, tm.Frames);
        Func<int, float> SF = f => srcFrameOf != null && f < srcFrameOf.Length ? srcFrameOf[f] : frames > 1 ? f * (float)(sm.Frames - 1) / (frames - 1) : 0;
        var srcAt = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        for (int i = 0; i < src.Names.Length; i++) if (!srcAt.ContainsKey(src.Names[i])) srcAt[src.Names[i]] = i;
        var pair = new int[n];
        moved = 0;
        for (int j = 0; j < n; j++) { int si; pair[j] = srcAt.TryGetValue(tgt.Names[j], out si) ? si : -1; }
        // Each skeleton's turns measured from the master's T-pose (Humanoid.cs, 2026-11-18): from each one's own rest,
        // an A-pose against a T-pose left the arms out.
        var srcRest = Humanoid.TPoseOrRest(src);
        var tgtRest = Humanoid.TPoseOrRest(tgt);
        float hipScale = 1;
        {
            var sm2 = Humanoid.Map(src); var tm2 = Humanoid.Map(tgt);
            float fl;
            if (sm2[0] >= 0 && tm2[0] >= 0) hipScale = Humanoid.HipHeight(tgt, tm2, out fl) / Humanoid.HipHeight(src, sm2, out fl);
            if (hipScale < 0.3f || hipScale > 3f) hipScale = 1;
        }
        var rotV = new float[n][][];
        for (int j = 0; j < n; j++) rotV[j] = new float[frames][];
        var twAll = new float[frames][][];
        for (int f = 0; f < frames; f++)
        {
            float sf = SF(f);
            var sw = WorldRots(src, j => LocalRot(src, sm, j, sf));
            var tw = new float[n][];
            foreach (int j in tgt.Order)
            {
                int p = tgt.Parent[j];
                float[] pw = p >= 0 && p < n ? tw[p] : new float[] { 0, 0, 0, 1 };
                float[] world;
                if (pair[j] >= 0)
                    world = QNorm(QMul(QMul(sw[pair[j]], QInv(srcRest[pair[j]])), tgtRest[j]));
                else
                    world = QNorm(QMul(pw, LocalRot(tgt, tm, j, f)));
                tw[j] = world;
                var local = QNorm(QMul(QInv(pw), world));
                if (f > 0 && local[0] * rotV[j][f - 1][0] + local[1] * rotV[j][f - 1][1] + local[2] * rotV[j][f - 1][2] + local[3] * rotV[j][f - 1][3] < 0)
                    local = new[] { -local[0], -local[1], -local[2], -local[3] };
                rotV[j][f] = local;
            }
            twAll[f] = tw;
        }
        var rot = new Dictionary<int, Motion.Track>();
        for (int j = 0; j < n; j++)
        {
            Motion.Track own;
            if (pair[j] < 0) { if (tm.Rot.TryGetValue(j, out own)) rot[j] = own; continue; }
            rot[j] = Dense(rotV[j], true, 0.004f);
            moved++;
        }
        // Positions: the TARGET's, but the root's and the hips' from the SOURCE (stretched the same way).
        var pos = new Dictionary<int, Motion.Track>(tm.Pos);
        for (int j = 0; j < n; j++)
        {
            if (pair[j] < 0) continue;
            string nm = tgt.Names[j].ToUpperInvariant();
            if (nm != "N_ALL" && nm != "HIPSBONE") continue;
            Motion.Track st;
            if (!sm.Pos.TryGetValue(pair[j], out st)) continue;
            var v = new float[frames][];
            for (int f = 0; f < frames; f++) v[f] = Sample(st, SF(f));
            // the hips' place in the TARGET's leg length (a taller body sank, a shorter one floated)
            if (nm == "HIPSBONE" && hipScale != 1) for (int f = 0; f < frames; f++) v[f] = new[] { v[f][0] * hipScale, v[f][1] * hipScale, v[f][2] * hipScale };
            pos[j] = Dense(v, false, 0.002f);
        }
        // Two swords: RIGHTWEAPON (rotation and place) on the faster SOURCE blade of each frame.
        int trw = Joint(tgt, "RIGHTWEAPON"), srw = Joint(src, "RIGHTWEAPON"), slw = Joint(src, "LEFTWEAPON");
        if (dual && trw > 0 && srw >= 0 && slw >= 0 && tgt.Parent[trw] >= 0)
        {
            var at = Enumerable.Range(0, frames).Select(SF).ToArray();
            var spR = TipSpeeds(src, sm, srw, at); var spL = TipSpeeds(src, sm, slw, at);
            // the motion so far, for the hand's place each frame
            var sofar = new Motion { Name = tm.Name, Frames = frames, Rot = rot, Pos = pos };
            var rv = new float[frames][]; var pv = new float[frames][];
            int par = tgt.Parent[trw];
            // One blade for the whole animation (switching hands made the game's blade jump across): in an
            // attack the one that strikes (the faster around the fastest moment), otherwise the right one.
            int sj = srw;
            if (srcFrameOf != null)
            {
                int pk = 0;
                for (int f = 1; f < frames; f++) if (Math.Max(spR[f], spL[f]) > Math.Max(spR[pk], spL[pk])) pk = f;
                float r = 0, l = 0;
                for (int k = Math.Max(0, pk - 3); k <= Math.Min(frames - 1, pk + 3); k++) { r += spR[k]; l += spL[k]; }
                if (l > r) sj = slw;
            }
            for (int f = 0; f < frames; f++)
            {
                var sWorld = World(src, sm, at[f], true);
                var sw = WorldRots(src, j => LocalRot(src, sm, j, at[f]));
                var want = QNorm(QMul(QMul(sw[sj], QInv(srcRest[sj])), tgtRest[trw]));
                var local = QNorm(QMul(QInv(twAll[f][par]), want));
                if (f > 0 && local[0] * rv[f - 1][0] + local[1] * rv[f - 1][1] + local[2] * rv[f - 1][2] + local[3] * rv[f - 1][3] < 0)
                    local = new[] { -local[0], -local[1], -local[2], -local[3] };
                rv[f] = local;
                // place: the blade's base, in the hand's axes (rows of a rotation: the inverse is the transpose)
                var P = World(tgt, sofar, f, true)[par];
                float dx = sWorld[sj][12] - P[12], dy = sWorld[sj][13] - P[13], dz = sWorld[sj][14] - P[14];
                var t = new float[3];
                for (int a = 0; a < 3; a++)
                {
                    float l2 = P[a * 4] * P[a * 4] + P[a * 4 + 1] * P[a * 4 + 1] + P[a * 4 + 2] * P[a * 4 + 2];
                    t[a] = (dx * P[a * 4] + dy * P[a * 4 + 1] + dz * P[a * 4 + 2]) / (l2 > 1e-9f ? l2 : 1);
                }
                pv[f] = t;
            }
            rot[trw] = Dense(rv, true, 0.004f);
            pos[trw] = Dense(pv, false, 0.002f);
        }
        return WriteCmf(tgtCmf, n, rot, pos);
    }

    // A CMF file: the header of cmf (name, length, flags), then the two tables (n joints) and their tracks.
    internal static byte[] WriteCmf(byte[] cmf, int n, Dictionary<int, Motion.Track> rot, Dictionary<int, Motion.Track> pos)
    {
        var o = new List<byte>(cmf.Take(0xA0));
        Action<int, uint> put32 = (at, x) => { o[at] = (byte)(x >> 24); o[at + 1] = (byte)(x >> 16); o[at + 2] = (byte)(x >> 8); o[at + 3] = (byte)x; };
        o[6] = (byte)(n >> 8); o[7] = (byte)n;
        foreach (var tableAndTracks in new[] { Tuple.Create(rot, true), Tuple.Create(pos, false) })
        {
            int table = o.Count;
            put32(tableAndTracks.Item2 ? 0x94 : 0x98, (uint)table);
            for (int j = 0; j < n; j++) o.AddRange(new byte[4]);
            for (int j = 0; j < n; j++)
            {
                Motion.Track t;
                if (!tableAndTracks.Item1.TryGetValue(j, out t)) continue;
                put32(table + 4 * j, (uint)(o.Count - table));
                Track(o, t, tableAndTracks.Item2);
            }
        }
        while ((o.Count & 15) != 0) o.Add(0);
        return o.ToArray();
    }

    // ---- A body's own posture over another's animations (2026-11-18: the 2B stood with the father's wide stance) ----

    // The local turn that, put after a joint's local rotation, turns it (at rest) by R in the model's space:
    // O = restW^-1 * R * restW (world = parent * local, so world * O = R * restW at rest).
    static float[] LocalOffset(float[][] restRot, int j, float[] R)
    {
        return QNorm(QMul(QMul(QInv(restRot[j]), R), restRot[j]));
    }

    // Every frame of an animation with the given joints turned (offsets: joint -> R, a quaternion in the model's
    // space, as at rest). What hangs from a turned joint follows it; the movement itself is kept (local * O).
    public static byte[] Adjust(byte[] cmf, Skeleton s, Motion m, Dictionary<int, float[]> offsets)
    {
        if (offsets == null || offsets.Count == 0 || m == null) return cmf;
        var restRot = WorldRots(s, j => LocalRot(s, null, j, 0));
        var rot = new Dictionary<int, Motion.Track>(m.Rot);
        int frames = Math.Max(1, m.Frames);
        foreach (var kv in offsets)
        {
            int j = kv.Key;
            if (j < 0 || j >= s.Names.Length) continue;
            var O = LocalOffset(restRot, j, kv.Value);
            Motion.Track t;
            if (!rot.TryGetValue(j, out t))
            {
                var l = LocalRot(s, null, j, 0);
                t = frames > 1 ? new Motion.Track { Keys = new[] { 0, frames - 1 }, Vals = new[] { l, l } }
                               : new Motion.Track { Keys = new[] { 0 }, Vals = new[] { l } };
            }
            rot[j] = new Motion.Track { Keys = t.Keys, Vals = t.Vals.Select(v => QNorm(QMul(v, O))).ToArray() };
        }
        return WriteCmf(cmf, Math.Max(s.Names.Length, (int)Be.U16(cmf, 6)), rot, new Dictionary<int, Motion.Track>(m.Pos));
    }

    // Quaternion of turns about the model's X, then Y, then Z (degrees).
    public static float[] Euler(float rx, float ry, float rz)
    {
        Func<int, float, float[]> axis = (a, d) =>
        {
            float h = d * (float)Math.PI / 360f; var q = new float[] { 0, 0, 0, (float)Math.Cos(h) }; q[a] = (float)Math.Sin(h); return q;
        };
        return QNorm(QMul(axis(2, rz), QMul(axis(1, ry), axis(0, rx))));
    }

    // The posture turns of a body: legs closer together and arms closer to the body by the given degrees (negative:
    // apart), each joint's own (custom: joint name -> R). The side of each limb's turn is measured on the rest pose:
    // the limb's next joint (knee, elbow) must come closer to the hips' middle.
    public static Dictionary<int, float[]> Posture(Skeleton s, float legsDeg, float armsDeg, Dictionary<string, float[]> custom)
    {
        var r = new Dictionary<int, float[]>();
        var restRot = WorldRots(s, j => LocalRot(s, null, j, 0));
        var restW = World(s, null, 0, false);
        int hips = Joint(s, "HIPSBONE");
        float mid = hips >= 0 ? restW[hips][12] : 0;
        Action<string, string, float> limb = (joint, next, deg) =>
        {
            int j = Joint(s, joint), c = Joint(s, next);
            if (j < 0 || c < 0 || deg == 0) return;
            float h = Math.Abs(deg) * (float)Math.PI / 360f;
            float[] best = null; float bestD = float.MaxValue;
            foreach (float sg in new[] { 1f, -1f })  // about the forward axis (Z: the model faces +Z, X is its side)
            {
                var q = new float[] { 0, 0, sg * (float)Math.Sin(h), (float)Math.Cos(h) };
                var test = new Motion { Frames = 1 };
                test.Rot[j] = new Motion.Track { Keys = new[] { 0 }, Vals = new[] { QNorm(QMul(LocalRot(s, null, j, 0), LocalOffset(restRot, j, q))) } };
                float d = Math.Abs(World(s, test, 0, false)[c][12] - mid);
                if (d < bestD) { bestD = d; best = q; }
            }
            if (best == null) return;
            r[j] = deg > 0 ? best : QInv(best);
        };
        limb("LEFTUPLEG", "LEFTLEG", legsDeg); limb("RIGHTUPLEG", "RIGHTLEG", legsDeg);
        limb("LEFTARM", "LEFTFOREARM", armsDeg); limb("RIGHTARM", "RIGHTFOREARM", armsDeg);
        if (custom != null)
            foreach (var kv in custom)
            {
                int j = Joint(s, kv.Key);
                if (j >= 0) r[j] = r.ContainsKey(j) ? QNorm(QMul(kv.Value, r[j])) : kv.Value;
            }
        return r;
    }

    // Check (CLI "animtest"): the rest pose built from the local values against the file's, and inverse * rest.
    public static string Test(Skeleton s)
    {
        var w = World(s, null, 0, true);
        float e1 = 0, e2 = 0;
        for (int j = 0; j < w.Length; j++)
        {
            for (int i = 0; i < 16; i++) e1 = Math.Max(e1, Math.Abs(w[j][i] - s.RestWorld[j][i]));
            var id = Mul(s.InvRest[j], s.RestWorld[j]);
            for (int i = 0; i < 16; i++) e2 = Math.Max(e2, Math.Abs(id[i] - (i % 5 == 0 ? 1 : 0)));
        }
        return s.Names.Length + " juntas; repouso calculado x arquivo: erro " + e1.ToString("G3") + "; inversa x repouso = identidade: erro " + e2.ToString("G3");
    }
}
