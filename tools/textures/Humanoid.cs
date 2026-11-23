// NierTextureTool - the master humanoid skeleton (2026-11-18).
//
// The 2B imported over the father moved with Kaine's animations stood with her arms out: an animation was carried
// from one skeleton to another as each joint's turn away from ITS OWN rest pose, and the rests differ (one an
// A-pose, the other a T-pose). As Unity's "Humanoid" avatars (also VRM's and Mixamo's): every skeleton is read as
// the same 21 bones (the 15 required ones + chest, neck, shoulders, toes) and put in the same T-pose (standing,
// facing +Z, arms straight out to the sides, legs straight down, spine and neck straight up); an animation is kept
// as each bone's turn in the model's space away from that T-pose, and the hips' place over the feet in hip
// heights. Any humanoid skeleton (the game's, Kaine's, Mixamo's, VRoid's, an MMD's...) plays it: its own T-pose
// turned the same way. Only the bones' directions are put straight; their twist needs no matching, as the turns
// are applied in the model's space.
//
// Files (.hanim, text, to read and change by hand): "HANIM 1", "name", "frames", "left" (+1 when the left side is
// at +X), "bones" (the ones the animation has), then one line a frame: hips x y z, then x y z w of each bone.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

public static class Humanoid
{
    public static readonly string[] Bones = {
        "Hips", "Spine", "Chest", "Neck", "Head",
        "LeftShoulder", "LeftUpperArm", "LeftLowerArm", "LeftHand",
        "RightShoulder", "RightUpperArm", "RightLowerArm", "RightHand",
        "LeftUpperLeg", "LeftLowerLeg", "LeftFoot", "LeftToes",
        "RightUpperLeg", "RightLowerLeg", "RightFoot", "RightToes",
        "RightWeapon", "LeftWeapon" };  // the game's blades (RIGHTWEAPON / LEFTWEAPON; a model's "weapon", "sword", "prop" bones)
    // The bones each one points to in the T-pose (the first present).
    static readonly int[][] Next = {
        new int[0], new[] { 2, 3, 4 }, new[] { 3, 4 }, new[] { 4 }, new int[0],
        new[] { 6 }, new[] { 7 }, new[] { 8 }, new int[0],
        new[] { 10 }, new[] { 11 }, new[] { 12 }, new int[0],
        new[] { 14 }, new[] { 15 }, new[] { 16 }, new int[0],
        new[] { 18 }, new[] { 19 }, new[] { 20 }, new int[0], new int[0], new int[0] };
    // The T-pose direction: 0 up, 1 out to its side, 2 down, 3 forward.
    static readonly int[] Dir = { -1, 0, 0, 0, -1, 1, 1, 1, -1, 1, 1, 1, -1, 2, 2, 3, -1, 2, 2, 3, -1, -1, -1 };
    // Each bone's parent in the master's tree (Chest -> Neck; the blades in the hands).
    public static readonly int[] ParentOf = { -1, 0, 1, 2, 3, 2, 5, 6, 7, 2, 9, 10, 11, 0, 13, 14, 15, 0, 17, 18, 19, 12, 8 };
    static readonly int[] Required = { 0, 1, 4, 6, 7, 8, 10, 11, 12, 13, 14, 15, 17, 18, 19 };
    static int SideOf(int b) { return (b >= 5 && b <= 8) || (b >= 13 && b <= 16) || b == 22 ? 1 : (b >= 9 && b <= 12) || (b >= 17 && b <= 21) ? -1 : 0; }
    static int Mirror(int b) { return b >= 5 && b <= 8 ? b + 4 : b >= 9 && b <= 12 ? b - 4 : b >= 13 && b <= 16 ? b + 4 : b >= 17 && b <= 20 ? b - 4 : b == 21 ? 22 : b == 22 ? 21 : b; }

    // ---- a skeleton read as the master's bones ----

    static int Depth(int[] parent, int j) { int d = 0; for (int p = parent[j]; p >= 0 && p < parent.Length && d < 512; p = parent[p]) d++; return d; }
    static bool Below(int[] parent, int j, int anc) { for (int p = parent[j]; p >= 0 && p < parent.Length; p = parent[p]) if (p == anc) return true; return false; }

