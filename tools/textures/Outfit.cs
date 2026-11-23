// A character from outside the game (an OBJ + MTL with its textures, e.g. one exported from another game or
// made in Blender) as the player's body: CLI "outfit <media> <BASE> <file.obj> <mod folder>".
//
// The base (a player model, NIER030) keeps its skeleton, animations, sword and shaders; its body polygons go
// and the OBJ's come in. The OBJ is turned to the base's axes (Y up, facing the same way: told by where the
// feet point), scaled to the base's height and stood on its feet; each of its vertices takes the bones of the
// nearest vertex of the base's body (EditModel.AddSkinned). Its textures go into one atlas painted over the
// base's body texture (same size), and the texture coordinates follow. The shadow gets the same polygons.
// A picture of the result (front and side, rest pose and a frame of an animation) is saved next to the mod.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;

static class Outfit
{
    // The joints of the body itself (spine, head, arms, hands and fingers, legs, feet).
    const string BodyJoints = "^(AS_.*|N_ALL|HIPSBONE|HIPS|SPINE\\d*|NECK\\d*|HEAD|(LEFT|RIGHT)(SHOULDER|ARM|FOREARM|HAND|UPLEG|LEG|FOOT|TOEBASE|TOE|ARMROLL|FOREARMROLL|UPLEGROLL|LEGROLL)\\d*|(LEFT|RIGHT)HAND(THUMB|INDEX|MIDDLE|RING|PINKY)\\d*)$";

    // Cut: its texture's alpha cuts holes (hair strands, feathers, lashes): drawn with the base's hair material.
    class Mat { public string Name, File; public byte[] Px; public int W, H, X, Y, SW, SH; public float MinU, MinV; public bool Cut; }

    // For the editor: the colour picture each mesh of an FBX would use on its own (its first material slot's), or null.
    public static Dictionary<string, string> AutoTextures(FbxModel fbx, string file)
    {
        string dir = Path.GetDirectoryName(Path.GetFullPath(file));
        var r = new Dictionary<string, string>();
        foreach (var me in fbx.Meshes)
        {
            string c = null;
            for (int s = 0; s < me.MatTexAll.Count && c == null; s++) c = ColourOf(me.MatTexAll[s], s < me.MatTex.Count ? me.MatTex[s] : null, dir);
            r[me.Name] = c;
        }
        return r;
    }

