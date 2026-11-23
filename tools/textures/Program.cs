// NierTextureTool - command line.
//   list <media> [filter]               packs and textures
//   find <media> <text>                 files of the CPKs by name
//   cat <media> <dir/name> <out>        one (decompressed) file
//   extract <media> <out> [filter]      textures to <out>\<dir>\<pack>\<texture>.png
//   build <media> <png dir> <out dir>   packs with the edited PNGs -> <out>\<dir>\<pack>.2DV/.2DP
//   roundtrip <media> <filter>          encoder check
// <media> is the folder with layer1.cpk / layer2.cpk.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Text;

public static class PngIo
{
    public static byte[] Encode(byte[] rgba, int w, int h)
    {
        string tmp = System.IO.Path.GetTempFileName();
        try { Save(tmp, rgba, w, h); return File.ReadAllBytes(tmp); } finally { File.Delete(tmp); }
    }

    public static byte[] LoadBytes(byte[] png, out int w, out int h)
    {
        string tmp = System.IO.Path.GetTempFileName();
        try { File.WriteAllBytes(tmp, png); return Load(tmp, out w, out h); } finally { File.Delete(tmp); }
    }

    public static void Save(string path, byte[] rgba, int w, int h)
    {
        using (var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb))
        {
            var d = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
            var row = new byte[w * 4];
            for (int y = 0; y < h; y++)
            {
                for (int x = 0; x < w; x++)
                {
                    int s = (y * w + x) * 4;
                    row[x * 4] = rgba[s + 2]; row[x * 4 + 1] = rgba[s + 1]; row[x * 4 + 2] = rgba[s]; row[x * 4 + 3] = rgba[s + 3];
                }
                System.Runtime.InteropServices.Marshal.Copy(row, 0, d.Scan0 + y * d.Stride, row.Length);
            }
            bmp.UnlockBits(d);
            Directory.CreateDirectory(System.IO.Path.GetDirectoryName(path));
            bmp.Save(path, ImageFormat.Png);
        }
    }

    // DDS (DXT1 / DXT3 / DXT5, top level): textures extracted from games (Outfit.cs reads a model's own).
    // TGA (2026-11-16, Zero's .mtl links them; Windows' image reader has none): true colour 24/32 bits, plain or RLE,
    // 8-bit grey; rows bottom-up unless the header's bit 5 says top-down.
    static byte[] LoadTga(byte[] d, out int w, out int h)
    {
        int idLen = d[0], cmapType = d[1], type = d[2];
        int cmapLen = d[5] | d[6] << 8, cmapBits = d[7];
        w = d[12] | d[13] << 8; h = d[14] | d[15] << 8;
        int bpp = d[16], desc = d[17];
        if (cmapType != 0 || !(type == 2 || type == 3 || type == 10 || type == 11) || !(bpp == 24 || bpp == 32 || bpp == 8))
            throw new Exception("TGA tipo " + type + " de " + bpp + " bits nao suportado");
        int at = 18 + idLen + cmapLen * ((cmapBits + 7) / 8);
        int bytes = bpp / 8, n = w * h;
        var raw = new byte[n * bytes];
        if (type < 9) Buffer.BlockCopy(d, at, raw, 0, Math.Min(raw.Length, d.Length - at));
        else
            for (int o = 0; o < raw.Length && at < d.Length;)
            {
                int c = d[at++], count = (c & 0x7F) + 1;
                if ((c & 0x80) != 0) { for (int i = 0; i < count && o < raw.Length; i++, o += bytes) Buffer.BlockCopy(d, at, raw, o, bytes); at += bytes; }
                else { int len = Math.Min(count * bytes, raw.Length - o); Buffer.BlockCopy(d, at, raw, o, len); at += count * bytes; o += len; }
            }
        var px = new byte[n * 4];
        bool topDown = (desc & 0x20) != 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                int s = ((topDown ? y : h - 1 - y) * w + x) * bytes, o = (y * w + x) * 4;
                if (bytes == 1) { px[o] = px[o + 1] = px[o + 2] = raw[s]; px[o + 3] = 255; continue; }
                px[o] = raw[s + 2]; px[o + 1] = raw[s + 1]; px[o + 2] = raw[s]; px[o + 3] = bytes == 4 ? raw[s + 3] : (byte)255;
            }
        return px;
    }

    static byte[] LoadDds(byte[] d, out int w, out int h)
    {
        h = BitConverter.ToInt32(d, 12); w = BitConverter.ToInt32(d, 16);
        string cc = Encoding.ASCII.GetString(d, 84, 4);
        int at = 128, bpb;
        if (cc == "DX10") { int f = BitConverter.ToInt32(d, 128); at += 20; cc = f == 71 || f == 72 ? "DXT1" : f == 74 || f == 75 ? "DXT3" : f == 77 || f == 78 ? "DXT5" : "?"; }
        if (cc == "DXT1") bpb = 8; else if (cc == "DXT3" || cc == "DXT5") bpb = 16;
        else throw new Exception("DDS " + cc + " nao suportado");
        var px = new byte[w * h * 4];
        int bw = (w + 3) / 4, bh = (h + 3) / 4;
        var col = new byte[4][];
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++)
            {
                int o = at + (by * bw + bx) * bpb;
                var alpha = new byte[16];
                for (int i = 0; i < 16; i++) alpha[i] = 255;
                if (bpb == 16)
                {
                    if (cc == "DXT3") { for (int i = 0; i < 16; i++) alpha[i] = (byte)(((d[o + i / 2] >> ((i & 1) * 4)) & 15) * 17); }
                    else
                    {
                        int a0 = d[o], a1 = d[o + 1]; var ap = new int[8]; ap[0] = a0; ap[1] = a1;
                        if (a0 > a1) for (int i = 1; i < 7; i++) ap[i + 1] = ((7 - i) * a0 + i * a1) / 7;
                        else { for (int i = 1; i < 5; i++) ap[i + 1] = ((5 - i) * a0 + i * a1) / 5; ap[6] = 0; ap[7] = 255; }
                        ulong bits = 0; for (int i = 0; i < 6; i++) bits |= (ulong)d[o + 2 + i] << (8 * i);
                        for (int i = 0; i < 16; i++) alpha[i] = (byte)ap[(bits >> (3 * i)) & 7];
                    }
                    o += 8;
                }
                int c0 = d[o] | d[o + 1] << 8, c1 = d[o + 2] | d[o + 3] << 8;
                Func<int, byte[]> rgb = c => new[] { (byte)((c >> 11 & 31) * 255 / 31), (byte)((c >> 5 & 63) * 255 / 63), (byte)((c & 31) * 255 / 31), (byte)255 };
                col[0] = rgb(c0); col[1] = rgb(c1);
                if (c0 > c1 || bpb == 16)
                {
                    col[2] = new[] { (byte)((2 * col[0][0] + col[1][0]) / 3), (byte)((2 * col[0][1] + col[1][1]) / 3), (byte)((2 * col[0][2] + col[1][2]) / 3), (byte)255 };
                    col[3] = new[] { (byte)((col[0][0] + 2 * col[1][0]) / 3), (byte)((col[0][1] + 2 * col[1][1]) / 3), (byte)((col[0][2] + 2 * col[1][2]) / 3), (byte)255 };
                }
                else
                {
                    col[2] = new[] { (byte)((col[0][0] + col[1][0]) / 2), (byte)((col[0][1] + col[1][1]) / 2), (byte)((col[0][2] + col[1][2]) / 2), (byte)255 };
                    col[3] = new byte[] { 0, 0, 0, 0 };
                }
                uint idx = BitConverter.ToUInt32(d, o + 4);
                for (int i = 0; i < 16; i++)
                {
                    int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2); if (x >= w || y >= h) continue;
                    var c = col[(idx >> (2 * i)) & 3]; int q = (y * w + x) * 4;
                    px[q] = c[0]; px[q + 1] = c[1]; px[q + 2] = c[2]; px[q + 3] = bpb == 16 ? alpha[i] : c[3];
                }
            }
        return px;
    }

    public static byte[] Load(string path, out int w, out int h)
    {
        if (path.EndsWith(".dds", StringComparison.OrdinalIgnoreCase)) return LoadDds(File.ReadAllBytes(path), out w, out h);
        if (path.EndsWith(".tga", StringComparison.OrdinalIgnoreCase)) return LoadTga(File.ReadAllBytes(path), out w, out h);
        using (var src = new Bitmap(path))
        using (var bmp = src.Clone(new Rectangle(0, 0, src.Width, src.Height), PixelFormat.Format32bppArgb))
        {
            w = bmp.Width; h = bmp.Height;
            var d = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            var rgba = new byte[w * h * 4];
            var row = new byte[w * 4];
            for (int y = 0; y < h; y++)
            {
                System.Runtime.InteropServices.Marshal.Copy(d.Scan0 + y * d.Stride, row, 0, row.Length);
                for (int x = 0; x < w; x++)
                {
                    int s = (y * w + x) * 4;
                    rgba[s] = row[x * 4 + 2]; rgba[s + 1] = row[x * 4 + 1]; rgba[s + 2] = row[x * 4]; rgba[s + 3] = row[x * 4 + 3];
                }
            }
            bmp.UnlockBits(d);
            return rgba;
        }
    }
}

public class PackRef
{
    public string Dir;        // inside the CPK, e.g. "2D" or "2D/MAP"
    public string VName, PName;
    public Pack2D Pack;
}

static class Program
{
    [System.Runtime.InteropServices.DllImport("user32.dll")] static extern bool SetProcessDPIAware();
    static string VExt = "2DV", PExt = "2DP";

    internal static Cpk[] OpenCpks(string media)
    {
        var l = new List<Cpk>();
        string iso = XIso.ImageAt(media);  // the game's .iso (or a folder with it): read inside it
        foreach (var n in new[] { "layer1.cpk", "layer2.cpk" })
        {
            long at, size;
            if (iso != null) { if (XIso.Find(iso, "media/" + n, out at, out size)) l.Add(Cpk.Open(iso, at, size, n)); continue; }
            string p = Path.Combine(media, n);
            if (File.Exists(p)) l.Add(Cpk.Open(p));
        }
        if (l.Count == 0) throw new Exception("nao achei layer1.cpk / layer2.cpk em " + media);
        return l.ToArray();
    }

    internal static IEnumerable<PackRef> Packs(Cpk[] cpks, string filter)
    {
        foreach (var c in cpks)
        {
            var byPath = c.Files.ToDictionary(f => f.Path, f => f, StringComparer.OrdinalIgnoreCase);
            foreach (var f in c.Files)
            {
                if (!f.Name.EndsWith("." + VExt, StringComparison.OrdinalIgnoreCase)) continue;
                string name = f.Name.Substring(0, f.Name.Length - 4);
                if (filter != null && (f.Dir + "/" + name).IndexOf(filter, StringComparison.OrdinalIgnoreCase) < 0) continue;
                CpkEntry p;
                if (!byPath.TryGetValue(f.Dir + "/" + name + "." + PExt, out p)) continue;
                Pack2D pack = null;
                try { pack = Pack2D.Load(name, c.Read(f), c.Read(p)); }
                catch (Exception e) { Console.WriteLine("  pulei " + name + ": " + e.Message); }
                if (pack != null) yield return new PackRef { Dir = f.Dir, VName = f.Name, PName = p.Name, Pack = pack };
            }
        }
    }

    internal static string Safe(string s) { foreach (char ch in Path.GetInvalidFileNameChars()) s = s.Replace(ch, '_'); return s; }


    // PNGs of <pngDir>\<dir>\<pack>\<texture>.png that differ from the game's ->
    // packs in <outDir>\<dir>\. Returns a summary line.
    internal static string Build(Cpk[] cpks, string pngDir, string outDir)
    {
        int packs = 0, changed = 0;
        foreach (var r in Packs(cpks, null))
        {
            string dir = Path.Combine(pngDir, r.Dir.Replace('/', Path.DirectorySeparatorChar), r.Pack.Name);
            if (!Directory.Exists(dir)) continue;
            int n = 0;
            foreach (var t in r.Pack.Textures.ToList())
            {
                string png = Path.Combine(dir, Safe(t.Name) + ".png");
                if (!t.Supported || !File.Exists(png)) continue;
                int w, h;
                var rgba = PngIo.Load(png, out w, out h);
                if (w == t.Width && h == t.Height)
                {
                    var orig = r.Pack.Decode(t);
                    bool same = true;
                    for (int i = 0; i < orig.Length && same; i += 4)
                        if (orig[i + 3] != rgba[i + 3] || (orig[i + 3] != 0 && (orig[i] != rgba[i] || orig[i + 1] != rgba[i + 1] || orig[i + 2] != rgba[i + 2]))) same = false;
                    if (same) continue;
                }
                r.Pack.Replace(t, rgba, w, h);
                n++;
            }
            if (n == 0) continue;
            string o = Path.Combine(outDir, r.Dir.Replace('/', Path.DirectorySeparatorChar));
            Directory.CreateDirectory(o);
            File.WriteAllBytes(Path.Combine(o, r.VName), r.Pack.Vir);
            File.WriteAllBytes(Path.Combine(o, r.PName), r.Pack.Pix);
            Console.WriteLine(r.Dir + "/" + r.Pack.Name + ": " + n + " textura(s) trocada(s)");
            packs++; changed += n;
        }
        return packs + " pacote(s), " + changed + " textura(s)";
    }

    // KPKy pack: "KPKy", count, offsets[count], sizes[count] (relative to the pack), entries 0x80-aligned.
    internal static byte[][] KpkItems(byte[] d, int at, int size)
    {
        if (size < 8 || at + 8 > d.Length || Encoding.ASCII.GetString(d, at, 4) != "KPKy") return null;
        int n = (int)Be.U32(d, at + 4);
        if (n <= 0 || n > 100000 || at + 8 + 8 * n > d.Length) return null;
        var res = new byte[n][];
        for (int i = 0; i < n; i++)
        {
            int off = (int)Be.U32(d, at + 8 + 4 * i), len = (int)Be.U32(d, at + 8 + 4 * n + 4 * i);
            if (off == 0 || len == 0) continue;
            if (off + len > size) return null;
            res[i] = new byte[len];
            Array.Copy(d, at + off, res[i], 0, len);
        }
        return res;
    }

    static byte[] Kpk(byte[][] items)
    {
        int n = items.Length, pos = (8 + 8 * n + 0x7F) & ~0x7F;
        var offs = new int[n];
        for (int i = 0; i < n; i++) if (items[i] != null) { offs[i] = pos; pos = (pos + items[i].Length + 0x7F) & ~0x7F; }
        var res = new byte[pos];
        Encoding.ASCII.GetBytes("KPKy").CopyTo(res, 0);
        Be.Put32(res, 4, (uint)n);
        for (int i = 0; i < n; i++)
        {
            if (items[i] == null) continue;
            Be.Put32(res, 8 + 4 * i, (uint)offs[i]); Be.Put32(res, 8 + 4 * n + 4 * i, (uint)items[i].Length);
            items[i].CopyTo(res, offs[i]);
        }
        return res;
    }

    // A model .MDV (unpacked) is a KPKy of 3: [1] the body: KPKy of 4 ([1] KPKy of MESH / NODT / HEAP,
    // [3] CCF), [2] a KPKy of the joint list (CJF) and the animations (KPKy of CMF). MESH holds the
    // polygons' side: strings (STRL), materials, and a BONE list (names + rest matrices) that the skinning
    // does not use: a vertex's bone numbers are places in the CJF (see BoneMap). NODT is the node tree.
    // Body swap: SOURCE's MESH and HEAP (polygons, textures; its .MDP goes with them, its vertices
    // renumbered by BodySwapMdp) on the TARGET's nodes, CCF, joints and animations. Null: not a model.
    // The MESH record of a model .MDV (unpacked), as BodySwapMdv finds it; null when not a model.
    static byte[] KpkMesh(byte[] mdv)
    {
        var top = KpkItems(mdv, 0, mdv.Length);
        if (top == null || top.Length != 3 || top[1] == null) return null;
        var l1 = KpkItems(top[1], 0, top[1].Length);
        if (l1 == null || l1.Length < 2 || l1[1] == null) return null;
        var l11 = KpkItems(l1[1], 0, l1[1].Length);
        if (l11 == null) return null;
        return l11.FirstOrDefault(x => x != null && x.Length >= 4 && Encoding.ASCII.GetString(x, 0, 4) == "MESH");
    }

    static byte[] BodySwapMdv(byte[] target, byte[] source)
    {
        var tTop = KpkItems(target, 0, target.Length); var sTop = KpkItems(source, 0, source.Length);
        if (tTop == null || sTop == null || tTop.Length != 3 || sTop.Length != 3 || tTop[1] == null || sTop[1] == null) return null;
        var t1 = KpkItems(tTop[1], 0, tTop[1].Length); var s1 = KpkItems(sTop[1], 0, sTop[1].Length);
        if (t1 == null || s1 == null || t1.Length < 2 || s1.Length != t1.Length || t1[1] == null || s1[1] == null) return null;
        var t11 = KpkItems(t1[1], 0, t1[1].Length); var s11 = KpkItems(s1[1], 0, s1[1].Length);
        if (t11 == null || s11 == null || t11.Length != s11.Length) return null;
        Func<byte[][], string, int> find = (items, tag) => Array.FindIndex(items, x => x != null && x.Length >= 4 && Encoding.ASCII.GetString(x, 0, 4) == tag);
        int mesh = find(s11, "MESH"), nodt = find(t11, "NODT");
        if (mesh < 0 || nodt < 0 || find(t11, "MESH") != mesh || find(s11, "NODT") != nodt) return null;
        var body = (byte[][])s11.Clone();
        // The SOURCE's BONE list names bones the TARGET's skeleton may not have (a face's chin, an
        // apron): the skinning does not use it, but the game looks its names up in the skeleton
        // (an NPC on the father crashed reading a name as a pointer). Those take the nearest bone's name.
        var tj = CjfJoints(target);
        if (tj != null) body[mesh] = RenameMissingBones(s11[mesh], new HashSet<string>(tj.Skip(1).Select(x => x.Item1), StringComparer.OrdinalIgnoreCase));
        body[nodt] = MixNodt(t11[nodt], s11[nodt]);
        if (body[nodt] == null) return null;
        var b1 = (byte[][])s1.Clone();
        // [3] CCF (cloth) works by bone number: the TARGET's one, which matches its skeleton.
        if (t1.Length > 3 && b1.Length > 3) b1[3] = t1[3];
        b1[1] = Kpk(body);
        var top = (byte[][])tTop.Clone();
        top[1] = Kpk(b1);
        return Kpk(top);
    }