    // Master bone -> joint (-1 when missing), from the joints' names (Outfit.Slot: Mixamo, VRoid, Unreal, Blender,
    // Biped, MMD, the game's own).
    public static int[] Map(string[] names, int[] parent)
    {
        var map = Enumerable.Repeat(-1, Bones.Length).ToArray();
        var spine = new List<int>();
        var cands = Enumerable.Range(0, Bones.Length).Select(_ => new List<int>()).ToArray();
        for (int j = 0; j < names.Length; j++)
        {
            // The blades: the game's RIGHTWEAPON / LEFTWEAPON, a model's weapon / sword / prop bone (its side by name).
            string up = names[j].ToUpperInvariant();
            if (System.Text.RegularExpressions.Regex.IsMatch(up, "WEAPON|SWORD|PROP") && !up.Contains("ATTACH"))
            {
                bool left = up.Contains("LEFT") || up.EndsWith("_L") || up.EndsWith(".L") || up.StartsWith("L_");
                int wb = left ? 22 : 21;
                if (map[wb] < 0 || Depth(parent, j) < Depth(parent, map[wb])) map[wb] = j;
                continue;
            }
            int side; string slot = Outfit.Slot(names[j], out side);
            if (slot == null) continue;
            int b = -1, l = side > 0 ? 0 : 4;
            switch (slot)
            {
                case "HIPS": b = 0; break;
                case "SPINE": spine.Add(j); continue;
                case "NECK": b = 3; break;
                case "HEAD": b = 4; break;
                case "SHOULDER": b = 5 + l; break;
                case "ARM": b = 6 + l; break;
                case "FOREARM": b = 7 + l; break;
                case "HAND": b = 8 + l; break;
                case "UPLEG": b = 13 + l; break;
                case "LEG": b = 14 + l; break;
                case "FOOT": b = 15 + l; break;
                case "TOEBASE": b = 16 + l; break;
            }
            if (b < 0 || (side == 0 && SideOf(b) != 0)) continue;
            cands[b].Add(j);
            if (map[b] < 0 || Depth(parent, j) < Depth(parent, map[b])) map[b] = j;
        }
        // A helper beside the chain (Kaine's AS_RIGHTELBOW) loses to a bone on the way to the next one (the hand).
        for (int b = 0; b < Bones.Length; b++)
        {
            var nxl = Next[b].Select(k => map[k]).Where(k => k >= 0).ToList();
            int nx = nxl.Count > 0 ? nxl[0] : -1;
            if (map[b] < 0 || nx < 0 || Below(parent, nx, map[b])) continue;
            var on = cands[b].Where(j => Below(parent, nx, j)).OrderBy(j => Depth(parent, j)).ToList();
            if (on.Count > 0) map[b] = on[0];
        }
        var chain = spine.Where(j => map[0] < 0 || Below(parent, j, map[0])).OrderBy(j => Depth(parent, j)).ToList();
        if (chain.Count > 0) map[1] = chain[0];
        if (chain.Count > 1) map[2] = chain[chain.Count - 1];
        return map;
    }
    public static int[] Map(Skeleton s) { return Map(s.Names, s.Parent); }
    public static string Missing(int[] map) { return string.Join(", ", Required.Where(b => map[b] < 0).Select(b => Bones[b])); }
    public static string Describe(Skeleton s, int[] map)
    {
        return string.Join(", ", Enumerable.Range(0, Bones.Length).Where(b => map[b] >= 0).Select(b => Bones[b] + "=" + s.Names[map[b]]));
    }