    public static void Build(Cpk[] cpks, string baseName, string obj, string modDir, Action<string> log, Dictionary<string, string> texMap = null)
    {
        texMap = texMap ?? new Dictionary<string, string>();
        CpkEntry ve, pe; var c = Program.FindModel(cpks, "CHARA/" + baseName, out ve, out pe);
        if (c == null) throw new Exception("modelo " + baseName + " nao achado");
        var em = EditModel.Load(baseName, c.Read(ve), c.Read(pe));

        // ---- the OBJ ----
        var ci = CultureInfo.InvariantCulture;
        var V = new List<float[]>(); var VT = new List<float[]>();
        var faces = new List<int[]>();  // v0,t0,v1,t1,v2,t2,material
        // Which part of the file each face came from (FBX mesh; OBJ group and material, "\u0001" between), for the
        // editor's "follows" choice (follow:<part> = a bone group).
        var faceLabel = new Dictionary<int[], string>();
        var mats = new List<Mat>(); var matIx = new Dictionary<string, int>();
        // Meshes of other states a game model carries (torn clothes, other faces, levels of detail): left out.
        var skip = new System.Text.RegularExpressions.Regex(texMap.ContainsKey("sem") ? texMap["sem"] : "broken|damage|serious|angry|sad|surprise|lod[1-9]", System.Text.RegularExpressions.RegexOptions.IgnoreCase);
        string dir = Path.GetDirectoryName(Path.GetFullPath(obj));
        int cur = -1;
        Func<string, int> matOf = n => { int i; if (!matIx.TryGetValue(n, out i)) { i = mats.Count; matIx[n] = i; mats.Add(new Mat { Name = n }); } return i; };
        // FBX: its own skeleton and skin come along (the bone translator below); V gets the bones' places after
        // the mesh's points, so they turn and scale with it.
        List<Dictionary<int, double>> VW = null; FbxModel fbx = null; int nMesh;
        if (obj.EndsWith(".fbx", StringComparison.OrdinalIgnoreCase))
        {
            fbx = FbxModel.Load(obj);
            VW = new List<Dictionary<int, double>>();
            // Each kept mesh's colour map per material slot; a slot with none that looks like colour takes the
            // colour of the nearest mesh that has one (2B's head skin links only its normal map and a mask; its
            // colour is the face's, which the eyes use).
            var kept = fbx.Meshes.Where(me => { if (skip.IsMatch(me.Name)) { log("FBX: malha " + me.Name + " deixada de fora"); return false; } return true; }).ToList();
            Func<FbxModel.Mesh, double[]> centre = me => { var cc = new double[3]; int n = me.Pos.Length / 3; for (int i = 0; i < n; i++) for (int k = 0; k < 3; k++) cc[k] += me.Pos[i * 3 + k] / n; return cc; };
            var colour = kept.ToDictionary(me => me, me => Enumerable.Range(0, Math.Max(me.MatTex.Count, me.MatTexAll.Count)).Select(s =>
                s < me.MatTexAll.Count ? ColourOf(me.MatTexAll[s], s < me.MatTex.Count ? me.MatTex[s] : null, dir) : null).ToList());
            foreach (var me in kept)
                for (int s = 0; s < colour[me].Count; s++)
                {
                    if (colour[me][s] != null) continue;
                    var c0 = centre(me);
                    var near = kept.Where(o => o != me && colour[o].Any(x => x != null)).OrderBy(o => { var c1 = centre(o); return (c0[0] - c1[0]) * (c0[0] - c1[0]) + (c0[1] - c1[1]) * (c0[1] - c1[1]) + (c0[2] - c1[2]) * (c0[2] - c1[2]); }).FirstOrDefault();
                    // its average colour, flat: the neighbour's picture is laid out for other polygons (2B's head took
                    // the face sheet's teeth and eye)
                    if (near != null) { var f = colour[near].First(x => x != null); colour[me][s] = "flat:" + AverageColour(f); log("FBX: malha " + me.Name + " sem textura de cor: cor lisa " + colour[me][s].Substring(5) + " (media de " + Path.GetFileName(f) + ", de " + near.Name + ")"); }
                }
            // The editor's texture list: "mesh:<name>" = an image for every material slot of that mesh.
            foreach (var me in kept)
            {
                string ov;
                if (!texMap.TryGetValue("mesh:" + me.Name, out ov) || !File.Exists(ov)) continue;
                if (colour[me].Count == 0) colour[me].Add(null);
                for (int s = 0; s < colour[me].Count; s++) colour[me][s] = ov;
                log("FBX: malha " + me.Name + ": textura escolhida " + Path.GetFileName(ov));
            }
            foreach (var me in kept)
            {
                int vb = V.Count;
                for (int i = 0; i + 2 < me.Pos.Length; i += 3) { V.Add(new[] { (float)me.Pos[i], (float)me.Pos[i + 1], (float)me.Pos[i + 2] }); VW.Add(me.Weights != null ? me.Weights[i / 3] : new Dictionary<int, double>()); }
                for (int pi = 0; pi < me.Polys.Count; pi++)
                {
                    var poly = me.Polys[pi]; var puv = me.PolyUv[pi];
                    int slot = me.PolyMat[pi];
                    string texf = slot >= 0 && slot < colour[me].Count ? colour[me][slot] : null;
                    if (texf == null && colour[me].Count > 0 && texMap.ContainsKey("mesh:" + me.Name)) texf = colour[me][0];  // polygons with no slot
                    if (texf == null && slot >= 0 && slot < me.MatTex.Count) texf = me.MatTex[slot];
                    int mi = matOf(texf ?? "none");
                    if (texf != null) mats[mi].File = texf;
                    int tb = VT.Count;
                    for (int k = 0; k < poly.Length; k++) VT.Add(new[] { (float)puv[k * 2], (float)puv[k * 2 + 1] });
                    for (int k = 1; k + 1 < poly.Length; k++)
                    {
                        var face = new[] { vb + poly[0], tb, vb + poly[k], tb + k, vb + poly[k + 1], tb + k + 1, mi };
                        faces.Add(face); faceLabel[face] = me.Name;
                    }
                }
            }
            nMesh = V.Count;
            foreach (var b in fbx.Bones) V.Add(new[] { (float)b.World[3], (float)b.World[7], (float)b.World[11] });
            log("FBX: " + nMesh + " pontos, " + faces.Count + " triangulos, " + mats.Count + " texturas, " + fbx.Bones.Count + " ossos");
        }
        else nMesh = -1;
        bool skipGroup = false, skipMat = false; string objGroup = "", objMat = "";  // an OBJ group (o/g) left out by the same pattern as the FBX meshes (the editor's tick list)
        if (fbx == null) foreach (var raw in File.ReadLines(obj))
        {
            var t = raw.Trim().Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
            if (t.Length == 0 || t[0].StartsWith("#")) continue;
            if (t[0] == "v") V.Add(new[] { float.Parse(t[1], ci), float.Parse(t[2], ci), float.Parse(t[3], ci) });
            else if (t[0] == "vt") VT.Add(new[] { float.Parse(t[1], ci), float.Parse(t[2], ci) });
            else if (t[0] == "usemtl") { cur = matOf(t[1]); skipMat = skip.IsMatch(t[1]); objMat = t[1]; }
            else if (t[0] == "o" || t[0] == "g") { objGroup = string.Join(" ", t.Skip(1)); skipGroup = t.Length > 1 && skip.IsMatch(string.Join(" ", t.Skip(1))); if (skipGroup) log("OBJ: grupo " + string.Join(" ", t.Skip(1)) + " deixado de fora"); }
            else if (t[0] == "mtllib")
            {
                string mf = Path.Combine(dir, string.Join(" ", t.Skip(1))); string name = null;
                if (File.Exists(mf))
                    foreach (var ml in File.ReadLines(mf))
                    {
                        var u = ml.Trim().Split(new[] { ' ', '\t' }, 2, StringSplitOptions.RemoveEmptyEntries);
                        if (u.Length < 2) continue;
                        if (u[0] == "newmtl") name = u[1];
                        else if (u[0] == "map_Kd" && name != null) mats[matOf(name)].File = Path.Combine(dir, u[1]);
                    }
            }
            else if (t[0] == "f" && !skipGroup && !skipMat)
            {
                var vs = new List<int[]>();
                for (int k = 1; k < t.Length; k++)
                {
                    var s = t[k].Split('/');
                    int vi = int.Parse(s[0]); vi = vi < 0 ? V.Count + vi : vi - 1;
                    int ti = s.Length > 1 && s[1] != "" ? int.Parse(s[1]) : 0; ti = ti < 0 ? VT.Count + ti : ti - 1;
                    vs.Add(new[] { vi, ti });
                }
                for (int k = 1; k + 1 < vs.Count; k++)
                {
                    var face = new[] { vs[0][0], vs[0][1], vs[k][0], vs[k][1], vs[k + 1][0], vs[k + 1][1], cur < 0 ? matOf("none") : cur };
                    faces.Add(face); faceLabel[face] = objGroup + "\u0001" + objMat;
                }
            }
        }
        if (nMesh < 0)
        {
            // Exporters that write every triangle's corners apart (Prisma3D: 3 points per face): the same place is
            // one point, so the polygons hang together when they bend.
            var at = new Dictionary<Tuple<float, float, float>, int>(); var nv = new List<float[]>(); var remap = new int[V.Count];
            for (int i = 0; i < V.Count; i++)
            {
                var k = Tuple.Create(V[i][0], V[i][1], V[i][2]); int j;
                if (!at.TryGetValue(k, out j)) { j = nv.Count; at[k] = j; nv.Add(V[i]); }
                remap[i] = j;
            }
            foreach (var f in faces) { f[0] = remap[f[0]]; f[2] = remap[f[2]]; f[4] = remap[f[4]]; }
            if (nv.Count < V.Count) log("OBJ: " + V.Count + " pontos soldados em " + nv.Count);
            V = nv;
            nMesh = V.Count; log("OBJ: " + V.Count + " vertices, " + faces.Count + " triangulos, " + mats.Count + " materiais");
        }
        // Textures named on the command line (material=file) for an OBJ without its MTL.
        foreach (var kv in texMap) { int mi; if (matIx.TryGetValue(kv.Key, out mi)) mats[mi].File = kv.Value; }
        foreach (var m in mats) log("  material " + m.Name + " -> " + (m.File == null ? "(sem textura)" : Path.GetFileName(m.File)));
        var VM = V.Take(nMesh).ToList();  // the mesh's points only (the statistics)
        // ---- the base's body ----
        var body = em.Buffers.Where(b => b.Drawn && b.Tex != null && b.Tex.IndexOf("SWORD", StringComparison.OrdinalIgnoreCase) < 0).ToList();
        var main = body.OrderByDescending(b => b.Count).First();
        var sources = body.Where(b => b.Sig == main.Sig && b.Stride == main.Stride).ToList();
        log("parte principal " + main.Index + " (" + main.Tex + ", " + main.Count + " v), layout " + main.Sig + "; origens " + string.Join(",", sources.Select(b => b.Index)));
        var bp = new List<float[]>(); foreach (var b in body) for (int v = 0; v < b.Count; v++) bp.Add(em.Pos(b, v));

        // ---- axes, size and place ----
        // The OBJ's up axis: the longest side of its box (a standing character).
        var mn = new float[3]; var mx = new float[3];
        for (int k = 0; k < 3; k++) { mn[k] = VM.Min(p => p[k]); mx[k] = VM.Max(p => p[k]); }
        int up = 1; for (int k = 0; k < 3; k++) if (mx[k] - mn[k] > mx[up] - mn[up]) up = k;
        // Which end is the head: the one with more vertices (a face and hair beat two feet).
        int topCount = VM.Count(p => (p[up] - mn[up]) / (mx[up] - mn[up]) > 0.85f), botCount = VM.Count(p => (p[up] - mn[up]) / (mx[up] - mn[up]) < 0.15f);
        float upSign = topCount >= botCount ? 1 : -1;
        log("OBJ: eixo de cima " + "XYZ"[up] + (upSign > 0 ? "+" : "-") + " (topo " + topCount + " v, base " + botCount + " v)");
        // Rotations only (no mirror): up -> +Y; a head pointing down is turned half a circle about X.
        var P = V.Select(p =>
        {
            float x, y, z;
            if (up == 1) { x = p[0]; y = p[1]; z = p[2]; }
            else if (up == 2) { x = p[0]; y = p[2]; z = -p[1]; }
            else { x = -p[1]; y = p[0]; z = p[2]; }
            if (upSign < 0) { y = -y; z = -z; }
            return new[] { x, y, z };
        }).ToList();
        var PM = P.Take(nMesh).ToList();  // the same arrays: the mesh's points
        // Facing: the feet point forward (the bottom slab's centre is in front of the legs' middle).
        Func<List<float[]>, float> facing = pts =>
        {
            float lo = pts.Min(p => p[1]), hi = pts.Max(p => p[1]), hgt = hi - lo;
            var feet = pts.Where(p => p[1] < lo + hgt * 0.04f).ToList();
            var shins = pts.Where(p => p[1] > lo + hgt * 0.10f && p[1] < lo + hgt * 0.25f).ToList();
            return feet.Average(p => p[2]) - shins.Average(p => p[2]);
        };
        // A piece floating above the head (a halo) is not worn: a material wholly above every other one goes.
        for (int mi = 0; mi < mats.Count; mi++)
        {
            int m0 = mi;
            var mine = faces.Where(f => f[6] == m0).SelectMany(f => new[] { f[0], f[2], f[4] }).ToList();
            var rest = faces.Where(f => f[6] != m0).SelectMany(f => new[] { f[0], f[2], f[4] }).ToList();
            if (mine.Count == 0 || rest.Count == 0) continue;
            if (mine.Min(i => P[i][1]) > rest.Max(i => P[i][1]))
            {
                log("material " + mats[mi].Name + " flutua acima da cabeca (aureola?): fora");
                faces.RemoveAll(f => f[6] == m0);
            }
        }
        float fb = facing(bp), fo = facing(PM);
        log("frente: base " + fb.ToString("F3") + ", OBJ " + fo.ToString("F3"));
        // Turned half a circle when it faces away. A skeleton with sides says it surely: the model's left hand on the
        // base's left (2026-11-16: Caim's straight boots gave the feet test 0.000, read as "away": he was turned, his
        // Mixamo left arm on the father's right). Without one, the feet test, and no turn when it cannot tell.
        bool flip;
        {
            bool? byBones = null;
            if (fbx != null && fbx.Bones.Count > 1)
            {
                var lxs = new List<float>(); var rxs = new List<float>();
                for (int b = 0; b < fbx.Bones.Count && nMesh + b < P.Count; b++)
                {
                    int sd; string sl = Slot(fbx.Bones[b].Name, out sd);
                    if (sl != "HAND" && sl != "FOREARM" && sl != "ARM" && sl != "UPLEG") continue;
                    if (sd > 0) lxs.Add(P[nMesh + b][0]); else if (sd < 0) rxs.Add(P[nMesh + b][0]);
                }
                var bsk = Anim.Load(em.Mdv, new List<Motion>());
                int bl = Array.IndexOf(bsk.Names, "LEFTHAND"), br = Array.IndexOf(bsk.Names, "RIGHTHAND");
                if (lxs.Count > 0 && rxs.Count > 0 && bl >= 0 && br >= 0)
                    byBones = Math.Sign(lxs.Average() - rxs.Average()) != Math.Sign(bsk.RestWorld[bl][12] - bsk.RestWorld[br][12]);
            }
            float tiny = 0.002f * (PM.Max(p => p[1]) - PM.Min(p => p[1]));
            flip = byBones ?? Math.Sign(fb) != Math.Sign(fo);
            log("frente: " + (byBones != null ? "pelos ossos esquerdo/direito" : "pelos pes" + (Math.Abs(fo) <= tiny ? " (sinal fraco: confira a previa)" : "")) + (flip ? " -> virado 180 graus" : " -> como esta"));
        }
        if (flip) foreach (var p in P) { p[0] = -p[0]; p[2] = -p[2]; }
        float bLo = bp.Min(p => p[1]), bHi = bp.Max(p => p[1]), oLo = PM.Min(p => p[1]), oHi = PM.Max(p => p[1]);
        float scale = (bHi - bLo) / (oHi - oLo);
        float bcx = (bp.Min(p => p[0]) + bp.Max(p => p[0])) / 2, ocx = (PM.Min(p => p[0]) + PM.Max(p => p[0])) / 2;
        Func<List<float[]>, float, float, float> midZ = (pts, lo, hi) => pts.Where(p => p[1] > lo + (hi - lo) * 0.45f && p[1] < lo + (hi - lo) * 0.6f).Average(p => p[2]);
        float bcz = midZ(bp, bLo, bHi), ocz = midZ(PM, oLo, oHi);
        foreach (var p in P) { p[0] = (p[0] - ocx) * scale + bcx; p[1] = (p[1] - oLo) * scale + bLo; p[2] = (p[2] - ocz) * scale + bcz; }
        Func<List<float[]>, float, float, float> span = (pts, lo, hi) => { var s = pts.Where(p => p[1] > lo + (hi - lo) * 0.7f && p[1] < lo + (hi - lo) * 0.85f).ToList(); return s.Max(p => p[0]) - s.Min(p => p[0]); };
        log("altura " + (bHi - bLo).ToString("F2") + " m (escala " + scale.ToString("G4") + "); largura nos ombros/bracos: base " + span(bp, bLo, bHi).ToString("F2") + ", OBJ " + span(PM, bLo, bHi).ToString("F2"));
        LastAlign = new Alignment { Up = up, UpSign = upSign, Flip = flip, Scale = scale,
                                    From = new[] { ocx, oLo, ocz }, To = new[] { bcx, bLo, bcz } };
        if (texMap.ContainsKey("alignonly")) return;
        // The editor's moves of a part (xform:<part> = "x y z rx ry rz scale": metres, degrees, factor), about the
        // middle of its box, in the base's frame, before the bones.
        foreach (var kv in texMap.Where(q => q.Key.StartsWith("xform:")))
        {
            string xpart = kv.Key.Substring(6);
            var pts = new HashSet<int>();
            foreach (var fc in faces)
            {
                string lab; if (!faceLabel.TryGetValue(fc, out lab)) continue;
                if (lab.Split('\u0001').Contains(xpart)) { pts.Add(fc[0]); pts.Add(fc[2]); pts.Add(fc[4]); }
            }
            if (pts.Count == 0) continue;
            var moved = PartXform(pts.Select(i => P[i]).ToList(), kv.Value);
            int k = 0; foreach (int i in pts) { var q = moved[k++]; P[i][0] = q[0]; P[i][1] = q[1]; P[i][2] = q[2]; }
            log("ajuste: " + xpart + " (" + kv.Value + "), " + pts.Count + " pontos");
        }
        // Faces inside out (2026-11-16: Caim, a PS2 rip, turns his triangles the other way round: the game culls what
        // it takes for the back and drew him hollow, the trousers as strips, his face seen from behind; the editor's
        // preview draws both sides). Each part's signed volume about its middle against the father's body: the parts
        // of the other sign get their triangles turned.
        {
            // the share of triangles (by area) whose normal points away from the part's middle, -1..1 (1 all out)
            Func<List<float[]>, float> vol = tri =>
            {
                double cx = 0, cy = 0, cz = 0; foreach (var q in tri) { cx += q[0]; cy += q[1]; cz += q[2]; }
                cx /= tri.Count; cy /= tri.Count; cz /= tri.Count;
                double outA = 0, all = 0;
                for (int i = 0; i + 2 < tri.Count; i += 3)
                {
                    double ux = tri[i + 1][0] - tri[i][0], uy = tri[i + 1][1] - tri[i][1], uz = tri[i + 1][2] - tri[i][2];
                    double vx = tri[i + 2][0] - tri[i][0], vy = tri[i + 2][1] - tri[i][1], vz = tri[i + 2][2] - tri[i][2];
                    double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx, area = Math.Sqrt(nx * nx + ny * ny + nz * nz);
                    if (area < 1e-12) continue;
                    double gx = (tri[i][0] + tri[i + 1][0] + tri[i + 2][0]) / 3 - cx, gy = (tri[i][1] + tri[i + 1][1] + tri[i + 2][1]) / 3 - cy, gz = (tri[i][2] + tri[i + 1][2] + tri[i + 2][2]) / 3 - cz;
                    double d = nx * gx + ny * gy + nz * gz;
                    all += area; outA += d > 0 ? area : -area;
                }
                return all > 0 ? (float)(outA / all) : 0;
            };
            var baseTri = new List<float[]>(); var mainTris = em.Tris(main);
            for (int i = 0; i + 2 < mainTris.Count; i += 3) { baseTri.Add(em.Pos(main, mainTris[i])); baseTri.Add(em.Pos(main, mainTris[i + 1])); baseTri.Add(em.Pos(main, mainTris[i + 2])); }
            float baseOut = vol(baseTri), baseSign = Math.Sign(baseOut);
            var byPart = new Dictionary<string, List<int[]>>();
            foreach (var fc in faces) { string lab; faceLabel.TryGetValue(fc, out lab); lab = lab ?? ""; List<int[]> l; if (!byPart.TryGetValue(lab, out l)) byPart[lab] = l = new List<int[]>(); l.Add(fc); }
            // Consistent winding (2026-11-16: Caim, from PS2 triangle strips, mixes triangles turned either way in one
            // part): faces sharing an edge must run it in opposite directions; walking the edges, each connected piece
            // of a part keeps the way most of its faces had, and the ones against it are turned. Positions are welded
            // for this (Caim's FBX gives every triangle its own points).
            int fixedWinding = 0, pieces = 0, closedTurned = 0;
            // a model with its own skeleton and weights is taken as made right (2B's skirt is lined inside on purpose);
            // a rip without one (Caim, Zero) may have whole pieces inside out
            bool ripModel = fbx == null || fbx.Bones.Count <= 1;
            if (Environment.GetEnvironmentVariable("NT_OUTFIT_NOWINDING") == null)
                foreach (var kv in byPart)
                {
                    var fl = kv.Value;
                    var wid = new Dictionary<Tuple<int, int, int>, int>();
                    Func<int, int> W0 = v => { var k = Tuple.Create((int)Math.Round(P[v][0] * 1e4), (int)Math.Round(P[v][1] * 1e4), (int)Math.Round(P[v][2] * 1e4)); int id; if (!wid.TryGetValue(k, out id)) { id = wid.Count; wid[k] = id; } return id; };
                    var tri = fl.Select(fc => new[] { W0(fc[0]), W0(fc[2]), W0(fc[4]) }).ToList();
                    var edges = new Dictionary<long, List<int>>();
                    Func<int, int, long> ek = (a, b) => a < b ? ((long)a << 32) | (uint)b : ((long)b << 32) | (uint)a;
                    for (int i = 0; i < tri.Count; i++)
                        for (int e = 0; e < 3; e++) { long k = ek(tri[i][e], tri[i][(e + 1) % 3]); List<int> l; if (!edges.TryGetValue(k, out l)) edges[k] = l = new List<int>(); l.Add(i); }
                    var wflip = new int[tri.Count];  // 0 unseen, 1 as is, -1 turned
                    for (int s = 0; s < tri.Count; s++)
                    {
                        if (wflip[s] != 0) continue;
                        pieces++;
                        var comp = new List<int> { s }; wflip[s] = 1;
                        for (int qi = 0; qi < comp.Count; qi++)
                        {
                            int f = comp[qi];
                            for (int e = 0; e < 3; e++)
                            {
                                int a = tri[f][e], b = tri[f][(e + 1) % 3];
                                if (wflip[f] < 0) { int x = a; a = b; b = x; }  // f's edge as it will run
                                var sharing = edges[ek(a, b)];
                                if (sharing.Count != 2) continue;  // edges of 3+ faces (layers, double-sided copies): not followed
                                foreach (int g in sharing)
                                {
                                    if (g == f || wflip[g] != 0) continue;
                                    // the same triangle again (a double-sided copy): a different piece
                                    if (tri[g].OrderBy(x => x).SequenceEqual(tri[f].OrderBy(x => x))) continue;
                                    // does g run the edge a->b too (then it must be turned)?
                                    bool same = false;
                                    for (int k = 0; k < 3; k++) if (tri[g][k] == a && tri[g][(k + 1) % 3] == b) same = true;
                                    wflip[g] = same ? -1 : 1;
                                    comp.Add(g);
                                }
                            }
                        }
                        // the piece keeps the way most of it had
                        int turnedN = comp.Count(f => wflip[f] < 0);
                        bool invert = turnedN * 2 > comp.Count;
                        // a closed piece (every edge on two faces) pointing mostly inward is inside out as a whole
                        // (Caim's boots); open pieces (a skirt's lining, hair cards) are left as their maker made them
                        if (comp.Count >= 8)
                        {
                            int open = 0, all = 0;
                            foreach (int f in comp) for (int e = 0; e < 3; e++) { all++; if (edges[ek(tri[f][e], tri[f][(e + 1) % 3])].Count != 2) open++; }
                            var pts = new List<float[]>();
                            foreach (int f in comp)
                            {
                                var fc = fl[f]; bool t = (wflip[f] < 0) != invert;
                                pts.Add(P[fc[0]]); if (t) { pts.Add(P[fc[4]]); pts.Add(P[fc[2]]); } else { pts.Add(P[fc[2]]); pts.Add(P[fc[4]]); }
                            }
                            float outv = vol(pts);
                            if (ripModel && open * 20 <= all && outv * baseSign < -0.3f) { invert = !invert; closedTurned++; }
                            if (Environment.GetEnvironmentVariable("NT_WINDING_LOG") != null && comp.Count >= 30)
                                log("  pedaco de " + kv.Key.Replace("\u0001", "/") + ": " + comp.Count + " tri, bordas abertas " + (open * 100 / Math.Max(1, all)) + "%, para fora " + (int)(50 + outv * baseSign * 50) + "%");
                        }
                        foreach (int f in comp)
                        {
                            bool turnIt = (wflip[f] < 0) != invert;
                            if (!turnIt) continue;
                            var fc = fl[f]; int a = fc[2], b = fc[3]; fc[2] = fc[4]; fc[3] = fc[5]; fc[4] = a; fc[5] = b;
                            fixedWinding++;
                        }
                    }
                }
            log("faces do avesso misturadas: " + fixedWinding + " triangulos virados para acompanhar os vizinhos (" + pieces + " pedacos" + (closedTurned > 0 ? ", " + closedTurned + " pecas fechadas viradas do avesso inteiras" : "") + ")");
            var turned = new List<string>(); int turnedTris = 0;
            if (Environment.GetEnvironmentVariable("NT_OUTFIT_FLIPALL") == "1")
                foreach (var fc in faces) { int a = fc[2], b = fc[3]; fc[2] = fc[4]; fc[3] = fc[5]; fc[4] = a; fc[5] = b; }
            if (baseSign != 0 && Environment.GetEnvironmentVariable("NT_OUTFIT_NOWINDING") == null && Environment.GetEnvironmentVariable("NT_OUTFIT_FLIPALL") == null)
                foreach (var kv in byPart)
                {
                    var tri = new List<float[]>(); foreach (var fc in kv.Value) { tri.Add(P[fc[0]]); tri.Add(P[fc[2]]); tri.Add(P[fc[4]]); }
                    float v = vol(tri);
                    // turned only when clearly the other way: 65 % of its area or more (cloth cards and skirts, open
                    // surfaces, say little and are left alone)
                    if (v * baseSign > -0.3f || Environment.GetEnvironmentVariable("NT_OUTFIT_OUTWARD") == null) continue;  // off unless asked: 2B's skirts are lined inside on purpose
                    foreach (var fc in kv.Value) { int a = fc[2], b = fc[3]; fc[2] = fc[4]; fc[3] = fc[5]; fc[4] = a; fc[5] = b; }
                    turned.Add(kv.Key.Replace("\u0001", "/") + " " + (int)(50 - v * baseSign * 50) + "% p/ dentro"); turnedTris += kv.Value.Count;
                }
            log("faces do avesso: " + (turned.Count == 0 ? "nenhuma parte" : turned.Count + " de " + byPart.Count + " partes viradas (" + turnedTris + " triangulos): " + string.Join(", ", turned.Take(12))));
        }
        // "Spread the legs" (spreadlegs = metres): below the father's crotch every point goes out on its own side,
        // fully from 8 cm under it (2026-11-16: Caim's trousers are one piece; the legs stood too close for the
        // father's). The points in the middle are cut apart after the bones (SplitLegs) and each copy finishes on its
        // side.
        float spreadM = 0, spreadCx = 0, spreadCrotch = 0; var spreadHave = new Dictionary<int, float>();
        if (texMap.ContainsKey("spreadlegs") && float.TryParse(texMap["spreadlegs"], NumberStyles.Float, CultureInfo.InvariantCulture, out spreadM) && spreadM != 0)
        {
            var ssk = Anim.Load(em.Mdv, new List<Motion>());
            int sh = Array.IndexOf(ssk.Names, "HIPSBONE"), sl = Array.IndexOf(ssk.Names, "LEFTUPLEG"), sr = Array.IndexOf(ssk.Names, "RIGHTUPLEG");
            if (sh >= 0 && sl >= 0 && sr >= 0)
            {
                spreadCx = ssk.RestWorld[sh][12]; spreadCrotch = (ssk.RestWorld[sl][13] + ssk.RestWorld[sr][13]) / 2;
                int moved = 0;
                for (int i = 0; i < P.Count; i++)
                {
                    float off = SpreadOffset(P[i][0], P[i][1], spreadCx, spreadCrotch, spreadM);
                    if (off == 0) continue;
                    P[i][0] += off; spreadHave[i] = off; moved++;
                }
                log("pernas afastadas " + (spreadM * 100).ToString("0.#") + " cm para cada lado: " + moved + " pontos");
                if (!texMap.ContainsKey("splitlegs")) texMap["splitlegs"] = "1";
            }
        }

        // ---- bones of the new body ----
        var sk = Anim.Load(em.Mdv, new List<Motion>());
        int[][] rigJ = null; float[][] rigW = null;
        Places = null;
        bool translated = fbx != null && Environment.GetEnvironmentVariable("NT_OUTFIT_AUTORIG") == null && TranslateBones(fbx, P, nMesh, VW, sk, out rigJ, out rigW, log);
        if (!translated) AutoRig(sk, PM, bp, bLo, bHi, out rigJ, out rigW, log);
        Follow(texMap, faces, faceLabel, P, sk, rigJ, rigW, log);
        if (texMap.ContainsKey("splitlegs")) SplitLegs(faces, P, ref rigJ, ref rigW, sk, log, spreadM, spreadCx, spreadCrotch, spreadHave);
        // The seam between the legs (2026-11-16: Caim's trousers are one piece, the legs sewn together inside; their
        // triangles, one point on each leg, stretched up to 9x into a spike as he ran): below the crotch a face whose
        // points hang on the two different legs goes. Auto-rigged models and "separate the legs" only (a model's own
        // weights, a skirt, are left as they are).
        // 2026-11-17: only with "separate the legs" (Caim). On Zero the same faces were her dress: 180 triangles went and
        // the skirt tore apart as she ran. Without it such faces are a skirt: their points hang mostly on the hips.
        if (rigJ != null)
        {
            Func<int, int> legOf = s =>
            {
                if (s < 0 || s >= rigJ.Length || rigJ[s] == null || rigJ[s].Length == 0) return 0;
                int b = 0; for (int k = 1; k < rigJ[s].Length; k++) if (rigW[s][k] > rigW[s][b]) b = k;
                string n = sk.Names[rigJ[s][b]];
                if (!System.Text.RegularExpressions.Regex.IsMatch(n, "UPLEG|LEG|FOOT|TOE")) return 0;
                return n.StartsWith("LEFT") ? 1 : n.StartsWith("RIGHT") ? -1 : 0;
            };
            Func<int[], bool> bridges = f =>
            {
                int la = legOf(f[0]), lb = legOf(f[2]), lc = legOf(f[4]);
                return (la == 1 || lb == 1 || lc == 1) && (la == -1 || lb == -1 || lc == -1);
            };
            if (texMap.ContainsKey("splitlegs"))
            {
                int seam = faces.RemoveAll(f => bridges(f));
                if (seam > 0) log("costura entre as pernas: " + seam + " triangulos ligando a perna esquerda a direita, tirados");
            }
            else
            {
                int hips = Array.IndexOf(sk.Names, "HIPSBONE");
                var skirt = new HashSet<int>();
                foreach (var f in faces) if (bridges(f)) { skirt.Add(f[0]); skirt.Add(f[2]); skirt.Add(f[4]); }
                if (hips >= 0 && skirt.Count > 0)
                {
                    foreach (int v in skirt)
                    {
                        if (v < 0 || v >= rigJ.Length || rigJ[v] == null || rigJ[v].Length == 0) continue;
                        int b = 0; for (int k = 1; k < rigJ[v].Length; k++) if (rigW[v][k] > rigW[v][b]) b = k;
                        int leg = rigJ[v][b];
                        rigJ[v] = leg == hips ? new[] { hips } : new[] { hips, leg };
                        rigW[v] = leg == hips ? new[] { 1f } : new[] { 0.7f, 0.3f };
                    }
                    log("saia: " + skirt.Count + " pontos que ligam as pernas presos mais ao quadril (sem rasgar)");
                }
            }
        }

        // ---- textures: one atlas the size of the base's body texture ----
        var tex = em.Texture(main.Tex);
        int AW = tex.Width, AH = tex.Height;
        foreach (var m in mats)
        {
            // a texture not where the file says: looked for by name in the model's folder and the ones under it
            if (m.File != null && !File.Exists(m.File))
            {
                var found = Directory.GetFiles(dir, Path.GetFileName(m.File), SearchOption.AllDirectories).FirstOrDefault();
                if (found != null) m.File = found;
            }
            if (m.File != null && m.File.StartsWith("flat:"))
            {
                int rgb = Convert.ToInt32(m.File.Substring(5), 16); m.W = m.H = 4; m.Px = new byte[64];
                for (int i = 0; i < 16; i++) { m.Px[i * 4] = (byte)(rgb >> 16); m.Px[i * 4 + 1] = (byte)(rgb >> 8); m.Px[i * 4 + 2] = (byte)rgb; m.Px[i * 4 + 3] = 255; }
            }
            else if (m.File != null && File.Exists(m.File)) m.Px = PngIo.Load(m.File, out m.W, out m.H);
            else { m.W = m.H = 4; m.Px = Enumerable.Repeat((byte)200, 64).ToArray(); }
            var mine = faces.Where(f => f[6] == mats.IndexOf(m)).ToList();
            var used = mine.SelectMany(f => new[] { f[1], f[3], f[5] }).Where(i => i >= 0 && i < VT.Count).ToList();
            m.MinU = used.Count > 0 ? (float)Math.Floor(used.Min(i => VT[i][0])) : 0; m.MinV = used.Count > 0 ? (float)Math.Floor(used.Min(i => VT[i][1])) : 0;
            // See-through where the model's own triangles lie (2026-11-16: Caim's body sheet has transparent margins
            // outside its UV islands, and the whole picture's alpha sent his body to the hair part, shredded in game).
            {
                int holes = 0, n = 0, midTone = 0, step = Math.Max(1, mine.Count / 3000);
                float[][] bary = { new[] { 1 / 3f, 1 / 3f }, new[] { 0.6f, 0.2f }, new[] { 0.2f, 0.6f }, new[] { 0.2f, 0.2f } };
                for (int fi = 0; fi < mine.Count; fi += step)
                {
                    var f = mine[fi];
                    if (f[1] < 0 || f[3] < 0 || f[5] < 0 || f[1] >= VT.Count || f[3] >= VT.Count || f[5] >= VT.Count) continue;
                    foreach (var b in bary)
                    {
                        float u = VT[f[1]][0] * b[0] + VT[f[3]][0] * b[1] + VT[f[5]][0] * (1 - b[0] - b[1]) - m.MinU;
                        float v = VT[f[1]][1] * b[0] + VT[f[3]][1] * b[1] + VT[f[5]][1] * (1 - b[0] - b[1]) - m.MinV;
                        u -= (float)Math.Floor(u); v -= (float)Math.Floor(v);
                        int px = Math.Min(m.W - 1, (int)(u * (m.W - 1) + 0.5f)), py = Math.Min(m.H - 1, (int)((1 - v) * (m.H - 1) + 0.5f));
                        int al = m.Px[(py * m.W + px) * 4 + 3];
                        n++; if (al < 128) holes++; if (al > 16 && al < 240) midTone++;
                    }
                }
                // Cut-outs are mostly clear or solid; an alpha full of mid tones is a mask (gloss, reflection: Caim's
                // PS2-era sheets), drawn solid.
                bool cutName = System.Text.RegularExpressions.Regex.IsMatch(Path.GetFileNameWithoutExtension(m.Name ?? "") + " " + Path.GetFileNameWithoutExtension(m.File ?? ""),
                    "hair|lash|fur|feather|leaf|leaves|grass|fringe|bang|cabelo|pelo|cilio|pena", System.Text.RegularExpressions.RegexOptions.IgnoreCase);
                m.Cut = n > 0 && holes > n / 20 && (cutName || midTone * 100 < n * 15);
                if (holes > n / 20 && !m.Cut) log("  " + Path.GetFileName(m.Name) + ": alfa e uma mascara (" + (midTone * 100 / Math.Max(1, n)) + "% de tons medios), desenhada opaca");
            }
        }
        // Grown to fill the atlas when they are small (whole-number factor, nearest pixel).
        // Too big together: all shrunk by the same amount until they fit.
        float factor = 1;
        for (int f = 8; f > 1; f--) if (Pack(mats, AW, AH, f, 4)) { factor = f; break; }
        while (!Pack(mats, AW, AH, factor, 4)) { factor -= 0.02f; if (factor < 0.1f) throw new Exception("texturas nao cabem em " + AW + "x" + AH); }
        var atlas = new byte[AW * AH * 4];
        foreach (var m in mats)
        {
            int w = (int)(m.W * factor), h = (int)(m.H * factor);
            m.SW = w; m.SH = h;
            for (int y = -4; y < h + 4; y++)
                for (int x = -4; x < w + 4; x++)
                {
                    int ax = m.X + x, ay = m.Y + y; if (ax < 0 || ay < 0 || ax >= AW || ay >= AH) continue;
                    // the source area under this pixel, averaged (shrinking) or the nearest pixel (growing)
                    int x0 = (int)(Math.Max(0, Math.Min(w - 1, x)) / factor), y0 = (int)(Math.Max(0, Math.Min(h - 1, y)) / factor);
                    int x1 = Math.Max(x0 + 1, (int)((Math.Max(0, Math.Min(w - 1, x)) + 1) / factor)), y1 = Math.Max(y0 + 1, (int)((Math.Max(0, Math.Min(h - 1, y)) + 1) / factor));
                    int r = 0, g = 0, b = 0, a = 0, n = 0;
                    for (int sy = y0; sy < y1 && sy < m.H; sy++)
                        for (int sx = x0; sx < x1 && sx < m.W; sx++) { int q = (sy * m.W + sx) * 4; r += m.Px[q]; g += m.Px[q + 1]; b += m.Px[q + 2]; a += m.Px[q + 3]; n++; }
                    if (n == 0) continue;
                    // alpha only where it cuts (the hair material's test); opaque elsewhere
                    int o = (ay * AW + ax) * 4; atlas[o] = (byte)(r / n); atlas[o + 1] = (byte)(g / n); atlas[o + 2] = (byte)(b / n); atlas[o + 3] = (m.Cut || (m.File != null && mats.Any(q => q.Cut && string.Equals(q.File, m.File, StringComparison.OrdinalIgnoreCase)))) ? (byte)(a / n) : (byte)255;
                }
        }
        foreach (var m in mats.Where(q => q.Cut)) log("  " + Path.GetFileName(m.Name) + ": transparencia recortada (material de cabelo)");
        log("atlas " + AW + "x" + AH + ", texturas x" + factor + " (" + tex.Name + ", formato 0x" + tex.Format.ToString("X") + ")");

        // ---- the polygons: one vertex per (position, coordinate, material) ----
        var key = new Dictionary<Tuple<int, float, float, int>, int>(); var pos = new List<float>(); var uv = new List<float>(); var tris = new List<int>();
        var srcV = new List<int>();  // the point of V each new vertex is
        var matV = new List<int>();  // and its material
        foreach (var f in faces)
            for (int k = 0; k < 3; k++)
            {
                int vi = f[k * 2], ti = f[k * 2 + 1], mi = f[6];
                var kk = Tuple.Create(vi, ti >= 0 && ti < VT.Count ? VT[ti][0] : 0f, ti >= 0 && ti < VT.Count ? VT[ti][1] : 0f, mi);
                int at;
                if (!key.TryGetValue(kk, out at))
                {
                    at = pos.Count / 3; key[kk] = at;
                    pos.AddRange(P[vi]); srcV.Add(vi); matV.Add(mi);
                    var m = mats[mi];
                    float u = ti >= 0 && ti < VT.Count ? VT[ti][0] - m.MinU : 0, v = ti >= 0 && ti < VT.Count ? VT[ti][1] - m.MinV : 0;
                    u = Math.Max(0, Math.Min(1, u)); v = Math.Max(0, Math.Min(1, v));
                    float w = m.SW, h = m.SH;
                    uv.Add((m.X + 0.5f + u * (w - 1)) / AW); uv.Add((m.Y + 0.5f + (1 - v) * (h - 1)) / AH);
                }
                tris.Add(at);
            }
        // The base's winding: the OBJ's faces are counter-clockwise from outside; check against its normals later.
        // Everything goes but the sword.
        // How this base packs its normals, measured on its own body before it goes (EditModel.NormalCheck).
        log(em.NormalCheck(main));
        log(em.UvExtraReport(main));
        foreach (var b in em.Buffers) if (!b.Drawn || b.Tex == null || b.Tex.IndexOf("SWORD", StringComparison.OrdinalIgnoreCase) < 0) em.ClearTris(b);
        var P3 = pos.ToArray(); var T3 = tris.ToArray(); var U2 = uv.ToArray();
        // Bones from the body only: a vertex of the base moved by the coat, the hair, the collars or the face
        // (lips, eyelids) would drag the new body with it (the legs inside the coat swung with its tails).
        if (Environment.GetEnvironmentVariable("NT_OUTFIT_JOINTS") != null) log("juntas: " + string.Join(" ", sk.Names));
        Func<string, bool> bodyJoint = n => System.Text.RegularExpressions.Regex.IsMatch(n.ToUpperInvariant(), BodyJoints);
        Func<EditModel.VBuf, int, bool> usable = (s, v) =>
        {
            var w = s.Layout.FirstOrDefault(a => a.Usage == 1); var ix = s.Layout.FirstOrDefault(a => a.Usage == 2);
            if (w == null || ix == null) return true;
            for (int k = 0; k < 4; k++)
            {
                if (s.Data[v * s.Stride + w.Offset + k] == 0) continue;
                int j = s.Data[v * s.Stride + ix.Offset + k] + 1;
                if (j >= sk.Names.Length || !bodyJoint(sk.Names[j])) return false;
            }
            return true;
        };
        log("vertices do corpo-base usados para os ossos: " + sources.Sum(s => Enumerable.Range(0, s.Count).Count(v => usable(s, v))) + " de " + sources.Sum(s => s.Count));
        for (int mi = 0; mi < mats.Count; mi++)
        {
            var ys = faces.Where(f => f[6] == mi).SelectMany(f => new[] { P[f[0]][1], P[f[2]][1], P[f[4]][1] }).ToList();
            if (ys.Count > 0) log("  material " + mats[mi].Name + ": " + ys.Count / 3 + " tri, altura " + ys.Min().ToString("F2") + "-" + ys.Max().ToString("F2"));
        }
        if (P3.Length / 3 >= 0xFFFF) throw new Exception("modelo com " + P3.Length / 3 + " vertices (maximo 65534 numa parte)");
        var newCount = new Dictionary<EditModel.VBuf, int>();  // vertices put at the end of each buffer
        // A copy of the body in buffer b (its material's bone count: "-2weight" takes the 2 heaviest).
        Action<EditModel.VBuf, List<EditModel.VBuf>, float[], float[], int[], int[], bool> put = (b, from, p, u, t, sv, filter) =>
        {
            var add = em.AddSkinned(b, from, p, u, t, filter ? usable : null);
            int mw = em.MaxWeights(b);
            if (rigJ != null)
                for (int i = 0; i < add.Count; i++)
                {
                    int[] j = rigJ[sv[i]]; float[] w = rigW[sv[i]];
                    if (j.Length > mw) { j = j.Take(mw).ToArray(); w = w.Take(mw).ToArray(); }
                    em.SetBones(b, add[i], j, w);
                }
            newCount[b] = add.Count;
        };
        // Strands, feathers and lashes (textures whose alpha cuts holes) go to the base's hair part: its material is
        // the one that drops see-through pixels (the body's draws them solid: black spikes on 2B's cuffs).
        var hairBuf = em.Buffers.Where(b => b != main && b.Drawn && b.Sig == main.Sig && b.Stride == main.Stride && em.MaterialName(b).IndexOf("hair", StringComparison.OrdinalIgnoreCase) >= 0)
                                .OrderByDescending(b => b.Count).FirstOrDefault();
        var cutTris = new List<int>(); var solidTris = new List<int>();
        if (Environment.GetEnvironmentVariable("NT_OUTFIT_NOCUT") == "1") hairBuf = null;
        for (int i = 0; i + 2 < T3.Length; i += 3) (mats[matV[T3[i]]].Cut && hairBuf != null ? cutTris : solidTris).AddRange(new[] { T3[i], T3[i + 1], T3[i + 2] });
        Action<List<int>, Action<float[], float[], int[], int[]>> part = (tl, use) =>
        {   // the vertices a triangle list uses, renumbered
            var map = new Dictionary<int, int>(); var p = new List<float>(); var u = new List<float>(); var s = new List<int>(); var t = new int[tl.Count];
            for (int i = 0; i < tl.Count; i++)
            {
                int v = tl[i], at;
                if (!map.TryGetValue(v, out at)) { at = s.Count; map[v] = at; p.AddRange(new[] { P3[v * 3], P3[v * 3 + 1], P3[v * 3 + 2] }); u.AddRange(new[] { U2[v * 2], U2[v * 2 + 1] }); s.Add(srcV[v]); }
                t[i] = at;
            }
            use(p.ToArray(), u.ToArray(), t, s.ToArray());
        };
        if (cutTris.Count > 0)
        {
            part(solidTris, (p, u, t, s) => put(main, sources, p, u, t, s, true));
            part(cutTris, (p, u, t, s) => put(hairBuf, sources, p, u, t, s, true));
            log("recortados: " + cutTris.Count / 3 + " triangulos na parte " + hairBuf.Index + " (" + em.MaterialName(hairBuf) + ")");
        }
        else put(main, sources, P3, U2, T3, srcV.ToArray(), true);
        // The base's levels of detail (2026-11-16: 2B blinked in fights as the camera moved back and forth): NieR draws
        // a part near, another a bit farther (the same place, fewer polygons) and a whole simplified body far away
        // ("ALL_LOD_L"). The new body goes in each: whole near, simplified farther.
        Func<EditModel.VBuf, float[]> box = b =>
        {
            float x0 = float.MaxValue, x1 = float.MinValue, y0 = float.MaxValue, y1 = float.MinValue;
            for (int v = 0; v < b.Count; v++) { var q = em.Pos(b, v); x0 = Math.Min(x0, q[0]); x1 = Math.Max(x1, q[0]); y0 = Math.Min(y0, q[1]); y1 = Math.Max(y1, q[1]); }
            return new[] { x0, x1, y0, y1 };
        };
        var drawnOthers = em.Buffers.Where(b => b != main && !newCount.ContainsKey(b) && b.Drawn && b.Sig == main.Sig && b.Stride == main.Stride && b.Tex != null && b.Tex.IndexOf("SWORD", StringComparison.OrdinalIgnoreCase) < 0).ToList();
        float bodyW = bp.Max(q => q[0]) - bp.Min(q => q[0]), bodyH = bHi - bLo;
        // main's own region, from the base's vertices only (before the new ones)
        int n0main = main.Count - newCount[main];
        var ownMain = new float[] { float.MaxValue, float.MinValue, float.MaxValue, float.MinValue };
        for (int v = 0; v < n0main; v++) { var q = em.Pos(main, v); ownMain[0] = Math.Min(ownMain[0], q[0]); ownMain[1] = Math.Max(ownMain[1], q[0]); ownMain[2] = Math.Min(ownMain[2], q[1]); ownMain[3] = Math.Max(ownMain[3], q[1]); }
        Func<float[], float[], float> overlap = (a, b) =>
        {
            float ix = Math.Max(0, Math.Min(a[1], b[1]) - Math.Max(a[0], b[0])), iy = Math.Max(0, Math.Min(a[3], b[3]) - Math.Max(a[2], b[2]));
            float ua = (a[1] - a[0]) * (a[3] - a[2]) + (b[1] - b[0]) * (b[3] - b[2]) - ix * iy;
            return ua > 0 ? ix * iy / ua : 0;
        };
        var mid = drawnOthers.Where(b => overlap(box(b), ownMain) > 0.8f).ToList();
        var far = drawnOthers.Where(b => { var q = box(b); return q[1] - q[0] > bodyW * 0.8f && q[3] - q[2] > bodyH * 0.85f; }).ToList();
        int midTarget = Math.Max(2000, T3.Length / 3 / 3), farTarget = Math.Max(800, T3.Length / 3 / 10);
        // Joint 0 (the root) is written as bone 255 (joint - 1): a point on it falls to the model's origin in the game,
        // the ground between the feet (2026-11-16: Caim's black flap from a foot to the middle; the editor's preview
        // ignores such a bone). Points on it go to the hips.
        {
            int hipsJ = Array.IndexOf(sk.Names, "HIPSBONE"), rooted = 0;
            if (rigJ != null && hipsJ > 0)
                for (int s = 0; s < rigJ.Length; s++)
                {
                    if (rigJ[s] == null || !rigJ[s].Contains(0)) continue;
                    rooted++;
                    var acc = new Dictionary<int, float>();
                    for (int k = 0; k < rigJ[s].Length; k++) { int j = rigJ[s][k] <= 0 ? hipsJ : rigJ[s][k]; float w; acc.TryGetValue(j, out w); acc[j] = w + rigW[s][k]; }
                    rigJ[s] = acc.Keys.ToArray(); rigW[s] = acc.Values.ToArray();
                }
            if (rooted > 0) log("pontos presos a junta 0 (" + sk.Names[0] + ", iriam para o chao entre os pes): " + rooted + ", passados ao quadril");
        }
        // Diagnostic: points below the crotch held by a bone that is not of their leg (env NT_RIG_CHECK).
        if (Environment.GetEnvironmentVariable("NT_RIG_CHECK") != null && rigJ != null)
        {
            int dh = Array.IndexOf(sk.Names, "HIPSBONE"), dl = Array.IndexOf(sk.Names, "LEFTUPLEG"), dr = Array.IndexOf(sk.Names, "RIGHTUPLEG");
            float dcx = sk.RestWorld[dh][12], dcr = (sk.RestWorld[dl][13] + sk.RestWorld[dr][13]) / 2;
            float lsgn = Math.Sign(sk.RestWorld[dl][12] - dcx);
            var bad = new Dictionary<string, int>();
            foreach (int s in srcV.Distinct())
            {
                if (s >= rigJ.Length || rigJ[s] == null || P[s][1] >= dcr - 0.05f) continue;
                string want = (P[s][0] - dcx) * lsgn > 0 ? "LEFT" : "RIGHT";
                for (int k = 0; k < rigJ[s].Length; k++)
                {
                    if (rigW[s][k] < 0.05f) continue;
                    string n = sk.Names[rigJ[s][k]];
                    bool ok = n == "HIPSBONE" || (n.StartsWith(want) && System.Text.RegularExpressions.Regex.IsMatch(n, "UPLEG|LEG|FOOT|TOE"));
                    if (ok) continue;
                    string bk = n + " (ponto do lado " + want + ")"; int bc; bad.TryGetValue(bk, out bc); bad[bk] = bc + 1;
                }
            }
            log("rig-check: " + (bad.Count == 0 ? "nenhum osso estranho nas pernas" : string.Join(", ", bad.OrderByDescending(kv => kv.Value).Select(kv => kv.Key + " x" + kv.Value))));
        }
        // The limb of a point (by its heaviest bone): 1/2 left/right leg, 3/4 left/right arm, 0 the rest.
        Func<int, int> limb = s =>
        {
            if (rigJ == null || s < 0 || s >= rigJ.Length || rigJ[s] == null || rigJ[s].Length == 0) return 0;
            int best = 0; for (int k = 1; k < rigJ[s].Length; k++) if (rigW[s][k] > rigW[s][best]) best = k;
            string n = sk.Names[rigJ[s][best]];
            bool left = n.StartsWith("LEFT"), right = n.StartsWith("RIGHT");
            if (!left && !right) return 0;
            bool leg = System.Text.RegularExpressions.Regex.IsMatch(n, "UPLEG|LEG|FOOT|TOE|KNEE|PANTS");
            return leg ? (left ? 1 : 2) : (left ? 3 : 4);
        };
        float[] mp, mu; int[] mt, ms; Decimate(P3, U2, T3, srcV.ToArray(), midTarget, out mp, out mu, out mt, out ms, limb);
        float[] fp, fu; int[] ft, fs; Decimate(P3, U2, T3, srcV.ToArray(), farTarget, out fp, out fu, out ft, out fs, limb);
        foreach (var b in mid) { put(b, sources, mp, mu, mt, ms, true); log("distancia media: parte " + b.Index + " (" + em.MaterialName(b) + ") com " + mt.Length / 3 + " triangulos"); }
        foreach (var b in far.Except(mid)) { put(b, sources, fp, fu, ft, fs, true); log("longe: parte " + b.Index + " (" + em.MaterialName(b) + ") com " + ft.Length / 3 + " triangulos"); }
        // The shadow: the biggest shadow buffer gets the far body (a shadow map is coarse; room in the file for the
        // body itself), bones from all of them.
        var shadows = em.Buffers.Where(b => !b.Drawn).ToList();
        if (shadows.Count > 0)
        {
            var smain = shadows.OrderByDescending(b => b.Count).First();
            put(smain, shadows.Where(b => b.Sig == smain.Sig && b.Stride == smain.Stride).ToList(), fp, null, ft, fs, false);
        }
        // The base's own vertices are not drawn any more: out (room for the new body in the game's memory).
        foreach (var b in em.Buffers)
        {
            bool keepAll = b.Drawn && b.Tex != null && b.Tex.IndexOf("SWORD", StringComparison.OrdinalIgnoreCase) >= 0;
            if (keepAll) continue;
            int nn; int newFrom = newCount.TryGetValue(b, out nn) ? b.Count - nn : b.Count - 1;
            if (newFrom > 0) em.DeleteVertices(b, new HashSet<int>(Enumerable.Range(0, newFrom)));
        }
        // The new vertices' own normals and tangents (they had the copied base vertex's: metallic, mottled light).
        if (Environment.GetEnvironmentVariable("NT_OUTFIT_KEEPNORMALS") == null)
        {
            int nset = 0;
            foreach (var kv in newCount)
                if (kv.Value > 0) nset += em.SetNormals(kv.Key, Enumerable.Range(kv.Key.Count - kv.Value, kv.Value).ToList());
            log("normais e tangentes do modelo novo: " + nset + " vertices");
        }
        em.PaintTexture(main.Tex, atlas);
        // The hair part draws with its own picture (the father's hair sheet): the cut triangles moved there have the
        // atlas' coordinates, so that picture becomes the atlas too, at its size (2026-11-16: drawn with the hair
        // sheet, they came out as scraps of it). The base's hair is gone from the model, nothing else uses it.
        if (cutTris.Count > 0 && hairBuf != null && hairBuf.Tex != null && !string.Equals(hairBuf.Tex, main.Tex, StringComparison.OrdinalIgnoreCase))
        {
            var ht = em.Texture(hairBuf.Tex);
            if (ht != null)
            {
                int HW = ht.Width, HH = ht.Height;
                var hp = new byte[HW * HH * 4];
                for (int y = 0; y < HH; y++)
                    for (int x = 0; x < HW; x++)
                    {
                        int sx0 = x * AW / HW, sx1 = Math.Max(sx0 + 1, (x + 1) * AW / HW), sy0 = y * AH / HH, sy1 = Math.Max(sy0 + 1, (y + 1) * AH / HH);
                        int r = 0, g = 0, b = 0, a = 0, n = 0;
                        for (int sy = sy0; sy < sy1; sy++)
                            for (int sx = sx0; sx < sx1; sx++) { int q = (sy * AW + sx) * 4; r += atlas[q]; g += atlas[q + 1]; b += atlas[q + 2]; a += atlas[q + 3]; n++; }
                        int o = (y * HW + x) * 4; hp[o] = (byte)(r / n); hp[o + 1] = (byte)(g / n); hp[o + 2] = (byte)(b / n); hp[o + 3] = (byte)(a / n);
                    }
                em.PaintTexture(hairBuf.Tex, hp);
                log("textura do cabelo " + hairBuf.Tex + " (" + HW + "x" + HH + "): o atlas, para as partes recortadas");
                string hn = hairBuf.Tex.Replace("_A_", "_N_");
                var hnt = hn == hairBuf.Tex ? null : em.Texture(hn);
                if (hnt != null && Environment.GetEnvironmentVariable("NT_OUTFIT_NOPAINT") != "1")
                {
                    var np = new byte[hnt.Width * hnt.Height * 4];
                    for (int i = 0; i < hnt.Width * hnt.Height; i++) { np[i * 4] = 128; np[i * 4 + 1] = 128; np[i * 4 + 2] = 255; np[i * 4 + 3] = 128; }
                    em.PaintTexture(hn, np);
                }
            }
            else log("textura do cabelo " + hairBuf.Tex + " nao achada no modelo: partes recortadas ficam com a do cabelo");
        }
        // The base's normal (_N) and gloss (_G) maps of that material were laid out for its own body: over the new
        // one they drew bumps and a wet black shine (2B looked plastic). Flat normal, no gloss.
        foreach (var kind in Environment.GetEnvironmentVariable("NT_OUTFIT_NOPAINT") == "1" ? new string[0] : (Environment.GetEnvironmentVariable("NT_OUTFIT_PAINTONLY") ?? "_N_,_G_").Split(','))
        {
            string name = main.Tex.Replace("_A_", kind);
            var t = name == main.Tex ? null : em.Texture(name);
            // The father's _C has no trailing number (NIER00_NIER030_ALL_001_C_psd beside ..._A_psd1).
            if (t == null && name.Length > 1 && char.IsDigit(name[name.Length - 1])) { name = name.Substring(0, name.Length - 1); t = em.Texture(name); }
            if (t == null) continue;
            var px = new byte[t.Width * t.Height * 4];
            var orig = em.TexturePixels(name);
            { long[] sum = new long[4]; for (int i = 0; i < orig.Length; i++) sum[i & 3] += orig[i]; int n = orig.Length / 4; log("  " + kind + " original: media R" + sum[0] / n + " G" + sum[1] / n + " B" + sum[2] / n + " A" + sum[3] / n); }
            if (Environment.GetEnvironmentVariable("NT_OUTFIT_ROUNDTRIP") == "1") { em.PaintTexture(name, orig); log("  " + kind + " recodificada igual"); continue; }
            // _G (2026-11-17): all zero turned the body black (a channel scales the colour: the father's averages are
            // R228 G20 B194 A36); its father's pattern over the new body is what looked mottled and metallic. A
            // uniform one: env NT_OUTFIT_G="r g b a" (TEST), else the father's averages with G (shine) 0.
            int[] gv = { 228, 0, 194, 36 };
            var genv = (Environment.GetEnvironmentVariable("NT_OUTFIT_G") ?? "").Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
            if (genv.Length == 4) gv = genv.Select(int.Parse).ToArray();
            if (kind == "_C_")
            {   // TEST (2026-11-17): the 32x32 _C over the new body; env NT_OUTFIT_C="r g b a", else its average
                long[] s = new long[4]; for (int i = 0; i < orig.Length; i++) s[i & 3] += orig[i]; int cnt = Math.Max(1, orig.Length / 4);
                int[] cv = { (int)(s[0] / cnt), (int)(s[1] / cnt), (int)(s[2] / cnt), (int)(s[3] / cnt) };
                var cenv = (Environment.GetEnvironmentVariable("NT_OUTFIT_C") ?? "").Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
                if (cenv.Length == 4) cv = cenv.Select(int.Parse).ToArray();
                for (int i = 0; i < t.Width * t.Height; i++) for (int k = 0; k < 4; k++) px[i * 4 + k] = (byte)cv[k];
                em.PaintTexture(name, px);
                log("textura " + name + ": uniforme " + string.Join(",", cv));
                continue;
            }
            for (int i = 0; i < t.Width * t.Height; i++)
                if (kind == "_N_") { px[i * 4] = 128; px[i * 4 + 1] = 128; px[i * 4 + 2] = 255; px[i * 4 + 3] = 128; }
                else { px[i * 4] = (byte)gv[0]; px[i * 4 + 1] = (byte)gv[1]; px[i * 4 + 2] = (byte)gv[2]; px[i * 4 + 3] = (byte)gv[3]; }
            if (kind == "_G_" && genv.Length != 4)
            {
                // The shading per material (the editor's choice, texMap "shade:<material>" = nulo / tecido / pele / couro /
                // metal; else by its name): its place in the atlas painted with that kind's shine (G) and rim light (A),
                // measured 2026-11-17 on Caim (G: shine; A: light on the edges; R scales the colour, kept).
                var used = new List<string>();
                // A material's parts (an FBX's meshes), for a choice made on a part.
                var partsOf = new Dictionary<int, HashSet<string>>();
                foreach (var f in faces)
                {
                    string lab; if (!faceLabel.TryGetValue(f, out lab)) continue;
                    HashSet<string> set; if (!partsOf.TryGetValue(f[6], out set)) partsOf[f[6]] = set = new HashSet<string>();
                    foreach (var pn in lab.Split('\u0001')) if (pn != "") set.Add(pn);
                }
                for (int mi = 0; mi < mats.Count; mi++)
                {
                    var m = mats[mi];
                    if (m.SW <= 0 || m.SH <= 0) continue;
                    string kindName = null;
                    if (!texMap.TryGetValue("shade:" + m.Name, out kindName))
                    {
                        HashSet<string> ps2; kindName = null;
                        if (partsOf.TryGetValue(mi, out ps2)) foreach (var pn in ps2) if (texMap.TryGetValue("shade:" + pn, out kindName)) break;
                        if (kindName == null) kindName = ShadeByName(m.Name);
                    }
                    int[] sv = ShadeValues(kindName);
                    if (sv == null) continue;
                    int gx0 = m.X * t.Width / AW, gy0 = m.Y * t.Height / AH, gx1 = Math.Min(t.Width, (m.X + m.SW) * t.Width / AW + 1), gy1 = Math.Min(t.Height, (m.Y + m.SH) * t.Height / AH + 1);
                    for (int gy = gy0; gy < gy1; gy++)
                        for (int gx = gx0; gx < gx1; gx++) { int q = (gy * t.Width + gx) * 4; px[q + 1] = (byte)sv[0]; px[q + 3] = (byte)sv[1]; }
                    used.Add(m.Name + "=" + kindName);
                }
                if (used.Count > 0) log("shading: " + string.Join(", ", used));
            }
            em.PaintTexture(name, px);
            log("textura " + name + " (" + t.Width + "x" + t.Height + ", formato 0x" + t.Format.ToString("X") + "): " + (kind == "_N_" ? "relevo plano" : "sem brilho"));
        }
        log("malha: " + P3.Length / 3 + " vertices, " + T3.Length / 3 + " triangulos na parte " + main.Index);

        var mdp = c.Read(pe);
        int limit = Lzo.IsContainer(mdp) ? (int)Be.U32(mdp, 0x10) : mdp.Length;
        var built = em.Build(limit, log);
        if (Places != null)
        {
            built = Tuple.Create(Program.SetRestPositions(built.Item1, Places), built.Item2);
            log("esqueleto com as proporcoes do modelo: " + Places.Count + " juntas no lugar dos ossos dele");
        }
        Program.SwapSource.Clear();
        Program.SwapSource[ve.Path.ToUpperInvariant()] = built.Item1;
        Program.SwapSource[pe.Path.ToUpperInvariant()] = built.Item2;
        try
        {
            if (Directory.Exists(modDir)) Directory.Delete(modDir, true);
            Directory.CreateDirectory(modDir);
            // Every outfit of the father (the base too) gets the new body; the skeleton is the father's, so the
            // animations stay his (NT_KEEP_ANIMS).
            Environment.SetEnvironmentVariable("NT_KEEP_ANIMS", "1");
            if (Places != null) Environment.SetEnvironmentVariable("NT_SWAP_EXACT", "1");
            Program.AnimHook = AnimationsOf(cpks, texMap, log);
            Program.AnimHookDone.Clear();
            Program.Swap(cpks, string.Join(",", new[] { "NIER030", "NIER031", "NIER040" }.Select(n => n + "=" + baseName)), modDir, "CHARA");
        }
        finally
        {
            Program.SwapSource.Clear(); Program.AnimHook = null; Program.AnimHookDone.Clear();
            Environment.SetEnvironmentVariable("NT_KEEP_ANIMS", null); Environment.SetEnvironmentVariable("NT_SWAP_EXACT", null);
        }
        Preview(built.Item1, built.Item2, Path.Combine(modDir, "previa.png"), atlas, AW, AH, log);
        PngIo.Save(Path.Combine(modDir, "atlas.png"), atlas, AW, AH);
    }