    // NODT: REM, STRB (STRL strings), then NODE records [index, link, link, name, 4x4 matrix, count] in index
    // order. The last NODE is the polygons' node: INST > MESC [id, name] > CLUS [part, name, -1]..., then
    // GRPS [group, name, -1] > GRPC [part]... (parts by number, names in this NODT's strings). The result is
    // the TARGET's skeleton nodes with the SOURCE's polygons node, its names re-pointed to the TARGET's strings.
    static byte[] MixNodt(byte[] target, byte[] source)
    {
        Func<byte[], List<string>> strings = d =>
        {
            var res = new List<string>();
            for (int p = 8; p + 8 <= d.Length; p += 8 + (int)Be.U32(d, p + 4))
                if (Encoding.ASCII.GetString(d, p, 4) == "STRB" && Encoding.ASCII.GetString(d, p + 12, 4) == "STRL")
                    return StrlNames(d, p);
            return res;
        };
        Func<byte[], int> lastNode = d =>
        {
            int last = -1;
            for (int p = 8; p + 8 <= d.Length; p += 8 + (int)Be.U32(d, p + 4))
                if (Encoding.ASCII.GetString(d, p, 4) == "NODE") last = p;
            return last;
        };
        var tStr = strings(target); var sStr = strings(source);
        int tg = lastNode(target), sg = lastNode(source);
        if (tg < 0 || sg < 0 || tStr.Count == 0) return null;
        // The SOURCE's polygon nodes: the trailing NODEs that hold polygons (more than the 84-byte node). An NPC can
        // have several (SCW010: SET_C, SET_B, SET_A, siblings; only the last came along and the body was one loose
        // piece, 2026-11-17): all of them, numbered from the TARGET's polygons node on, each the next one's sibling.
        var geomNodes = new List<int>();
        for (int p = 8; p + 8 <= source.Length; p += 8 + (int)Be.U32(source, p + 4))
        {
            if (Encoding.ASCII.GetString(source, p, 4) != "NODE") continue;
            if (Be.U32(source, p + 4) > 84) geomNodes.Add(p); else geomNodes.Clear();
        }
        // EXPERIMENT (2026-11-17, not working yet: SCW010 drew nothing with all three sets, merged or not, groups kept or
        // not): only with NT_NPCSETS=1; otherwise the last polygons node alone, as before.
        bool npcSets = Environment.GetEnvironmentVariable("NT_NPCSETS") != null;
        if (geomNodes.Count == 0 || !npcSets) { geomNodes.Clear(); geomNodes.Add(sg); }
        sg = geomNodes[0];
        var geom = new byte[source.Length - sg];
        Array.Copy(source, sg, geom, 0, geom.Length);
        uint firstIndex = Be.U32(target, tg + 8);
        var nodeAt = geomNodes.Select(p => p - sg).ToList();
        bool ok = true;
        var extra = new List<string>();  // names the TARGET's list lacks: added at its end
        // Part groups by another name (TEST, 2026-11-17: an NPC's PARTS_GROUP100..300 are not the father's, whose code
        // shows his own groups only): env NT_GROUPMAP="PARTS_GROUP300=PARTS_GROUP000,..."; "*" = every group.
        var groupMap = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var kv in (Environment.GetEnvironmentVariable("NT_GROUPMAP") ?? "").Split(new[] { ',' }, StringSplitOptions.RemoveEmptyEntries))
        { var pair = kv.Split('='); if (pair.Length == 2) groupMap[pair[0].Trim()] = pair[1].Trim(); }
        Action<int, bool> renameAs = (at, group) =>
        {
            int s = (int)Be.U32(geom, at);
            if (s >= sStr.Count) { Console.WriteLine("   nome fora da lista: #" + s); ok = false; return; }
            string name = sStr[s], mapped;
            if (group && (groupMap.TryGetValue(name, out mapped) || groupMap.TryGetValue("*", out mapped))) { Console.WriteLine("   grupo " + name + " -> " + mapped); name = mapped; }
            int t = tStr.IndexOf(name);
            if (t < 0) { t = extra.IndexOf(name); if (t < 0) { extra.Add(name); t = extra.Count - 1; } t += tStr.Count; }
            Be.Put32(geom, at, (uint)t);
        };
        Action<int> rename = at => renameAs(at, false);
        Action<int, int> walk = null;
        walk = (p, end) =>
        {
            while (p + 8 <= end)
            {
                string tag = Encoding.ASCII.GetString(geom, p, 4); int size = (int)Be.U32(geom, p + 4);
                if (tag == "INST") walk(p + 8, p + 8 + size);
                else if (tag == "MESC") { rename(p + 12); walk(p + 16, p + 8 + size); }
                else if (tag == "CLUS") rename(p + 12);
                else if (tag == "GRPS") renameAs(p + 12, true);
                p += 8 + size;
            }
        };
        for (int k = 0; k < nodeAt.Count; k++)
        {
            int at = nodeAt[k], end = k + 1 < nodeAt.Count ? nodeAt[k + 1] : geom.Length;
            Be.Put32(geom, at + 8, firstIndex + (uint)k);
            Be.Put32(geom, at + 12, k + 1 < nodeAt.Count ? firstIndex + (uint)k + 1 : 0xFFFFFFFFu);  // next sibling
            if (nodeAt.Count == 1) Be.Put32(geom, at + 20, Be.U32(target, tg + 20));  // the TARGET's node name
            else rename(at + 20);
            walk(at + 8 + 0x54, end);
        }
        if (nodeAt.Count > 1) Console.WriteLine("   " + nodeAt.Count + " nos de poligonos da origem");
        if (!ok) return null;
        // Part groups the TARGET does not know left out (2026-11-17): the father's code shows only his own groups
        // (PARTS_GROUP000, 001, ...), so an NPC's parts in its groups (100, 200, 300) were never drawn; a model without
        // groups (Yonah) has all its parts drawn. A group renamed to a TARGET's name stays. Sizes are rebuilt.
        int dropped = 0;
        Func<int, int, List<byte>> rebuild = null;
        rebuild = (p, end) =>
        {
            var outb = new List<byte>();
            while (p + 8 <= end)
            {
                string tag = Encoding.ASCII.GetString(geom, p, 4); int size = (int)Be.U32(geom, p + 4);
                int head = tag == "INST" ? 8 : tag == "MESC" ? 16 : -1;
                if (npcSets && tag == "GRPS" && Be.U32(geom, p + 12) >= (uint)tStr.Count && Environment.GetEnvironmentVariable("NT_KEEPGROUPS") == null) { dropped++; p += 8 + size; continue; }
                if (head < 0) { for (int i = p; i < p + 8 + size; i++) outb.Add(geom[i]); }
                else
                {
                    var inner = rebuild(p + head, p + 8 + size);
                    var rec = new byte[head + inner.Count];
                    Array.Copy(geom, p, rec, 0, head);
                    Be.Put32(rec, 4, (uint)(head - 8 + inner.Count));
                    outb.AddRange(rec);  // the header, then its children
                    outb.AddRange(inner);
                }
                p += 8 + size;
            }
            return outb;
        };
        {
            // One polygons node, as the TARGET has (several drew nothing, 2026-11-17): the first node's header (the
            // TARGET's index and name, no sibling) over one INST holding every node's MESC records.
            const int nodeHead = 8 + 0x54;
            var mescs = new List<byte>();
            for (int k = 0; k < nodeAt.Count; k++)
            {
                int at = nodeAt[k], end = k + 1 < nodeAt.Count ? nodeAt[k + 1] : geom.Length;
                var inner = rebuild(at + nodeHead, end);  // INST records
                for (int q = 0; q + 8 <= inner.Count; q += 8 + (int)Be.U32(inner.ToArray(), q + 4))
                {
                    var arr = inner.ToArray();
                    int size = (int)Be.U32(arr, q + 4);
                    if (Encoding.ASCII.GetString(arr, q, 4) == "INST") for (int i = q + 8; i < q + 8 + size; i++) mescs.Add(arr[i]);
                    else for (int i = q; i < q + 8 + size; i++) mescs.Add(arr[i]);
                }
            }
            var node = new byte[nodeHead];
            Array.Copy(geom, nodeAt[0], node, 0, nodeHead);
            Be.Put32(node, 8, firstIndex);
            Be.Put32(node, 12, 0xFFFFFFFFu);
            Be.Put32(node, 20, Be.U32(target, tg + 20));
            var inst = new byte[8];
            Encoding.ASCII.GetBytes("INST").CopyTo(inst, 0);
            Be.Put32(inst, 4, (uint)mescs.Count);
            Be.Put32(node, 4, (uint)(nodeHead - 8 + 8 + mescs.Count));
            var newGeom = new List<byte>(node);
            newGeom.AddRange(inst);
            newGeom.AddRange(mescs);
            if (dropped > 0) Console.WriteLine("   " + dropped + " grupo(s) de pecas que o alvo nao conhece removidos (pecas sempre desenhadas)");
            if (dropped > 0 || nodeAt.Count > 1) geom = newGeom.ToArray();
        }
        if (extra.Count > 0) { target = AppendStrings(target, extra); tg = lastNode(target); }
        var mixed = new byte[tg + geom.Length];
        Array.Copy(target, 0, mixed, 0, tg);
        geom.CopyTo(mixed, tg);
        Be.Put32(mixed, 4, (uint)(mixed.Length - 8));
        return mixed;
    }

    // A record list (MESH, NODT: [tag][size] then records) with names added at the end of its string list
    // (STRB [count] > STRL [bytes] strings, NUL-padded to 4 bytes); their indices follow the old ones.
    // The count (STRB +8) is the game's bound for a name index: it must grow with the list (it did not,
    // and the NPC-on-the-father body swap read a name past the end: the crash at 0x5F436869).
    static byte[] AppendStrings(byte[] m, List<string> add)
    {
        for (int p = 8; p + 8 <= m.Length; p += 8 + (int)Be.U32(m, p + 4))
        {
            if (Encoding.ASCII.GetString(m, p, 4) != "STRB" || Encoding.ASCII.GetString(m, p + 12, 4) != "STRL") continue;
            int size = (int)Be.U32(m, p + 4), strl = (int)Be.U32(m, p + 16), start = p + 20, dataEnd = start + strl;
            int count = (int)Be.U32(m, p + 8);
            // The end of the count-th string: the padding after it is dropped, the new names go there.
            int end = start, seen = 0;
            while (end < dataEnd && seen < count) { if (m[end] == 0) seen++; end++; }
            if (seen < count) { Console.WriteLine("   lista de nomes: contagem " + count + " > " + seen); return m; }
            var bytes = new List<byte>();
            for (int s = start; s < end; s++) bytes.Add(m[s]);
            foreach (var s in add) { bytes.AddRange(Encoding.ASCII.GetBytes(s)); bytes.Add(0); }
            while ((bytes.Count & 3) != 0) bytes.Add(0);
            int grow = bytes.Count - strl;
            var res = new byte[m.Length + grow];
            Array.Copy(m, 0, res, 0, start);
            bytes.CopyTo(res, start);
            Array.Copy(m, dataEnd, res, start + bytes.Count, m.Length - dataEnd);
            Be.Put32(res, p + 16, (uint)bytes.Count);
            Be.Put32(res, p + 4, (uint)(size + grow));
            Be.Put32(res, p + 8, (uint)(count + add.Count));
            Be.Put32(res, 4, (uint)(res.Length - 8));
            return res;
        }
        return m;
    }

    // The names of a STRB > STRL list: its count of them (the padding after the last is not one).
    static List<string> StrlNames(byte[] d, int p)
    {
        var res = new List<string>();
        int count = (int)Be.U32(d, p + 8), s = p + 20, e = s + (int)Be.U32(d, p + 16);
        while (s < e && res.Count < count) { int q = s; while (q < e && d[q] != 0) q++; res.Add(Encoding.ASCII.GetString(d, s, q - s)); s = q + 1; }
        return res;
    }

    // A MESH whose BONE names the skeleton (known) lacks now name the nearest bone it has (names
    // added at the end of its string list when it has none of them yet).
    static byte[] RenameMissingBones(byte[] m, HashSet<string> known)
    {
        var strings = MeshStrings(m);
        var wanted = new List<string>();
        foreach (var b in MeshBones(m))
        {
            if (b.Item2 == "" || known.Contains(b.Item2)) continue;
            string near = Nearest(b.Item2, known);
            wanted.Add(near);
        }
        if (wanted.Count == 0) return m;
        var extra = wanted.Distinct(StringComparer.OrdinalIgnoreCase)
            .Where(w => !strings.Any(s => string.Equals(s, w, StringComparison.OrdinalIgnoreCase))).ToList();
        if (extra.Count > 0) { m = AppendStrings(m, extra); strings = MeshStrings(m); }
        int renamed = 0;
        foreach (var b in MeshBones(m))
        {
            if (b.Item2 == "" || known.Contains(b.Item2)) continue;
            string near = Nearest(b.Item2, known);
            int idx = strings.FindIndex(s => string.Equals(s, near, StringComparison.OrdinalIgnoreCase));
            if (idx < 0) continue;
            Be.Put32(m, b.Item1, (uint)idx);
            renamed++;
        }
        Console.WriteLine("   ossos sem par no esqueleto renomeados para o mais proximo: " + renamed);
        return m;
    }

    // MESH record walk: (STRL strings, list of (position of the BOIF name field, bone name)).
    static List<string> MeshStrings(byte[] m)
    {
        var res = new List<string>();
        for (int p = 8; p + 8 <= m.Length; )
        {
            string tag = Encoding.ASCII.GetString(m, p, 4).TrimEnd('\0'); int size = (int)Be.U32(m, p + 4);
            if (tag == "STRB" && Encoding.ASCII.GetString(m, p + 12, 4) == "STRL") return StrlNames(m, p);
            p += 8 + size;
        }
        return res;
    }

    static List<Tuple<int, string>> MeshBones(byte[] m)
    {
        var strings = MeshStrings(m); var res = new List<Tuple<int, string>>();
        for (int p = 8; p + 8 <= m.Length; )
        {
            string tag = Encoding.ASCII.GetString(m, p, 4).TrimEnd('\0'); int size = (int)Be.U32(m, p + 4);
            if (tag == "BONE")
            {
                for (int q = p + 12; q + 16 <= p + 8 + size && Encoding.ASCII.GetString(m, q, 4) == "BOIF"; q += 8 + (int)Be.U32(m, q + 4))
                {
                    int s = (int)Be.U32(m, q + 8);
                    res.Add(Tuple.Create(q + 8, s < strings.Count ? strings[s] : ""));
                }
                break;
            }
            p += 8 + size;
        }
        return res;
    }

    // A bone the TARGET's skeleton lacks (hair, apron, collar...) follows the closest one it has.
    static string Nearest(string name, HashSet<string> known)
    {
        if (known.Contains(name)) return name;
        string n = name.ToLowerInvariant();
        string[] tries = n.Contains("epron") || n.Contains("apron") || n.Contains("skirt") ? new[] { "HipsBone", "Hips" }
            : n.Contains("hair") || n.Contains("meziri") || n.Contains("eye") || n.Contains("kuti") || n.Contains("bone")
              || n.Contains("chin") || n.Contains("jaw") || n.Contains("face") || n.Contains("mouth") || n.Contains("nose")
              || n.Contains("cheek") || n.Contains("brow") || n.Contains("lip") || n.Contains("tongue") || n.Contains("teeth") ? new[] { "Head", "Neck" }
            : n.Contains("bust") || n.Contains("breast") || n.Contains("chest") ? new[] { "Spine2", "Spine1", "Spine" }
            : n.Contains("weapon") && n.Contains("left") ? new[] { "LeftHand", "LeftForeArm", "LeftArm" }
            : n.Contains("weapon") && n.Contains("right") ? new[] { "RightHand", "RightForeArm", "RightArm" }
            : n.Contains("collar") || n.Contains("back") || n.Contains("acce") ? new[] { "Spine2", "Spine1", "Spine" }
            : n.StartsWith("left") && (n.Contains("arm") || n.Contains("hand")) ? new[] { "LeftArm", "LeftShoulder" }
            : n.StartsWith("right") && (n.Contains("arm") || n.Contains("hand")) ? new[] { "RightArm", "RightShoulder" }
            : n.Contains("left") && (n.Contains("leg") || n.Contains("pants")) ? new[] { "LeftUpLeg" }
            : n.Contains("right") && (n.Contains("leg") || n.Contains("pants")) ? new[] { "RightUpLeg" }
            : new[] { "HipsBone", "Hips" };
        foreach (var t in tries.Concat(new[] { "HipsBone", "Hips" }))
            if (known.Contains(t)) return known.First(k => string.Equals(k, t, StringComparison.OrdinalIgnoreCase));
        return known.First();
    }

    static float BeF(byte[] d, int o) { return BitConverter.ToSingle(new[] { d[o + 3], d[o + 2], d[o + 1], d[o] }, 0); }
    static void PutF(byte[] d, int o, float v) { var b = BitConverter.GetBytes(v); d[o] = b[3]; d[o + 1] = b[2]; d[o + 2] = b[1]; d[o + 3] = b[0]; }

    // The joint list of a model (.MDV top[2] > CJF): the skeleton the animations move, in joint order.
    // Header: "CJF\1", name, then [count][?][names][sizes][parents][bind][?][?]. Names: 36 bytes each
    // (hash + 32 chars, upper case); bind: the joint's rest pose in model space, 4x4, translation in row 4.
    static List<Tuple<string, double[]>> CjfJoints(byte[] mdv)
    {
        var top = KpkItems(mdv, 0, mdv.Length);
        if (top == null || top.Length < 3 || top[2] == null) return null;
        var l2 = KpkItems(top[2], 0, top[2].Length);
        var cjf = l2 == null ? null : l2.FirstOrDefault(x => x != null && x.Length > 0x40 && x[0] == 'C' && x[1] == 'J' && x[2] == 'F');
        if (cjf == null) return null;
        int n = (int)Be.U32(cjf, 0x24), names = (int)Be.U32(cjf, 0x2C), bind = (int)Be.U32(cjf, 0x38);
        var res = new List<Tuple<string, double[]>>();
        for (int i = 0; i < n; i++)
        {
            int a = names + i * 36 + 4, e = a; while (e < a + 32 && cjf[e] != 0) e++;
            int m = bind + i * 64;
            res.Add(Tuple.Create(Encoding.ASCII.GetString(cjf, a, e - a), new double[] { BeF(cjf, m + 48), BeF(cjf, m + 52), BeF(cjf, m + 56) }));
        }
        return res;
    }

    // The game skins each vertex by its bone number = the joint's place in the model's joint list (CJF)
    // minus one (joint 0 is the root, N_ALL); the MESH's BONE list is not what it uses. Body swap: each
    // SOURCE joint -> the place of the same joint (or the nearest one) in the TARGET's list, and how far
    // that joint sits from where it is in the SOURCE (so the polygons take the TARGET's proportions).
    internal static bool DualWield(IEnumerable<string> source, IEnumerable<string> target)
    {
        var s = new HashSet<string>(source, StringComparer.OrdinalIgnoreCase); var t = new HashSet<string>(target, StringComparer.OrdinalIgnoreCase);
        return s.Contains("LEFTWEAPON") && s.Contains("RIGHTWEAPON") && t.Contains("RIGHTWEAPON") && !t.Contains("LEFTWEAPON");
    }

    static int[] BoneMap(byte[] sMdv, byte[] tMdv, out double[][] shift)
    {
        shift = null;
        var s = CjfJoints(sMdv); var t = CjfJoints(tMdv);
        if (s == null || t == null) { var tp = KpkItems(sMdv, 0, sMdv.Length); Console.WriteLine("   sem lista de juntas (CJF) " + (s == null) + " " + (t == null) + " top " + (tp == null ? -1 : tp.Length) + " len " + sMdv.Length + " " + Encoding.ASCII.GetString(sMdv, 0, 4)); return null; }
        var known = new HashSet<string>(t.Skip(1).Select(x => x.Item1), StringComparer.OrdinalIgnoreCase);
        // Two swords (Kaine) on a one-sword skeleton with the SOURCE's own animations: both blades held by
        // the hands; the TARGET's RIGHTWEAPON is left to the game's hits, following the striking blade
        // (Anim.Convert, DualWield).
        if (DualWield(s.Select(x => x.Item1), t.Select(x => x.Item1)) && Environment.GetEnvironmentVariable("NT_KEEP_ANIMS") == null) known.Remove("RIGHTWEAPON");
        var tAt = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        var sAt = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        for (int j = 1; j < t.Count; j++) if (!tAt.ContainsKey(t[j].Item1)) tAt[t[j].Item1] = j;
        for (int i = 1; i < s.Count; i++) if (!sAt.ContainsKey(s[i].Item1)) sAt[s[i].Item1] = i;
        var map = new int[s.Count - 1]; shift = new double[s.Count - 1][];
        int same = 0;
        for (int i = 1; i < s.Count; i++)
        {
            string n = Nearest(s[i].Item1, known);
            if (string.Equals(n, s[i].Item1, StringComparison.OrdinalIgnoreCase)) same++;
            else Console.WriteLine("   junta " + s[i].Item1 + " -> " + n);
            int j = tAt[n], si;
            if (!sAt.TryGetValue(n, out si)) si = i;
            map[i - 1] = j - 1;
            var pt = t[j].Item2; var ps = s[si].Item2;
            shift[i - 1] = new[] { pt[0] - ps[0], pt[1] - ps[1], pt[2] - ps[2] };
            if (Environment.GetEnvironmentVariable("NT_JOINTS") != null) Console.WriteLine(string.Format(System.Globalization.CultureInfo.InvariantCulture, "   {0} -> {1}: {2:F3} {3:F3} {4:F3} -> {5:F3} {6:F3} {7:F3}", s[i].Item1, n, ps[0], ps[1], ps[2], pt[0], pt[1], pt[2]));
        }
        // The face moves as one with the head: its joints (eyes, jaw, mouth...) sit where the SOURCE's
        // face has them, and moving each to the TARGET's face stretched it (Kaine on the father).
        var parents = CjfParents(sMdv);
        int head;
        if (parents != null && sAt.TryGetValue("Head", out head))
        {
            int moved = 0;
            for (int i = 1; i < s.Count; i++)
            {
                if (i == head) continue;
                int p = i, guard = 0;
                while (p > 0 && p < parents.Length && p != head && guard++ < 64) p = parents[p];
                if (p != head) continue;
                shift[i - 1] = shift[head - 1];
                moved++;
            }
            Console.WriteLine("   rosto: " + moved + " juntas seguem a cabeca inteira");
        }
        return map;
    }

    // CJF blocks: @0x30 one float per joint (its bone length), @0x38 rest pose in model space (4x4, rows,
    // translation in row 4, row vectors: world = local * parent), @0x3C its inverse, @0x40 48 bytes per
    // joint: the local rest pose (quaternion x y z w, translation x y z, 0, scale x y z, 0). The
    // animations (CMF) are rotation tracks only: a joint's offset from its parent is the local
    // translation of the CJF. So the TARGET's skeleton with the SOURCE's bone lengths moves with the
    // TARGET's animations in the SOURCE's proportions (Kaine on the father stays Kaine-sized).
    // The model's skeleton with some joints moved (model space, by name); every other joint keeps its offset from its
    // parent, so it follows. The rest pose, its inverse, the local values and the bone lengths are rewritten
    // (Outfit: the father's skeleton with an outside model's own proportions).
    internal static byte[] SetRestPositions(byte[] mdv, Dictionary<string, float[]> place)
    {
        var tp = KpkItems(mdv, 0, mdv.Length);
        var l = KpkItems(tp[2], 0, tp[2].Length);
        int at = Array.FindIndex(l, x => x != null && x.Length > 0x44 && x[0] == 'C' && x[1] == 'J' && x[2] == 'F');
        var c = (byte[])l[at].Clone();
        int n = (int)Be.U32(c, 0x24), lens = (int)Be.U32(c, 0x30), par = (int)Be.U32(c, 0x34), bind = (int)Be.U32(c, 0x38), inv = (int)Be.U32(c, 0x3C), loc = (int)Be.U32(c, 0x40);
        Func<int, string> name = j => { int a = (int)Be.U32(c, 0x2C) + j * 36 + 4, e = a; while (e < a + 32 && c[e] != 0) e++; return Encoding.ASCII.GetString(c, a, e - a); };
        Func<int, int> parentOf = j => { int v = (c[par + 2 * j] << 8) | c[par + 2 * j + 1]; return v == 0xFFFF || v == j || v >= n ? -1 : v; };
        var world = new double[n][];
        // World = local * parent (rows); the parent's rows are its axes, orthonormal up to scale.
        Func<int, double[]> get = null;
        get = j =>
        {
            if (world[j] != null) return world[j];
            int p = parentOf(j);
            int o = loc + j * 48;
            double[] pw = p < 0 ? null : get(p);
            float[] want;
            if (pw != null && place.TryGetValue(name(j), out want))
            {
                // local translation = (wanted - parent's place) in the parent's axes
                double dx = want[0] - pw[12], dy = want[1] - pw[13], dz = want[2] - pw[14];
                var t = new double[3];
                for (int a = 0; a < 3; a++)
                {
                    double s2 = pw[a * 4] * pw[a * 4] + pw[a * 4 + 1] * pw[a * 4 + 1] + pw[a * 4 + 2] * pw[a * 4 + 2];
                    t[a] = (dx * pw[a * 4] + dy * pw[a * 4 + 1] + dz * pw[a * 4 + 2]) / (s2 > 1e-12 ? s2 : 1);
                }
                double old = Math.Sqrt(Math.Pow(BeF(c, o + 16), 2) + Math.Pow(BeF(c, o + 20), 2) + Math.Pow(BeF(c, o + 24), 2));
                PutF(c, o + 16, (float)t[0]); PutF(c, o + 20, (float)t[1]); PutF(c, o + 24, (float)t[2]);
                if (lens > 0 && lens + 4 * j + 4 <= c.Length && Math.Abs(BeF(c, lens + 4 * j) - old) < 1e-3) PutF(c, lens + 4 * j, (float)Math.Sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]));
            }
            double x = BeF(c, o), y = BeF(c, o + 4), z = BeF(c, o + 8), w = BeF(c, o + 12);
            double sx = BeF(c, o + 32), sy = BeF(c, o + 36), sz = BeF(c, o + 40);
            var lm = new[] {
                (1 - 2 * (y * y + z * z)) * sx, (2 * (x * y + z * w)) * sx, (2 * (x * z - y * w)) * sx, 0,
                (2 * (x * y - z * w)) * sy, (1 - 2 * (x * x + z * z)) * sy, (2 * (y * z + x * w)) * sy, 0,
                (2 * (x * z + y * w)) * sz, (2 * (y * z - x * w)) * sz, (1 - 2 * (x * x + y * y)) * sz, 0,
                BeF(c, o + 16), BeF(c, o + 20), BeF(c, o + 24), 1 };
            if (pw == null) { world[j] = lm; return lm; }
            var r = new double[16];
            for (int i = 0; i < 4; i++) for (int k = 0; k < 4; k++) { double s = 0; for (int m = 0; m < 4; m++) s += lm[i * 4 + m] * pw[m * 4 + k]; r[i * 4 + k] = s; }
            world[j] = r; return r;
        };
        for (int j = 0; j < n; j++) get(j);
        for (int j = 0; j < n; j++)
        {
            var m = world[j];
            for (int k = 0; k < 16; k++) PutF(c, bind + j * 64 + k * 4, (float)m[k]);
            var r = new double[16]; var s2 = new double[3];
            for (int a = 0; a < 3; a++) s2[a] = m[a * 4] * m[a * 4] + m[a * 4 + 1] * m[a * 4 + 1] + m[a * 4 + 2] * m[a * 4 + 2];
            for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) r[b * 4 + a] = m[a * 4 + b] / (s2[a] > 1e-12 ? s2[a] : 1);
            for (int b = 0; b < 3; b++) r[12 + b] = -(m[12] * r[b] + m[13] * r[4 + b] + m[14] * r[8 + b]);
            r[15] = 1;
            for (int k = 0; k < 16; k++) PutF(c, inv + j * 64 + k * 4, (float)r[k]);
        }
        l[at] = c;
        tp[2] = Kpk(l);
        return Kpk(tp);
    }

    static byte[] RetargetCjf(byte[] target, byte[] source)
    {
        Func<byte[], byte[]> cjfOf = mdv =>
        {
            var tp = KpkItems(mdv, 0, mdv.Length);
            if (tp == null || tp.Length < 3 || tp[2] == null) return null;
            var l = KpkItems(tp[2], 0, tp[2].Length);
            return l == null ? null : l.FirstOrDefault(x => x != null && x.Length > 0x44 && x[0] == 'C' && x[1] == 'J' && x[2] == 'F');
        };
        var tc = cjfOf(target); var sc = cjfOf(source);
        if (tc == null || sc == null) return null;
        Func<byte[], int, string> name = (c, j) => { int a = (int)Be.U32(c, 0x2C) + j * 36 + 4, e = a; while (e < a + 32 && c[e] != 0) e++; return Encoding.ASCII.GetString(c, a, e - a); };
        int tn = (int)Be.U32(tc, 0x24), sn = (int)Be.U32(sc, 0x24);
        var sAt = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        for (int i = 0; i < sn; i++) if (!sAt.ContainsKey(name(sc, i))) sAt[name(sc, i)] = i;
        var c2 = (byte[])tc.Clone();
        int lens = (int)Be.U32(c2, 0x30), par = (int)Be.U32(c2, 0x34), bind = (int)Be.U32(c2, 0x38), inv = (int)Be.U32(c2, 0x3C), loc = (int)Be.U32(c2, 0x40);
        int sLoc = (int)Be.U32(sc, 0x40);
        // Rebuilding the TARGET's own rest pose from its locals first checks the conventions above.
        var world = new double[tn][];
        Func<int, double[]> local = j =>
        {
            int o = loc + j * 48;
            double x = BeF(c2, o), y = BeF(c2, o + 4), z = BeF(c2, o + 8), w = BeF(c2, o + 12);
            double tx = BeF(c2, o + 16), ty = BeF(c2, o + 20), tz = BeF(c2, o + 24);
            double sx = BeF(c2, o + 32), sy = BeF(c2, o + 36), sz = BeF(c2, o + 40);
            // Rows = the joint's axes in its parent's space (the transpose of the column-vector matrix).
            return new[] {
                (1 - 2 * (y * y + z * z)) * sx, (2 * (x * y + z * w)) * sx, (2 * (x * z - y * w)) * sx, 0,
                (2 * (x * y - z * w)) * sy, (1 - 2 * (x * x + z * z)) * sy, (2 * (y * z + x * w)) * sy, 0,
                (2 * (x * z + y * w)) * sz, (2 * (y * z - x * w)) * sz, (1 - 2 * (x * x + y * y)) * sz, 0,
                tx, ty, tz, 1 };
        };
        Func<double[], double[], double[]> mul = (a, b) =>
        {
            var r = new double[16];
            for (int i = 0; i < 4; i++) for (int k = 0; k < 4; k++) { double s = 0; for (int m = 0; m < 4; m++) s += a[i * 4 + m] * b[m * 4 + k]; r[i * 4 + k] = s; }
            return r;
        };
        Func<int, int> parentOf = j => { int v = (c2[par + 2 * j] << 8) | c2[par + 2 * j + 1]; return v == 0xFFFF ? -1 : v; };
        Action solve = () =>
        {
            var done = new bool[tn];
            Func<int, double[]> get = null;
            get = j =>
            {
                if (done[j]) return world[j];
                int p = parentOf(j);
                world[j] = p < 0 || p >= tn || p == j ? local(j) : mul(local(j), get(p));
                done[j] = true;
                return world[j];
            };
            for (int j = 0; j < tn; j++) get(j);
        };
        solve();
        double err = 0;
        for (int j = 0; j < tn; j++) for (int k = 0; k < 16; k++) err = Math.Max(err, Math.Abs(world[j][k] - BeF(c2, bind + j * 64 + k * 4)));
        Console.WriteLine("   esqueleto: pose de repouso refeita das locais, erro " + err.ToString("F4"));
        if (err > 0.01) { Console.WriteLine("   convencao diferente: esqueleto do alvo mantido"); return null; }
        // The SOURCE's bone lengths, along the TARGET's bone directions.
        int changed = 0;
        for (int j = 1; j < tn; j++)
        {
            int si;
            if (!sAt.TryGetValue(name(c2, j), out si)) continue;
            int o = loc + j * 48, so = sLoc + si * 48;
            double tx = BeF(c2, o + 16), ty = BeF(c2, o + 20), tz = BeF(c2, o + 24);
            double sx = BeF(sc, so + 16), sy = BeF(sc, so + 20), sz = BeF(sc, so + 24);
            double tl = Math.Sqrt(tx * tx + ty * ty + tz * tz), sl = Math.Sqrt(sx * sx + sy * sy + sz * sz);
            // NT_SWAP_EXACT (Outfit: a model from outside the game, whose skeleton is the target's with the model's own
            // joint places): the whole offset, not only its length along the target's direction (2B kept the
            // father's shoulder and hip width).
            if (Environment.GetEnvironmentVariable("NT_SWAP_EXACT") != null)
            {
                PutF(c2, o + 16, (float)sx); PutF(c2, o + 20, (float)sy); PutF(c2, o + 24, (float)sz);
                if (lens > 0 && lens + 4 * j + 4 <= c2.Length && Math.Abs(BeF(c2, lens + 4 * j) - tl) < 1e-3) PutF(c2, lens + 4 * j, (float)sl);
                changed++;
                continue;
            }
            if (tl < 1e-6) continue;
            double f = sl / tl;
            PutF(c2, o + 16, (float)(tx * f)); PutF(c2, o + 20, (float)(ty * f)); PutF(c2, o + 24, (float)(tz * f));
            if (lens > 0 && lens + 4 * j + 4 <= c2.Length && Math.Abs(BeF(c2, lens + 4 * j) - tl) < 1e-3) PutF(c2, lens + 4 * j, (float)sl);
            changed++;
        }
        solve();
        for (int j = 0; j < tn; j++)
        {
            var m = world[j];
            for (int k = 0; k < 16; k++) PutF(c2, bind + j * 64 + k * 4, (float)m[k]);
            // Inverse of a rotation (+ uniform-free scale) and translation: rows are axes.
            var r = new double[16];
            double[] s2 = new double[3];
            for (int a = 0; a < 3; a++) s2[a] = m[a * 4] * m[a * 4] + m[a * 4 + 1] * m[a * 4 + 1] + m[a * 4 + 2] * m[a * 4 + 2];
            for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) r[b * 4 + a] = m[a * 4 + b] / (s2[a] > 1e-12 ? s2[a] : 1);
            for (int b = 0; b < 3; b++) r[12 + b] = -(m[12] * r[b] + m[13] * r[4 + b] + m[14] * r[8 + b]);
            r[15] = 1;
            for (int k = 0; k < 16; k++) PutF(c2, inv + j * 64 + k * 4, (float)r[k]);
        }
        Console.WriteLine("   esqueleto: " + changed + " ossos com o comprimento da origem");
        var top = KpkItems(target, 0, target.Length);
        var l2 = KpkItems(top[2], 0, top[2].Length);
        int at = Array.FindIndex(l2, x => x != null && x.Length > 0x44 && x[0] == 'C' && x[1] == 'J' && x[2] == 'F');
        l2[at] = c2;
        top[2] = Kpk(l2);
        return Kpk(top);
    }

    // A map's collision: the CGOB records of its .MDV (unpacked; BG/*.MDV top[1] > [2]). CGOB: @0x18 box
    // (min, max), @0x3C cell count, @0x40 the cells' offsets (u32, from the record's start). A cell: @0x00
    // its size (0x68), box, sphere, ...; @0x54 vertices (3 floats each), @0x58 triangles (u16 a, b, c and
    // a u16 mark), @0x5C vertex count (u16), @0x5E triangle count (u16). Positions in the map's space.
    // Returns 9 floats per triangle; null when the file has none.
    internal static float[] CollisionTris(byte[] mdv)
    {
        var res = new List<float>();
        Action<byte[]> walk = null;
        walk = d =>
        {
            if (d == null || d.Length < 8) return;
            string tag = Encoding.ASCII.GetString(d, 0, 4);
            if (tag == "KPKy") { var items = KpkItems(d, 0, d.Length); if (items != null) foreach (var x in items) walk(x); return; }
            if (tag != "CGOB" || d.Length < 0x44) return;
            int cells = (int)Be.U32(d, 0x3C), table = (int)Be.U32(d, 0x40);
            for (int c = 0; c < cells && table + 4 * c + 4 <= d.Length; c++)
            {
                int at = (int)Be.U32(d, table + 4 * c);
                if (at <= 0 || at + 0x60 > d.Length) continue;
                int vo = (int)Be.U32(d, at + 0x54), fo = (int)Be.U32(d, at + 0x58);
                int nv = (d[at + 0x5C] << 8) | d[at + 0x5D], nf = (d[at + 0x5E] << 8) | d[at + 0x5F];
                if (vo <= 0 || fo <= 0 || vo + nv * 12 > d.Length || fo + nf * 8 > d.Length) continue;
                for (int f = 0; f < nf; f++)
                {
                    int q = fo + f * 8;
                    for (int k = 0; k < 3; k++)
                    {
                        int v = (d[q + 2 * k] << 8) | d[q + 2 * k + 1];
                        if (v >= nv) v = 0;
                        res.Add(BeF(d, vo + v * 12)); res.Add(BeF(d, vo + v * 12 + 4)); res.Add(BeF(d, vo + v * 12 + 8));
                    }
                }
            }
        };
        walk(mdv);
        return res.Count == 0 ? null : res.ToArray();
    }

    // Each joint's parent in the joint list (CJF u32 @0x34 -> u16 per joint; 0xFFFF = none -> -1).
    static int[] CjfParents(byte[] mdv)
    {
        var top = KpkItems(mdv, 0, mdv.Length);
        if (top == null || top.Length < 3 || top[2] == null) return null;
        var l2 = KpkItems(top[2], 0, top[2].Length);
        var cjf = l2 == null ? null : l2.FirstOrDefault(x => x != null && x.Length > 0x40 && x[0] == 'C' && x[1] == 'J' && x[2] == 'F');
        if (cjf == null) return null;
        int n = (int)Be.U32(cjf, 0x24), at = (int)Be.U32(cjf, 0x34);
        if (at <= 0 || at + 2 * n > cjf.Length) return null;
        var res = new int[n];
        for (int i = 0; i < n; i++) { int v = (cjf[at + 2 * i] << 8) | cjf[at + 2 * i + 1]; res[i] = v == 0xFFFF ? -1 : v; }
        return res;
    }
    // The SOURCE's .MDP (lzo container) with every vertex's bone numbers put in the TARGET's order and its
    // position moved with its bones' joints (weighted), packed again.
    // The player's model data must fit where the game unpacks it: no bigger (unpacked) than the
    // TARGET's own (YONAH010's 7.2 MB on the father, 5.1 MB, left him without a model: crash at 0x30).
    // The textures end the data: the biggest DXT ones lose their top level (their own level 1, already
    // in the file, becomes the base: half size, same mips after it) until it fits; the textures are
    // packed one after the other again and their records/headers in the .MDV (changed in place) follow.
    // Mip addresses are relative to the texture (fetch constant word 5, 4 KB units).
    internal static byte[] FitMdpPublic(byte[] mdv, byte[] plain, int limit, Action<string> Log) { return FitMdp(mdv, plain, limit, Log); }
    static byte[] FitMdp(byte[] mdv, byte[] plain, int limit, Action<string> Log)
    {
        // NT_MDP_LIMIT=<bytes> (0: none): another ceiling than the replaced model's own size, to measure the game's
        // real one (2026-11-16: Kaine's 5.05 MB fit in the father's 5.08, Yonah's 7.2 MB did not; nothing between tried).
        // 2026-11-16: the game's two 5 MB character pools are 16 MB now (nier_model_pool_mb, game_patches.cpp): a body
        // up to 16 MB unpacked loads as it is; past that the textures are shrunk.
        limit = Math.Max(limit, 16 << 20);
        var over = Environment.GetEnvironmentVariable("NT_MDP_LIMIT");
        int ov; if (over != null && int.TryParse(over, out ov)) limit = ov <= 0 ? int.MaxValue : ov;
        if (plain.Length <= limit) return plain;
        var md = MdlFile.Load("x", mdv, plain);
        var texs = md.Textures().Textures.OrderBy(t => t.DataOffset).ToList();
        if (texs.Count == 0) return plain;
        int front = texs[0].DataOffset;
        foreach (var h in md.Heaps)
            foreach (var r in h.Recs)
                if (r.Tag != "TX2D" && (r.Tag == "VXBF" || r.Tag == "IXBF") && r.C + r.D > front) { Log("   MDP: dados depois das texturas, nao reduzi"); return plain; }
        Func<Tex2D, int> levelSize = t =>
        {
            int bpb = t.Format == 0x12 || t.Format == 0x3C ? 8 : 16;
            int bw = ((t.Width / 2 + 3) / 4 + 31) / 32 * 32, bh = ((t.Height / 2 + 3) / 4 + 31) / 32 * 32;
            return bw * bh * bpb;
        };
        Func<Tex2D, bool> canHalve = t => (t.Format == 0x12 || t.Format == 0x14 || t.Format == 0x13 || t.Format == 0x3C) && t.Width >= 256 && t.Height >= 256
            && t.MaxMip >= 1 && t.Tiled && ((t.F5 >> 12) << 12) > 0 && ((t.F5 >> 12) << 12) < t.DataSize;
        // 2026-11-14: after the textures come the shaders' microcode and constants (VXSH, PXSH, SHCO records; Yonah's at
        // 0x6E0000 on): kept, moved behind the packed textures with their records. Dropping them left the body without
        // shaders: Yonah as Nier was invisible.
        int tailStart = plain.Length;
        foreach (var h in md.Heaps)
            foreach (var r in h.Recs)
                if (r.Tag != "TX2D" && r.C != 0xFFFFFFFF && r.D != 0 && r.C >= front && r.C < tailStart) tailStart = (int)r.C;
        int tailSize = plain.Length - tailStart;
        Func<int, int> tailAt = end => ((end + 0xFFF) & ~0xFFF) + (tailStart & 0xFFF);  // its alignment kept
        var halve = new HashSet<Tex2D>();
        Func<int> total = () =>
        {
            int at = front;
            foreach (var t in texs) { int size = halve.Contains(t) ? t.DataSize - (int)((t.F5 >> 12) << 12) : t.DataSize; at = (at + 0xFFF) & ~0xFFF; at += size; }
            return tailSize > 0 ? tailAt(at) + tailSize : at;
        };
        foreach (var t in texs.Where(canHalve).OrderByDescending(t => t.DataSize))
        {
            if (total() <= limit) break;
            halve.Add(t);
        }
        if (total() > limit) Log("   MDP: mesmo reduzindo as texturas fica " + total() + " > " + limit + " bytes (pode nao aparecer)");
        var res = new byte[total()];
        Array.Copy(plain, res, front);
        int pos = front;
        foreach (var t in texs)
        {
            pos = (pos + 0xFFF) & ~0xFFF;
            int mip = (int)((t.F5 >> 12) << 12);
            int from = halve.Contains(t) ? t.DataOffset + mip : t.DataOffset, size = halve.Contains(t) ? t.DataSize - mip : t.DataSize;
            Array.Copy(plain, from, res, pos, size);
            Be.Put32(mdv, t.EntryOffset + 0x14, (uint)pos);
            Be.Put32(mdv, t.EntryOffset + 0x18, (uint)size);
            if (halve.Contains(t))
            {
                int f = t.HeaderOffset + 0x1C, w = t.Width / 2, h = t.Height / 2, pitch = (int)((t.F0 >> 22) & 0x1FF);
                uint f0 = (t.F0 & ~(0x1FFu << 22)) | (uint)Math.Max(1, pitch / 2) << 22;
                uint f2 = (t.F2 & ~0x03FFFFFFu) | (uint)(w - 1) | (uint)(h - 1) << 13;
                uint f4 = (t.F4 & ~(0xFu << 6)) | (uint)(t.MaxMip - 1) << 6;
                uint f5 = (t.F5 & 0xFFFu) | (t.MaxMip - 1 >= 1 ? (uint)levelSize(t) : 0u);
                Be.Put32(mdv, f, f0); Be.Put32(mdv, f + 8, f2); Be.Put32(mdv, f + 16, f4); Be.Put32(mdv, f + 20, f5);
                Log("   textura " + t.Name + ": " + t.Width + "x" + t.Height + " -> " + w + "x" + h);
            }
            pos += size;
        }
        if (tailSize > 0)
        {
            int to = tailAt(pos);
            Array.Copy(plain, tailStart, res, to, tailSize);
            foreach (var h in md.Heaps)
                foreach (var r in h.Recs)
                    if (r.Tag != "TX2D" && r.C != 0xFFFFFFFF && r.D != 0 && r.C >= tailStart)
                        Be.Put32(mdv, r.Pos + 0x14, (uint)(r.C - tailStart + to));
        }
        Log("   MDP: " + plain.Length + " -> " + res.Length + " bytes (cabe em " + limit + ")");
        return res;
    }

    static byte[] BodySwapMdp(byte[] sourceMdv, byte[] mdp, int[] map, double[][] shift)
    {
        var plain = Lzo.IsContainer(mdp) ? Lzo.Unpack(mdp) : (byte[])mdp.Clone();
        var md = MdlFile.Load("x", sourceMdv, plain);
        var done = new HashSet<uint>();
        int parts = 0, verts = 0;
        foreach (var h in md.Heaps)
        {
            var ix = h.Recs.Where(r => r.Tag == "IXBF").ToList();
            var vx = h.Recs.Where(r => r.Tag == "VXBF").ToList();
            var va = h.Recs.Where(r => r.Tag == "VXAR").ToList();
            for (int k = 0; k < ix.Count && k < vx.Count && k < va.Count; k++)
            {
                var lay = md.Layout(h, va[k]);
                var pos = lay.FirstOrDefault(a => a.Usage == 0); var bw = lay.FirstOrDefault(a => a.Usage == 1); var bi = lay.FirstOrDefault(a => a.Usage == 2);
                if (bi == null || !done.Add(vx[k].C)) continue;
                int count = (int)ix[k].D / 2, ioff = (int)ix[k].C, max = 0;
                for (int i = 0; i < count; i++) { int x = Be.U16(plain, ioff + i * 2); if (x != 0xFFFF && x > max) max = x; }
                int vcount = max + 1, stride = (int)vx[k].D / vcount, voff = (int)vx[k].C;
                for (int v = 0; v < vcount; v++)
                {
                    int o = voff + v * stride;
                    double sx = 0, sy = 0, sz = 0, wsum = 0;
                    for (int j = 0; j < 4; j++)
                    {
                        int b = plain[o + bi.Offset + j];
                        if (b >= map.Length) continue;
                        double w = bw != null ? plain[o + bw.Offset + j] / 255.0 : (j == 0 ? 1 : 0);
                        sx += w * shift[b][0]; sy += w * shift[b][1]; sz += w * shift[b][2]; wsum += w;
                        plain[o + bi.Offset + j] = (byte)map[b];
                    }
                    if (pos != null && pos.Format == 57 && wsum > 0)
                    {
                        PutF(plain, o + pos.Offset, (float)(BeF(plain, o + pos.Offset) + sx / wsum));
                        PutF(plain, o + pos.Offset + 4, (float)(BeF(plain, o + pos.Offset + 4) + sy / wsum));
                        PutF(plain, o + pos.Offset + 8, (float)(BeF(plain, o + pos.Offset + 8) + sz / wsum));
                    }
                }
                parts++; verts += vcount;
            }
        }
        Console.WriteLine("   MDP: " + parts + " partes, " + verts + " vertices no esqueleto novo");
        return Lzo.IsContainer(mdp) ? Lzo.Pack(plain, mdp) : plain;
    }

    // Same key the game's GPU code computes for a texture it loads (SDK GuestTextureKey): FNV-1a over the
    // first 256 bytes of its data, then width, height and format.
    internal static ulong TexKey(byte[] data, int at, int w, int h, int fmt)
    {
        ulong x = 1469598103934665603UL;
        for (int i = 0; i < 256; i++) { x ^= data[at + i]; x *= 1099511628211UL; }
        x ^= ((ulong)(uint)w << 32) | ((ulong)(uint)h << 8) | (uint)fmt;
        x *= 1099511628211UL;
        return x;
    }

    // Edited PNGs of models (<pngDir>\<dir>\<model>\<texture>.png) -> <outDir>\TEXMOD\<key>.bin: the new
    // bytes of each changed texture, which the game puts over the original while it loads it. Same-size
    // textures only (the base level is re-encoded; the mip levels stay as they were).
    internal static string BuildModels(Cpk[] cpks, string pngDir, string outDir)
    {
        int models = 0, changed = 0;
        string texDir = Path.Combine(outDir, "TEXMOD");
        foreach (var c in cpks)
        {
            var byPath = c.Files.ToDictionary(f => f.Path, f => f, StringComparer.OrdinalIgnoreCase);
            foreach (var f in c.Files)
            {
                if (!f.Name.EndsWith(".MDV", StringComparison.OrdinalIgnoreCase)) continue;
                string name = f.Name.Substring(0, f.Name.Length - 4);
                string dir = Path.Combine(pngDir, f.Dir.Replace('/', Path.DirectorySeparatorChar), name);
                if (!Directory.Exists(dir)) continue;
                CpkEntry p;
                if (!byPath.TryGetValue(f.Dir + "/" + name + ".MDP", out p)) continue;
                var mdl = MdlFile.Load(name, c.Read(f), c.Read(p));
                var tex = mdl.Textures();
                int n = 0;
                foreach (var t in tex.Textures)
                {
                    string png = Path.Combine(dir, Safe(t.Name) + ".png");
                    if (!t.Supported || !File.Exists(png)) continue;
                    int w, h;
                    var rgba = PngIo.Load(png, out w, out h);
                    if (w != t.Width || h != t.Height) continue;  // HD sizes are not applied yet
                    if (t.DataSize < 256 || t.DataOffset + t.DataSize > tex.Pix.Length) continue;
                    var orig = tex.Decode(t);
                    bool same = true;
                    for (int i = 0; i < orig.Length && same; i += 4)
                        if (orig[i + 3] != rgba[i + 3] || (orig[i + 3] != 0 && (orig[i] != rgba[i] || orig[i + 1] != rgba[i + 1] || orig[i + 2] != rgba[i + 2]))) same = false;
                    if (same) continue;
                    ulong key = TexKey(tex.Pix, t.DataOffset, t.Width, t.Height, t.Format);
                    tex.Encode(t, rgba);
                    var blob = new byte[t.DataSize];
                    Array.Copy(tex.Pix, t.DataOffset, blob, 0, blob.Length);
                    Directory.CreateDirectory(texDir);
                    File.WriteAllBytes(Path.Combine(texDir, key.ToString("x16") + ".bin"), blob);
                    n++;
                }
                if (n == 0) continue;
                Console.WriteLine(f.Dir + "/" + name + ": " + n + " textura(s) de modelo");
                models++; changed += n;
            }
        }
        return models + " modelo(s), " + changed + " textura(s)";
    }    [System.Runtime.InteropServices.DllImport("kernel32.dll")] static extern bool AttachConsole(int pid);

    [STAThread]
    static int Main(string[] args)
    {
        try { if (args.Length == 3 && args[0] == "--gui-test") { if (Environment.GetEnvironmentVariable("NT_ANIMSCREEN") != null) SetProcessDPIAware(); Gui.TestModel = args[1]; Gui.TestShot = args[2]; return Gui.Run(); } if (args.Length == 0) return Gui.Run(); AttachConsole(-1); return Run(args); }
        catch (Exception e) { Console.WriteLine("erro: " + e.Message); return 2; }
    }

    // The game's shaders live in its models and maps: a HEAP record (VXSH, PXSH...) whose header-area bytes are a
    // D3D shader container's first part ([flags 0x102A11xx][virtual size][physical size]...), the second part (the
    // microcode) in the .MDP at the record's C, D bytes. Each model's containers go into <outDir>\<DIR>_<NAME>.bin,
    // one after the other: Renut's build (XenosRecomp) finds them in any file, keys them by content and drops repeats.
    internal static string ExtractShaders(Cpk[] cpks, string outDir)
    {
        Directory.CreateDirectory(outDir);
        int models = 0, found = 0, failed = 0;
        var seen = new HashSet<string>();
        using (var sha = System.Security.Cryptography.SHA1.Create())
            foreach (var c in cpks)
            {
                var byPath = new Dictionary<string, CpkEntry>(StringComparer.OrdinalIgnoreCase);
                foreach (var f in c.Files) byPath[f.Path] = f;
                foreach (var f in c.Files)
                {
                    if (!(f.Name.EndsWith("V", StringComparison.OrdinalIgnoreCase) && byPath.ContainsKey(f.Path.Substring(0, f.Path.Length - 1) + "P")))
                    {
                        // other files: whole containers in one piece (as XenosRecomp finds them)
                        if (f.Name.EndsWith("P", StringComparison.OrdinalIgnoreCase) && byPath.ContainsKey(f.Path.Substring(0, f.Path.Length - 1) + "V")) continue;
                        try
                        {
                            var d = c.Read(f);
                            if (Lzo.IsContainer(d)) d = Lzo.Unpack(d);
                            var o2 = new MemoryStream();
                            for (int z = 0; z + 0x24 <= d.Length; z += 4)
                            {
                                if ((Be.U32(d, z) & 0xFFFFFF00u) != 0x102A1100u || Be.U32(d, z + 0x1C) != 0 || Be.U32(d, z + 0x20) != 0) continue;
                                long n = (long)Be.U32(d, z + 4) + Be.U32(d, z + 8);
                                if (n < 0x24 || z + n > d.Length) continue;
                                var box = new byte[n];
                                Array.Copy(d, z, box, 0, n);
                                if (seen.Add(Convert.ToBase64String(sha.ComputeHash(box)))) { o2.Write(box, 0, box.Length); found++; }
                                z += (int)n - 4;
                            }
                            if (o2.Length > 0) { File.WriteAllBytes(Path.Combine(outDir, (f.Dir + "_" + f.Name).Replace('/', '_') + ".bin"), o2.ToArray()); models++; }
                        }
                        catch (Exception) { failed++; }
                        continue;
                    }
                    string name = f.Name.Substring(0, f.Name.Length - 1);
                    CpkEntry p;
                    if (!byPath.TryGetValue(f.Path.Substring(0, f.Path.Length - 1) + "P", out p)) continue;
                    try
                    {
                        var md = MdlFile.Load(name, c.Read(f), c.Read(p));
                        var o = new MemoryStream();
                        foreach (var h in md.Heaps)
                            foreach (var r in h.Recs)
                            {
                                // the record holds the game's own data, then the container's first part
                                int at = -1, rs = h.HeaderBase + (int)r.Off, re = Math.Min(md.Vir.Length - 0x24, rs + (int)r.Size);
                                for (int z = Math.Max(0, rs); z < re; z += 4) if ((Be.U32(md.Vir, z) & 0xFFFFFF00u) == 0x102A1100u) { at = z; break; }
                                if (at < 0) continue;
                                int vs = (int)Be.U32(md.Vir, at + 4), ps = (int)Be.U32(md.Vir, at + 8);
                                if (vs <= 0 || at + vs > md.Vir.Length || ps < 0 || ps > (int)r.D || (long)r.C + ps > md.Pix.Length) { failed++; continue; }
                                var box = new byte[vs + ps];
                                Array.Copy(md.Vir, at, box, 0, vs);
                                Array.Copy(md.Pix, (int)r.C, box, vs, ps);
                                if (!seen.Add(Convert.ToBase64String(sha.ComputeHash(box)))) continue;
                                o.Write(box, 0, box.Length);
                                found++;
                            }
                        if (o.Length == 0) continue;
                        File.WriteAllBytes(Path.Combine(outDir, (f.Dir + "_" + name).Replace('/', '_') + ".bin"), o.ToArray());
                        models++;
                    }
                    catch (Exception) { failed++; }
                }
            }
        return found + " shaders de " + models + " modelos/mapas (" + failed + " ignorados) em " + outDir;
    }

    // A mod (modDir: swap.txt + SWAP\) where the game's TARGET model becomes SOURCE, for each "TARGET=SOURCE"
    // of pairs. Characters go on the TARGET's skeleton and animations (body swap). Returns what was done.
    // The TARGET model with the SOURCE's bone lengths (RetargetCjf), or as it is (same model, or
    // NT_KEEP_SKELETON set, or a skeleton it can't rebuild).
    static byte[] Retargeted(byte[] target, byte[] source, string[] pair)
    {
        var r = RetargetedCore(target, source, pair);
        if (AnimHook == null) return r;
        // Swap asks twice for each model (the .MDV and the .MDP's bone map): the hook's work done once.
        string key = pair[0] + "|" + r.Length;
        byte[] done;
        if (!AnimHookDone.TryGetValue(key, out done)) AnimHookDone[key] = done = AnimHook(r, pair[0]);
        return done;
    }
    static byte[] RetargetedCore(byte[] target, byte[] source, string[] pair)
    {
        // Same model: kept, unless it is an imported body (NT_SWAP_EXACT): its own joint places go in (2026-11-18: the
        // father's base outfit kept HIS skeleton under the 2B, her neck and shoulders stretched to his).
        if ((string.Equals(pair[0], pair[1], StringComparison.OrdinalIgnoreCase) && Environment.GetEnvironmentVariable("NT_SWAP_EXACT") == null)
            || Environment.GetEnvironmentVariable("NT_KEEP_SKELETON") != null) return target;
        var r = RetargetCjf(target, source) ?? target;
        return Environment.GetEnvironmentVariable("NT_KEEP_ANIMS") != null ? r : OwnAnimations(r, source, pair[0], pair[1]);
    }
    // Set by an import (Outfit): the animations of each TARGET model of a swap made over (another character's
    // movement, the body's own posture) after its skeleton is set. Takes the .MDV and the TARGET's name.
    internal static Func<byte[], string, byte[]> AnimHook;
    internal static Dictionary<string, byte[]> AnimHookDone = new Dictionary<string, byte[]>();

    // Each animation of a model .MDV (unpacked) passed through f (skeleton, motion, file) -> new file.
    internal static byte[] MapMotions(byte[] mdv, Func<Skeleton, Motion, byte[], byte[]> f, out int changed)
    {
        changed = 0;
        var top = KpkItems(mdv, 0, mdv.Length);
        if (top == null || top.Length < 3 || top[2] == null) return mdv;
        var l = KpkItems(top[2], 0, top[2].Length);
        var sk = Skeleton.Read(l.FirstOrDefault(x => x != null && x.Length > 0x44 && x[0] == 'C' && x[1] == 'J' && x[2] == 'F'));
        int at = Array.FindIndex(l, x => x != null && x.Length > 8 && Encoding.ASCII.GetString(x, 0, 4) == "KPKy");
        if (sk == null || at < 0) return mdv;
        var ms = KpkItems(l[at], 0, l[at].Length);
        for (int i = 0; i < ms.Length; i++)
        {
            var m = Motion.Read(ms[i]);
            if (m == null) continue;
            var o = f(sk, m, ms[i]);
            if (o == null || ReferenceEquals(o, ms[i])) continue;
            ms[i] = o; changed++;
        }
        if (changed == 0) return mdv;
        l[at] = Kpk(ms);
        top[2] = Kpk(l);
        return Kpk(top);
    }

    // The animations of another body (orig: the skeleton they were made for) on this model's own proportions, as
    // VRChat and such do: they only turn the joints; the few places they set (the hips' height, collars, fingers,
    // the blade in the hand) become this model's rest place plus the move, scaled by the bone's length (the hips: by
    // the hip height). N_ALL (the walk through the world) stays.
    internal static byte[] OwnProportions(byte[] mdv, Skeleton orig, Action<string> log)
    {
        float hipScale = 1; bool first = true;
        var origAt = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        for (int i = 0; i < orig.Names.Length; i++) if (!origAt.ContainsKey(orig.Names[i])) origAt[orig.Names[i]] = i;
        int changed;
        var r = MapMotions(mdv, (sk, m, file) =>
        {
            if (first)
            {
                first = false;
                var mm = Humanoid.Map(sk); var om = Humanoid.Map(orig);
                float fl;
                if (mm[0] >= 0 && om[0] >= 0) hipScale = Humanoid.HipHeight(sk, mm, out fl) / Humanoid.HipHeight(orig, om, out fl);
                if (hipScale < 0.2f || hipScale > 5f) hipScale = 1;
            }
            if (m.Pos.Count == 0) return file;
            var pos = new Dictionary<int, Motion.Track>();
            bool any = false;
            foreach (var kv in m.Pos)
            {
                int j = kv.Key, oj;
                string nm = j < sk.Names.Length ? sk.Names[j] : "";
                if (j >= sk.Names.Length || sk.Parent[j] < 0 || nm.Equals("N_ALL", StringComparison.OrdinalIgnoreCase) || !origAt.TryGetValue(nm, out oj)) { pos[j] = kv.Value; continue; }
                float[] mr = { sk.Local[j][4], sk.Local[j][5], sk.Local[j][6] }, orr = { orig.Local[oj][4], orig.Local[oj][5], orig.Local[oj][6] };
                float ml = (float)Math.Sqrt(mr[0] * mr[0] + mr[1] * mr[1] + mr[2] * mr[2]), ol = (float)Math.Sqrt(orr[0] * orr[0] + orr[1] * orr[1] + orr[2] * orr[2]);
                float sc = nm.Equals("HIPSBONE", StringComparison.OrdinalIgnoreCase) ? hipScale : ol > 1e-5f ? ml / ol : 1;
                if (Math.Abs(mr[0] - orr[0]) + Math.Abs(mr[1] - orr[1]) + Math.Abs(mr[2] - orr[2]) < 1e-5f && Math.Abs(sc - 1) < 1e-4f) { pos[j] = kv.Value; continue; }
                pos[j] = new Motion.Track { Keys = kv.Value.Keys, Vals = kv.Value.Vals.Select(v => new[] { mr[0] + (v[0] - orr[0]) * sc, mr[1] + (v[1] - orr[1]) * sc, mr[2] + (v[2] - orr[2]) * sc }).ToArray() };
                any = true;
            }
            return any ? Anim.WriteCmf(file, Math.Max(sk.Names.Length, (int)Be.U16(file, 6)), m.Rot, pos) : file;
        }, out changed);
        if (log != null) log("proporcoes proprias: " + changed + " animacoes com posicoes no tamanho do modelo (quadril x" + hipScale.ToString("F2") + ")");
        return r;
    }

    // The body's own posture (Anim.Posture) over every animation but the added ones ("add_": blink, anger).
    internal static byte[] PostureAnimations(byte[] mdv, float legsDeg, float armsDeg, Dictionary<string, float[]> custom, Action<string> log)
    {
        Dictionary<int, float[]> offsets = null;
        int changed;
        var r = MapMotions(mdv, (sk, m, file) =>
        {
            if (offsets == null) offsets = Anim.Posture(sk, legsDeg, armsDeg, custom);
            int u = m.Name.IndexOf('_');
            if (u > 0 && m.Name.Substring(u + 1).StartsWith("add_", StringComparison.OrdinalIgnoreCase)) return file;
            return Anim.Adjust(file, sk, m, offsets);
        }, out changed);
        if (log != null) log("postura propria: " + (offsets == null ? 0 : offsets.Count) + " juntas em " + changed + " animacoes");
        return r;
    }

    // The TARGET's animations (MDV top[2] > KPKy of CMF) that the SOURCE character also has (same name after
    // the model's: "_01_walk^lo_01") are rebuilt with the SOURCE's movement (Anim.Convert); the rest stay.
    // NT_KEEP_ANIMS: the TARGET's all (as before 2026-11-15).
    internal static int LastOwnAnims;

    // The player's attacks' hit frames (STRUCT2BIN/PLAYER_TABLE_PACK.BIN, records of 0xE4 bytes: a 32-byte name
    // "_AT_SWORD^C01_01", then words; words 12 and 13 after the name = the first and last hit frame, e.g. 7-18;
    // checked against where the father's blade moves fast). Key: the name without "_", "at_sword^c01_01".
    internal static Dictionary<string, Tuple<int, int>> HitWindows = new Dictionary<string, Tuple<int, int>>(StringComparer.OrdinalIgnoreCase);
    internal static void LoadHitWindows(Cpk[] cpks)
    {
        if (HitWindows.Count > 0) return;
        foreach (var c in cpks)
        {
            var e = c.Files.FirstOrDefault(f => string.Equals(f.Path, "STRUCT2BIN/PLAYER_TABLE_PACK.BIN", StringComparison.OrdinalIgnoreCase));
            if (e == null) continue;
            var d = c.Read(e);
            for (int at = 0; at + 32 + 56 <= d.Length; at += 4)
            {
                if (d[at] != '_' || d[at + 1] != 'A' || d[at + 2] != 'T' || d[at + 3] != '_') continue;
                int end = at; while (end < at + 32 && d[end] != 0) end++;
                if (end >= at + 32) continue;
                int a = (int)Be.U32(d, at + 32 + 48), b = (int)Be.U32(d, at + 32 + 52);
                if (a < 0 || b < a || b > 1000) continue;
                string key = Encoding.ASCII.GetString(d, at + 1, end - at - 1);
                if (!HitWindows.ContainsKey(key)) HitWindows[key] = Tuple.Create(a, b);
            }
        }
        Console.WriteLine("   tabela de golpes: " + HitWindows.Count + " janelas de acerto");
    }
    internal static byte[] OwnAnimations(byte[] target, byte[] source, string tName, string sName)
    {
        LastOwnAnims = 0;
        try
        {
            var tTop = KpkItems(target, 0, target.Length); var sTop = KpkItems(source, 0, source.Length);
            if (tTop == null || sTop == null || tTop.Length < 3 || sTop.Length < 3 || tTop[2] == null || sTop[2] == null) return target;
            var tl = KpkItems(tTop[2], 0, tTop[2].Length); var sl = KpkItems(sTop[2], 0, sTop[2].Length);
            Func<byte[][], byte[]> cjf = l => l.FirstOrDefault(x => x != null && x.Length > 0x44 && x[0] == 'C' && x[1] == 'J' && x[2] == 'F');
            var tSk = Skeleton.Read(cjf(tl)); var sSk = Skeleton.Read(cjf(sl));
            if (tSk == null || sSk == null) return target;
            int at = Array.FindIndex(tl, x => x != null && x.Length > 8 && Encoding.ASCII.GetString(x, 0, 4) == "KPKy");
            int sat = Array.FindIndex(sl, x => x != null && x.Length > 8 && Encoding.ASCII.GetString(x, 0, 4) == "KPKy");
            if (at < 0 || sat < 0) return target;
            var tm = KpkItems(tl[at], 0, tl[at].Length); var smi = KpkItems(sl[sat], 0, sl[sat].Length);
            var bySuffix = new Dictionary<string, Motion>(StringComparer.OrdinalIgnoreCase);
            foreach (var d in smi)
            {
                var m = Motion.Read(d);
                int u = m == null ? -1 : m.Name.IndexOf('_'); if (u > 0) bySuffix[m.Name.Substring(u + 1)] = m;
            }
            // 2026-11-19 (the user: "Kaine's power plays her animation"): a TARGET animation the SOURCE has no same-named
            // one for takes the one named here (target suffix = source suffix). Kaine's magic in place of Nier's (the
            // moveset mod plays Nier's magic actions for its powers); NT_ANIM_MAP="tgt=src;..." adds or overrides.
            var map = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
            {
                { "at_magic^spear^s_01", "at_magic^c03^s_01" }, { "at_magic^spear^lo_01", "at_magic^c03^lo_01" },
                { "at_magic^spear^e_01", "at_magic^c03^e_01" }, { "at_magic^spear^step_01", "at_magic^c01_01" },
                { "at_magic^hand^s_01", "at_magic^c04^s_01" }, { "at_magic^hand^lo_01", "at_magic^c04^lo_10" },
                { "at_magic^hand^e_01", "at_magic^c04^e_01" }, { "at_magic^hand^step_01", "at_magic^c02_01" },
                { "at_magic^slash_01", "at_magic^c01_01" }, { "at_magic^slash_02", "at_magic^c02_01" },
                { "at_magic^slash_03", "at_magic^c01_01" }, { "at_magic^slash_04", "at_magic^c02_01" },
                { "at_magic^slash^s_01", "at_magic^c01_01" }, { "at_magic^slash^e_01", "at_magic^c02_01" },
            };
            var extra = Environment.GetEnvironmentVariable("NT_ANIM_MAP");
            if (!string.IsNullOrEmpty(extra))
                foreach (var pair in extra.Split(';'))
                {
                    var kv = pair.Split('=');
                    if (kv.Length == 2 && kv[0].Trim().Length > 0) map[kv[0].Trim()] = kv[1].Trim();
                }
            foreach (var kv in map)
            {
                Motion src;
                if (!bySuffix.ContainsKey(kv.Key) && bySuffix.TryGetValue(kv.Value, out src)) bySuffix[kv.Key] = src;
            }
            bool dual = DualWield(sSk.Names, tSk.Names);
            if (dual) Console.WriteLine("   duas espadas: a do jogo (RIGHTWEAPON) segue a que golpeia; as da origem ficam nas maos");
            int done = 0, before = 0, after = 0;
            for (int i = 0; i < tm.Length; i++)
            {
                var m = Motion.Read(tm[i]);
                Motion s;
                // The name after the model's ("nier030_01_walk^lo_01": the outfits NIER031/040 use nier030's names too).
                int u = m == null ? -1 : m.Name.IndexOf('_');
                if (u <= 0 || !bySuffix.TryGetValue(m.Name.Substring(u + 1), out s)) continue;
                // "add_" ones (blink, anger...) are added over another animation: their values are no pose to turn.
                if (m.Name.Substring(u + 1).StartsWith("add_", StringComparison.OrdinalIgnoreCase)) continue;
                int movedJ;
                // Attacks: the strike on the TARGET's hit frames; two swords: the hits follow the striking one.
                bool attack = m.Name.Substring(u + 1).StartsWith("at_", StringComparison.OrdinalIgnoreCase);
                Tuple<int, int> win;
                int hitCenter = attack && HitWindows.TryGetValue(m.Name.Substring(u + 1), out win) ? (win.Item1 + win.Item2) / 2 : -1;
                var conv = Anim.Convert(tm[i], tSk, m, sSk, s, attack ? Anim.AttackWarp(tSk, m, sSk, s, hitCenter) : null, dual, out movedJ);
                before += tm[i].Length; after += conv.Length;
                tm[i] = conv; done++;
            }
            LastOwnAnims = done;
            Console.WriteLine("   animacoes proprias: " + done + " de " + tm.Length + " (" + (before / 1024) + " -> " + (after / 1024) + " KB)");
            if (done == 0) return target;
            tl[at] = Kpk(tm);
            tTop[2] = Kpk(tl);
            return Kpk(tTop);
        }
        catch (Exception e) { Console.WriteLine("   animacoes proprias: erro " + e.Message); return target; }
    }
    // Swap with a SOURCE edited in the 3D Editor: its files (DIR/NAME.MDV / .MDP, unpacked) in place of
    // the disc's. Empty: the disc's.
    internal static Dictionary<string, byte[]> SwapSource = new Dictionary<string, byte[]>();
    static byte[] Src(Cpk c, CpkEntry e) { byte[] d; return SwapSource.TryGetValue(e.Path.ToUpperInvariant(), out d) ? d : c.Read(e); }    internal static string Swap(Cpk[] cpks, string pairs, string modDir, string dirName)
    {
        var log = new StringBuilder();
        Action<string> Log = s => { Console.WriteLine(s); log.AppendLine(s); };
        LoadHitWindows(cpks);
        // Both files of the model (.MDV header, .MDP data) are read by the game from its pack: the bytes of
        // the SOURCE files (as stored in the pack) are served in place of the TARGET's, and the sizes in the
        // pack's table are patched, so the game plans its reads, unpacks and fixes up as it always does.
        // A SOURCE bigger than the TARGET would cover the files stored after it, so it is placed
        // past the end of the pack instead (the table's offset is patched; the game serves it).
        var lines = new List<string>();
        var tail = new Dictionary<string, long>();  // pack -> next free offset past its end
        foreach (var pair in pairs.Split(','))
        {
            var ts = pair.Split('=');
            foreach (var c in cpks)
            {
                bool ok = false; bool bodyMixed = false;  // the .MDV was put on the TARGET's skeleton: its .MDP is renumbered to match
                byte[] mixedMdv = null; string mixedMdvPath = null;  // rewritten when the .MDP's textures move (FitMdp)
                foreach (var ext in new[] { ".MDV", ".MDP" })
                {
                    CpkEntry tm = null, sm = null;
                    foreach (var f in c.Files)
                    {
                        if (string.Equals(f.Path, dirName + "/" + ts[0] + ext, StringComparison.OrdinalIgnoreCase)) tm = f;
                        if (string.Equals(f.Path, dirName + "/" + ts[1] + ext, StringComparison.OrdinalIgnoreCase)) sm = f;
                    }
                    if (tm == null || sm == null) continue;
                    ok = true;
                    Directory.CreateDirectory(Path.Combine(modDir, "SWAP"));
                    string rel = "SWAP\\" + ts[1] + ext;
                    byte[] data = SwapSource.ContainsKey(sm.Path.ToUpperInvariant()) ? SwapSource[sm.Path.ToUpperInvariant()] : c.ReadRaw(sm);
                    long newSize = sm.Size, newExtract = sm.ExtractSize;
                    if (SwapSource.ContainsKey(sm.Path.ToUpperInvariant())) newSize = newExtract = data.Length;
                    // The game moves the model with the TARGET's skeleton and animations: the SOURCE's
                    // polygons are put on them (stored unpacked).
                    byte[] mixed = ext == ".MDV" ? BodySwapMdv(Retargeted(c.Read(tm), Src(c, sm), ts), Src(c, sm)) : null; bodyMixed |= mixed != null;
                    if (ext == ".MDP" && bodyMixed && !string.Equals(ts[0], ts[1], StringComparison.OrdinalIgnoreCase))
                    {
                        // The vertices name their bones by number in the model's own list: they get the TARGET's numbers.
                        var tv = c.Files.FirstOrDefault(f => string.Equals(f.Path, dirName + "/" + ts[0] + ".MDV", StringComparison.OrdinalIgnoreCase));
                        var sv = c.Files.FirstOrDefault(f => string.Equals(f.Path, dirName + "/" + ts[1] + ".MDV", StringComparison.OrdinalIgnoreCase));
                        if (tv != null && sv != null)
                        {
                            var sMdv = Src(c, sv);
                            double[][] shift;
                            var map = BoneMap(sMdv, Retargeted(c.Read(tv), sMdv, ts), out shift);
                            if (map != null) mixed = BodySwapMdp(sMdv, Src(c, sm), map, shift);
                            if (mixed != null && mixedMdv != null)
                            {
                                var tgt = c.Read(tm);
                                int limit = Lzo.IsContainer(tgt) ? (int)Be.U32(tgt, 0x10) : tgt.Length;
                                var plainM = Lzo.IsContainer(mixed) ? Lzo.Unpack(mixed) : mixed;
                                var fitted = FitMdp(mixedMdv, plainM, limit, Log);
                                if (!ReferenceEquals(fitted, plainM)) { mixed = Lzo.IsContainer(mixed) ? Lzo.Pack(fitted, mixed) : fitted; File.WriteAllBytes(mixedMdvPath, mixedMdv); }
                            }
                        }
                    }
                    if (mixed != null)
                    {
                        data = mixed; newSize = newExtract = mixed.Length;
                        rel = "SWAP\\" + ts[1] + "-EM-" + ts[0] + ext;
                    }
                    File.WriteAllBytes(Path.Combine(modDir, rel), data);
                    if (ext == ".MDV" && mixed != null) { mixedMdv = data; mixedMdvPath = Path.Combine(modDir, rel); }
                    string cpkName = c.Name.ToLowerInvariant();
                    long at = tm.Offset, size = tm.Size;
                    var fields = new List<Tuple<long, int, long>> { Tuple.Create(tm.SizeAt, tm.SizeLen, newSize), Tuple.Create(tm.ExtractAt, tm.ExtractLen, newExtract) };
                    if (newSize > tm.Size)
                    {
                        if (tm.OffsetAt < 0) { Log(ts[0] + ext + ": tabela sem FileOffset, pulei"); continue; }
                        long free;
                        if (!tail.TryGetValue(cpkName, out free)) free = (c.Length + 0xFFFFF) & ~0xFFFFFL;
                        at = free; size = newSize;
                        tail[cpkName] = (free + newSize + 0x7FF) & ~0x7FFL;
                        fields.Add(Tuple.Create(tm.OffsetAt, tm.OffsetLen, at - tm.OffsetBase));
                    }
                    foreach (var fld in fields)
                    {
                        if (fld.Item1 < 0 || fld.Item2 <= 0) continue;
                        var sb2 = new StringBuilder();
                        for (int k = 0; k < fld.Item2; k++)
                        {
                            byte plain = (byte)(fld.Item3 >> (8 * (fld.Item2 - 1 - k)));
                            byte stored = tm.TocEncrypted ? (byte)(plain ^ Cpk.Key((int)(fld.Item1 - tm.TocTableAt) + k)) : plain;
                            sb2.Append(stored.ToString("X2"));
                        }
                        lines.Add("patch " + cpkName + " " + fld.Item1.ToString("X") + " " + sb2);
                    }
                    lines.Add(cpkName + " " + at.ToString("X") + " " + size.ToString("X") + " " + rel);
                    Log(ts[0] + ext + " <- " + ts[1] + ext + (mixed != null ? " (no esqueleto de " + ts[0] + ")" : "") + "  (" + tm.Size + " -> " + newSize + " bytes)");
                }
                if (ok) break;
            }
        }
        File.WriteAllLines(Path.Combine(modDir, "swap.txt"), lines);
        return log.ToString();
    }

    // A model edited outside (Blender...): the OBJ files written by "Exportar OBJ" / mdlobj (one per part,
    // <NAME>_<part>.obj, every vertex in the game's order) give the vertices their new positions. Same
    // vertex count per part (moving, scaling, sculpting; no vertices added or removed). The result is a
    // mod (modDir: swap.txt + SWAP\<NAME>-EDIT.MDP, the model's data unpacked) the game serves in place
    // of the model's .MDP. Returns what was done.
    internal static string ImportObj(Cpk[] cpks, string model, string objDir, string modDir)
    {
        var log = new StringBuilder();
        Action<string> Log = s => { Console.WriteLine(s); log.AppendLine(s); };
        LoadHitWindows(cpks);
        var ci = System.Globalization.CultureInfo.InvariantCulture;
        foreach (var c in cpks)
        {
            var v = c.Files.FirstOrDefault(f => string.Equals(f.Path, model + ".MDV", StringComparison.OrdinalIgnoreCase));
            var p = c.Files.FirstOrDefault(f => string.Equals(f.Path, model + ".MDP", StringComparison.OrdinalIgnoreCase));
            if (v == null || p == null) continue;
            string name = Path.GetFileNameWithoutExtension(v.Name);
            var mdl = MdlFile.Load(name, c.Read(v), c.Read(p));
            byte[] pix = (byte[])mdl.Pix.Clone();
            int changed = 0, parts = 0;
            foreach (var part in mdl.VertexParts())
            {
                string obj = Path.Combine(objDir, name + "_" + part.Item1 + ".obj");
                if (!File.Exists(obj)) continue;
                var pos = new List<float[]>();
                foreach (var line in File.ReadLines(obj))
                {
                    if (!line.StartsWith("v ")) continue;
                    var q = line.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
                    if (q.Length < 4) continue;
                    pos.Add(new[] { float.Parse(q[1], ci), float.Parse(q[2], ci), float.Parse(q[3], ci) });
                }
                if (pos.Count != part.Item3)
                {
                    Log("   " + Path.GetFileName(obj) + ": " + pos.Count + " vertices, o jogo tem " + part.Item3 + " (nao pode adicionar nem apagar vertices) - pulei");
                    continue;
                }
                int moved = 0;
                for (int i = 0; i < pos.Count; i++)
                {
                    int o = part.Item2 + i * part.Item4;
                    for (int k = 0; k < 3; k++)
                    {
                        if (Math.Abs(BeF(pix, o + k * 4) - pos[i][k]) > 1e-5f) { PutF(pix, o + k * 4, pos[i][k]); moved++; }
                    }
                }
                parts++; changed += moved > 0 ? 1 : 0;
                Log("   " + Path.GetFileName(obj) + ": " + pos.Count + " vertices" + (moved > 0 ? "" : " (sem mudanca)"));
            }
            if (parts == 0) { Log("nenhum OBJ de " + name + " em " + objDir); return log.ToString(); }
            WriteMdpMod(c, p, name, pix, modDir, Log);
            Log("(" + changed + " de " + parts + " partes mudadas)");
            return log.ToString();
        }
        Log("nao achei " + model);
        return log.ToString();
    }

    // The model's .MDP data (unpacked, as MdlFile.Pix) edited: a mod serving it in place of the game's.
    // Model data packed as the game stores it (an "lzo" container like the game's own file), when that is.
    internal static byte[] PackLikeGame(byte[] plain, byte[] gameFile)
    {
        if (!Lzo.IsContainer(gameFile) || Environment.GetEnvironmentVariable("NT_NO_LZO") != null) return plain;
        try { return Lzo.Pack(plain, gameFile); } catch (Exception e) { Console.WriteLine("   LZO: " + e.Message + " (fica sem compressao)"); return plain; }
    }

    internal static void WriteMdpMod(Cpk c, CpkEntry mdp, string name, byte[] pix, string modDir, Action<string> Log)
    {
        Directory.CreateDirectory(Path.Combine(modDir, "SWAP"));
        string rel = "SWAP\\" + name + "-EDIT.MDP";
        File.WriteAllBytes(Path.Combine(modDir, rel), pix);
        var lines = new List<string>();
        var tail = new Dictionary<string, long>();
        if (ServeFile(c, mdp, pix.Length, pix.Length, rel, lines, tail, Log)) Log(name + ".MDP <- " + rel);
        File.WriteAllLines(Path.Combine(modDir, "swap.txt"), lines);
    }

    // The game's pack holding DIR/NAME.MDV and .MDP, and those entries.
    internal static Cpk FindModel(Cpk[] cpks, string model, out CpkEntry mdv, out CpkEntry mdp)
    {
        foreach (var c in cpks)
        {
            mdv = c.Files.FirstOrDefault(f => string.Equals(f.Path, model + ".MDV", StringComparison.OrdinalIgnoreCase));
            mdp = c.Files.FirstOrDefault(f => string.Equals(f.Path, model + ".MDP", StringComparison.OrdinalIgnoreCase));
            if (mdv != null && mdp != null) return c;
        }
        mdv = mdp = null;
        return null;
    }

    // swap.txt lines serving `size` bytes (the mod's file `rel`) in place of the pack entry tm: its sizes in
    // the pack's table patched, and, when bigger than the stored one, placed past the end of the pack.
    internal static bool ServeFile(Cpk c, CpkEntry tm, long newSize, long newExtract, string rel, List<string> lines,
                          Dictionary<string, long> tail, Action<string> Log)
    {
        string cpkName = c.Name.ToLowerInvariant();
        long at = tm.Offset, size = tm.Size;
        var fields = new List<Tuple<long, int, long>> { Tuple.Create(tm.SizeAt, tm.SizeLen, newSize), Tuple.Create(tm.ExtractAt, tm.ExtractLen, newExtract) };
        if (newSize > tm.Size)
        {
            if (tm.OffsetAt < 0) { Log(tm.Path + ": tabela sem FileOffset, pulei"); return false; }
            long free;
            if (!tail.TryGetValue(cpkName, out free)) free = (c.Length + 0xFFFFF) & ~0xFFFFFL;
            at = free; size = newSize;
            tail[cpkName] = (free + newSize + 0x7FF) & ~0x7FFL;
            fields.Add(Tuple.Create(tm.OffsetAt, tm.OffsetLen, at - tm.OffsetBase));
        }
        foreach (var fld in fields)
        {
            if (fld.Item1 < 0 || fld.Item2 <= 0) continue;
            var sb2 = new StringBuilder();
            for (int k = 0; k < fld.Item2; k++)
            {
                byte plain = (byte)(fld.Item3 >> (8 * (fld.Item2 - 1 - k)));
                byte stored = tm.TocEncrypted ? (byte)(plain ^ Cpk.Key((int)(fld.Item1 - tm.TocTableAt) + k)) : plain;
                sb2.Append(stored.ToString("X2"));
            }
            lines.Add("patch " + cpkName + " " + fld.Item1.ToString("X") + " " + sb2);
        }
        lines.Add(cpkName + " " + at.ToString("X") + " " + size.ToString("X") + " " + rel);
        return true;
    }

    static int Run(string[] args)
    {
        if (args.Length < 2) { Console.WriteLine("uso: list|find|cat|extract|build|roundtrip <media> ..."); return 1; }
        string cmd = args[0], media = args[1];
        if (Environment.GetEnvironmentVariable("NT_EXT") != null) { var e = Environment.GetEnvironmentVariable("NT_EXT").Split(','); VExt = e[0]; PExt = e[1]; }
        if (cmd == "unpack")  // unpack <.2DV> <.2DP> <out>: decode loose files (checks a built pack)
        {
            var pk = Pack2D.Load(Path.GetFileNameWithoutExtension(args[1]), File.ReadAllBytes(args[1]), File.ReadAllBytes(args[2]));
            foreach (var t in pk.Textures)
            {
                Console.WriteLine(t.Name + " " + t.Width + "x" + t.Height + " " + t.FormatName + " off=" + t.DataOffset.ToString("X") + " size=" + t.DataSize.ToString("X") + " base=" + t.BaseSize.ToString("X") + " pitch=" + t.Pitch + " tiled=" + t.Tiled + " maxmip=" + t.MaxMip + " packed=" + t.PackedMips + " F1=" + t.F1.ToString("X8") + " F5=" + t.F5.ToString("X8"));
                if (args.Length > 3 && t.Supported) PngIo.Save(Path.Combine(args[3], Safe(t.Name) + ".png"), pk.Decode(t), t.Width, t.Height);
            }
            return 0;
        }
        if (cmd == "mdlfile")  // mdlfile <.MDV> <.MDP>: summary of loose model files (NT_BIDX=1: bone numbers per part)
        {
            var vir = File.ReadAllBytes(args[1]);
            var top = KpkItems(vir, 0, vir.Length);
            Console.WriteLine(MdlFile.Load(Path.GetFileNameWithoutExtension(args[1]), vir, File.ReadAllBytes(args[2])).Summary());
            return 0;
        }
        if (cmd == "unlzo")  // unlzo <file> <out>: a loose "lzo" container unpacked
        {
            File.WriteAllBytes(args[2], Lzo.Unpack(File.ReadAllBytes(args[1])));
            return 0;
        }
        if (cmd == "lzotest")  // lzotest <file> <hex offset> <out>
        {
            var d = File.ReadAllBytes(args[1]);
            int end;
            var o = Lzo.Decompress1X(d, Convert.ToInt32(args[2], 16), out end);
            Console.WriteLine("saida " + o.Length + " bytes, fim da entrada em 0x" + end.ToString("X"));
            File.WriteAllBytes(args[3], o);
            return 0;
        }
        if (cmd == "texsheet")  // texsheet <folder> <out.png>: every texture of a folder (png, jpg, dds) side by side, 256 px each
        {
            var files = Directory.GetFiles(args[1]).Where(f => System.Text.RegularExpressions.Regex.IsMatch(f, "\\.(png|jpg|jpeg|dds|bmp|tga)$", System.Text.RegularExpressions.RegexOptions.IgnoreCase)).OrderBy(f => f).ToList();
            int cols = 6, S = 256, rows = (files.Count + cols - 1) / cols;
            var sheet = new byte[cols * S * rows * (S + 20) * 4];
            for (int i = 0; i < files.Count; i++)
            {
                int w, h; byte[] px;
                try { px = PngIo.Load(files[i], out w, out h); } catch (Exception e) { Console.WriteLine(Path.GetFileName(files[i]) + ": " + e.Message); continue; }
                int ox = (i % cols) * S, oy = (i / cols) * (S + 20);
                for (int y = 0; y < S; y++) for (int x = 0; x < S; x++)
                    {
                        int sx = x * w / S, sy = y * h / S, q = (sy * w + sx) * 4, o = ((oy + y) * cols * S + ox + x) * 4;
                        sheet[o] = px[q]; sheet[o + 1] = px[q + 1]; sheet[o + 2] = px[q + 2]; sheet[o + 3] = 255;
                    }
                Console.WriteLine((i + 1) + ". " + Path.GetFileName(files[i]) + " " + w + "x" + h);
            }
            PngIo.Save(args[2], sheet, cols * S, rows * (S + 20));
            return 0;
        }
        if (cmd == "esticados")  // esticados <mdv> <mdp>: in the run, the triangles that stretch most, and their points' bones
        {
            var mdvB = File.ReadAllBytes(args[1]);
            var motions = new List<Motion>(); var sk = Anim.Load(mdvB, motions);
            var parts = MdlFile.Load("x", mdvB, File.ReadAllBytes(args[2])).Parts();
            var run = motions.FirstOrDefault(m => m.Name.EndsWith("_run_01", StringComparison.OrdinalIgnoreCase)) ?? motions.First();
            var mats = Anim.SkinMatrices(sk, Anim.World(sk, run, run.Frames / 3f, false));
            var ci = System.Globalization.CultureInfo.InvariantCulture;
            for (int pi = 0; pi < parts.Count; pi++)
            {
                var p = parts[pi]; if (p.BI == null) continue;
                var q = Anim.Skin(p, mats);
                Func<float[], int, int, float> len = (v, a, b) => (float)Math.Sqrt(Math.Pow(v[a * 3] - v[b * 3], 2) + Math.Pow(v[a * 3 + 1] - v[b * 3 + 1], 2) + Math.Pow(v[a * 3 + 2] - v[b * 3 + 2], 2));
                var list = new List<Tuple<float, int>>();
                for (int t = 0; t + 2 < p.F.Length; t += 3)
                {
                    float r = 0;
                    for (int e = 0; e < 3; e++) { int a = p.F[t + e], b = p.F[t + (e + 1) % 3]; float l0 = len(p.V, a, b), l1 = len(q.V, a, b); if (l0 > 1e-4f) r = Math.Max(r, l1 / l0); else if (l1 > 0.05f) r = Math.Max(r, 99); }
                    list.Add(Tuple.Create(r, t));
                }
                var top = list.OrderByDescending(x => x.Item1).Take(6).ToList();
                Console.WriteLine("parte " + pi + " (" + p.Tex + ", " + p.F.Length / 3 + " tri): esticados >3x: " + list.Count(x => x.Item1 > 3));
                foreach (var tp in top)
                {
                    var sb = new StringBuilder("  x" + tp.Item1.ToString("0.0", ci) + ":");
                    for (int e = 0; e < 3; e++)
                    {
                        int v = p.F[tp.Item2 + e];
                        sb.Append(string.Format(ci, " [{0:0.00},{1:0.00},{2:0.00}", p.V[v * 3], p.V[v * 3 + 1], p.V[v * 3 + 2]));
                        for (int k = 0; k < 4; k++) if (p.BW[v * 4 + k] > 0.01f) { int j = p.BI[v * 4 + k] + 1; sb.Append(" " + (j >= 0 && j < sk.Names.Length ? sk.Names[j] : "#" + j) + ":" + p.BW[v * 4 + k].ToString("0.00", ci)); }
                        sb.Append("]");
                    }
                    Console.WriteLine(sb.ToString());
                }
            }
            return 0;
        }
        if (cmd == "previa")  // previa <mdv> <mdp> <atlas.png> <out.png>: the outfit preview of any built pair
        {
            Outfit.PreviewFiles(args[1], args[2], args[3], args[4]);
            return 0;
        }
        if (cmd == "fbxmesh")  // fbxmesh <file.fbx> <name part>: a mesh's box, points near the origin, longest edges
        {
            var fbx = FbxModel.Load(args[1]);
            foreach (var me in fbx.Meshes.Where(m => m.Name.IndexOf(args[2], StringComparison.OrdinalIgnoreCase) >= 0))
            {
                int n = me.Pos.Length / 3;
                var lo = new double[] { double.MaxValue, double.MaxValue, double.MaxValue }; var hi = new double[] { double.MinValue, double.MinValue, double.MinValue };
                for (int i = 0; i < n; i++) for (int k = 0; k < 3; k++) { lo[k] = Math.Min(lo[k], me.Pos[i * 3 + k]); hi[k] = Math.Max(hi[k], me.Pos[i * 3 + k]); }
                int zero = 0; for (int i = 0; i < n; i++) if (Math.Abs(me.Pos[i * 3]) + Math.Abs(me.Pos[i * 3 + 1]) + Math.Abs(me.Pos[i * 3 + 2]) < 1e-6) zero++;
                var edges = new List<Tuple<double, int, int>>();
                foreach (var poly in me.Polys)
                    for (int k = 0; k < poly.Length; k++)
                    {
                        int a = poly[k], b = poly[(k + 1) % poly.Length];
                        double dx = me.Pos[a * 3] - me.Pos[b * 3], dy = me.Pos[a * 3 + 1] - me.Pos[b * 3 + 1], dz = me.Pos[a * 3 + 2] - me.Pos[b * 3 + 2];
                        edges.Add(Tuple.Create(Math.Sqrt(dx * dx + dy * dy + dz * dz), a, b));
                    }
                var sorted = edges.OrderByDescending(e => e.Item1).ToList();
                double median = sorted[sorted.Count / 2].Item1;
                Console.WriteLine(string.Format(System.Globalization.CultureInfo.InvariantCulture, "{0}: {1} pontos, {2} poligonos, caixa {3:0.0000},{4:0.0000},{5:0.0000} .. {6:0.0000},{7:0.0000},{8:0.0000}; na origem {9}; aresta mediana {10:0.00000}, maiores {11}",
                    me.Name, n, me.Polys.Count, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], zero, median,
                    string.Join(" ", sorted.Take(4).Select(e => e.Item1.ToString("0.0000", System.Globalization.CultureInfo.InvariantCulture) + "(" + e.Item2 + "-" + e.Item3 + ")"))));
                Console.WriteLine("  matriz da malha: " + string.Join(" ", me.World.Select(x => x.ToString("0.###", System.Globalization.CultureInfo.InvariantCulture))));
                // polygons with points on both sides of the middle (x = 0), far apart: a bridge between the legs
                int bridge = 0, left = 0, right = 0;
                foreach (var poly in me.Polys)
                {
                    double mn = poly.Min(c => me.Pos[c * 3]), mx = poly.Max(c => me.Pos[c * 3]);
                    if (mn < -0.002 && mx > 0.002) bridge++; else if (mx <= 0.002) right++; else left++;
                }
                Console.WriteLine("  poligonos: lado +x " + left + ", lado -x " + right + ", atravessando o meio " + bridge);
            }
            return 0;
        }
        if (cmd == "fbx2obj")  // fbx2obj <file.fbx> <folder>: OBJ + MTL + textures (+ a ZIP of them), e.g. for Mixamo's auto-rigger
        {
            var fbx = FbxModel.Load(args[1]);
            string outDir = args[2], name = Path.GetFileNameWithoutExtension(args[1]);
            Directory.CreateDirectory(outDir);
            var auto = Outfit.AutoTextures(fbx, args[1]);
            var ci = System.Globalization.CultureInfo.InvariantCulture;
            var obj = new StringBuilder(); var mtl = new StringBuilder(); var texs = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            obj.AppendLine("mtllib " + name + ".mtl");
            // Points at the same place welded into one (Caim's FBX gives every triangle its own: Mixamo's auto-rigger
            // saw two thousand loose triangles and failed); meshes named sword/weapon left out (env NT_FBX2OBJ_KEEPALL
            // keeps them).
            var weld = new Dictionary<Tuple<double, double, double>, int>(); var wpos = new List<double[]>();
            Func<double, double, double, int> wid = (x, y, z) => { var key = Tuple.Create(Math.Round(x, 5), Math.Round(y, 5), Math.Round(z, 5)); int id; if (!weld.TryGetValue(key, out id)) { id = wpos.Count; weld[key] = id; wpos.Add(new[] { x, y, z }); } return id; };
            bool keepAll = Environment.GetEnvironmentVariable("NT_FBX2OBJ_KEEPALL") == "1";
            var body = new StringBuilder();
            int tbase = 1, skipped = 0;
            foreach (var me in fbx.Meshes)
            {
                if (!keepAll && System.Text.RegularExpressions.Regex.IsMatch(me.Name, "sword|weapon|blade|katana|gun", System.Text.RegularExpressions.RegexOptions.IgnoreCase)) { skipped++; continue; }
                string tex; auto.TryGetValue(me.Name, out tex);
                string mat = "mat_none";
                if (tex != null && File.Exists(tex))
                {
                    if (!texs.ContainsKey(tex)) { texs[tex] = "mat" + texs.Count; File.Copy(tex, Path.Combine(outDir, Path.GetFileName(tex)), true); mtl.AppendLine("newmtl " + texs[tex]).AppendLine("Kd 1 1 1").AppendLine("map_Kd " + Path.GetFileName(tex)); }
                    mat = texs[tex];
                }
                body.AppendLine("o " + me.Name).AppendLine("usemtl " + mat);
                int tcount = 0;
                foreach (var puv in me.PolyUv) for (int k = 0; k + 1 < puv.Length; k += 2) { body.AppendLine(string.Format(ci, "vt {0} {1}", puv[k], puv[k + 1])); tcount++; }
                int t = tbase;
                for (int pi = 0; pi < me.Polys.Count; pi++)
                {
                    var poly = me.Polys[pi];
                    body.Append("f");
                    for (int k = 0; k < poly.Length; k++) { int c = poly[k]; body.Append(" " + (1 + wid(me.Pos[c * 3], me.Pos[c * 3 + 1], me.Pos[c * 3 + 2])) + "/" + (t + k)); }
                    body.AppendLine();
                    t += me.PolyUv[pi].Length / 2;
                }
                tbase += tcount;
            }
            // With the game (fbx2obj <fbx> <folder> <media>): written as the importer lays it over the father (standing
            // on +Y, facing +Z, his size): the way Mixamo expects a character (2026-11-16: Caim went up facing away
            // and Mixamo built its skeleton backwards, his left side named right).
            Outfit.Alignment al = null;
            if (args.Length > 3)
            {
                Outfit.LastAlign = null;
                Outfit.Build(OpenCpks(args[3]), "NIER030", args[1], Path.Combine(outDir, "_alinhar"), s => { if (s.StartsWith("frente")) Console.WriteLine(s); },
                             new Dictionary<string, string> { { "alignonly", "1" } });
                al = Outfit.LastAlign;
                Console.WriteLine(al != null ? "orientado como o pai (em pe, de frente para +Z)" : "nao deu para orientar: escrito como no arquivo");
            }
            foreach (var p in wpos)
            {
                double[] q = p;
                if (al != null) { var a = al.Apply(new[] { (float)p[0], (float)p[1], (float)p[2] }); q = new double[] { a[0], a[1], a[2] }; }
                obj.AppendLine(string.Format(ci, "v {0} {1} {2}", q[0], q[1], q[2]));
            }
            obj.Append(body);
            Console.WriteLine("pontos soldados: " + wpos.Count + (skipped > 0 ? "; " + skipped + " partes de arma deixadas de fora" : ""));
            if (!texs.Values.Contains("mat_none")) mtl.AppendLine("newmtl mat_none").AppendLine("Kd 0.8 0.8 0.8");
            File.WriteAllText(Path.Combine(outDir, name + ".obj"), obj.ToString());
            File.WriteAllText(Path.Combine(outDir, name + ".mtl"), mtl.ToString());
            Console.WriteLine("OBJ: " + Path.Combine(outDir, name + ".obj") + " (" + fbx.Meshes.Count + " partes, " + texs.Count + " texturas)");
            return 0;
        }
        if (cmd == "hanimfbx")  // hanimfbx <file.fbx> <out dir>: the FBX's animations as master-skeleton files (.hanim)
        {
            var fm = FbxModel.Load(args[1]);
            var sk = Humanoid.SkeletonOf(fm);
            var map = Humanoid.Map(sk);
            Console.WriteLine("ossos: " + Humanoid.Describe(sk, map));
            if (Humanoid.Missing(map) != "") { Console.WriteLine("faltam: " + Humanoid.Missing(map)); return 1; }
            Directory.CreateDirectory(args[2]);
            int i = 0;
            foreach (var take in fm.Takes)
            {
                var clip = Humanoid.FromMotion(sk, Humanoid.MotionOf(sk, take), take.Name);
                string file = Path.Combine(args[2], (fm.Takes.Count == 1 ? Path.GetFileNameWithoutExtension(args[1]) : Path.GetFileNameWithoutExtension(args[1]) + "_" + (i++)) + ".hanim");
                Humanoid.Write(clip, file);
                Console.WriteLine(take.Name + ": " + clip.Frames + " quadros -> " + file);
            }
            if (fm.Takes.Count == 0) Console.WriteLine("nenhuma animacao no FBX");
            return 0;
        }
        if (cmd == "fbxinfo")  // fbxinfo <file.fbx>: what the FBX reader sees (bones, meshes, skin, textures)
        {
            var fm = FbxModel.Load(args[1]);
            Console.WriteLine("FBX " + fm.Version + ": " + fm.Bones.Count + " ossos, " + fm.Meshes.Count + " malhas");
            double[] lo = { 1e9, 1e9, 1e9 }, hi = { -1e9, -1e9, -1e9 }; int pts = 0, skinned = 0, polys = 0;
            foreach (var me in fm.Meshes)
            {
                for (int i = 0; i + 2 < me.Pos.Length; i += 3) for (int k = 0; k < 3; k++) { lo[k] = Math.Min(lo[k], me.Pos[i + k]); hi[k] = Math.Max(hi[k], me.Pos[i + k]); }
                pts += me.Pos.Length / 3; polys += me.Polys.Count;
                if (me.Weights != null) skinned += me.Weights.Count(w => w.Count > 0);
            }
            Console.WriteLine("pontos " + pts + " (com pele " + skinned + "), poligonos " + polys + ", caixa " + string.Join(" ", lo.Select(x => x.ToString("F2"))) + " .. " + string.Join(" ", hi.Select(x => x.ToString("F2"))));
            foreach (var me in fm.Meshes)
            {
                var zs = Enumerable.Range(0, me.Pos.Length / 3).Select(i => me.Pos[i * 3 + 2]).ToList();
                var bonesUsed = me.Weights == null ? 0 : me.Weights.SelectMany(w => w.Keys).Distinct().Count();
                Console.WriteLine("  malha " + me.Name + ": " + me.Pos.Length / 3 + " pontos, " + me.Polys.Count + " poligonos, z " + zs.Min().ToString("F0") + ".." + zs.Max().ToString("F0") + ", " + bonesUsed + " ossos; texturas " + string.Join(", ", me.MatTex.Select(t => t == null ? "-" : Path.GetFileName(t))) + "; pesos max/ponto " + (me.Weights == null ? 0 : me.Weights.Max(w => w.Count)));
            }
            for (int i = 0; i < fm.Bones.Count; i++) { var b = fm.Bones[i]; Console.WriteLine("  " + i + " " + b.Name + " pai " + b.Parent + " em " + b.World[3].ToString("F2") + " " + b.World[7].ToString("F2") + " " + b.World[11].ToString("F2")); }
            return 0;
        }
        var cpks = OpenCpks(media);

        if (cmd == "swap") { Swap(cpks, args[2], args[3], args.Length > 4 ? args[4] : "CHARA"); return 0; }
        if (cmd == "swapfile")  // swapfile <media> <DIR/FILE in the pack> <mod folder> <file in the mod folder>: served in its place
        {
            // 2026-11-19 (Kaine's voice for the father's): any file of the packs, as Swap does the models': its size (and,
            // when bigger, its offset past the pack's end) patched in the table, the lines added to the mod's swap.txt.
            string modDir = args[3], rel = args[4];
            byte[] data = File.ReadAllBytes(Path.Combine(modDir, rel));
            var lines = File.Exists(Path.Combine(modDir, "swap.txt")) ? File.ReadAllLines(Path.Combine(modDir, "swap.txt")).ToList() : new List<string>();
            foreach (var c in cpks)
            {
                var tm = c.Files.FirstOrDefault(f => string.Equals(f.Path, args[2], StringComparison.OrdinalIgnoreCase));
                if (tm == null) continue;
                string cpkName = c.Name.ToLowerInvariant();
                long at = tm.Offset, size = tm.Size, newSize = data.Length;
                var fields = new List<Tuple<long, int, long>> { Tuple.Create(tm.SizeAt, tm.SizeLen, newSize), Tuple.Create(tm.ExtractAt, tm.ExtractLen, newSize) };
                if (newSize > tm.Size)
                {
                    if (tm.OffsetAt < 0) { Console.WriteLine("tabela sem FileOffset"); return 1; }
                    // Past the pack's end and past whatever swap.txt already put there.
                    long free = (c.Length + 0xFFFFF) & ~0xFFFFFL;
                    foreach (var l in lines)
                    {
                        var p = l.Split(' ');
                        if (p.Length == 4 && p[0] == cpkName) free = Math.Max(free, (Convert.ToInt64(p[1], 16) + Convert.ToInt64(p[2], 16) + 0x7FF) & ~0x7FFL);
                    }
                    at = free; size = newSize;
                    fields.Add(Tuple.Create(tm.OffsetAt, tm.OffsetLen, at - tm.OffsetBase));
                }
                foreach (var fld in fields)
                {
                    if (fld.Item1 < 0 || fld.Item2 <= 0) continue;
                    var sb2 = new StringBuilder();
                    for (int k = 0; k < fld.Item2; k++)
                    {
                        byte plain = (byte)(fld.Item3 >> (8 * (fld.Item2 - 1 - k)));
                        byte stored = tm.TocEncrypted ? (byte)(plain ^ Cpk.Key((int)(fld.Item1 - tm.TocTableAt) + k)) : plain;
                        sb2.Append(stored.ToString("X2"));
                    }
                    lines.Add("patch " + cpkName + " " + fld.Item1.ToString("X") + " " + sb2);
                }
                lines.Add(cpkName + " " + at.ToString("X") + " " + size.ToString("X") + " " + rel);
                File.WriteAllLines(Path.Combine(modDir, "swap.txt"), lines);
                Console.WriteLine(args[2] + " <- " + rel + " (" + tm.Size + " -> " + newSize + " bytes" + (at != tm.Offset ? ", depois do fim do pacote" : "") + ")");
                return 0;
            }
            Console.WriteLine("nao achei " + args[2]);
            return 1;
        }  // swap <media> <TARGET=SOURCE,...> <mod folder> [DIR]
        if (cmd == "mdledit")  // mdledit <media> <DIR/NAME> <same|cut|dup> <mod folder>: EditModel checks (rebuild, delete x > 0.1, copy shifted)
        {
            CpkEntry v, p; var c = FindModel(cpks, args[2], out v, out p);
            if (c == null) { Console.WriteLine("nao achei " + args[2]); return 1; }
            var em = EditModel.Load(Path.GetFileNameWithoutExtension(v.Name), c.Read(v), c.Read(p));
            foreach (var b in em.Buffers)
            {
                var ps = Enumerable.Range(0, b.Count).Select(i => em.Pos(b, i)).ToList();
                string box = ps.Count == 0 ? "" : " caixa x " + ps.Min(q => q[0]).ToString("F2") + ".." + ps.Max(q => q[0]).ToString("F2") + " y " + ps.Min(q => q[1]).ToString("F2") + ".." + ps.Max(q => q[1]).ToString("F2");
                Console.WriteLine("  parte " + b.Index + ": " + b.Count + " vertices, " + em.Tris(b).Count / 3 + " faces, stride " + b.Stride + " @" + b.C.ToString("X") + "+" + b.Data.Length.ToString("X") + (b.Drawn ? "" : " (sombra)") + " " + b.Tex + box);
            }
            foreach (var b in em.Buffers)
            {
                var all = new HashSet<int>(Enumerable.Range(0, b.Count));
                if (args[3] == "cut") em.DeleteVertices(b, new HashSet<int>(all.Where(i => em.Pos(b, i)[0] > 0.1f)));
                if (args[3] == "ops" && b.Index == 2)
                {
                    em.Subdivide(b, null);
                    var every = new HashSet<int>(Enumerable.Range(0, b.Count));
                    em.Inflate(b, every, 0.03f); em.Smooth(b, every, 1, 0.3f);
                    Console.WriteLine("  soldados " + em.Weld(b, 0.00001f) + "; parte 2 agora " + b.Count + " v, " + em.Tris(b).Count / 3 + " f");
                }
                if (args[3] == "other" && b.Index == 2)
                {
                    CpkEntry ov, op; var oc = FindModel(cpks, "CHARA/KAINE010", out ov, out op);
                    var other = EditModel.Load("KAINE010", oc.Read(ov), oc.Read(op));
                    var o = other.Buffers.First(x => x.Drawn && x.Count > 1000);
                    var pos = new float[o.Count * 3]; var uv = new float[o.Count * 2];
                    for (int i = 0; i < o.Count; i++) { var q = other.Pos(o, i); var t = other.Uv(o, i); pos[i * 3] = q[0] + 0.7f; pos[i * 3 + 1] = q[1]; pos[i * 3 + 2] = q[2]; uv[i * 2] = t[0]; uv[i * 2 + 1] = t[1]; }
                    em.AddGeometry(b, pos, uv, other.Tris(o).ToArray());
                    Console.WriteLine("  trazidos " + o.Count + " vertices da KAINE010 parte " + o.Index + " (" + o.Tex + ")");
                }
                if (args[3] == "dup" && b.Drawn)
                {
                    foreach (int i in em.Duplicate(b, all)) { var q = em.Pos(b, i); em.SetPos(b, i, q[0] + 0.8f, q[1], q[2]); }
                }
            }
            var mdp = c.Read(p);
            int limit = Lzo.IsContainer(mdp) ? (int)Be.U32(mdp, 0x10) : mdp.Length;
            if (Environment.GetEnvironmentVariable("NT_PLAYAS") != null)
            {   // the edited model as the father's body (the 3D Editor's "Play with this body")
                var ed = em.Build(0, s => Console.WriteLine(s));
                SwapSource[v.Path.ToUpperInvariant()] = ed.Item1; SwapSource[p.Path.ToUpperInvariant()] = ed.Item2;
                Swap(cpks, "NIER030=" + em.Name, args[4], "CHARA");
                SwapSource.Clear();
                return 0;
            }
            var built = em.Build(limit, s => Console.WriteLine(s));
            string dir = args[4];
            Directory.CreateDirectory(Path.Combine(dir, "SWAP"));
            var lines = new List<string>(); var tail = new Dictionary<string, long>();
            string relV = "SWAP\\" + em.Name + "-EDIT.MDV", relP = "SWAP\\" + em.Name + "-EDIT.MDP";
            File.WriteAllBytes(Path.Combine(dir, relV), built.Item1); File.WriteAllBytes(Path.Combine(dir, relP), built.Item2);
            ServeFile(c, v, built.Item1.Length, built.Item1.Length, relV, lines, tail, s => Console.WriteLine(s));
            ServeFile(c, p, built.Item2.Length, built.Item2.Length, relP, lines, tail, s => Console.WriteLine(s));
            File.WriteAllLines(Path.Combine(dir, "swap.txt"), lines);
            Console.WriteLine("MDP " + em.Pix.Length + " -> " + built.Item2.Length + " (limite " + limit + ")");
            return 0;
        }
        if (cmd == "outfit")  // outfit <media> <BASE> <file.obj|file.fbx> <mod folder> [material=texture.png ...]
        {
            var tm = new Dictionary<string, string>();
            foreach (var a in args.Skip(5)) { int e = a.IndexOf('='); if (e > 0) tm[a.Substring(0, e)] = a.Substring(e + 1); }
            Outfit.Build(cpks, args[2], args[3], args[4], s => Console.WriteLine(s), tm);
            return 0;
        }
        if (cmd == "mdlimport") { ImportObj(cpks, args[2], args[3], args[4]); return 0; }  // mdlimport <media> <DIR/NAME> <OBJ folder> <mod folder>
        if (cmd == "swing")  // swing <media> <TARGET> <SOURCE>: per attack both have, where each one's sword tips move fastest (frames in the TARGET's length)
        {
            byte[] tv = null, sv = null;
            foreach (var c in cpks) foreach (var f in c.Files)
                {
                    if (string.Equals(f.Path, "CHARA/" + args[2] + ".MDV", StringComparison.OrdinalIgnoreCase)) tv = c.Read(f);
                    if (string.Equals(f.Path, "CHARA/" + args[3] + ".MDV", StringComparison.OrdinalIgnoreCase)) sv = c.Read(f);
                }
            var tms = new List<Motion>(); var sms = new List<Motion>();
            var tsk = Anim.Load(tv, tms); var ssk = Anim.Load(sv, sms);
            Func<Skeleton, string, int> J = (s, n) => Array.FindIndex(s.Names, x => string.Equals(x, n, StringComparison.OrdinalIgnoreCase));
            // tip = the joint's place + its X axis * length (the blades lie along X, bladedir)
            Func<Skeleton, Motion, float, string, float, float[]> tip = (s, m, fr, joint, len) =>
            {
                int j = J(s, joint); if (j < 0) return null;
                var w = Anim.World(s, m, fr, false)[j];
                return new[] { w[12] + w[0] * len, w[13] + w[1] * len, w[14] + w[2] * len };
            };
            foreach (var tm in tms)
            {
                int u = tm.Name.IndexOf('_'); string suf = tm.Name.Substring(u + 1);
                if (!suf.StartsWith("at_", StringComparison.OrdinalIgnoreCase)) continue;
                var sm = sms.FirstOrDefault(m => m.Name.Substring(m.Name.IndexOf('_') + 1).Equals(suf, StringComparison.OrdinalIgnoreCase));
                if (sm == null) continue;
                Func<Skeleton, Motion, string, float, bool, string> speeds = (s, m, joint, len, stretch) =>
                {
                    var sb = new StringBuilder();
                    float[] last = null; var sp = new List<float>();
                    for (int f = 0; f < tm.Frames; f++)
                    {
                        float fr = stretch ? f * (float)(m.Frames - 1) / Math.Max(1, tm.Frames - 1) : f;
                        var p = tip(s, m, fr, joint, len);
                        if (p == null) return "-";
                        sp.Add(last == null ? 0 : (float)Math.Sqrt((p[0] - last[0]) * (p[0] - last[0]) + (p[1] - last[1]) * (p[1] - last[1]) + (p[2] - last[2]) * (p[2] - last[2])));
                        last = p;
                    }
                    float mx = sp.Max();
                    // the frames where the tip moves at more than half its top speed
                    var fast = Enumerable.Range(0, sp.Count).Where(i => sp[i] > mx * 0.5f).ToList();
                    return "pico " + (mx * 30).ToString("F1") + " m/s, rapidos " + (fast.Count > 0 ? fast.First() + "-" + fast.Last() : "-");
                };
                Console.WriteLine(suf + " (" + tm.Frames + " q.; origem " + sm.Frames + ")");
                Console.WriteLine("   alvo  direita: " + speeds(tsk, tm, "RIGHTWEAPON", 1.5f, false));
                Console.WriteLine("   origem direita: " + speeds(ssk, sm, "RIGHTWEAPON", 0.79f, true) + " | esquerda: " + speeds(ssk, sm, "LEFTWEAPON", 0.79f, true));
            }
            return 0;
        }
        if (cmd == "swing2")  // swing2 <media> <TARGET> <SOURCE>: the swap's converted attacks: where the game's blade (RIGHTWEAPON) is fast
        {
            byte[] tv = null, sv = null;
            foreach (var c in cpks) foreach (var f in c.Files)
                {
                    if (string.Equals(f.Path, "CHARA/" + args[2] + ".MDV", StringComparison.OrdinalIgnoreCase)) tv = c.Read(f);
                    if (string.Equals(f.Path, "CHARA/" + args[3] + ".MDV", StringComparison.OrdinalIgnoreCase)) sv = c.Read(f);
                }
            LoadHitWindows(cpks);
            var orig = new List<Motion>(); var osk = Anim.Load(tv, orig);
            var conv = new List<Motion>(); var csk = Anim.Load(Retargeted(tv, sv, new[] { args[2], args[3] }), conv);
            int rw = Array.FindIndex(osk.Names, x => x == "RIGHTWEAPON");
            foreach (var cm in conv)
            {
                string suf = cm.Name.Substring(cm.Name.IndexOf('_') + 1);
                if (!suf.StartsWith("at_sword", StringComparison.OrdinalIgnoreCase)) continue;
                var om = orig.First(x => x.Name == cm.Name);
                var at = Enumerable.Range(0, cm.Frames).Select(i => (float)i).ToArray();
                Func<Skeleton, Motion, string> fast = (s, m) =>
                {
                    var sp = Anim.TipSpeeds(s, m, rw, at); float mx = sp.Max();
                    var f = Enumerable.Range(0, sp.Length).Where(i => sp[i] > mx * 0.5f).ToList();
                    return "pico " + (mx * 30).ToString("F1") + " m/s em " + Array.IndexOf(sp, mx) + ", rapidos " + f.First() + "-" + f.Last();
                };
                Tuple<int, int> hw; string ws = HitWindows.TryGetValue(suf, out hw) ? hw.Item1 + "-" + hw.Item2 : "?";
                Console.WriteLine(suf + " [acerto " + ws + "]: pai " + fast(osk, om) + " | convertida " + fast(csk, cm));
            }
            return 0;
        }
        if (cmd == "bladedir")  // bladedir <media> <MODEL>: per weapon joint, its vertices' farthest point in the rest pose (the blade)
        {
            foreach (var c in cpks)
            {
                var v = c.Files.FirstOrDefault(x => string.Equals(x.Path, "CHARA/" + args[2] + ".MDV", StringComparison.OrdinalIgnoreCase));
                var p = c.Files.FirstOrDefault(x => string.Equals(x.Path, "CHARA/" + args[2] + ".MDP", StringComparison.OrdinalIgnoreCase));
                if (v == null || p == null) continue;
                var mv = c.Read(v);
                var sk = Anim.Load(mv, new List<Motion>());
                var parts = MdlFile.Load(args[2], mv, c.Read(p)).Parts();
                for (int j = 0; j < sk.Names.Length; j++)
                {
                    if (sk.Names[j].IndexOf("WEAPON", StringComparison.OrdinalIgnoreCase) < 0 && sk.Names[j].IndexOf("HAND", StringComparison.OrdinalIgnoreCase) < 0) continue;
                    var rw = sk.RestWorld[j];
                    float bx = 0, by = 0, bz = 0, bd = 0; int count = 0;
                    foreach (var pt in parts)
                    {
                        if (pt.BI == null) continue;
                        for (int i = 0; i < pt.N; i++)
                        {
                            int best = 0; for (int k = 1; k < 4; k++) if (pt.BW[i * 4 + k] > pt.BW[i * 4 + best]) best = k;
                            if (pt.BI[i * 4 + best] + 1 != j || pt.BW[i * 4 + best] < 0.9f) continue;
                            count++;
                            float dx = pt.V[i * 3] - rw[12], dy = pt.V[i * 3 + 1] - rw[13], dz = pt.V[i * 3 + 2] - rw[14];
                            float d = dx * dx + dy * dy + dz * dz;
                            if (d > bd) { bd = d; bx = dx; by = dy; bz = dz; }
                        }
                    }
                    if (count == 0) continue;
                    float l = (float)Math.Sqrt(bd);
                    // the direction in the joint's own axes (rows of its rest matrix)
                    Func<int, float> ax = a => (bx * rw[a * 4] + by * rw[a * 4 + 1] + bz * rw[a * 4 + 2]) / l;
                    Console.WriteLine(sk.Names[j] + ": " + count + " vertices, ponta a " + l.ToString("F2") + " m, mundo (" + (bx / l).ToString("F2") + " " + (by / l).ToString("F2") + " " + (bz / l).ToString("F2") + "), eixos da junta (" + ax(0).ToString("F2") + " " + ax(1).ToString("F2") + " " + ax(2).ToString("F2") + ")");
                }
                return 0;
            }
            return 1;
        }
        if (cmd == "animconv")  // animconv <media> <TARGET> <SOURCE> <suffix> <out dir>: the converted animation (and both skeletons) as files
        {
            byte[] tv = null, sv = null;
            foreach (var c in cpks) foreach (var f in c.Files)
                {
                    if (string.Equals(f.Path, "CHARA/" + args[2] + ".MDV", StringComparison.OrdinalIgnoreCase)) tv = c.Read(f);
                    if (string.Equals(f.Path, "CHARA/" + args[3] + ".MDV", StringComparison.OrdinalIgnoreCase)) sv = c.Read(f);
                }
            var conv = Retargeted(tv, sv, new[] { args[2], args[3] });
            Directory.CreateDirectory(args[5]);
            foreach (var pr in new[] { Tuple.Create("tgt", conv, args[2]), Tuple.Create("src", sv, args[3]) })
            {
                var top = KpkItems(pr.Item2, 0, pr.Item2.Length); var l = KpkItems(top[2], 0, top[2].Length);
                File.WriteAllBytes(Path.Combine(args[5], pr.Item1 + "_cjf.bin"), l.First(x => x != null && x.Length > 4 && x[0] == 'C' && x[1] == 'J'));
                foreach (var d in KpkItems(l.First(x => x != null && x.Length > 4 && x[0] == 'K' && x[1] == 'P'), 0, l.First(x => x != null && x.Length > 4 && x[0] == 'K' && x[1] == 'P').Length))
                {
                    var m = Motion.Read(d);
                    if (m != null && m.Name.EndsWith(args[4], StringComparison.OrdinalIgnoreCase)) { File.WriteAllBytes(Path.Combine(args[5], pr.Item1 + "_cmf.bin"), d); Console.WriteLine(pr.Item1 + ": " + m.Name + " " + m.Frames + " quadros, " + d.Length + " bytes"); }
                }
            }
            return 0;
        }
        if (cmd == "hanim")  // hanim <media> <MODEL> <out dir> [filter]: the model's animations as master-skeleton files (.hanim)
        {
            foreach (var c in cpks)
                foreach (var f in c.Files)
                    if (string.Equals(f.Path, "CHARA/" + args[2] + ".MDV", StringComparison.OrdinalIgnoreCase))
                    {
                        var motions = new List<Motion>();
                        var sk = Anim.Load(c.Read(f), motions);
                        if (sk == null) { Console.WriteLine("sem esqueleto"); return 1; }
                        var map = Humanoid.Map(sk);
                        Console.WriteLine("ossos: " + Humanoid.Describe(sk, map));
                        if (Humanoid.Missing(map) != "") { Console.WriteLine("faltam: " + Humanoid.Missing(map)); return 1; }
                        // the T-pose's check: each bone along its direction
                        var tl = Humanoid.TPoseLocal(sk, map);
                        Directory.CreateDirectory(args[3]);
                        int n = 0;
                        foreach (var m in motions)
                        {
                            if (args.Length > 4 && m.Name.IndexOf(args[4], StringComparison.OrdinalIgnoreCase) < 0) continue;
                            var clip = Humanoid.FromMotion(sk, m, m.Name);
                            Humanoid.Write(clip, Path.Combine(args[3], m.Name.Replace('^', '-') + ".hanim"));
                            n++;
                        }
                        Console.WriteLine(n + " animacoes em " + args[3]);
                        return 0;
                    }
            return 1;
        }
        if (cmd == "bvhtest")  // bvhtest <media> <MODEL> <file.hanim> <out.bvh>: the clip as BVH on the model's sizes, read back
        {
            foreach (var c in cpks)
                foreach (var f in c.Files)
                    if (string.Equals(f.Path, "CHARA/" + args[2] + ".MDV", StringComparison.OrdinalIgnoreCase))
                    {
                        var sk = Anim.Load(c.Read(f), new List<Motion>());
                        var clip = Humanoid.Read(args[3]);
                        Humanoid.WriteBvh(clip, sk, args[4]);
                        var back = Humanoid.ReadBvh(args[4]);
                        float worst = 0, hip = 0;
                        for (int b = 0; b < Humanoid.Bones.Length; b++)
                            if (clip.Rot[b] != null && back.Rot[b] != null)
                                for (int fr = 0; fr < clip.Frames; fr++)
                                {
                                    var a = clip.Rot[b][fr]; var q = back.Rot[b][fr];
                                    float d = Math.Abs(a[0] * q[0] + a[1] * q[1] + a[2] * q[2] + a[3] * q[3]);
                                    worst = Math.Max(worst, d >= 1 ? 0 : 2 * (float)Math.Acos(d) * 57.3f);
                                }
                        for (int fr = 0; fr < clip.Frames; fr++) for (int k = 0; k < 3; k++) hip = Math.Max(hip, Math.Abs(clip.Hips[fr][k] - back.Hips[fr][k]));
                        Console.WriteLine("BVH " + back.Frames + " quadros, " + back.Rot.Count(r => r != null) + " ossos; maior diferenca " + worst.ToString("F2") + " graus, quadril " + hip.ToString("F4"));
                        return 0;
                    }
            return 1;
        }
        if (cmd == "hanimtest")  // hanimtest <media> <MODEL> <file.hanim> <its animation name>: the clip on the model, read back
        {
            foreach (var c in cpks)
                foreach (var f in c.Files)
                    if (string.Equals(f.Path, "CHARA/" + args[2] + ".MDV", StringComparison.OrdinalIgnoreCase))
                    {
                        var mdv = c.Read(f);
                        var motions = new List<Motion>();
                        var sk = Anim.Load(mdv, motions);
                        var clip = Humanoid.Read(args[3]);
                        byte[] cmf = null; Motion tm = null;
                        int changed;
                        MapMotions(mdv, (s2, m, file) => { if (m.Name.EndsWith(args[4], StringComparison.OrdinalIgnoreCase)) { cmf = file; tm = m; } return file; }, out changed);
                        if (cmf == null) { Console.WriteLine("animacao nao achada"); return 1; }
                        int moved;
                        var outM = Motion.Read(Humanoid.ToCmf(clip, cmf, sk, tm, out moved));
                        var back = Humanoid.FromMotion(sk, outM, "back");
                        float worst = 0;
                        for (int b = 0; b < Humanoid.Bones.Length; b++)
                            if (clip.Rot[b] != null && back.Rot[b] != null)
                                for (int fr = 0; fr < back.Frames; fr++)
                                {
                                    var a = clip.Rot[b][Math.Min(clip.Frames - 1, (int)Math.Round(fr * (clip.Frames - 1.0) / Math.Max(1, back.Frames - 1)))]; var q = back.Rot[b][fr];
                                    float d = Math.Abs(a[0] * q[0] + a[1] * q[1] + a[2] * q[2] + a[3] * q[3]);
                                    worst = Math.Max(worst, d >= 1 ? 0 : 2 * (float)Math.Acos(d) * 57.3f);
                                }
                        var map = Humanoid.Map(sk);
                        var w = Anim.World(sk, outM, 0, false);
                        Func<int, string> P = j => "(" + w[j][12].ToString("F2") + " " + w[j][13].ToString("F2") + " " + w[j][14].ToString("F2") + ")";
                        Console.WriteLine(moved + " juntas; maior diferenca de ida e volta " + worst.ToString("F1") + " graus");
                        Console.WriteLine("quadro 0: ombro E " + P(map[6]) + " mao E " + P(map[8]) + " | ombro D " + P(map[10]) + " mao D " + P(map[12]) + " | quadril " + P(map[0]) + " pe E " + P(map[15]));
                        return 0;
                    }
            return 1;
        }
        if (cmd == "animtest")  // animtest <media> <MODEL>: the skeleton math (Anim.cs) against the file's rest pose
        {
            foreach (var c in cpks)
                foreach (var f in c.Files)
                    if (string.Equals(f.Path, "CHARA/" + args[2] + ".MDV", StringComparison.OrdinalIgnoreCase))
                    {
                        var motions = new List<Motion>();
                        var sk = Anim.Load(c.Read(f), motions);
                        if (sk == null) { Console.WriteLine("sem esqueleto"); return 1; }
                        Console.WriteLine(Anim.Test(sk) + "; " + motions.Count + " animacoes");
                        // Every frame of every animation skinned: a vertex far from the body is a broken pose.
                        var pf = c.Files.FirstOrDefault(x => string.Equals(x.Path, "CHARA/" + args[2] + ".MDP", StringComparison.OrdinalIgnoreCase));
                        if (pf != null)
                        {
                            var mdl = MdlFile.Load(args[2], c.Read(f), c.Read(pf));
                            var parts = mdl.Parts();
                            int bad = 0, frames = 0;
                            foreach (var m in motions)
                                for (int fr = 0; fr < m.Frames; fr++)
                                {
                                    frames++;
                                    var mats = Anim.SkinMatrices(sk, Anim.World(sk, m, fr, false));
                                    float far = 0;
                                    foreach (var pt in parts)
                                    {
                                        var q = Anim.Skin(pt, mats);
                                        for (int i = 0; i + 2 < q.V.Length; i += 3)
                                        {
                                            float d = q.V[i] * q.V[i] + q.V[i + 1] * q.V[i + 1] + q.V[i + 2] * q.V[i + 2];
                                            if (float.IsNaN(d)) d = 1e9f;
                                            if (d > far) far = d;
                                        }
                                    }
                                    if (far > 9f) { bad++; if (bad <= 10) Console.WriteLine("  longe: " + m.Name + " quadro " + fr + " dist " + Math.Sqrt(far).ToString("F1")); }
                                }
                            Console.WriteLine(frames + " quadros, " + bad + " com vertice a mais de 3 m");
                        }
                        foreach (var m in motions.Take(5)) Console.WriteLine("  " + m.Name + " " + m.Frames + " quadros, " + m.Rot.Count + " rot, " + m.Pos.Count + " pos");
                        return 0;
                    }
            Console.WriteLine("nao achei"); return 1;
        }
        if (cmd == "anims")  // anims <media> <MODEL> [hex item]: the joint list and animations of CHARA/<MODEL>.MDV
        {
            CpkEntry e = null; Cpk owner = null;
            string want = args[2].Contains("/") ? args[2] : "CHARA/" + args[2] + ".MDV";
            foreach (var c in cpks) foreach (var f in c.Files) if (string.Equals(f.Path, want, StringComparison.OrdinalIgnoreCase)) { e = f; owner = c; }
            if (e == null) { Console.WriteLine("nao achei"); return 1; }
            var mdv = owner.Read(e);
            var top = KpkItems(mdv, 0, mdv.Length);
            Console.WriteLine("top " + top.Length);
            Action<byte[], string> show = null;
            show = (d, ind) =>
            {
                var sub = d != null && d.Length >= 8 && Encoding.ASCII.GetString(d, 0, 4) == "KPKy" ? KpkItems(d, 0, d.Length) : null;
                if (sub == null) { Console.WriteLine(ind + (d == null ? "null" : Encoding.ASCII.GetString(d, 0, Math.Min(4, d.Length)).Replace('\0', '.') + " " + d.Length + " " + Encoding.ASCII.GetString(d, 4, Math.Min(Environment.GetEnvironmentVariable("NT_ANIMS_LONG") != null ? 72 : 28, d.Length - 4)).Replace('\0', '.'))); return; }
                Console.WriteLine(ind + "KPKy x" + sub.Length);
                for (int i = 0; i < sub.Length && i < 400; i++) show(sub[i], ind + "  [" + i + "] ");
            };
            show(args[2].Contains("/") ? mdv : top[2], "");
            if (args.Length > 3)
            {
                var path = args[3].Split('.').Select(int.Parse).ToArray();
                byte[] d = args[2].Contains("/") ? mdv : top[2];
                foreach (var k in path) d = KpkItems(d, 0, d.Length)[k];
                File.WriteAllBytes(args[2].Replace("/", "_") + "_" + args[3] + ".bin", d);
                for (int o = 0; o < Math.Min(d.Length, 0x200); o += 16)
                {
                    var sb = new StringBuilder(o.ToString("X4") + ": ");
                    for (int k = 0; k < 16 && o + k < d.Length; k++) sb.Append(d[o + k].ToString("X2") + (k % 4 == 3 ? " " : ""));
                    Console.WriteLine(sb);
                }
            }
            return 0;
        }
        if (cmd == "shaders")  // shaders <media> <out dir>: every shader the models and maps carry, for Renut's cache
        {
            Console.WriteLine(ExtractShaders(cpks, args[2]));
            return 0;
        }
        if (cmd == "lzorepack")  // lzorepack <media> <DIR/NAME>: packs the .MDP again and checks it unpacks the same
        {
            foreach (var c in cpks)
            {
                var p = c.Files.FirstOrDefault(f => string.Equals(f.Path, args[2] + ".MDP", StringComparison.OrdinalIgnoreCase));
                if (p == null) continue;
                var d = c.Read(p);
                var hx = new StringBuilder(); for (int z = 0; z < 0x20; z += 4) hx.Append(Be.U32(d, z).ToString("X8") + " ");
                Console.WriteLine("cabecalho " + hx);
                var plain = Lzo.Unpack(d);
                var sw = System.Diagnostics.Stopwatch.StartNew();
                var again = Lzo.Pack(plain, d);
                var back = Lzo.Unpack(again);
                bool same = back.Length == plain.Length;
                for (int i = 0; same && i < plain.Length; i++) same = back[i] == plain[i];
                Console.WriteLine("original " + d.Length + " -> " + again.Length + " bytes, " + Be.U32(again, 0xC) + " blocos, " + sw.ElapsedMilliseconds + " ms, igual=" + same);
                return same ? 0 : 1;
            }
            return 1;
        }
        if (cmd == "joints")  // joints <media> <DIR/NAME>: the skeleton's joints (CJF) by number, with their parents' places
        {
            foreach (var c in cpks)
            {
                var v = c.Files.FirstOrDefault(f => string.Equals(f.Path, args[2] + ".MDV", StringComparison.OrdinalIgnoreCase));
                if (v == null) continue;
                var j = CjfJoints(c.Read(v));
                if (j == null) { Console.WriteLine("sem CJF"); return 1; }
                for (int i = 0; i < j.Count; i++)
                    Console.WriteLine(i + " " + j[i].Item1 + (j[i].Item2 != null && j[i].Item2.Length >= 3 ? string.Format(" pos {0:F3} {1:F3} {2:F3}", j[i].Item2[0], j[i].Item2[1], j[i].Item2[2]) : ""));
                return 0;
            }
            return 1;
        }
        if (cmd == "nodt")  // nodt <media> <DIR/NAME>: the skeleton's nodes (NODT) in order
        {
            foreach (var c in cpks)
            {
                var v = c.Files.FirstOrDefault(f => string.Equals(f.Path, args[2] + ".MDV", StringComparison.OrdinalIgnoreCase));
                if (v == null) continue;
                var raw = c.Read(v);
                var top = KpkItems(raw, 0, raw.Length); var l1 = KpkItems(top[1], 0, top[1].Length); var l11 = KpkItems(l1[1], 0, l1[1].Length);
                if (Environment.GetEnvironmentVariable("NT_TREE") != null)
                {
                    Action<byte[], string> tree = null;
                    tree = (blob, ind) =>
                    {
                        var items = KpkItems(blob, 0, blob.Length);
                        if (items == null) return;
                        for (int i = 0; i < items.Length; i++)
                        {
                            var it = items[i];
                            if (it == null) { Console.WriteLine(ind + "[" + i + "] null"); continue; }
                            string tg = Encoding.ASCII.GetString(it, 0, Math.Min(4, it.Length)).Replace('\0', ' ');
                            int nb = 0, ni = 0;
                            for (int q = 0; q + 4 <= it.Length; q += 4) { if (it[q] == 'B' && it[q + 1] == 'O' && it[q + 2] == 'I' && it[q + 3] == 'F') nb++; if (it[q] == 'I' && it[q + 1] == 'M' && it[q + 2] == 'T' && it[q + 3] == 'X') ni++; }
                            Console.WriteLine(ind + "[" + i + "] " + tg + " " + it.Length + " bytes BOIF=" + nb + " IMTX=" + ni);
                            if (tg == "KPKy" && ind.Length < 8) tree(it, ind + "  ");
                        }
                    };
                    tree(raw, "");
                    return 0;
                }
                if (Environment.GetEnvironmentVariable("NT_CJF") != null)
                {
                    var l2 = KpkItems(top[2], 0, top[2].Length);
                    for (int i = 0; i < l2.Length; i++)
                    {
                        var it = l2[i]; if (it == null) { Console.WriteLine("[" + i + "] null"); continue; }
                        Console.WriteLine("[" + i + "] " + Encoding.ASCII.GetString(it, 0, Math.Min(4, it.Length)) + " " + it.Length + " bytes");
                        if (i == 1) File.WriteAllBytes(Environment.GetEnvironmentVariable("NT_CJF"), it);
                    }
                    return 0;
                }
                var d = l11.First(x => x != null && x.Length >= 4 && Encoding.ASCII.GetString(x, 0, 4) == "NODT");
                var strs = new List<string>();
                for (int p = 8; p + 8 <= d.Length; p += 8 + (int)Be.U32(d, p + 4))
                {
                    string tag = Encoding.ASCII.GetString(d, p, 4);
                    if (tag == "STRB" && Encoding.ASCII.GetString(d, p + 12, 4) == "STRL")
                    {
                        int s = p + 20, e = s + (int)Be.U32(d, p + 16);
                        while (s < e) { int q = s; while (q < e && d[q] != 0) q++; strs.Add(Encoding.ASCII.GetString(d, s, q - s)); s = q + 1; }
                    }
                    else if (tag == "NODE")
                    {
                        uint ni = Be.U32(d, p + 20);
                        int nsz = (int)Be.U32(d, p + 4);
                        Console.WriteLine("NODE " + Be.U32(d, p + 8) + " links " + (int)Be.U32(d, p + 12) + "," + (int)Be.U32(d, p + 16) + " " + (ni < strs.Count ? strs[(int)ni] : "#" + ni) + " size " + nsz + " last " + (int)Be.U32(d, p + 8 + 80) + " m0 " + BeF(d, p + 24).ToString("F2") + " m12 " + BeF(d, p + 24 + 48).ToString("F2") + "," + BeF(d, p + 24 + 52).ToString("F2") + "," + BeF(d, p + 24 + 56).ToString("F2"));
                        if (nsz > 84)
                        {
                            Action<int, int, string> walk = null;
                            walk = (q, end, ind) =>
                            {
                                while (q + 8 <= end)
                                {
                                    string tg = Encoding.ASCII.GetString(d, q, 4); int sz = (int)Be.U32(d, q + 4);
                                    // Groups and clusters: [number, name, -1] then (groups) GRPC [part] records.
                                    Func<uint, string> nm = w => w < strs.Count ? strs[(int)w] : "#" + w;
                                    if (tg == "CLUS") { Console.WriteLine(ind + "CLUS parte " + Be.U32(d, q + 8) + " " + nm(Be.U32(d, q + 12))); q += 8 + sz; continue; }
                                    if (tg == "GRPS")
                                    {
                                        var parts = new List<string>();
                                        for (int g = q + 20; g + 12 <= q + 8 + sz; g += 8 + (int)Be.U32(d, g + 4))
                                            parts.Add(Encoding.ASCII.GetString(d, g, 4) == "GRPC" ? Be.U32(d, g + 8).ToString() : Encoding.ASCII.GetString(d, g, 4));
                                        Console.WriteLine(ind + "GRPS grupo " + Be.U32(d, q + 8) + " " + nm(Be.U32(d, q + 12)) + " partes: " + string.Join(",", parts));
                                        q += 8 + sz; continue;
                                    }
                                    var hx = new StringBuilder();
                                    for (int z = q + 8; z < q + 8 + Math.Min(sz, 48) && z + 4 <= d.Length; z += 4) { uint w = Be.U32(d, z); hx.Append(w < strs.Count && w > 2 ? "[" + strs[(int)w] + "] " : w.ToString("X") + " "); }
                                    Console.WriteLine(ind + tg + " size " + sz + ": " + hx);
                                    if (tg == "INST" || tg == "MESC" || tg == "GRPS") walk(q + (tg == "MESC" || tg == "GRPS" ? 16 : 8), q + 8 + sz, ind + "  ");
                                    q += 8 + sz;
                                }
                            };
                            walk(p + 8 + 0x54, p + 8 + nsz, "  ");
                        }
                    }
                    else Console.WriteLine(tag + " size " + Be.U32(d, p + 4));
                }
                return 0;
            }
            return 1;
        }
        if (cmd == "findbytes")  // findbytes <media> <hex> [hex...]: game files holding any of these byte strings (big-endian words)
        {
            var pats = args.Skip(2).Select(h => Enumerable.Range(0, h.Length / 2).Select(i => Convert.ToByte(h.Substring(i * 2, 2), 16)).ToArray()).ToList();
            foreach (var c in cpks)
                foreach (var f in c.Files)
                {
                    byte[] d; try { d = c.Read(f); } catch { continue; }
                    if (d == null) continue;
                    foreach (var p in pats)
                        for (int i = 0; i + p.Length <= d.Length; i++)
                        {
                            int k = 0; while (k < p.Length && d[i + k] == p[k]) k++;
                            if (k == p.Length) { Console.WriteLine(f.Path + " @" + i.ToString("X") + " " + BitConverter.ToString(p).Replace("-", "")); }
                        }
                }
            return 0;
        }
        if (cmd == "vari")  // vari <media> <DIR/NAME>: the VARI records (draw variants) word by word
        {
            foreach (var c in cpks)
            {
                var v = c.Files.FirstOrDefault(f => string.Equals(f.Path, args[2] + ".MDV", StringComparison.OrdinalIgnoreCase));
                if (v == null) continue;
                var m = MdlFile.LoadHeader("x", c.Read(v));
                m.ParseMesh();
                foreach (var r in m.Records.Where(r => r.Tag == "VARI" || r.Tag == "LODS" || r.Tag == "LOD " || r.Tag == "MESH"))
                    Console.WriteLine(r.Tag + " size " + r.Size + ": " + string.Join(" ", r.F.Select(x => x.ToString("X8"))));
                Console.WriteLine("tags: " + string.Join(" ", m.Records.Select(r => r.Tag).Distinct()));
                for (int i = 0; i < m.Strings.Count; i++) Console.WriteLine("  str " + i.ToString("X") + " " + m.Strings[i]);
                var pinfo = m.PartInfo();
                for (int i = 0; i < pinfo.Count; i++) Console.WriteLine("  desenho " + i + ": material " + pinfo[i].Item1 + " variante " + pinfo[i].Item2);
                foreach (var r in m.Records.Where(r => r.Tag == "MATE")) Console.WriteLine("MATE " + string.Join(" ", r.F.Take(12).Select(x => x.ToString("X"))));
                return 0;
            }
            return 1;
        }
        if (cmd == "bones")  // bones <media> <DIR/NAME>: strings of the MDV's string list and BONE record
        {
            foreach (var c in cpks)
            {
                var v = c.Files.FirstOrDefault(f => string.Equals(f.Path, args[2] + ".MDV", StringComparison.OrdinalIgnoreCase));
                if (v == null) continue;
                var m = MdlFile.LoadHeader("x", c.Read(v));
                m.ParseMesh();
                Console.WriteLine(m.Strings.Count + " strings");
                foreach (var r in m.Records.Where(r => r.Tag == "BONE")) Console.WriteLine("BONE size " + r.Size + " fields " + r.F.Length + " first: " + string.Join(" ", r.F.Take(24).Select(x => x.ToString("X"))));
                var raw = c.Read(v);
                var meshRec = KpkMesh(raw);
                if (meshRec != null) foreach (var bo in MeshBones(meshRec)) Console.WriteLine("BOIF " + bo.Item2 + " k=" + Be.U32(meshRec, bo.Item1 + 4) + " at " + bo.Item1.ToString("X"));
                if (meshRec != null)
                {   // the BONE record: header, then what follows the BOIF list
                    for (int p = 8; p + 8 <= meshRec.Length; p += 8 + (int)Be.U32(meshRec, p + 4))
                    {
                        string tag = Encoding.ASCII.GetString(meshRec, p, 4); int size = (int)Be.U32(meshRec, p + 4);
                        Console.WriteLine("REC " + tag + " @" + p.ToString("X") + " size " + size.ToString("X"));
                        if (tag != "BONE") continue;
                        var sb = new StringBuilder(); for (int q = p; q < p + 32; q += 4) sb.Append(Be.U32(meshRec, q).ToString("X8") + " ");
                        Console.WriteLine("BONEHDR " + sb);
                        int qq = p + 12; while (qq + 16 <= p + 8 + size && Encoding.ASCII.GetString(meshRec, qq, 4) == "BOIF") qq += 8 + (int)Be.U32(meshRec, qq + 4);
                        sb.Clear(); for (int q = qq; q < qq + 96 && q + 4 <= meshRec.Length; q += 4) sb.Append(Be.U32(meshRec, q).ToString("X8") + " ");
                        Console.WriteLine("AFTERBOIF @" + qq.ToString("X") + " " + sb);
                    }
                }
                Console.WriteLine(string.Join(",", m.Strings));
                break;
            }
            return 0;
        }
        if (cmd == "partsdbg")  // partsdbg <media> <DIR/NAME>: why parts are drawn or not
        {
            foreach (var c in cpks)
            {
                var v = c.Files.FirstOrDefault(f => string.Equals(f.Path, args[2] + ".MDV", StringComparison.OrdinalIgnoreCase));
                var p = c.Files.FirstOrDefault(f => string.Equals(f.Path, args[2] + ".MDP", StringComparison.OrdinalIgnoreCase));
                if (v == null || p == null) continue;
                var m = MdlFile.Load("x", c.Read(v), c.Read(p));
                m.ParseMesh();
                Console.WriteLine(m.Records.Count + " records, " + m.Strings.Count + " strings: " + string.Join(" ", m.Records.GroupBy(r => r.Tag).Select(g => g.Key + "x" + g.Count())));
                Console.WriteLine("mats " + m.MaterialTextures().Count + " partinfo " + m.PartInfo().Count + " variants: " + string.Join(",", m.PartInfo().GroupBy(t => t.Item2).Select(g => g.Key + ":" + g.Count())));
                foreach (var h in m.Heaps) Console.WriteLine("heap ix " + h.Recs.Count(r => r.Tag == "IXBF") + " vx " + h.Recs.Count(r => r.Tag == "VXBF") + " va " + h.Recs.Count(r => r.Tag == "VXAR"));
                var drawn = m.Parts();
                Console.WriteLine("parts drawn " + drawn.Count);
                var own = new HashSet<string>(m.Textures().Textures.Select(t => t.Name), StringComparer.OrdinalIgnoreCase);
                foreach (var g in drawn.GroupBy(q => q.Tex).OrderByDescending(g => g.Sum(q => q.F.Length)).Take(int.Parse(Environment.GetEnvironmentVariable("NT_TOP") ?? "0")))
                    Console.WriteLine("  " + (own.Contains(g.Key) ? "own " : "LIB ") + g.Key + " parts=" + g.Count() + " tris=" + g.Sum(q => q.F.Length / 3));
                if (Environment.GetEnvironmentVariable("NT_TOP") != null) { var mt = m.MaterialTextures(); for (int i = 0; i < Math.Min(mt.Count, 12); i++) Console.WriteLine("  mat " + i + ": " + string.Join(", ", mt[i])); }
                int k = 0; foreach (var h in m.Heaps) foreach (var r in h.Recs.Where(r => r.Tag == "VXAR").Take(3)) Console.WriteLine("layout " + (k++) + ": " + string.Join(" ", m.Layout(h, r).Select(a => a.Usage + "." + a.Index + "@" + a.Offset + "f" + a.Format)));
                break;
            }
            return 0;
        }
        if (cmd == "slots")  // slots <media> <DIR/NAME>: LZO container slot table of a .MDP
        {
            foreach (var c in cpks)
            {
                var p = c.Files.FirstOrDefault(f => string.Equals(f.Path, args[2] + ".MDP", StringComparison.OrdinalIgnoreCase));
                if (p == null) continue;
                var d = c.Read(p);
                int slot = (int)Be.U32(d, 8), count = (int)Be.U32(d, 0xC);
                Console.WriteLine("len " + d.Length + " slot " + slot.ToString("X") + " count " + count + " total " + Be.U32(d, 0x10).ToString("X"));
                for (int k = 0; k < count; k++)
                {
                    int h = k == 0 ? 0x20 : k * slot;
                    if (h + 12 > d.Length) { Console.WriteLine(k + " fora"); break; }
                    string r; try { int e; var o = Lzo.Decompress1X(d, h + 12, out e, 0); r = "ok " + o.Length + " fim " + (e - h).ToString("X"); } catch (Exception ex) { r = "ERRO " + ex.Message; }
                    Console.WriteLine(k + " @" + h.ToString("X") + " off=" + Be.U32(d, h).ToString("X") + " size=" + Be.U32(d, h + 4).ToString("X") + " packed=" + Be.U32(d, h + 8).ToString("X") + " " + r + " b0=" + d[h + 12].ToString("X2"));
                }
                break;
            }
            return 0;
        }
        if (cmd == "survey")  // survey <media> [filter]: per model, parts / parts with texture / textures found
        {
            foreach (var c in cpks)
                foreach (var v in c.Files.Where(f => f.Path.EndsWith(".MDV", StringComparison.OrdinalIgnoreCase)))
                {
                    if (args.Length > 2 && v.Path.IndexOf(args[2], StringComparison.OrdinalIgnoreCase) < 0) continue;
                    var p = c.Files.FirstOrDefault(f => string.Equals(f.Path, v.Path.Substring(0, v.Path.Length - 4) + ".MDP", StringComparison.OrdinalIgnoreCase));
                    if (p == null) continue;
                    try
                    {
                        var m = MdlFile.Load(Path.GetFileNameWithoutExtension(v.Name), c.Read(v), c.Read(p));
                        var texs = m.Textures().Textures.Select(t => t.Name).ToList();
                        var parts = m.Parts();
                        var mats = m.MaterialTextures(); var pm = m.PartMaterials();
                        int miss = parts.Count(q => !texs.Contains(q.Tex));
                        if (Environment.GetEnvironmentVariable("NT_MISS") != null) foreach (var q in parts.Where(q => !texs.Contains(q.Tex)).Select(q => q.Tex).Distinct()) Console.WriteLine("   falta " + q);
                        Console.WriteLine(v.Path + " texs="+ texs.Count + " mats=" + mats.Count + " prims=" + pm.Count + " parts=" + parts.Count + " partsMissingTex=" + miss + (parts.Count == 0 ? " NOPARTS" : ""));
                    }
                    catch (Exception e) { Console.WriteLine(v.Path + " ERRO " + e.Message); }
                }
            return 0;
        }
        if (cmd == "mdl" || cmd == "mdlobj" || cmd == "mdlview" || cmd == "mdlrender")  // mdl <media> <DIR/NAME> | mdlobj <media> <DIR/NAME> <out>
        {
            MdlFile mdl = null;
            foreach (var c in cpks)
            {
                var v = c.Files.FirstOrDefault(f => string.Equals(f.Path, args[2] + ".MDV", StringComparison.OrdinalIgnoreCase));
                var p = c.Files.FirstOrDefault(f => string.Equals(f.Path, args[2] + ".MDP", StringComparison.OrdinalIgnoreCase));
                if (v != null && p != null) { mdl = MdlFile.Load(Path.GetFileNameWithoutExtension(v.Name), c.Read(v), c.Read(p)); break; }
            }
            if (mdl == null) { Console.WriteLine("nao achei " + args[2]); return 1; }
            Console.WriteLine(mdl.Summary());
            if (Environment.GetEnvironmentVariable("NT_MATS") != null) Console.WriteLine(mdl.DumpMaterials());
            if (cmd == "mdlrender")  // textured software render (front and back) -> png
            {
                var texPack = mdl.Textures();
                var texImg = new Dictionary<string, Tuple<byte[], int, int>>();
                foreach (var t in texPack.Textures) if (t.Supported) texImg[t.Name] = Tuple.Create(texPack.Decode(t), t.Width, t.Height);
                var parts = new List<Tuple<string, List<float[]>, List<float[]>, List<int[]>>>();  // tex, v, vt, faces
                foreach (var kv in mdl.ExportObj())
                {
                    if (!kv.Key.EndsWith(".obj")) continue;
                    string tex = null; var vs = new List<float[]>(); var vts = new List<float[]>(); var fs = new List<int[]>();
                    foreach (var line in kv.Value.Split('\n'))
                    {
                        var p = line.Trim().Split(' ');
                        var ci = System.Globalization.CultureInfo.InvariantCulture;
                        if (p[0] == "usemtl") tex = p[1];
                        else if (p[0] == "v") vs.Add(new[] { float.Parse(p[1], ci), float.Parse(p[2], ci), float.Parse(p[3], ci) });
                        else if (p[0] == "vt") vts.Add(new[] { float.Parse(p[1], ci), float.Parse(p[2], ci) });
                        else if (p[0] == "f" && p.Length >= 4) fs.Add(new[] { int.Parse(p[1].Split('/')[0]) - 1, int.Parse(p[2].Split('/')[0]) - 1, int.Parse(p[3].Split('/')[0]) - 1 });
                    }
                    if (tex != null && vts.Count > 0) parts.Add(Tuple.Create(tex, vs, vts, fs));  // untextured parts are shadow casters
                }
                float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
                foreach (var pt in parts) foreach (var v in pt.Item2) { x0 = Math.Min(x0, v[0]); x1 = Math.Max(x1, v[0]); y0 = Math.Min(y0, v[1]); y1 = Math.Max(y1, v[1]); }
                float s = 640f / Math.Max(0.001f, y1 - y0);
                int W = 1500, H = 700;
                var pix = new byte[W * H * 4];
                for (int i = 0; i < pix.Length; i += 4) { pix[i] = 30; pix[i + 1] = 30; pix[i + 2] = 34; pix[i + 3] = 255; }
                for (int view = 0; view < 2; view++)
                {
                    var zb = new float[W * H];
                    for (int i = 0; i < zb.Length; i++) zb[i] = -1e9f;
                    int ox = view == 0 ? 80 : 780;
                    foreach (var pt in parts)
                    {
                        Tuple<byte[], int, int> img = null;
                        if (pt.Item1 != null) texImg.TryGetValue(pt.Item1, out img);
                        foreach (var f in pt.Item4)
                        {
                            if (f[0] >= pt.Item2.Count || f[1] >= pt.Item2.Count || f[2] >= pt.Item2.Count) continue;
                            var A = pt.Item2[f[0]]; var B = pt.Item2[f[1]]; var C = pt.Item2[f[2]];
                            float sx = view == 0 ? 1 : -1, sz = view == 0 ? 1 : -1;
                            float ax = ox + (sx * A[0] - (view == 0 ? x0 : -x1)) * s, ay = 660 - (A[1] - y0) * s;
                            float bx = ox + (sx * B[0] - (view == 0 ? x0 : -x1)) * s, by = 660 - (B[1] - y0) * s;
                            float cx = ox + (sx * C[0] - (view == 0 ? x0 : -x1)) * s, cy = 660 - (C[1] - y0) * s;
                            int minx = (int)Math.Max(0, Math.Floor(Math.Min(ax, Math.Min(bx, cx)))), maxx = (int)Math.Min(W - 1, Math.Ceiling(Math.Max(ax, Math.Max(bx, cx))));
                            int miny = (int)Math.Max(0, Math.Floor(Math.Min(ay, Math.Min(by, cy)))), maxy = (int)Math.Min(H - 1, Math.Ceiling(Math.Max(ay, Math.Max(by, cy))));
                            float den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
                            if (Math.Abs(den) < 1e-6f) continue;
                            for (int py = miny; py <= maxy; py++)
                                for (int px = minx; px <= maxx; px++)
                                {
                                    float w0 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / den;
                                    float w1 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / den;
                                    float w2 = 1 - w0 - w1;
                                    if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                                    float z = sz * (w0 * A[2] + w1 * B[2] + w2 * C[2]);
                                    int o = py * W + px;
                                    if (z < zb[o]) continue;
                                    byte r = 160, g = 160, bl = 170;
                                    if (img != null && pt.Item3.Count > f[2])
                                    {
                                        float u = w0 * pt.Item3[f[0]][0] + w1 * pt.Item3[f[1]][0] + w2 * pt.Item3[f[2]][0];
                                        float v = w0 * pt.Item3[f[0]][1] + w1 * pt.Item3[f[1]][1] + w2 * pt.Item3[f[2]][1];
                                        int tx = ((int)Math.Floor(u * img.Item2) % img.Item2 + img.Item2) % img.Item2, ty = ((int)Math.Floor(v * img.Item3) % img.Item3 + img.Item3) % img.Item3;
                                        int q = (ty * img.Item2 + tx) * 4;
                                        r = img.Item1[q]; g = img.Item1[q + 1]; bl = img.Item1[q + 2];
                                    }
                                    zb[o] = z;
                                    pix[o * 4] = r; pix[o * 4 + 1] = g; pix[o * 4 + 2] = bl; pix[o * 4 + 3] = 255;
                                }
                        }
                    }
                }
                PngIo.Save(args[3], pix, W, H);
                Console.WriteLine("ok " + args[3]);
                return 0;
            }
            if (cmd == "mdlview")  // wireframe front / side / top views -> png
            {
                var tris = mdl.Triangles();
                float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
                foreach (var t in tris) for (int k = 0; k < 3; k++) { x0 = Math.Min(x0, t[k * 3]); x1 = Math.Max(x1, t[k * 3]); y0 = Math.Min(y0, t[k * 3 + 1]); y1 = Math.Max(y1, t[k * 3 + 1]); z0 = Math.Min(z0, t[k * 3 + 2]); z1 = Math.Max(z1, t[k * 3 + 2]); }
                Console.WriteLine(tris.Count + " triangulos, x " + x0 + ".." + x1 + " y " + y0 + ".." + y1 + " z " + z0 + ".." + z1);
                float s = 640f / Math.Max(0.001f, y1 - y0);
                using (var bmp = new Bitmap(1500, 700))
                using (var g = Graphics.FromImage(bmp))
                using (var pen = new Pen(Color.FromArgb(150, 120, 200, 255)))
                {
                    g.Clear(Color.Black);
                    Action<int, int, int, float, float> draw = (ax, ay, ox, mx, my) =>
                    {
                        foreach (var t in tris)
                        {
                            var P = new PointF[3];
                            for (int k = 0; k < 3; k++) P[k] = new PointF(ox + (t[k * 3 + ax] - mx) * s, 660 - (t[k * 3 + ay] - my) * s);
                            g.DrawPolygon(pen, P);
                        }
                    };
                    draw(0, 1, 40, x0, y0);   // front: x,y
                    draw(2, 1, 540, z0, y0);  // side: z,y
                    draw(0, 2, 1040, x0, z0); // top: x,z
                    bmp.Save(args[3]);
                }
                return 0;
            }
            if (cmd == "mdlobj")
            {
                Directory.CreateDirectory(args[3]);
                foreach (var kv in mdl.ExportObj()) { File.WriteAllText(Path.Combine(args[3], kv.Key), kv.Value); Console.WriteLine("  " + kv.Key); }
                var tex = mdl.Textures();
                foreach (var t in tex.Textures)
                    if (t.Supported) PngIo.Save(Path.Combine(args[3], Safe(t.Name) + ".png"), tex.Decode(t), t.Width, t.Height);
            }
            return 0;
        }
        if (cmd == "cat")
        {
            foreach (var c in cpks)
                foreach (var f in c.Files)
                    if (string.Equals(f.Path, args[2], StringComparison.OrdinalIgnoreCase)) { File.WriteAllBytes(args[3], c.Read(f)); Console.WriteLine("ok " + f.Size + " -> " + f.ExtractSize); return 0; }
            Console.WriteLine("nao achei"); return 1;
        }
        if (cmd == "grep")  // grep <media> <text>: files whose content (after CRILAYLA / LZO) has the text, ASCII or UTF-16
        {
            var pats = new[] { Encoding.ASCII.GetBytes(args[2]), Encoding.BigEndianUnicode.GetBytes(args[2]), Encoding.Unicode.GetBytes(args[2]) };
            Func<byte[], int> hit = d => { foreach (var p in pats) for (int i = 0; i + p.Length <= d.Length; i++) { int k = 0; while (k < p.Length && (d[i + k] | 0x20) == (p[k] | 0x20)) k++; if (k == p.Length) return i; } return -1; };
            foreach (var c in cpks)
                foreach (var f in c.Files)
                {
                    byte[] d; try { d = c.Read(f); } catch { continue; }
                    int at = hit(d);
                    if (at < 0 && d.Length >= 4 && d[0] == 'l' && d[1] == 'z' && d[2] == 'o') { try { var u = Lzo.Unpack(d); if (u != null && (at = hit(u)) >= 0) at = -2 - at; } catch { } }
                    if (at != -1) Console.WriteLine(c.Name + " " + f.Path + (at < -1 ? " (lzo) @" + (-2 - at).ToString("X") : " @" + at.ToString("X")));
                }
            return 0;
        }
        if (cmd == "find")
        {
            foreach (var c in cpks) foreach (var f in c.Files) if (f.Path.IndexOf(args[2], StringComparison.OrdinalIgnoreCase) >= 0) Console.WriteLine(c.Name + " " + f.Path + " " + f.Size + "/" + f.ExtractSize + " @" + f.Offset.ToString("X"));
            return 0;
        }
        if (cmd == "list")
        {
            foreach (var r in Packs(cpks, args.Length > 2 ? args[2] : null))
            {
                Console.WriteLine(r.Dir + "/" + r.Pack.Name + "  (" + r.Pack.Textures.Count + " texturas, 2DP " + r.Pack.Pix.Length + " bytes)");
                foreach (var t in r.Pack.Textures)
                    Console.WriteLine("   " + t.Name + "  " + t.Width + "x" + t.Height + " " + t.FormatName + " tiled=" + t.Tiled + " dados=" + t.DataSize + " base=" + t.BaseSize);
            }
            return 0;
        }
        if (cmd == "raw")  // raw <media> <path part> <out dir>: every game file whose path holds it, as stored (unpacked)
        {
            Directory.CreateDirectory(args[3]);
            int n = 0;
            foreach (var c in cpks) foreach (var f in c.Files)
                    if (f.Path.IndexOf(args[2], StringComparison.OrdinalIgnoreCase) >= 0)
                    { File.WriteAllBytes(Path.Combine(args[3], f.Path.Replace('/', '_')), c.Read(f)); n++; }
            Console.WriteLine(n + " arquivos em " + args[3]);
            return 0;
        }
        if (cmd == "extract")
        {
            string outDir = args[2]; string filter = args.Length > 3 ? args[3] : null;
            int n = 0;
            foreach (var r in Packs(cpks, filter))
            {
                foreach (var t in r.Pack.Textures)
                {
                    if (!t.Supported) { Console.WriteLine("  formato " + t.FormatName + " nao suportado: " + r.Pack.Name + "/" + t.Name); continue; }
                    PngIo.Save(Path.Combine(outDir, r.Dir.Replace('/', Path.DirectorySeparatorChar), r.Pack.Name, Safe(t.Name) + ".png"), r.Pack.Decode(t), t.Width, t.Height);
                    n++;
                }
            }
            Console.WriteLine(n + " imagens");
            return 0;
        }
        if (cmd == "build") { Console.WriteLine(Build(cpks, args[2], args[3])); Console.WriteLine(BuildModels(cpks, args[2], args[3])); return 0; }
        if (cmd == "roundtrip")
        {
            foreach (var r in Packs(cpks, args[2]))
                foreach (var t in r.Pack.Textures)
                {
                    if (!t.Supported) continue;
                    var a = r.Pack.Decode(t);
                    var before = (byte[])r.Pack.Pix.Clone();
                    r.Pack.Encode(t, a);
                    var b = r.Pack.Decode(t);
                    long err = 0; for (int i = 0; i < a.Length; i++) err += Math.Abs(a[i] - b[i]);
                    Console.WriteLine(r.Pack.Name + "/" + t.Name + " " + t.FormatName + " erro medio=" + (err / (double)a.Length).ToString("0.00"));
                    Array.Copy(before, r.Pack.Pix, before.Length);
                }
            return 0;
        }
        return 1;
    }
}