    static float[] Pos(float[] m) { return new[] { m[12], m[13], m[14] }; }
    static float[] Sub(float[] a, float[] b) { return new[] { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
    static float[] Unit(float[] a) { float l = (float)Math.Sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); return l > 1e-9f ? new[] { a[0] / l, a[1] / l, a[2] / l } : new float[] { 0, 1, 0 }; }
    static float Dot(float[] a, float[] b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

    // The shortest turn taking direction a to b.
    static float[] FromTo(float[] a, float[] b)
    {
        float d = Dot(a, b);
        if (d < -0.9999f)
        {
            var ax = Math.Abs(a[0]) < 0.9f ? new float[] { 1, 0, 0 } : new float[] { 0, 1, 0 };
            var c = Unit(new[] { a[1] * ax[2] - a[2] * ax[1], a[2] * ax[0] - a[0] * ax[2], a[0] * ax[1] - a[1] * ax[0] });
            return new[] { c[0], c[1], c[2], 0f };
        }
        return Anim.QNorm(new[] { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0], 1 + d });
    }

    static float[][] Poses(Skeleton s, float[][] local)
    {
        var m = new Motion { Frames = 1 };
        for (int j = 0; j < local.Length; j++) m.Rot[j] = new Motion.Track { Keys = new[] { 0 }, Vals = new[] { local[j] } };
        return Anim.World(s, m, 0, false);
    }

    // +1 when the skeleton's left side is at +X.
    public static float LeftSign(Skeleton s, int[] map)
    {
        var w = Anim.World(s, null, 0, false);
        int l = map[6] >= 0 ? map[6] : map[13], r = map[10] >= 0 ? map[10] : map[17];
        if (l < 0 || r < 0) return 1;
        return w[l][12] >= w[r][12] ? 1 : -1;
    }

    // The joints' local rotations in the T-pose (every joint; the master's bones turned to their directions, the
    // rest carried along by their parents).
    public static float[][] TPoseLocal(Skeleton s, int[] map)
    {
        int n = s.Names.Length;
        var loc = Enumerable.Range(0, n).Select(j => Anim.LocalRot(s, null, j, 0)).ToArray();
        float lx = LeftSign(s, map);
        var todo = Enumerable.Range(0, Bones.Length).Where(b => map[b] >= 0 && Dir[b] >= 0 && Next[b].Any(c => map[c] >= 0))
                             .OrderBy(b => Depth(s.Parent, map[b])).ToList();
        foreach (int b in todo)
        {
            int j = map[b], c = map[Next[b].First(k => map[k] >= 0)];
            float side = SideOf(b) * lx;
            var want = Dir[b] == 0 ? new float[] { 0, 1, 0 } : Dir[b] == 1 ? new float[] { side, 0, 0 } : Dir[b] == 2 ? new float[] { 0, -1, 0 } : new float[] { 0, 0, 1 };
            var w = Poses(s, loc);
            var d = Unit(Sub(Pos(w[c]), Pos(w[j])));
            var S = FromTo(d, want);
            var wr = Anim.WorldRots(s, k => loc[k]);
            int p = s.Parent[j];
            var keep = loc[j];
            // The turn in the model's space; the convention checked by the result (the other way otherwise).
            foreach (var turn in new[] { S, Anim.QInv(S) })
            {
                var nw = Anim.QNorm(Anim.QMul(turn, wr[j]));
                loc[j] = p >= 0 ? Anim.QNorm(Anim.QMul(Anim.QInv(wr[p]), nw)) : nw;
                var w2 = Poses(s, loc);
                if (Dot(Unit(Sub(Pos(w2[c]), Pos(w2[j]))), want) > 0.999f) break;
                loc[j] = keep;
            }
        }
        return loc;
    }
    public static float[][] TPose(Skeleton s, int[] map)
    {
        var loc = TPoseLocal(s, map);
        return Anim.WorldRots(s, k => loc[k]);
    }
    // The T-pose's world rotations when the skeleton has the master's required bones, else its rest's.
    public static float[][] TPoseOrRest(Skeleton s)
    {
        var map = Map(s);
        if (Missing(map) != "") return Anim.WorldRots(s, j => Anim.LocalRot(s, null, j, 0));
        return TPose(s, map);
    }

    // Height of the hips over the feet at rest (the unit of the hips' place), and the feet's level.
    public static float HipHeight(Skeleton s, int[] map, out float floor)
    {
        var w = Anim.World(s, null, 0, false);
        floor = 0;
        if (map[0] < 0) return 1;
        var feet = new[] { map[15], map[19] }.Where(f => f >= 0).ToList();
        floor = feet.Count > 0 ? feet.Min(f => w[f][13]) : 0;
        float h = w[map[0]][13] - floor;
        return h > 1e-4f ? h : 1;
    }

    // ---- animations in the master's terms ----

    public class Clip
    {
        public string Name = "";
        public int Frames;
        public float Left = 1;
        public float[][][] Rot = new float[Bones.Length][][];  // [bone][frame] the turn from the T-pose (null: none)
        public float[][] Hips;                                  // [frame] x, y over the feet, z; in hip heights
    }

    public static Clip FromMotion(Skeleton s, Motion m, string name)
    {
        var map = Map(s);
        if (Missing(map) != "") return null;
        var T = TPose(s, map);
        float floor, H = HipHeight(s, map, out floor);
        int frames = Math.Max(1, m.Frames);
        var c = new Clip { Name = name ?? m.Name, Frames = frames, Left = LeftSign(s, map), Hips = new float[frames][] };
        for (int b = 0; b < Bones.Length; b++) if (map[b] >= 0) c.Rot[b] = new float[frames][];
        for (int f = 0; f < frames; f++)
        {
            var w = Anim.WorldRots(s, j => Anim.LocalRot(s, m, j, f));
            for (int b = 0; b < Bones.Length; b++)
            {
                if (map[b] < 0) continue;
                var q = Anim.QNorm(Anim.QMul(w[map[b]], Anim.QInv(T[map[b]])));
                if (f > 0 && q[0] * c.Rot[b][f - 1][0] + q[1] * c.Rot[b][f - 1][1] + q[2] * c.Rot[b][f - 1][2] + q[3] * c.Rot[b][f - 1][3] < 0)
                    q = new[] { -q[0], -q[1], -q[2], -q[3] };
                c.Rot[b][f] = q;
            }
            // (hips that are the root, as Mixamo's, move by their own track: the game's root is N_ALL, kept still)
            var hw = Anim.World(s, m, f, s.Parent[map[0]] < 0)[map[0]];
            c.Hips[f] = new[] { hw[12] / H, (hw[13] - floor) / H, hw[14] / H };
        }
        return c;
    }

    static float[] Nlerp(float[] a, float[] b, float u)
    {
        float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], s = d < 0 ? -1 : 1;
        return Anim.QNorm(new[] { a[0] + (s * b[0] - a[0]) * u, a[1] + (s * b[1] - a[1]) * u, a[2] + (s * b[2] - a[2]) * u, a[3] + (s * b[3] - a[3]) * u });
    }
    static float[] At(float[][] v, float f)
    {
        int a = Math.Max(0, Math.Min(v.Length - 1, (int)Math.Floor(f))), b = Math.Min(v.Length - 1, a + 1);
        float u = Math.Max(0, Math.Min(1, f - a));
        if (v[a].Length == 4) return Nlerp(v[a], v[b], u);
        return new[] { v[a][0] + (v[b][0] - v[a][0]) * u, v[a][1] + (v[b][1] - v[a][1]) * u, v[a][2] + (v[b][2] - v[a][2]) * u };
    }