    // ---- the body's animations, apart from the father's (2026-11-18) ----
    // Options of the import (texMap):
    //   anims=<MODEL>   whose movement the body has (a game character with animations of its own, e.g. KAINE010): the
    //                   father's animations that model also has are made over with its movement (Program.OwnAnimations);
    //                   the rest (and an empty value) stay the father's;
    //   legs=<degrees>  legs closer together (negative: apart), arms=<degrees> arms closer to the body;
    //   pose:<JOINT>=<rx ry rz>  a joint turned (degrees, the model's axes), in every animation.
    // Null when none is set (the father's animations as they are).
    static Func<byte[], string, byte[]> AnimationsOf(Cpk[] cpks, Dictionary<string, string> texMap, Action<string> log)
    {
        if (texMap == null) texMap = new Dictionary<string, string>();
        string anims;
        texMap.TryGetValue("anims", out anims);
        Func<string, float> num = k =>
        {
            string v; float f;
            return texMap.TryGetValue(k, out v) && float.TryParse(v, NumberStyles.Float, CultureInfo.InvariantCulture, out f) ? f : 0;
        };
        float legs = num("legs"), arms = num("arms");
        var custom = new Dictionary<string, float[]>(StringComparer.OrdinalIgnoreCase);
        foreach (var kv in texMap)
        {
            if (!kv.Key.StartsWith("pose:", StringComparison.OrdinalIgnoreCase)) continue;
            var p = kv.Value.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries)
                            .Select(x => { float f; return float.TryParse(x, NumberStyles.Float, CultureInfo.InvariantCulture, out f) ? f : 0; }).ToArray();
            if (p.Length >= 3) custom[kv.Key.Substring(5)] = Anim.Euler(p[0], p[1], p[2]);
        }
        // Master-skeleton animations (.hanim, Humanoid.cs): a folder (anims=<folder>: each file stands for the father's
        // animation whose name ends as its own does, "kaine010_01_walk-lo_01" for "nier030_01_walk^lo_01"), or one by
        // one (hanim:<the animation's name after the model's, e.g. 01_wait-s_01>=<file>).
        var clips = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        Func<string, string> suffix = nm => { nm = nm.Replace('^', '-'); int u = nm.IndexOf('_'); return u > 0 ? nm.Substring(u + 1) : nm; };
        if (!string.IsNullOrEmpty(anims) && Directory.Exists(anims))
        {
            foreach (var f in Directory.GetFiles(anims, "*.hanim")) clips[suffix(Path.GetFileNameWithoutExtension(f))] = f;
            log("animacoes do esqueleto mestre: " + clips.Count + " arquivos em " + anims);
            anims = null;
        }
        foreach (var kv in texMap)
            if (kv.Key.StartsWith("hanim:", StringComparison.OrdinalIgnoreCase) && File.Exists(kv.Value)) clips[kv.Key.Substring(6).Replace('^', '-')] = kv.Value;
        byte[] srcMdv = null;
        if (!string.IsNullOrEmpty(anims) && !anims.StartsWith("NIER03", StringComparison.OrdinalIgnoreCase))
        {
            foreach (var c in cpks)
            {
                var e = c.Files.FirstOrDefault(f => string.Equals(f.Path, "CHARA/" + anims + ".MDV", StringComparison.OrdinalIgnoreCase));
                if (e != null) { srcMdv = c.Read(e); break; }
            }
            log(srcMdv != null ? "animacoes de " + anims.ToUpperInvariant() : "animacoes: " + anims + " nao encontrado, ficam as do pai");
        }
        var loaded = new Dictionary<string, Humanoid.Clip>(StringComparer.OrdinalIgnoreCase);
        return (mdv, target) =>
        {
            var r = mdv;
            // First: the father's animations on the model's own proportions (its skeleton is its own: Program.RetargetedCore).
            byte[] disc = null;
            foreach (var c in cpks)
            {
                var e = c.Files.FirstOrDefault(f => string.Equals(f.Path, "CHARA/" + target + ".MDV", StringComparison.OrdinalIgnoreCase));
                if (e != null) { disc = c.Read(e); break; }
            }
            var origSk = disc != null ? Anim.Load(disc, new List<Motion>()) : null;
            if (origSk != null) r = Program.OwnProportions(r, origSk, s => log(target + ": " + s));
            if (clips.Count > 0)
            {
                int changed;
                r = Program.MapMotions(r, (sk, m, file) =>
                {
                    string path;
                    if (!clips.TryGetValue(suffix(m.Name), out path)) return file;
                    Humanoid.Clip clip;
                    if (!loaded.TryGetValue(path, out clip)) loaded[path] = clip = Humanoid.Read(path);
                    int moved;
                    return Humanoid.ToCmf(clip, file, sk, m, out moved);
                }, out changed);
                log(target + ": " + changed + " animacoes do esqueleto mestre");
            }
            if (srcMdv != null)
            {
                r = Program.OwnAnimations(r, srcMdv, target, anims.ToUpperInvariant());
                log(target + ": " + Program.LastOwnAnims + " animacoes de " + anims.ToUpperInvariant());
            }
            if (legs != 0 || arms != 0 || custom.Count > 0) r = Program.PostureAnimations(r, legs, arms, custom, s => log(target + ": " + s));
            return r;
        };
    }

    // ---- automatic bones for a model without a skeleton (an OBJ) ----
    // The base's skeleton is first bent into the model's pose (each arm turned at the shoulder so the hand
    // points where the model's hand is, each leg at the hip towards its foot: an A-pose against the father's
    // T-pose); each point then takes the nearest bone and, near a joint, the neighbouring bone of the same limb
    // (never the other leg: legs stuck together came from blending across); last, the points are bent back
    // with those bones into the skeleton's own pose. P is changed in place.
    static readonly string[] RigNames = { "HIPSBONE", "SPINE", "SPINE1", "SPINE2", "NECK", "HEAD",
        "LEFTSHOULDER", "LEFTARM", "LEFTFOREARM", "LEFTHAND", "RIGHTSHOULDER", "RIGHTARM", "RIGHTFOREARM", "RIGHTHAND",
        "LEFTUPLEG", "LEFTLEG", "LEFTFOOT", "LEFTTOEBASE", "RIGHTUPLEG", "RIGHTLEG", "RIGHTFOOT", "RIGHTTOEBASE" };

    static float[] Sub(float[] a, float[] b) { return new[] { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
    static float Dot(float[] a, float[] b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    static float Len(float[] a) { return (float)Math.Sqrt(Dot(a, a)); }
    // The smallest rotation taking direction a to direction b (3x3, rows).
    static float[] Turn(float[] a, float[] b)
    {
        float la = Len(a), lb = Len(b);
        var u = new[] { a[0] / la, a[1] / la, a[2] / la }; var v = new[] { b[0] / lb, b[1] / lb, b[2] / lb };
        var k = new[] { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
        float s = Len(k), c = Dot(u, v);
        if (s < 1e-6f) return new float[] { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
        k[0] /= s; k[1] /= s; k[2] /= s;
        float t = 1 - c;
        return new[] {
            c + k[0] * k[0] * t, k[0] * k[1] * t - k[2] * s, k[0] * k[2] * t + k[1] * s,
            k[1] * k[0] * t + k[2] * s, c + k[1] * k[1] * t, k[1] * k[2] * t - k[0] * s,
            k[2] * k[0] * t - k[1] * s, k[2] * k[1] * t + k[0] * s, c + k[2] * k[2] * t };
    }
    static float[] RotP(float[] r, float[] piv, float[] p, bool inverse)
    {
        var d = Sub(p, piv);
        float x, y, z;
        if (!inverse) { x = r[0] * d[0] + r[1] * d[1] + r[2] * d[2]; y = r[3] * d[0] + r[4] * d[1] + r[5] * d[2]; z = r[6] * d[0] + r[7] * d[1] + r[8] * d[2]; }
        else { x = r[0] * d[0] + r[3] * d[1] + r[6] * d[2]; y = r[1] * d[0] + r[4] * d[1] + r[7] * d[2]; z = r[2] * d[0] + r[5] * d[1] + r[8] * d[2]; }
        return new[] { x + piv[0], y + piv[1], z + piv[2] };
    }
    static float SegDist(float[] p, float[] a, float[] b)
    {
        var ab = Sub(b, a); var ap = Sub(p, a);
        float l2 = Dot(ab, ab), t = l2 > 0 ? Math.Max(0, Math.Min(1, Dot(ap, ab) / l2)) : 0;
        var q = new[] { a[0] + ab[0] * t - p[0], a[1] + ab[1] * t - p[1], a[2] + ab[2] * t - p[2] };
        return Len(q);
    }

    static void AutoRig(Skeleton sk, List<float[]> P, List<float[]> baseBody, float lo, float hi, out int[][] J, out float[][] W, Action<string> log)
    {
        float h = hi - lo;
        var js = RigNames.Select(n => Array.IndexOf(sk.Names, n)).Where(j => j >= 0).ToList();
        var inSet = new HashSet<int>(js);
        Func<int, float[]> at = j => new[] { sk.RestWorld[j][12], sk.RestWorld[j][13], sk.RestWorld[j][14] };
        Func<int, int> bpar = j => { int p = sk.Parent[j]; while (p >= 0 && !inSet.Contains(p)) p = sk.Parent[p]; return p; };
        var kids = js.ToDictionary(j => j, j => js.Where(c => bpar(c) == j).ToList());
        Func<string, int> J0 = n => Array.IndexOf(sk.Names, n);
        // Segment of each joint: to its most central child (the spine goes on to the neck, not a shoulder); a
        // joint with no child goes on its parent's way (head up, hand and toe forward).
        var segEnd = new Dictionary<int, float[]>();
        foreach (int j in js)
        {
            var p = at(j);
            if (kids[j].Count > 0) { segEnd[j] = at(kids[j].OrderBy(c => Math.Abs(at(c)[0] - p[0]) + (sk.Names[c].Contains("UPLEG") ? 1 : 0)).First()); continue; }
            int par = bpar(j); var d = par >= 0 ? Sub(p, at(par)) : new float[] { 0, 1, 0 }; float l = Len(d);
            float ext = sk.Names[j] == "HEAD" ? 0.12f * h : sk.Names[j].Contains("HAND") ? 0.06f * h : 0.04f * h;
            segEnd[j] = l > 0 ? new[] { p[0] + d[0] / l * ext, p[1] + d[1] / l * ext, p[2] + d[2] / l * ext } : new[] { p[0], p[1] + ext, p[2] };
        }
        // Limb tips of a body: the hand = the point farthest out on its side between hip and shoulder height;
        // the foot = the middle of the lowest slab on its side.
        float lx = at(J0("LEFTHAND"))[0] > at(J0("RIGHTHAND"))[0] ? 1 : -1;  // the side "LEFT" is on
        Func<List<float[]>, float> centre = pts => pts.Where(p => p[1] > lo + h * 0.45f && p[1] < lo + h * 0.6f).Average(p => p[0]);
        Func<List<float[]>, float, float[]> handTip = (pts, side) =>
        {
            float cx = centre(pts);
            var c = pts.Where(p => p[1] > lo + h * 0.3f && p[1] < lo + h * 0.85f && (p[0] - cx) * side > 0).OrderByDescending(p => (p[0] - cx) * side).Take(20).ToList();
            return new[] { c.Average(p => p[0]), c.Average(p => p[1]), c.Average(p => p[2]) };
        };
        Func<List<float[]>, float, float[]> footMid = (pts, side) =>
        {
            float cx = centre(pts);
            var c = pts.Where(p => p[1] < lo + h * 0.05f && (p[0] - cx) * side > 0).ToList();
            return c.Count == 0 ? null : new[] { c.Average(p => p[0]), c.Average(p => p[1]), c.Average(p => p[2]) };
        };
        // Pose: per limb root, a turn about it applied to its whole subtree.
        var turn = new Dictionary<int, Tuple<float[], float[]>>();  // limb root -> (rotation, pivot)
        foreach (var side in new[] { 1f, -1f })
        {
            string S = side == lx ? "LEFT" : "RIGHT";
            int arm = J0(S + "ARM"), leg = J0(S + "UPLEG");
            if (arm >= 0)
            {
                var piv = at(arm); var from = Sub(handTip(baseBody, side), piv); var to = Sub(handTip(P, side), piv);
                var r = Turn(from, to); turn[arm] = Tuple.Create(r, piv);
                log("  " + S + " braco: " + (Math.Acos(Math.Max(-1, Math.Min(1, Dot(from, to) / Len(from) / Len(to)))) * 180 / Math.PI).ToString("F0") + " graus ate a mao do modelo");
            }
            var fb = footMid(baseBody, side); var fm = footMid(P, side);
            if (leg >= 0 && fb != null && fm != null)
            {
                var piv = at(leg); var foot = at(J0(S + "FOOT"));
                var to = Sub(new[] { foot[0] + fm[0] - fb[0], foot[1], foot[2] + fm[2] - fb[2] }, piv);
                var r = Turn(Sub(foot, piv), to); turn[leg] = Tuple.Create(r, piv);
                log("  " + S + " perna: pe do modelo " + (fm[0] - fb[0]).ToString("+0.00;-0.00") + " m para o lado");
            }
        }
        Func<int, Tuple<float[], float[]>> turnOf = j => { for (int a = j; a >= 0; a = sk.Parent[a]) if (turn.ContainsKey(a)) return turn[a]; return null; };
        var segA = new Dictionary<int, float[]>(); var segB = new Dictionary<int, float[]>();
        foreach (int j in js)
        {
            var t = turnOf(j);
            segA[j] = t == null ? at(j) : RotP(t.Item1, t.Item2, at(j), false);
            segB[j] = t == null ? segEnd[j] : RotP(t.Item1, t.Item2, segEnd[j], false);
        }
        // Weights.
        J = new int[P.Count][]; W = new float[P.Count][];
        var near = new Dictionary<int, HashSet<int>>();
        foreach (int j in js) { var s = new HashSet<int>(kids[j]); int p = bpar(j); if (p >= 0) s.Add(p); near[j] = s; }
        // Below the crotch a point takes only its own side's leg (2026-11-16: Caim's wide trousers touch in the middle;
        // 221 points of the left leg took right-leg bones and stretched a spike between the legs as he walked).
        float crotchY = (at(J0("LEFTUPLEG"))[1] + at(J0("RIGHTUPLEG"))[1]) / 2, midX = at(J0("HIPSBONE"))[0];
        float leftX = Math.Sign(at(J0("LEFTUPLEG"))[0] - midX);
        var legSide = new Dictionary<int, int>();  // joint -> +1 left leg, -1 right leg
        foreach (int j in js)
        {
            string n = sk.Names[j];
            if (!System.Text.RegularExpressions.Regex.IsMatch(n, "UPLEG|LEG|FOOT|TOE")) continue;
            if (n.StartsWith("LEFT")) legSide[j] = 1; else if (n.StartsWith("RIGHT")) legSide[j] = -1;
        }
        int sided = 0;
        for (int i = 0; i < P.Count; i++)
        {
            var p = P[i];
            int mySide = p[1] < crotchY ? ((p[0] - midX) * leftX >= 0 ? 1 : -1) : 0;
            Func<int, bool> allowed = j => { int ls; return mySide == 0 || !legSide.TryGetValue(j, out ls) || ls == mySide; };
            int best = js[0]; float bd = float.MaxValue, freeBest = float.MaxValue;
            foreach (int j in js) { float d = SegDist(p, segA[j], segB[j]); if (d < freeBest) freeBest = d; if (!allowed(j)) continue; if (d < bd) { bd = d; best = j; } }
            if (bd > freeBest) sided++;
            int second = -1; float sd = float.MaxValue;
            foreach (int j in near[best]) { if (!allowed(j)) continue; float d = SegDist(p, segA[j], segB[j]); if (d < sd) { sd = d; second = j; } }
            float w1 = 1 / (float)Math.Pow(Math.Max(bd, 1e-4f), 4), w2 = second >= 0 && sd < bd * 1.6f ? 1 / (float)Math.Pow(Math.Max(sd, 1e-4f), 4) : 0;
            // the blend fades out as the second bone gets farther (none at 1.6x the distance)
            if (w2 > 0) w2 *= (1.6f - sd / Math.Max(bd, 1e-4f)) / 0.6f;
            float sum = w1 + w2;
            J[i] = w2 > 0 ? new[] { best, second } : new[] { best };
            W[i] = w2 > 0 ? new[] { w1 / sum, w2 / sum } : new[] { 1f };
            // Back to the skeleton's pose.
            float[] r = new float[3];
            for (int k = 0; k < J[i].Length; k++)
            {
                var t = turnOf(J[i][k]);
                var q = t == null ? p : RotP(t.Item1, t.Item2, p, true);
                r[0] += q[0] * W[i][k]; r[1] += q[1] * W[i][k]; r[2] += q[2] * W[i][k];
            }
            P[i][0] = r[0]; P[i][1] = r[1]; P[i][2] = r[2];
        }
        var use = J.SelectMany(x => x).GroupBy(x => x).OrderByDescending(g => g.Count()).Select(g => sk.Names[g.Key] + " " + g.Count());
        log("auto-rig: " + string.Join(", ", use));
        if (sided > 0) log("auto-rig: " + sided + " pontos das pernas ficaram com a perna do seu lado (a outra estava mais perto)");
    }

    // How Build lays a model over the base (the editor shows the model the same way): up axis to +Y, turned to face
    // as the base does, scaled to its height, its middle on the base's.
    public class Alignment
    {
        public int Up; public float UpSign, Scale; public bool Flip; public float[] From, To;
        public float[] Apply(float[] p)
        {
            float x, y, z;
            if (Up == 1) { x = p[0]; y = p[1]; z = p[2]; }
            else if (Up == 2) { x = p[0]; y = p[2]; z = -p[1]; }
            else { x = -p[1]; y = p[0]; z = p[2]; }
            if (UpSign < 0) { y = -y; z = -z; }
            if (Flip) { x = -x; z = -z; }
            return new[] { (x - From[0]) * Scale + To[0], (y - From[1]) * Scale + To[1], (z - From[2]) * Scale + To[2] };
        }
    }
    public static Alignment LastAlign;

    // A part's points moved by "x y z rx ry rz scale" (metres, degrees about X, Y then Z, factor) about the middle of
    // their box.
    public static List<float[]> PartXform(List<float[]> pts, string spec)
    {
        var a = spec.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries).Select(s => float.Parse(s, CultureInfo.InvariantCulture)).ToArray();
        if (a.Length < 7 || pts.Count == 0) return pts;
        var c = new float[3];
        for (int k = 0; k < 3; k++) c[k] = (pts.Min(p => p[k]) + pts.Max(p => p[k])) / 2;
        double rx = a[3] * Math.PI / 180, ry = a[4] * Math.PI / 180, rz = a[5] * Math.PI / 180;
        return pts.Select(p =>
        {
            double x = (p[0] - c[0]) * a[6], y = (p[1] - c[1]) * a[6], z = (p[2] - c[2]) * a[6], t;
            t = y * Math.Cos(rx) - z * Math.Sin(rx); z = y * Math.Sin(rx) + z * Math.Cos(rx); y = t;
            t = x * Math.Cos(ry) + z * Math.Sin(ry); z = -x * Math.Sin(ry) + z * Math.Cos(ry); x = t;
            t = x * Math.Cos(rz) - y * Math.Sin(rz); y = x * Math.Sin(rz) + y * Math.Cos(rz); x = t;
            return new[] { (float)(x + c[0] + a[0]), (float)(y + c[1] + a[1]), (float)(z + c[2] + a[2]) };
        }).ToList();
    }

    // "Separate the legs" (2026-11-16: Caim's trousers are one piece joining both legs; the points between them went
    // with both and stretched): below the crotch every face goes to the side its middle is on, a point used by faces
    // of both sides is doubled (the piece is cut down the middle), and a point of one side has the other leg's bones
    // swapped for its own side's (LEFTLEG <-> RIGHTLEG), the weights kept.
    // How far a point at (x, y) goes out for a leg spread of d metres (0 above the crotch, full 8 cm under it).
    public static float SpreadOffset(float x, float y, float cx, float crotch, float d)
    {
        if (y >= crotch || x == cx) return 0;
        float t = Math.Min(1, (crotch - y) / 0.08f);
        return Math.Sign(x - cx) * d * t;
    }

    static void SplitLegs(List<int[]> faces, List<float[]> P, ref int[][] rigJ, ref float[][] rigW, Skeleton sk, Action<string> log,
                          float spreadM = 0, float spreadCx = 0, float spreadCrotch = 0, Dictionary<int, float> spreadHave = null)
    {
        Func<string, int> J0 = n => Array.IndexOf(sk.Names, n);
        int hips = J0("HIPSBONE"), lu = J0("LEFTUPLEG"), ru = J0("RIGHTUPLEG");
        if (hips < 0 || lu < 0 || ru < 0 || rigJ == null) { log("separar pernas: esqueleto sem quadril/coxas"); return; }
        float cx = sk.RestWorld[hips][12], crotch = (sk.RestWorld[lu][13] + sk.RestWorld[ru][13]) / 2;
        float leftSign = Math.Sign(sk.RestWorld[lu][12] - cx); if (leftSign == 0) leftSign = 1;
        var other = new int[sk.Names.Length];
        for (int j = 0; j < other.Length; j++)
        {
            string n = sk.Names[j];
            other[j] = n.StartsWith("LEFT") ? J0("RIGHT" + n.Substring(4)) : n.StartsWith("RIGHT") ? J0("LEFT" + n.Substring(5)) : -1;
        }
        var sideOf = new Dictionary<int, int>();   // point -> side (+1 left, -1 right) it was given first
        var copy = new Dictionary<int, int>();     // point -> its copy for the other side
        int faceCount = 0, start = P.Count;
        var newJ = new List<int[]>(); var newW = new List<float[]>();
        foreach (var f in faces)
        {
            float my = (P[f[0]][1] + P[f[2]][1] + P[f[4]][1]) / 3, mx = (P[f[0]][0] + P[f[2]][0] + P[f[4]][0]) / 3;
            if (my >= crotch) continue;
            int side = (mx - cx) * leftSign >= 0 ? 1 : -1;
            faceCount++;
            for (int k = 0; k < 6; k += 2)
            {
                int v = f[k], s;
                if (!sideOf.TryGetValue(v, out s)) { sideOf[v] = side; continue; }
                if (s == side) continue;
                int c;
                if (!copy.TryGetValue(v, out c))
                {
                    c = P.Count; copy[v] = c; P.Add((float[])P[v].Clone());
                    if (spreadHave != null) { float hv; if (spreadHave.TryGetValue(v, out hv)) spreadHave[c] = hv; }
                    newJ.Add(v < rigJ.Length && rigJ[v] != null ? (int[])rigJ[v].Clone() : new[] { hips });
                    newW.Add(v < rigW.Length && rigW[v] != null ? (float[])rigW[v].Clone() : new[] { 1f });
                    sideOf[c] = side;
                }
                f[k] = c;
            }
        }
        if (faceCount == 0) { log("separar pernas: nada abaixo do quadril"); return; }
        var J = new int[P.Count][]; var W = new float[P.Count][];
        Array.Copy(rigJ, J, Math.Min(rigJ.Length, J.Length)); Array.Copy(rigW, W, Math.Min(rigW.Length, W.Length));
        for (int i = 0; i < newJ.Count; i++) { J[start + i] = newJ[i]; W[start + i] = newW[i]; }
        int fixedPts = 0;
        foreach (var kv in sideOf)
        {
            int v = kv.Key; if (J[v] == null) continue;
            string want = kv.Value > 0 ? "LEFT" : "RIGHT", wrong = kv.Value > 0 ? "RIGHT" : "LEFT";
            var acc = new Dictionary<int, float>(); bool changed = false;
            for (int k = 0; k < J[v].Length; k++)
            {
                int j = J[v][k];
                string n = sk.Names[j];
                bool legBone = System.Text.RegularExpressions.Regex.IsMatch(n, "UPLEG|LEG|FOOT|TOE|KNEE|PANTS");
                if (legBone && n.StartsWith(wrong) && other[j] >= 0) { j = other[j]; changed = true; }
                float w; acc.TryGetValue(j, out w); acc[j] = w + W[v][k];
            }
            if (!changed) continue;
            J[v] = acc.Keys.ToArray(); W[v] = acc.Values.ToArray();
            fixedPts++;
        }
        if (spreadM != 0 && spreadHave != null)
        {   // each point finishes on the side its faces went to (the middle ones were moved by their own x)
            int fixedSpread = 0;
            foreach (var kv in sideOf)
            {
                int v = kv.Key; float have; spreadHave.TryGetValue(v, out have);
                float t = Math.Min(1, Math.Max(0, (spreadCrotch - P[v][1]) / 0.08f));
                float want = kv.Value * spreadM * t;
                if (Math.Abs(want - have) < 1e-5f) continue;
                P[v][0] += want - have; fixedSpread++;
            }
            if (fixedSpread > 0) log("pernas afastadas: " + fixedSpread + " pontos do meio levados para o seu lado");
        }
        rigJ = J; rigW = W;
        log("separar pernas: " + faceCount + " triangulos abaixo do quadril, " + copy.Count + " pontos do meio duplicados, " + fixedPts + " pontos passados para a perna do seu lado");
    }

    // Shading kinds (the editor's "shading" choice): G = shine, A = light on the edges, on the base's gloss map (_G).
    public static readonly string[] ShadeKinds = { "nulo", "tecido", "pele", "couro", "metal" };
    static int[] ShadeValues(string kind)
    {
        switch (kind)
        {
            case "nulo": return new[] { 0, 36 };
            case "tecido": return new[] { 0, 20 };
            case "pele": return new[] { 12, 70 };
            case "couro": return new[] { 45, 50 };
            case "metal": return new[] { 180, 90 };
        }
        return null;
    }
    static string ShadeByName(string name)
    {
        string n = (name ?? "").ToLowerInvariant();
        if (System.Text.RegularExpressions.Regex.IsMatch(n, "metal|armor|armour|plate|iron|steel|buckle")) return "metal";
        if (System.Text.RegularExpressions.Regex.IsMatch(n, "leather|belt|boot|glove")) return "couro";
        if (System.Text.RegularExpressions.Regex.IsMatch(n, "skin|face|body_?s|head")) return "pele";
        if (System.Text.RegularExpressions.Regex.IsMatch(n, "cloth|mant|cape|cloak|dress|skirt|coat|shirt|pants")) return "tecido";
        return "nulo";
    }

    // The editor's "follows" choice (2026-11-16: Zero's cape went with an arm, Caim's coat tail with a leg): every
    // point of a part marked follow:<part> = <group> is skinned to the bones of that group only, the nearest two.
    static readonly Dictionary<string, string[]> FollowGroups = new Dictionary<string, string[]>
    {
        { "TORSO", new[] { "HIPSBONE", "SPINE", "SPINE1", "SPINE2" } },
        { "CHEST", new[] { "SPINE1", "SPINE2" } },
        { "HIPS", new[] { "HIPSBONE" } },
        { "HEAD", new[] { "NECK", "HEAD" } },
        { "LARM", new[] { "LEFTSHOULDER", "LEFTARM", "LEFTFOREARM", "LEFTHAND" } },
        { "RARM", new[] { "RIGHTSHOULDER", "RIGHTARM", "RIGHTFOREARM", "RIGHTHAND" } },
        { "LLEG", new[] { "LEFTUPLEG", "LEFTLEG", "LEFTFOOT", "LEFTTOEBASE" } },
        { "RLEG", new[] { "RIGHTUPLEG", "RIGHTLEG", "RIGHTFOOT", "RIGHTTOEBASE" } },
        // A cape (2026-11-17): the father's back sash is a chain the game swings with its cloth (the CCF record):
        // CLOTH (waist, y 1.11) .. CLOTH5 (y 0.30), behind him. A cape's points follow the chest above it and, below,
        // the two chain joints at their height, blended: the game's cloth moves the cape. Bodies without the chain
        // (NIER031, NIER040: it maps to the legs there) get the chest only.
        { "CAPE", new[] { "SPINE2", "CLOTH", "CLOTH1", "CLOTH2", "CLOTH3", "CLOTH4", "CLOTH5" } },
    };

    static void Follow(Dictionary<string, string> texMap, List<int[]> faces, Dictionary<int[], string> faceLabel, List<float[]> P,
                       Skeleton sk, int[][] rigJ, float[][] rigW, Action<string> log)
    {
        var follow = texMap.Where(kv => kv.Key.StartsWith("follow:") && FollowGroups.ContainsKey(kv.Value)).ToDictionary(kv => kv.Key.Substring(7), kv => kv.Value);
        if (follow.Count == 0 || rigJ == null) return;
        var vg = new Dictionary<int, string>();
        foreach (var f in faces)
        {
            string lab; if (!faceLabel.TryGetValue(f, out lab)) continue;
            string g = null;
            foreach (var part in lab.Split('\u0001')) if (part != "" && follow.TryGetValue(part, out g)) break;
            if (g == null) continue;
            vg[f[0]] = g; vg[f[2]] = g; vg[f[4]] = g;
        }
        var count = new Dictionary<string, int>();
        foreach (var kv in vg)
        {
            int v = kv.Key; if (v < 0 || v >= rigJ.Length || v >= P.Count) continue;
            var js = FollowGroups[kv.Value].Select(n => Array.IndexOf(sk.Names, n)).Where(j => j >= 0).ToList();
            if (js.Count == 0) continue;
            var p = P[v];
            if (kv.Value == "CAPE")
            {
                // By height along the chain (top to bottom): above the first joint, it alone; between two, blended.
                var chain = js.OrderByDescending(j => sk.RestWorld[j][13]).ToList();
                float y = p[1];
                if (chain.Count == 1 || y >= sk.RestWorld[chain[0]][13]) { rigJ[v] = new[] { chain[0] }; rigW[v] = new[] { 1f }; }
                else if (y <= sk.RestWorld[chain[chain.Count - 1]][13]) { rigJ[v] = new[] { chain[chain.Count - 1] }; rigW[v] = new[] { 1f }; }
                else
                    for (int i = 0; i + 1 < chain.Count; i++)
                    {
                        float y0 = sk.RestWorld[chain[i]][13], y1 = sk.RestWorld[chain[i + 1]][13];
                        if (y > y0 || y < y1) continue;
                        float t = y0 - y1 > 1e-5f ? (y0 - y) / (y0 - y1) : 0;
                        rigJ[v] = new[] { chain[i], chain[i + 1] }; rigW[v] = new[] { 1 - t, t };
                        break;
                    }
                int cc; count.TryGetValue(kv.Value, out cc); count[kv.Value] = cc + 1;
                continue;
            }
            var near = js.Select(j => new { j, d = (float)Math.Sqrt(Math.Pow(sk.RestWorld[j][12] - p[0], 2) + Math.Pow(sk.RestWorld[j][13] - p[1], 2) + Math.Pow(sk.RestWorld[j][14] - p[2], 2)) })
                         .OrderBy(x => x.d).Take(2).ToList();
            if (near.Count == 1 || near[1].d > near[0].d * 2) { rigJ[v] = new[] { near[0].j }; rigW[v] = new[] { 1f }; }
            else
            {
                float a = 1 / (float)Math.Pow(Math.Max(near[0].d, 1e-4f), 2), b = 1 / (float)Math.Pow(Math.Max(near[1].d, 1e-4f), 2);
                rigJ[v] = new[] { near[0].j, near[1].j }; rigW[v] = new[] { a / (a + b), b / (a + b) };
            }
            int c; count.TryGetValue(kv.Value, out c); count[kv.Value] = c + 1;
        }
        foreach (var kv in follow) log("segue: " + kv.Key + " -> " + kv.Value);
        log("segue: pontos presos " + string.Join(", ", count.Select(kv => kv.Key + " " + kv.Value)));
    }

    // ---- the bone translator: a model's own skeleton (FBX) onto the base's ----    // Each bone is named by what it is (hips, spine, neck, head, shoulder, upper arm, forearm, hand, thigh,
    // shin, foot, toe; left / right) from the usual rigs' names (Mixamo, Blender, 3ds Max Biped, Unreal...);
    // fingers, ends and extras follow the nearest named bone above them. The model's spine bones share out the
    // base's by order. Each named bone then carries its own part of the mesh from the model's pose (an
    // A-pose) to the base's rest pose: turned so the bone lies along the base's, stretched to its length, its
    // start on the base's joint, and the skin's weights blend those moves. Fails (null) when the hips, head,
    // arms or legs are not found.
    internal static string Slot(string name, out int side)
    {
        string n = name; int c = Math.Max(n.LastIndexOf(':'), n.LastIndexOf('|')); if (c >= 0) n = n.Substring(c + 1);
        // MMD (2026-11-16): Japanese names, as the common humanoid words (左 left, 右 right; 腕 arm, ひじ elbow, 手首
        // wrist, 足/脚 leg, ひざ knee, 足首 ankle, つま先 toe, 上半身 upper body, 下半身 lower body, 首 neck, 頭 head, 肩
        // shoulder). Twist (捩), finger and "IK" bones are left out.
        if (System.Text.RegularExpressions.Regex.IsMatch(n, "[぀-ヿ一-鿿]"))
        {
            if (System.Text.RegularExpressions.Regex.IsMatch(n, "捩|ＩＫ|IK|親指|人指|中指|薬指|小指|指|先|ダミー|D$")) { if (!n.Contains("つま先")) { side = 0; return null; } }
            string jp = n;
            foreach (var kv in new[] { new[] { "左", "left_" }, new[] { "右", "right_" }, new[] { "上半身2", "chest" }, new[] { "上半身", "spine" },
                                       new[] { "下半身", "hips" }, new[] { "センター", "center" }, new[] { "つま先", "toe" }, new[] { "手首", "hand" },
                                       new[] { "足首", "foot" }, new[] { "ひざ", "leg" }, new[] { "ひじ", "forearm" }, new[] { "腕", "upperarm" },
                                       new[] { "足", "upleg" }, new[] { "脚", "upleg" }, new[] { "肩", "shoulder" }, new[] { "首", "neck" }, new[] { "頭", "head" } })
                jp = jp.Replace(kv[0], kv[1]);
            if (jp == "center") { side = 0; return null; }
            n = jp;
        }
        // VRoid (J_Bip_C_Hips, J_Bip_L_UpperArm, J_Bip_R_LowerLeg; J_Sec_ / J_Adj_ are hair, cloth and helpers).
        {
            var lo = n.ToLowerInvariant();
            if (lo.StartsWith("j_sec_") || lo.StartsWith("j_adj_")) { side = 0; return null; }
            if (lo.StartsWith("j_bip_") && lo.Length > 8)
            {
                char s = lo[6]; string rest = n.Substring(8);
                n = (s == 'l' ? "Left" : s == 'r' ? "Right" : "") + rest;
            }
        }
        string raw = n;
        n = n.ToLowerInvariant();
        foreach (var x in new[] { "mixamorig", "bip001", "bip01", "def-", "_", " ", "." , "-" }) n = n.Replace(x, "");
        side = 0;
        if (n.Contains("left")) { side = 1; n = n.Replace("left", ""); }
        else if (n.Contains("right")) { side = -1; n = n.Replace("right", ""); }
        else
        {
            var r = raw.ToLowerInvariant();
            if (r.EndsWith("_l") || r.EndsWith(".l") || r.EndsWith(" l") || r.StartsWith("l_") || r.StartsWith("bip01 l") || r.StartsWith("bip001 l")) { side = 1; if (n.EndsWith("l")) n = n.Substring(0, n.Length - 1); else if (n.StartsWith("l")) n = n.Substring(1); }
            else if (r.EndsWith("_r") || r.EndsWith(".r") || r.EndsWith(" r") || r.StartsWith("r_") || r.StartsWith("bip01 r") || r.StartsWith("bip001 r")) { side = -1; if (n.EndsWith("r")) n = n.Substring(0, n.Length - 1); else if (n.StartsWith("r")) n = n.Substring(1); }
        }
        if (n.EndsWith("end") || n.Contains("top") || n.Contains("nub")) return null;
        if (System.Text.RegularExpressions.Regex.IsMatch(n, "thumb|index|middle|ring|pinky|finger|twist|roll")) return null;
        if (side == 0)
        {
            if (n.Contains("hips") || n.Contains("pelvis")) return "HIPS";
            if (n.Contains("spine") || n.Contains("chest")) return "SPINE";
            if (n.Contains("neck")) return "NECK";
            if (n.Contains("head")) return "HEAD";
            return null;
        }
        if (n.Contains("shoulder") || n.Contains("clavicle") || n.Contains("collar")) return "SHOULDER";
        if (n.Contains("forearm") || n.Contains("lowerarm") || n.Contains("elbow")) return "FOREARM";
        if (n.Contains("upperarm") || n == "arm" || n.EndsWith("arm")) return "ARM";
        if (n.Contains("hand") || n.Contains("wrist")) return "HAND";
        if (n.Contains("upleg") || n.Contains("thigh") || n.Contains("upperleg")) return "UPLEG";
        if (n.Contains("toe")) return "TOEBASE";
        if (n.Contains("foot") || n.Contains("ankle")) return "FOOT";
        if (n.Contains("leg") || n.Contains("calf") || n.Contains("shin") || n.Contains("knee")) return "LEG";
        return null;
    }

    // Bones without usable names, read by the skeleton's shape (the body already turned to the base's axes): the
    // feet are the lowest skinned bones on each side, the hips where the two legs meet, the hands the skinned bones
    // farthest out on each side (between hip and head height), the chest where the arms meet; the neck and head go
    // up from the chest. Each chain (spine, each arm, each leg, neck) is shared out over the base's same chain by
    // the share of its length: the bone nearest each base joint's share moves it (primary), every bone of the chain
    // follows the base joint nearest to its own share (twist and helper bones included); the rest follow the
    // nearest chain bone above them.
    static void ShapeMap(FbxModel fbx, Func<int, float[]> sp, List<float[]> mesh, HashSet<int> used, Skeleton sk, float lx,
                         Dictionary<int, int> primary, int[] map, Action<string> log)
    {
        int nb = fbx.Bones.Count;
        float lo = mesh.Min(q => q[1]), hi = mesh.Max(q => q[1]), h = hi - lo;
        float cx = mesh.Where(q => q[1] > lo + h * 0.45f && q[1] < lo + h * 0.6f).Average(q => q[0]);
        Func<int, int> par = b => fbx.Bones[b].Parent;
        Func<int, List<int>> up = b => { var l = new List<int>(); for (int p = b; p >= 0; p = par(p)) l.Add(p); return l; };  // b .. root
        Func<int, int, int> lca = (a, b) => { var A = new HashSet<int>(up(a)); foreach (int p in up(b)) if (A.Contains(p)) return p; return -1; };
        Func<int, int, List<int>> path = (from, to) => { var l = up(to); int i = l.IndexOf(from); if (i < 0) return null; l = l.Take(i).ToList(); l.Reverse(); return l; };  // after 'from' .. 'to'
        var kids = new List<int>[nb]; for (int b = 0; b < nb; b++) kids[b] = new List<int>();
        for (int b = 0; b < nb; b++) if (par(b) >= 0) kids[par(b)].Add(b);
        Func<int, IEnumerable<int>> below = null; below = b => new[] { b }.Concat(kids[b].SelectMany(k => below(k)));
        var skinned = Enumerable.Range(0, nb).Where(used.Contains).ToList();
        Func<int, float[]> at = j => new[] { sk.RestWorld[j][12], sk.RestWorld[j][13], sk.RestWorld[j][14] };
        Func<string, int> J0 = n => Array.IndexOf(sk.Names, n);
        var foot = new int[2]; var hand = new int[2];
        for (int s = 0; s < 2; s++)
        {
            float side = s == 0 ? lx : -lx;
            var mine = skinned.Where(b => (sp(b)[0] - cx) * side > h * 0.02f).ToList();
            if (mine.Count == 0) { log("forma: nenhum osso de um dos lados"); return; }
            foot[s] = mine.OrderBy(b => sp(b)[1]).First();
            hand[s] = mine.Where(b => sp(b)[1] > lo + h * 0.3f && sp(b)[1] < lo + h * 0.85f).OrderByDescending(b => (sp(b)[0] - cx) * side).FirstOrDefault();
        }
        int hips = lca(foot[0], foot[1]), chest = lca(hand[0], hand[1]);
        if (hips < 0 || chest < 0) { log("forma: pernas ou bracos sem raiz comum"); return; }
        Func<int, string> where = b => fbx.Bones[b].Name + "(" + string.Join(",", sp(b).Select(v => v.ToString("F2"))) + ")";
        log("forma: pes " + where(foot[0]) + " " + where(foot[1]) + ", maos " + where(hand[0]) + " " + where(hand[1]) + ", quadril " + where(hips) + ", peito " + where(chest));
        // The legs may hang from a pelvis bone of their own beside the spine (a game's rig): the hips are then where
        // the legs and the spine meet; the legs still start after the bone they share.
        int legRoot = hips;
        if (!up(chest).Contains(hips)) { hips = lca(hips, chest); if (hips < 0) { log("forma: pernas e peito sem raiz comum"); return; } }
        // A base chain's joints and the share of its length at each (from the first joint of the chain).
        Func<List<int>, float[]> shares = pts =>
        {
            var f = new float[pts.Count]; float acc = 0;
            for (int i = 1; i < pts.Count; i++) { acc += Len(Sub(at(pts[i]), at(pts[i - 1]))); f[i] = acc; }
            for (int i = 0; i < f.Length; i++) f[i] = acc > 0 ? f[i] / acc : 0; return f;
        };
        Action<List<int>, List<int>, string> chain = (src, tgt, label) =>
        {
            tgt = tgt.Where(t => t >= 0).ToList();
            if (src == null || src.Count == 0 || tgt.Count == 0) return;
            var tf = shares(tgt);
            var sf = new float[src.Count]; float acc = 0;
            for (int i = 1; i < src.Count; i++) { acc += Len(Sub(sp(src[i]), sp(src[i - 1]))); sf[i] = acc; }
            for (int i = 0; i < sf.Length; i++) sf[i] = acc > 0 ? sf[i] / acc : 0;
            for (int i = 0; i < src.Count; i++)
            {
                int k = 0; for (int j = 1; j < tf.Length; j++) if (Math.Abs(tf[j] - sf[i]) < Math.Abs(tf[k] - sf[i])) k = j;
                map[src[i]] = tgt[k];
            }
            for (int j = 0; j < tgt.Count; j++)
            {
                int best = 0; for (int i = 1; i < src.Count; i++) if (Math.Abs(sf[i] - tf[j]) < Math.Abs(sf[best] - tf[j])) best = i;
                if (!primary.ContainsKey(tgt[j])) primary[tgt[j]] = src[best];
            }
            log("forma: " + label + " " + src.Count + " ossos -> " + string.Join(" ", tgt.Select(t => sk.Names[t])));
        };
        // spine: hips .. chest
        var spineSrc = new List<int> { hips }; spineSrc.AddRange(path(hips, chest));
        var tspine = new List<int>(); for (int p = sk.Parent[J0("NECK")]; p >= 0 && p != J0("HIPSBONE"); p = sk.Parent[p]) if (sk.Names[p].StartsWith("SPINE")) tspine.Insert(0, p);
        tspine.Insert(0, J0("HIPSBONE"));
        chain(spineSrc, tspine, "coluna");
        var armRoots = new HashSet<int>();
        for (int s = 0; s < 2; s++)
        {
            string S = s == 0 ? "LEFT" : "RIGHT";
            // arm: after the chest, down to the wrist (the first bone whose part beyond is hand-sized)
            var arm = path(chest, hand[s]);
            int wrist = arm.FindIndex(b => below(b).Max(q => Len(Sub(sp(q), sp(b)))) < h * 0.12f);
            if (wrist >= 0) arm = arm.Take(wrist + 1).ToList();
            if (arm.Count > 0) armRoots.Add(arm[0]);
            chain(arm, new List<int> { J0(S + "SHOULDER"), J0(S + "ARM"), J0(S + "FOREARM"), J0(S + "HAND") }, S.ToLower() + " braco");
            // leg: after the hips, down to the ankle (the first bone low enough); what is beyond is the foot
            var leg = path(legRoot, foot[s]);
            int ankle = leg.FindIndex(b => sp(b)[1] < lo + h * 0.12f);
            if (ankle >= 0) leg = leg.Take(ankle + 1).ToList();
            chain(leg, new List<int> { J0(S + "UPLEG"), J0(S + "LEG"), J0(S + "FOOT") }, S.ToLower() + " perna");
        }
        // neck and head: from the chest up to the highest skinned bone outside the arms
        var armSet = new HashSet<int>(armRoots.SelectMany(r => below(r)));
        var tops = below(chest).Where(b => b != chest && !armSet.Contains(b) && used.Contains(b)).OrderByDescending(b => sp(b)[1]).ToList();
        if (tops.Count > 0)
        {
            var neck = path(chest, tops[0]);
            // the head joint sits low in the head (the base's at 88% of its height): the chain ends at the bone nearest there
            float headAt = lo + h * ((at(J0("HEAD"))[1] - lo) / h);
            int cut = neck.Count == 0 ? -1 : neck.Select((b, i) => new { b, i }).OrderBy(x => Math.Abs(sp(x.b)[1] - headAt)).First().i;
            if (cut >= 0) neck = neck.Take(cut + 1).ToList();
            chain(neck, new List<int> { J0("NECK"), J0("HEAD") }, "pescoco");
        }
        // the rest: the nearest mapped bone above (the root and what is above the hips: the hips)
        for (int b = 0; b < nb; b++)
            if (map[b] < 0) { int p = par(b); while (p >= 0 && map[p] < 0) p = par(p); map[b] = p >= 0 ? map[p] : J0("HIPSBONE"); }
    }

    // The joints' new places (model space, by name) the last TranslateBones chose; null after the auto-rig.
    static Dictionary<string, float[]> Places;

    static bool TranslateBones(FbxModel fbx, List<float[]> P, int nMesh, List<Dictionary<int, double>> VW, Skeleton sk, out int[][] J, out float[][] W, Action<string> log)
    {
        J = null; W = null;
        int nb = fbx.Bones.Count;
        Func<int, float[]> sp = b => P[nMesh + b];
        Func<int, float[]> at = j => new[] { sk.RestWorld[j][12], sk.RestWorld[j][13], sk.RestWorld[j][14] };
        Func<string, int> J0 = n => Array.IndexOf(sk.Names, n);
        float lx = at(J0("LEFTHAND"))[0] > at(J0("RIGHTHAND"))[0] ? 1 : -1;
        // which side the model's "left" is, to check
        var depth = new int[nb]; for (int b = 0; b < nb; b++) { int d = 0; for (int p = fbx.Bones[b].Parent; p >= 0; p = fbx.Bones[p].Parent) d++; depth[b] = d; }
        var primary = new Dictionary<int, int>();  // base joint -> model bone that defines its move
        var map = new int[nb]; for (int b = 0; b < nb; b++) map[b] = -1;
        var spine = new List<int>();
        foreach (int b in Enumerable.Range(0, nb).OrderBy(b => depth[b]))
        {
            int side; string s = Slot(fbx.Bones[b].Name, out side);
            if (s == null) continue;
            if (s == "SPINE") { spine.Add(b); continue; }
            string tn = s == "HIPS" ? "HIPSBONE" : side == 0 ? s : (side > 0 ? "LEFT" : "RIGHT") + s;
            int t = J0(tn); if (t < 0 || primary.ContainsKey(t)) continue;
            primary[t] = b; map[b] = t;
        }
        int hips = J0("HIPSBONE"), neck = J0("NECK");
        // the base's spine: from the neck down to the hips
        var tspine = new List<int>(); for (int p = sk.Parent[neck]; p >= 0 && p != hips; p = sk.Parent[p]) if (sk.Names[p].StartsWith("SPINE")) tspine.Insert(0, p);
        for (int i = 0; i < spine.Count && tspine.Count > 0; i++)
        {
            int t = tspine[spine.Count == 1 ? tspine.Count - 1 : (int)Math.Round(i * (tspine.Count - 1) / (double)(spine.Count - 1))];
            map[spine[i]] = t; if (!primary.ContainsKey(t)) primary[t] = spine[i];
        }
        var need = new[] { "HIPSBONE", "HEAD", "LEFTARM", "RIGHTARM", "LEFTFOREARM", "RIGHTFOREARM", "LEFTUPLEG", "RIGHTUPLEG", "LEFTLEG", "RIGHTLEG" };
        var missing = need.Where(n => !primary.ContainsKey(J0(n))).ToList();
        if (missing.Count > 0)
        {   // names unknown (a game's "bone12"): the skeleton read by its shape
            log("tradutor de ossos: nomes desconhecidos (" + string.Join(", ", missing) + "): lendo pela forma do esqueleto");
            primary.Clear(); for (int b = 0; b < nb; b++) map[b] = -1;
            var used = new HashSet<int>(); for (int i = 0; i < nMesh && i < VW.Count; i++) foreach (var k in VW[i].Keys) used.Add(k);
            ShapeMap(fbx, sp, P.Take(nMesh).ToList(), used, sk, lx, primary, map, log);
            missing = need.Where(n => !primary.ContainsKey(J0(n))).ToList();
        }
        log("tradutor de ossos: " + string.Join(", ", primary.OrderBy(kv => kv.Key).Select(kv => sk.Names[kv.Key] + "=" + fbx.Bones[kv.Value].Name)));
        if (missing.Count > 0) { log("tradutor de ossos: faltam " + string.Join(", ", missing) + " (nomes desconhecidos): auto-rig"); return false; }
        // the model's left hand must be on the base's left side once turned
        if ((sp(primary[J0("LEFTHAND")] )[0] - sp(primary[J0("RIGHTHAND")])[0]) * lx < 0) log("tradutor de ossos: atencao, esquerda e direita trocadas no modelo");
        // the rest follow the nearest bone above with a joint
        for (int b = 0; b < nb; b++)
            if (map[b] < 0) { int p = fbx.Bones[b].Parent; while (p >= 0 && map[p] < 0) p = fbx.Bones[p].Parent; map[b] = p >= 0 ? map[p] : hips; }
        // moves, parents first
        var set = new HashSet<int>(primary.Keys);
        Func<int, int> bpar = j => { int p = sk.Parent[j]; while (p >= 0 && !set.Contains(p)) p = sk.Parent[p]; return p; };
        var R = new Dictionary<int, float[]>(); var S = new Dictionary<int, float>(); var U = new Dictionary<int, float[]>();
        foreach (int t in sk.Order.Where(set.Contains))
        {
            var kids = set.Where(c => bpar(c) == t).ToList();
            int next = kids.Count == 0 ? -1 : kids.OrderBy(c => Math.Abs(at(c)[0] - at(t)[0]) + (sk.Names[c].Contains("UPLEG") ? 1 : 0)).First();
            if (next >= 0)
            {
                var ds = Sub(sp(primary[next]), sp(primary[t])); var dt = Sub(at(next), at(t));
                if (Len(ds) > 1e-4f && Len(dt) > 1e-4f)
                {
                    R[t] = Turn(ds, dt); float l = Len(ds); U[t] = new[] { ds[0] / l, ds[1] / l, ds[2] / l };
                    S[t] = Math.Max(0.5f, Math.Min(2f, Len(dt) / l)); continue;
                }
            }
            int pp = bpar(t);
            R[t] = pp >= 0 && R.ContainsKey(pp) ? R[pp] : new float[] { 1, 0, 0, 0, 1, 0, 0, 0, 1 }; S[t] = 1; U[t] = new float[] { 0, 1, 0 };
        }
        // The skeleton takes the model's proportions (2026-11-16: stretched onto the father's joints, 2B came out with
        // his shoulders and hips): each joint goes where the model's own joint lands once its parents turned into the
        // base's pose. The mesh only turns, never stretches; the base's joints are moved there (SetRestPositions).
        var place = new Dictionary<int, float[]>();
        foreach (int t in sk.Order.Where(set.Contains))
        {
            int p = bpar(t);
            if (p < 0 || !place.ContainsKey(p)) { place[t] = sp(primary[t]); continue; }
            var r = RotP(R[p], new float[3], Sub(sp(primary[t]), sp(primary[p])), false);
            place[t] = new[] { place[p][0] + r[0], place[p][1] + r[1], place[p][2] + r[2] };
        }
        Places = place.ToDictionary(kv => sk.Names[kv.Key], kv => kv.Value);
        Func<int, float[], float[]> move = (t, v) =>
        {
            var r = RotP(R[t], new float[3], Sub(v, sp(primary[t])), false); var o = place[t];
            return new[] { r[0] + o[0], r[1] + o[1], r[2] + o[2] };
        };
        J = new int[nMesh][]; W = new float[nMesh][];
        for (int i = 0; i < nMesh; i++)
        {
            var acc = new Dictionary<int, float>();
            foreach (var kv in VW[i]) { int t = map[kv.Key]; float o; acc.TryGetValue(t, out o); acc[t] = o + (float)kv.Value; }
            if (acc.Count == 0) acc[hips] = 1;
            var top = acc.OrderByDescending(kv => kv.Value).Take(4).ToList(); float sum = top.Sum(kv => kv.Value);
            J[i] = top.Select(kv => kv.Key).ToArray(); W[i] = top.Select(kv => kv.Value / sum).ToArray();
            var v = P[i]; var r = new float[3];
            for (int k = 0; k < J[i].Length; k++) { var q = move(J[i][k], v); r[0] += q[0] * W[i][k]; r[1] += q[1] * W[i][k]; r[2] += q[2] * W[i][k]; }
            P[i][0] = r[0]; P[i][1] = r[1]; P[i][2] = r[2];
        }
        return true;
    }

    // A simpler copy of a mesh for the farther levels of detail (vertex clustering): the points in each cube of a
    // grid become one (the first, with its texture coordinate and bones); triangles left with two corners in one
    // cube go. The cube grows until the triangles are at most 'target'. A texture seam inside a cube smears a
    // little: seen from afar only.
    static void Decimate(float[] p, float[] uv, int[] t, int[] src, int target, out float[] op, out float[] ouv, out int[] ot, out int[] osrc, Func<int, int> limbOf = null)
    {
        float x0 = float.MaxValue, x1 = float.MinValue, y0 = float.MaxValue, y1 = float.MinValue, z0 = float.MaxValue, z1 = float.MinValue;
        for (int i = 0; i + 2 < p.Length; i += 3) { x0 = Math.Min(x0, p[i]); x1 = Math.Max(x1, p[i]); y0 = Math.Min(y0, p[i + 1]); y1 = Math.Max(y1, p[i + 1]); z0 = Math.Min(z0, p[i + 2]); z1 = Math.Max(z1, p[i + 2]); }
        float size = Math.Max(x1 - x0, Math.Max(y1 - y0, z1 - z0));
        float cell = size / 400;
        int n = p.Length / 3;
        while (true)
        {
            var at = new Dictionary<long, int>(); var map = new int[n]; var keep = new List<int>();
            for (int i = 0; i < n; i++)
            {
                long kx = (long)((p[i * 3] - x0) / cell), ky = (long)((p[i * 3 + 1] - y0) / cell), kz = (long)((p[i * 3 + 2] - z0) / cell);
                // points of different limbs never merge (2026-11-16: Caim's thighs touch; a merged point bound to one
                // leg stretched the other leg's triangles across, "a piece mixed between the legs" in the far copies)
                long k = (kx << 40) | (ky << 20) | kz | ((long)(limbOf == null ? 0 : limbOf(src[i])) << 60);
                int c; if (!at.TryGetValue(k, out c)) { c = keep.Count; at[k] = c; keep.Add(i); }
                map[i] = c;
            }
            var tri = new List<int>(); var seen = new HashSet<long>();
            for (int i = 0; i + 2 < t.Length; i += 3)
            {
                int a = map[t[i]], b = map[t[i + 1]], c = map[t[i + 2]];
                if (a == b || b == c || a == c) continue;
                // the same triangle twice (two folds of cloth into the same cubes): once
                int lo = Math.Min(a, Math.Min(b, c)), hi = Math.Max(a, Math.Max(b, c)), md = a + b + c - lo - hi;
                if (!seen.Add(((long)lo << 42) | ((long)md << 21) | (long)hi)) continue;
                tri.Add(a); tri.Add(b); tri.Add(c);
            }
            if (tri.Count / 3 <= target || cell > size)
            {
                op = new float[keep.Count * 3]; ouv = uv == null ? null : new float[keep.Count * 2]; osrc = new int[keep.Count];
                for (int i = 0; i < keep.Count; i++)
                {
                    int s = keep[i];
                    op[i * 3] = p[s * 3]; op[i * 3 + 1] = p[s * 3 + 1]; op[i * 3 + 2] = p[s * 3 + 2];
                    if (uv != null) { ouv[i * 2] = uv[s * 2]; ouv[i * 2 + 1] = uv[s * 2 + 1]; }
                    osrc[i] = src[s];
                }
                ot = tri.ToArray();
                return;
            }
            cell *= 1.25f;
        }
    }

    // The colour map among a material's textures. Games' rips link them under the wrong names (2B's face had a
    // 128 px normal map as its "DiffuseColor"), so they are judged by their look: a normal map is lilac-blue with red
    // and green about the middle; a mask is loud (saturated) colour. The linked one wins when it looks like colour.
    static readonly Dictionary<string, int> lookCache = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
    static int Look(string file)  // 0 colour, 1 mask-like, 2 normal map, 3 unreadable
    {
        int r; if (lookCache.TryGetValue(file, out r)) return r;
        try
        {
            int w, h; var px = PngIo.Load(file, out w, out h);
            double sr = 0, sg = 0, sb = 0, sat = 0; int n = 0;
            int step = Math.Max(1, w * h / 20000);
            for (int i = 0; i < w * h; i += step)
            {
                int R = px[i * 4], G = px[i * 4 + 1], B = px[i * 4 + 2];
                sr += R; sg += G; sb += B; sat += Math.Max(R, Math.Max(G, B)) - Math.Min(R, Math.Min(G, B)); n++;
            }
            sr /= n; sg /= n; sb /= n; sat /= n;
            r = sb > 190 && Math.Abs(sr - 128) < 45 && Math.Abs(sg - 128) < 45 ? 2 : sat > 110 || sb > sr + 40 ? 1 : 0;
        }
        catch { r = 3; }
        lookCache[file] = r;
        return r;
    }
    // The average colour of a texture's visible pixels, as RRGGBB.
    static string AverageColour(string file)
    {
        int w, h; var px = PngIo.Load(file, out w, out h);
        long r = 0, g = 0, b = 0, n = 0;
        for (int i = 0; i < w * h; i += Math.Max(1, w * h / 40000))
            if (px[i * 4 + 3] >= 128) { r += px[i * 4]; g += px[i * 4 + 1]; b += px[i * 4 + 2]; n++; }
        if (n == 0) return "808080";
        return (r / n).ToString("X2") + (g / n).ToString("X2") + (b / n).ToString("X2");
    }

    static string ColourOf(List<string> files, string linked, string dir)
    {
        Func<string, string> find = f => { if (f == null) return null; if (File.Exists(f)) return f; var g = Directory.GetFiles(dir, Path.GetFileName(f), SearchOption.AllDirectories).FirstOrDefault(); return g; };
        var real = files.Select(find).Where(f => f != null).Distinct(StringComparer.OrdinalIgnoreCase).ToList();
        string l = find(linked);
        if (l != null && Look(l) == 0) return l;
        return real.Where(f => Look(f) == 0).OrderByDescending(f => new FileInfo(f).Length).FirstOrDefault();  // null: none looks like colour
    }

    // Shelf packing, tallest first, gap pixels between.
    static bool Pack(List<Mat> mats, int AW, int AH, float f, int gap)
    {
        int x = gap, y = gap, row = 0;
        // Materials of one picture share its place (2026-11-16: Zero's body sheet was in the atlas four times, all of
        // it at a quarter of its size).
        var groups = mats.GroupBy(q => q.File != null && !q.File.StartsWith("flat:") ? q.File.ToLowerInvariant() : "\0" + mats.IndexOf(q)).ToList();
        foreach (var g in groups.OrderByDescending(q => q.First().H).ThenByDescending(q => q.First().W))
        {
            var m = g.First();
            int w = (int)(m.W * f), h = (int)(m.H * f);
            if (x + w + gap > AW) { x = gap; y += row + gap * 2; row = 0; }
            if (w + gap * 2 > AW || y + h + gap > AH) return false;
            foreach (var o in g) { o.X = x; o.Y = y; }
            x += w + gap * 2; row = Math.Max(row, h);
        }
        return true;
    }

    // A software picture: front and side, the rest pose and a frame of the run (the built model, skinned).
    public static void PreviewFiles(string mdv, string mdp, string atlasPng, string png)
    {
        int w, h; var atlas = PngIo.Load(atlasPng, out w, out h);
        Preview(File.ReadAllBytes(mdv), File.ReadAllBytes(mdp), png, atlas, w, h, s => Console.WriteLine(s));
    }
    static void Preview(byte[] mdv, byte[] mdp, string png, byte[] atlas, int AW, int AH, Action<string> log)
    {
        var motions = new List<Motion>();
        var sk = Anim.Load(mdv, motions);
        var parts = MdlFile.Load("x", mdv, mdp).Parts();
        string only = Environment.GetEnvironmentVariable("NT_PREVIEW_PART");  // one part (its number in the list), to find a bad one
        if (only != null) { int k = int.Parse(only); log("parte " + k + " de " + parts.Count + ": " + parts[k].F.Length / 3 + " triangulos, textura " + parts[k].Tex); parts = new List<MdlFile.MeshPart> { parts[k] }; }
        string want = Environment.GetEnvironmentVariable("NT_PREVIEW_ANIM");  // the animation shown (part of its name)
        var run = (want != null ? motions.FirstOrDefault(m => m.Name.IndexOf(want, StringComparison.OrdinalIgnoreCase) >= 0) : null)
               ?? motions.FirstOrDefault(m => m.Name.EndsWith("_run_01", StringComparison.OrdinalIgnoreCase))
               ?? motions.FirstOrDefault(m => m.Name.IndexOf("_run", StringComparison.OrdinalIgnoreCase) >= 0) ?? motions.FirstOrDefault();
        int S = 400;  // env NT_PREVIEW_SIZE: bigger views (a close look)
        int ps; if (int.TryParse(Environment.GetEnvironmentVariable("NT_PREVIEW_SIZE") ?? "", out ps) && ps >= 100 && ps <= 2000) S = ps;
        var img = new byte[S * 4 * S * 4];
        for (int i = 0; i < img.Length; i += 4) { img[i] = 60; img[i + 1] = 64; img[i + 2] = 72; img[i + 3] = 255; }
        var poses = new List<List<MdlFile.MeshPart>> { parts };
        if (run != null && sk != null)
        {
            var mats = Anim.SkinMatrices(sk, Anim.World(sk, run, run.Frames / 3f, false));
            poses.Add(parts.Select(p => Anim.Skin(p, mats)).ToList());
            log("previa: animacao " + run.Name);
        }
        for (int pi = 0; pi < poses.Count; pi++)
            for (int view = 0; view < 2; view++)
                Raster(poses[pi], img, S * 4, (pi * 2 + view) * S, S, view, atlas, AW, AH);
        PngIo.Save(png, img, S * 4, S);
        log("previa: " + png);
    }

    static void Raster(List<MdlFile.MeshPart> parts, byte[] img, int IW, int ox, int S, int view, byte[] atlas, int AW, int AH)
    {
        var all = parts.Where(p => p.V != null && p.F != null && p.F.Length > 0).ToList();
        if (all.Count == 0) return;
        float lo = float.MaxValue, hi = float.MinValue;
        foreach (var p in all) for (int i = 1; i < p.V.Length; i += 3) { lo = Math.Min(lo, p.V[i]); hi = Math.Max(hi, p.V[i]); }
        float sc = S * 0.9f / Math.Max(0.01f, hi - lo);
        var zb = new float[S * S]; for (int i = 0; i < zb.Length; i++) zb[i] = float.MinValue;
        foreach (var p in all)
        {
            bool atl = p.Tex != null && p.Tex.IndexOf("ALL", StringComparison.OrdinalIgnoreCase) >= 0;
            for (int f = 0; f + 2 < p.F.Length; f += 3)
            {
                var sx = new float[3]; var sy = new float[3]; var sz = new float[3]; var tu = new float[3]; var tv = new float[3];
                for (int k = 0; k < 3; k++)
                {
                    int v = p.F[f + k];
                    float x = p.V[v * 3], y = p.V[v * 3 + 1], z = p.V[v * 3 + 2];
                    float hx = view == 0 ? x : z, hz = view == 0 ? z : -x;
                    sx[k] = S / 2 + hx * sc; sy[k] = S * 0.95f - (y - lo) * sc; sz[k] = hz;
                    tu[k] = p.UV != null ? p.UV[v * 2] : 0; tv[k] = p.UV != null ? p.UV[v * 2 + 1] : 0;
                }
                // flat shade from the face's normal (screen space)
                float ax = sx[1] - sx[0], ay = sy[1] - sy[0], bx = sx[2] - sx[0], by = sy[2] - sy[0];
                float area = ax * by - ay * bx; if (Math.Abs(area) < 1e-6f) continue;
                int x0 = (int)Math.Max(0, Math.Floor(sx.Min())), x1 = (int)Math.Min(S - 1, Math.Ceiling(sx.Max()));
                int y0 = (int)Math.Max(0, Math.Floor(sy.Min())), y1 = (int)Math.Min(S - 1, Math.Ceiling(sy.Max()));
                for (int y = y0; y <= y1; y++)
                    for (int x = x0; x <= x1; x++)
                    {
                        float px = x + 0.5f - sx[0], py = y + 0.5f - sy[0];
                        float w1 = (px * by - py * bx) / area, w2 = (ax * py - ay * px) / area, w0 = 1 - w1 - w2;
                        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                        float z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
                        if (z <= zb[y * S + x]) continue;
                        zb[y * S + x] = z;
                        byte r = 150, g = 150, b = 160;
                        if (atl)
                        {
                            float u = w0 * tu[0] + w1 * tu[1] + w2 * tu[2], v = w0 * tv[0] + w1 * tv[1] + w2 * tv[2];
                            int tx = Math.Min(AW - 1, Math.Max(0, (int)(u * AW))), ty = Math.Min(AH - 1, Math.Max(0, (int)(v * AH)));
                            int q = (ty * AW + tx) * 4; r = atlas[q]; g = atlas[q + 1]; b = atlas[q + 2];
                        }
                        int o = (y * IW + ox + x) * 4; img[o] = r; img[o + 1] = g; img[o + 2] = b;
                    }
            }
        }
    }
}