    // A clip on a skeleton of the game: a CMF (header of tgtCmf, the TARGET's length kept: the clip stretched to it).
    // The master's bones turn as the clip says from the TARGET's T-pose; its other joints keep the TARGET's own
    // animation tm (fingers, cloth, weapons; null: their rest); the hips go where the clip puts them, in the
    // TARGET's hip heights. A clip of a mirrored skeleton (left at the other side) is mirrored.
    public static byte[] ToCmf(Clip c, byte[] tgtCmf, Skeleton tgt, Motion tm, out int moved)
    {
        moved = 0;
        var map = Map(tgt);
        if (Missing(map) != "") return tgtCmf;
        var T = TPose(tgt, map);
        float floor, H = HipHeight(tgt, map, out floor);
        bool mirror = Math.Sign(LeftSign(tgt, map)) != Math.Sign(c.Left);
        int n = tgt.Names.Length, frames = Math.Max(1, tm != null ? tm.Frames : c.Frames);
        var boneOf = Enumerable.Repeat(-1, n).ToArray();
        for (int b = 0; b < Bones.Length; b++) if (map[b] >= 0) boneOf[map[b]] = b;
        Func<int, float> SF = f => frames > 1 ? f * (float)(c.Frames - 1) / (frames - 1) : 0;
        var rotV = new float[n][][];
        for (int j = 0; j < n; j++) rotV[j] = new float[frames][];
        for (int f = 0; f < frames; f++)
        {
            float sf = SF(f);
            var tw = new float[n][];
            foreach (int j in tgt.Order)
            {
                int p = tgt.Parent[j];
                float[] pw = p >= 0 && p < n ? tw[p] : new float[] { 0, 0, 0, 1 };
                int b = boneOf[j], cb = b >= 0 && mirror ? Mirror(b) : b;
                float[] world;
                if (b >= 0 && c.Rot[cb] != null)
                {
                    var d = At(c.Rot[cb], sf);
                    if (mirror) d = new[] { d[0], -d[1], -d[2], d[3] };
                    world = Anim.QNorm(Anim.QMul(d, T[j]));
                }
                else world = Anim.QNorm(Anim.QMul(pw, Anim.LocalRot(tgt, tm, j, f)));
                tw[j] = world;
                var local = Anim.QNorm(Anim.QMul(Anim.QInv(pw), world));
                if (f > 0 && local[0] * rotV[j][f - 1][0] + local[1] * rotV[j][f - 1][1] + local[2] * rotV[j][f - 1][2] + local[3] * rotV[j][f - 1][3] < 0)
                    local = new[] { -local[0], -local[1], -local[2], -local[3] };
                rotV[j][f] = local;
            }
        }
        var rot = new Dictionary<int, Motion.Track>(tm != null ? tm.Rot : new Dictionary<int, Motion.Track>());
        for (int j = 0; j < n; j++)
        {
            int b = boneOf[j];
            if (b < 0 || c.Rot[mirror ? Mirror(b) : b] == null) continue;
            rot[j] = Anim.Dense(rotV[j], true, 0.004f);
            moved++;
        }
        // The hips: the clip's place (over the TARGET's feet), less where the hips' parent stands at rest.
        var pos = new Dictionary<int, Motion.Track>(tm != null ? tm.Pos : new Dictionary<int, Motion.Track>());
        int hj = map[0], hp = tgt.Parent[hj];
        var rest = Anim.World(tgt, null, 0, false);
        var hv = new float[frames][];
        for (int f = 0; f < frames; f++)
        {
            var h = At(c.Hips, SF(f));
            float x = (mirror ? -h[0] : h[0]) * H, y = h[1] * H + floor, z = h[2] * H;
            hv[f] = hp >= 0 ? new[] { x - rest[hp][12], y - rest[hp][13], z - rest[hp][14] } : new[] { x, y, z };
        }
        pos[hj] = Anim.Dense(hv, false, 0.002f);
        return Anim.WriteCmf(tgtCmf, n, rot, pos);
    }

    // ---- the editor's helpers ----

    class Fit { public int[] Map, BoneOf; public float[][] T; public float H, Floor, Left; }
    static readonly Dictionary<Skeleton, Fit> fits = new Dictionary<Skeleton, Fit>();
    static Fit FitOf(Skeleton s)
    {
        Fit f;
        if (fits.TryGetValue(s, out f)) return f;
        var map = Map(s);
        if (Missing(map) != "") { fits[s] = null; return null; }
        f = new Fit { Map = map, T = TPose(s, map), Left = LeftSign(s, map), BoneOf = Enumerable.Repeat(-1, s.Names.Length).ToArray() };
        f.H = HipHeight(s, map, out f.Floor);
        for (int b = 0; b < Bones.Length; b++) if (map[b] >= 0) f.BoneOf[map[b]] = b;
        fits[s] = f;
        return f;
    }

    // One frame of a clip on a skeleton (cf: the clip's frame; tm at tf: the skeleton's own animation for the joints
    // the clip has not): a one-frame Motion. Null when the skeleton has no master's bones.
    public static Motion PoseAt(Clip c, float cf, Skeleton tgt, Motion tm, float tf)
    {
        var fit = FitOf(tgt);
        if (fit == null || c == null || c.Frames < 1) return null;
        bool mirror = Math.Sign(fit.Left) != Math.Sign(c.Left);
        int n = tgt.Names.Length;
        var m = new Motion { Name = c.Name, Frames = 1 };
        var tw = new float[n][];
        foreach (int j in tgt.Order)
        {
            int p = tgt.Parent[j];
            float[] pw = p >= 0 && p < n ? tw[p] : new float[] { 0, 0, 0, 1 };
            int b = fit.BoneOf[j], cb = b >= 0 && mirror ? Mirror(b) : b;
            float[] world;
            if (b >= 0 && c.Rot[cb] != null)
            {
                var d = At(c.Rot[cb], cf);
                if (mirror) d = new[] { d[0], -d[1], -d[2], d[3] };
                world = Anim.QNorm(Anim.QMul(d, fit.T[j]));
            }
            else world = Anim.QNorm(Anim.QMul(pw, Anim.LocalRot(tgt, tm, j, tf)));
            tw[j] = world;
            m.Rot[j] = new Motion.Track { Keys = new[] { 0 }, Vals = new[] { Anim.QNorm(Anim.QMul(Anim.QInv(pw), world)) } };
        }
        if (tm != null) foreach (var kv in tm.Pos) m.Pos[kv.Key] = new Motion.Track { Keys = new[] { 0 }, Vals = new[] { Anim.Sample(kv.Value, tf) } };
        int hj = fit.Map[0], hp = tgt.Parent[hj];
        var rest = Anim.World(tgt, null, 0, false);
        var h = At(c.Hips, cf);
        float x = (mirror ? -h[0] : h[0]) * fit.H, y = h[1] * fit.H + fit.Floor, z = h[2] * fit.H;
        m.Pos[hj] = new Motion.Track { Keys = new[] { 0 }, Vals = new[] { hp >= 0 ? new[] { x - rest[hp][12], y - rest[hp][13], z - rest[hp][14] } : new[] { x, y, z } } };
        return m;
    }

    // The master's bone of each joint of a skeleton (-1: none), for the editor's picking and drawing.
    public static int[] JointsOf(Skeleton s) { var f = FitOf(s); return f == null ? null : f.Map; }

    // A part t (0..1) of a turn q.
    public static float[] Part(float[] q, float t)
    {
        var a = q[3] < 0 ? new[] { -q[0], -q[1], -q[2], -q[3] } : q;
        double ang = 2 * Math.Acos(Math.Min(1, a[3])), s = Math.Sin(ang / 2);
        if (s < 1e-6) return new float[] { 0, 0, 0, 1 };
        double h = ang * t / 2;
        return new[] { (float)(a[0] / s * Math.Sin(h)), (float)(a[1] / s * Math.Sin(h)), (float)(a[2] / s * Math.Sin(h)), (float)Math.Cos(h) };
    }

    // The bone and the ones hanging from it in the master's tree.
    public static List<int> Subtree(int b)
    {
        var l = new List<int> { b };
        for (int i = 0; i < l.Count; i++) for (int k = 0; k < Bones.Length; k++) if (ParentOf[k] == l[i] && !l.Contains(k)) l.Add(k);
        return l;
    }

    // Turns bones (with what hangs from them when kids) by R in the model's space at frame f, fading out over
    // 'reach' frames each side (all: every frame, fully).
    public static void Turn(Clip c, int bone, bool kids, float[] R, int f, int reach, bool all)
    {
        var bs = kids ? Subtree(bone) : new List<int> { bone };
        for (int fr = 0; fr < c.Frames; fr++)
        {
            float w = all ? 1 : Math.Abs(fr - f) > reach ? 0 : reach == 0 ? 1 : 0.5f * (1 + (float)Math.Cos(Math.PI * Math.Abs(fr - f) / (reach + 1)));
            if (w <= 0) continue;
            var q = Part(R, w);
            foreach (int b in bs) if (c.Rot[b] != null) c.Rot[b][fr] = Anim.QNorm(Anim.QMul(q, c.Rot[b][fr]));
        }
    }

    public static Clip Copy(Clip c)
    {
        var r = new Clip { Name = c.Name, Frames = c.Frames, Left = c.Left, Hips = c.Hips.Select(v => (float[])v.Clone()).ToArray() };
        for (int b = 0; b < Bones.Length; b++) if (c.Rot[b] != null) r.Rot[b] = c.Rot[b].Select(v => (float[])v.Clone()).ToArray();
        return r;
    }

    // ---- BVH (the common motion-capture text format: Blender, MotionBuilder, Mixamo) ----
    // Written on the master's bones in the T-pose of a reference skeleton (its bone lengths, centimetres), rotations
    // Z X Y; each joint's rotation = its parent's turn undone, then its own.
    public static void WriteBvh(Clip c, Skeleton refSk, string path)
    {
        var fit = FitOf(refSk);
        if (fit == null) throw new Exception("esqueleto de referencia sem os ossos mestres: " + Missing(Map(refSk)));
        var tw = Poses(refSk, TPoseLocal(refSk, fit.Map));
        Func<int, float[]> P = b => Pos(tw[fit.Map[b]]);
        var have = Enumerable.Range(0, Bones.Length).Where(b => c.Rot[b] != null && fit.Map[b] >= 0).ToList();
        Func<int, int> par = b => { int p = ParentOf[b]; while (p >= 0 && !have.Contains(p)) p = ParentOf[p]; return p; };
        var ci = CultureInfo.InvariantCulture;
        var sb = new StringBuilder("HIERARCHY\n");
        var order = new List<int>();
        Action<int, int> joint = null;
        joint = (b, depth) =>
        {
            order.Add(b);
            string ind = new string('\t', depth);
            // (the root's offset: its place at rest, which its position channels then replace)
            var off = par(b) < 0 ? P(b) : Sub(P(b), P(par(b)));
            sb.Append(ind + (par(b) < 0 ? "ROOT " : "JOINT ") + Bones[b] + "\n" + ind + "{\n");
            sb.Append(ind + "\tOFFSET " + string.Join(" ", off.Select(v => (v * 100).ToString("0.####", ci))) + "\n");
            sb.Append(ind + "\tCHANNELS " + (par(b) < 0 ? "6 Xposition Yposition Zposition Zrotation Xrotation Yrotation" : "3 Zrotation Xrotation Yrotation") + "\n");
            var kids = have.Where(k => par(k) == b).ToList();
            foreach (int k in kids) joint(k, depth + 1);
            if (kids.Count == 0) sb.Append(ind + "\tEnd Site\n" + ind + "\t{\n" + ind + "\t\tOFFSET 0 5 0\n" + ind + "\t}\n");
            sb.Append(ind + "}\n");
        };
        joint(0, 0);
        sb.Append("MOTION\nFrames: " + c.Frames + "\nFrame Time: 0.0333333\n");
        for (int f = 0; f < c.Frames; f++)
        {
            var vals = new List<float>();
            foreach (int b in order)
            {
                if (par(b) < 0) { var h = c.Hips[f]; vals.Add(h[0] * fit.H * 100); vals.Add((h[1] * fit.H + fit.Floor) * 100); vals.Add(h[2] * fit.H * 100); }
                var local = par(b) < 0 ? c.Rot[b][f] : Anim.QMul(Anim.QInv(c.Rot[par(b)][f]), c.Rot[b][f]);
                vals.AddRange(ToZxy(local));
            }
            sb.Append(string.Join(" ", vals.Select(v => v.ToString("0.####", ci))) + "\n");
        }
        File.WriteAllText(path, sb.ToString());
    }

    // R = Rz * Rx * Ry, degrees (z, x, y).
    static float[] ToZxy(float[] q)
    {
        float x = q[0], y = q[1], z = q[2], w = q[3];
        double m01 = 2 * (x * y - w * z), m11 = 1 - 2 * (x * x + z * z), m20 = 2 * (x * z - w * y), m21 = 2 * (y * z + w * x), m22 = 1 - 2 * (x * x + y * y);
        double ax = Math.Asin(Math.Max(-1, Math.Min(1, m21))), ay = Math.Atan2(-m20, m22), az = Math.Atan2(-m01, m11);
        const double d = 180 / Math.PI;
        return new[] { (float)(az * d), (float)(ax * d), (float)(ay * d) };
    }
    static float[] AxisQ(char a, float deg)
    {
        float h = deg * (float)Math.PI / 360f, s = (float)Math.Sin(h);
        return a == 'X' ? new[] { s, 0, 0, (float)Math.Cos(h) } : a == 'Y' ? new[] { 0, s, 0, (float)Math.Cos(h) } : new[] { 0, 0, s, (float)Math.Cos(h) };
    }

    // Any BVH (its own skeleton, any channel order): read as a skeleton and an animation, then as the master's.
    public static Clip ReadBvh(string path)
    {
        var ci = CultureInfo.InvariantCulture;
        var tok = File.ReadAllText(path).Split(new[] { ' ', '\t', '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
        var names = new List<string>(); var parent = new List<int>(); var offs = new List<float[]>(); var chans = new List<string[]>();
        var stack = new Stack<int>();
        int i = 0, last = -1; bool endSite = false;
        while (i < tok.Length && tok[i] != "MOTION")
        {
            string t = tok[i++];
            if (t == "ROOT" || t == "JOINT")
            {
                names.Add(tok[i++]); parent.Add(stack.Count > 0 ? stack.Peek() : -1); offs.Add(new float[3]); chans.Add(new string[0]);
                last = names.Count - 1;
            }
            else if (t == "End") { i++; endSite = true; }
            else if (t == "{") { if (endSite) stack.Push(-2); else stack.Push(last); }
            else if (t == "}") { int k = stack.Pop(); if (k == -2) endSite = false; }
            else if (t == "OFFSET")
            {
                var o = new[] { float.Parse(tok[i], ci), float.Parse(tok[i + 1], ci), float.Parse(tok[i + 2], ci) }; i += 3;
                if (!endSite && last >= 0) offs[last] = o;
            }
            else if (t == "CHANNELS") { int n = int.Parse(tok[i++]); chans[last] = tok.Skip(i).Take(n).ToArray(); i += n; }
        }
        // (an End Site's "{" pushed -2: its parent stays the joint above)
        i++;  // MOTION
        int frames = 0;
        while (i < tok.Length) { if (tok[i] == "Frames:") { frames = int.Parse(tok[i + 1]); i += 2; } else if (tok[i] == "Time:") { i += 2; break; } else i++; }
        int nj = names.Count;
        var sk = new Skeleton { Names = names.ToArray(), Parent = parent.Select(p => p < 0 ? -1 : p).ToArray(), Local = new float[nj][] };
        for (int j = 0; j < nj; j++) sk.Local[j] = new[] { 0, 0, 0, 1f, offs[j][0], offs[j][1], offs[j][2], 0, 1, 1, 1, 0 };
        var ord = new List<int>(); var done = new bool[nj];
        for (int pass = 0; pass < nj && ord.Count < nj; pass++)
            for (int j = 0; j < nj; j++) if (!done[j] && (sk.Parent[j] < 0 || done[sk.Parent[j]])) { done[j] = true; ord.Add(j); }
        sk.Order = ord.ToArray();
        sk.RestWorld = sk.InvRest = Anim.World(sk, null, 0, false);
        var m = new Motion { Name = Path.GetFileNameWithoutExtension(path), Frames = frames };
        var rv = Enumerable.Range(0, nj).Select(_ => new float[frames][]).ToArray();
        var pv = Enumerable.Range(0, nj).Select(_ => new float[frames][]).ToArray();
        for (int f = 0; f < frames; f++)
            for (int j = 0; j < nj; j++)
            {
                var q = new float[] { 0, 0, 0, 1 }; float[] p = null;
                foreach (var ch in chans[j])
                {
                    float v = i < tok.Length ? float.Parse(tok[i++], ci) : 0;
                    if (ch.EndsWith("rotation", StringComparison.OrdinalIgnoreCase)) q = Anim.QMul(q, AxisQ(char.ToUpperInvariant(ch[0]), v));
                    else { if (p == null) p = new float[3]; p[char.ToUpperInvariant(ch[0]) - 'X'] = v; }
                }
                if (f > 0 && q[0] * rv[j][f - 1][0] + q[1] * rv[j][f - 1][1] + q[2] * rv[j][f - 1][2] + q[3] * rv[j][f - 1][3] < 0) q = new[] { -q[0], -q[1], -q[2], -q[3] };
                rv[j][f] = Anim.QNorm(q); pv[j][f] = p;
            }
        var keys = Enumerable.Range(0, frames).ToArray();
        for (int j = 0; j < nj; j++)
        {
            m.Rot[j] = new Motion.Track { Keys = keys, Vals = rv[j] };
            if (pv[j][0] != null) m.Pos[j] = new Motion.Track { Keys = keys, Vals = pv[j] };
        }
        var clip = FromMotion(sk, m, m.Name);
        if (clip == null) throw new Exception("BVH sem os ossos mestres: " + Missing(Map(sk)));
        return clip;
    }

    // ---- .hanim files ----

    public static void Write(Clip c, string path)
    {
        var ci = CultureInfo.InvariantCulture;
        var have = Enumerable.Range(0, Bones.Length).Where(b => c.Rot[b] != null).ToList();
        var sb = new StringBuilder();
        sb.AppendLine("HANIM 1");
        sb.AppendLine("name " + c.Name);
        sb.AppendLine("frames " + c.Frames);
        sb.AppendLine("left " + c.Left.ToString(ci));
        sb.AppendLine("bones " + string.Join(" ", have.Select(b => Bones[b])));
        sb.AppendLine("# per frame: hips x y z (hip heights), then x y z w of each bone above (its turn from the T-pose)");
        for (int f = 0; f < c.Frames; f++)
        {
            sb.Append(string.Join(" ", c.Hips[f].Select(v => v.ToString("0.#####", ci))));
            foreach (int b in have) { sb.Append("  "); sb.Append(string.Join(" ", c.Rot[b][f].Select(v => v.ToString("0.#####", ci)))); }
            sb.AppendLine();
        }
        File.WriteAllText(path, sb.ToString());
    }

    public static Clip Read(string path)
    {
        var ci = CultureInfo.InvariantCulture;
        var c = new Clip();
        var have = new List<int>();
        var rows = new List<float[]>();
        foreach (var raw in File.ReadAllLines(path))
        {
            var line = raw.Trim();
            if (line == "" || line.StartsWith("#") || line.StartsWith("HANIM")) continue;
            if (line.StartsWith("name ")) { c.Name = line.Substring(5).Trim(); continue; }
            if (line.StartsWith("frames ")) continue;
            if (line.StartsWith("left ")) { c.Left = float.Parse(line.Substring(5), ci); continue; }
            if (line.StartsWith("bones ")) { have = line.Substring(6).Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries).Select(x => Array.IndexOf(Bones, x)).ToList(); continue; }
            rows.Add(line.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries).Select(x => float.Parse(x, ci)).ToArray());
        }
        c.Frames = rows.Count;
        c.Hips = rows.Select(r => new[] { r[0], r[1], r[2] }).ToArray();
        for (int i = 0; i < have.Count; i++)
        {
            int b = have[i];
            if (b < 0) continue;
            c.Rot[b] = rows.Select(r => Anim.QNorm(new[] { r[3 + 4 * i], r[4 + 4 * i], r[5 + 4 * i], r[6 + 4 * i] })).ToArray();
        }
        return c;
    }

    // ---- an FBX's skeleton and animations as the game's kinds ----

    // A column-vector matrix (FBX) as rotation, translation and scale.
    static float[] Local12(double[] m)
    {
        double sx = Math.Sqrt(m[0] * m[0] + m[4] * m[4] + m[8] * m[8]), sy = Math.Sqrt(m[1] * m[1] + m[5] * m[5] + m[9] * m[9]), sz = Math.Sqrt(m[2] * m[2] + m[6] * m[6] + m[10] * m[10]);
        if (sx < 1e-12) sx = 1; if (sy < 1e-12) sy = 1; if (sz < 1e-12) sz = 1;
        double r00 = m[0] / sx, r10 = m[4] / sx, r20 = m[8] / sx, r01 = m[1] / sy, r11 = m[5] / sy, r21 = m[9] / sy, r02 = m[2] / sz, r12 = m[6] / sz, r22 = m[10] / sz;
        double x, y, z, w, tr = r00 + r11 + r22;
        if (tr > 0) { double s = Math.Sqrt(tr + 1) * 2; w = s / 4; x = (r21 - r12) / s; y = (r02 - r20) / s; z = (r10 - r01) / s; }
        else if (r00 > r11 && r00 > r22) { double s = Math.Sqrt(1 + r00 - r11 - r22) * 2; w = (r21 - r12) / s; x = s / 4; y = (r01 + r10) / s; z = (r02 + r20) / s; }
        else if (r11 > r22) { double s = Math.Sqrt(1 + r11 - r00 - r22) * 2; w = (r02 - r20) / s; x = (r01 + r10) / s; y = s / 4; z = (r12 + r21) / s; }
        else { double s = Math.Sqrt(1 + r22 - r00 - r11) * 2; w = (r10 - r01) / s; x = (r02 + r20) / s; y = (r12 + r21) / s; z = s / 4; }
        var q = Anim.QNorm(new[] { (float)x, (float)y, (float)z, (float)w });
        return new[] { q[0], q[1], q[2], q[3], (float)m[3], (float)m[7], (float)m[11], 0, (float)sx, (float)sy, (float)sz, 0 };
    }

    public static Skeleton SkeletonOf(FbxModel f)
    {
        int n = f.Bones.Count;
        var s = new Skeleton { Names = f.Bones.Select(b => b.Name).ToArray(), Parent = f.Bones.Select(b => b.Parent).ToArray(), Local = new float[n][] };
        for (int j = 0; j < n; j++) s.Local[j] = Local12(f.Bones[j].Parent < 0 ? FbxModel.Mul(f.Bones[j].Outer ?? FbxModel.Ident(), f.Bones[j].Local) : f.Bones[j].Local);
        var order = new List<int>(); var done = new bool[n];
        for (int pass = 0; pass < n && order.Count < n; pass++)
            for (int j = 0; j < n; j++)
                if (!done[j] && (s.Parent[j] < 0 || done[s.Parent[j]])) { done[j] = true; order.Add(j); }
        s.Order = order.ToArray();
        var w = Anim.World(s, null, 0, false);
        s.RestWorld = w; s.InvRest = w;
        return s;
    }

    public static Motion MotionOf(Skeleton s, FbxModel.Take t)
    {
        var m = new Motion { Name = t.Name, Frames = t.Frames };
        for (int j = 0; j < s.Names.Length; j++)
        {
            var r = new float[t.Frames][]; var p = new float[t.Frames][];
            for (int f = 0; f < t.Frames; f++)
            {
                var l = Local12(t.Local[f][j]);
                r[f] = new[] { l[0], l[1], l[2], l[3] };
                if (f > 0 && r[f][0] * r[f - 1][0] + r[f][1] * r[f - 1][1] + r[f][2] * r[f - 1][2] + r[f][3] * r[f - 1][3] < 0) r[f] = new[] { -r[f][0], -r[f][1], -r[f][2], -r[f][3] };
                p[f] = new[] { l[4], l[5], l[6] };
            }
            m.Rot[j] = new Motion.Track { Keys = Enumerable.Range(0, t.Frames).ToArray(), Vals = r };
            m.Pos[j] = new Motion.Track { Keys = Enumerable.Range(0, t.Frames).ToArray(), Vals = p };
        }
        return m;
    }
}
