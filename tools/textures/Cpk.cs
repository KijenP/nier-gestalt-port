// CRI CPK reader + NieR 2D pack (.2DV/.2DP) parser, shared by NierTextureTool.
// Format notes: see NOTAS.md ("2DV = KPKy ...").
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

public class CpkEntry
{
    public string Dir, Name;
    public long Offset, Size, ExtractSize;
    public long SizeAt = -1, ExtractAt = -1, OffsetAt = -1;  // absolute file offsets of fields in the pack's table (as stored)
    public int SizeLen, ExtractLen, OffsetLen;
    public long OffsetBase;  // FileOffset in the table is relative to this
    public bool TocEncrypted; public long TocTableAt;
    public string Path { get { return Dir + "/" + Name; } }
}

public static class Be
{
    public static uint U32(byte[] d, int o) { return (uint)(d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3]); }
    public static int U16(byte[] d, int o) { return d[o] << 8 | d[o + 1]; }
    public static ulong U64(byte[] d, int o) { return (ulong)U32(d, o) << 32 | U32(d, o + 4); }
    public static void Put32(byte[] d, int o, uint v) { d[o] = (byte)(v >> 24); d[o + 1] = (byte)(v >> 16); d[o + 2] = (byte)(v >> 8); d[o + 3] = (byte)v; }
}

public class Cpk
{
    public string FilePath;      // the .cpk, or the .iso it is inside
    public long Base;            // where the .cpk starts in FilePath (0 for a loose .cpk)
    public long Length;          // the .cpk's size
    public string Name;          // "layer1.cpk"
    public List<CpkEntry> Files = new List<CpkEntry>();

    class Row { public Dictionary<string, ulong> Num = new Dictionary<string, ulong>(); public Dictionary<string, string> Str = new Dictionary<string, string>(); public Dictionary<string, int[]> Pos = new Dictionary<string, int[]>(); }
    static bool lastEncrypted, lastChunkEncrypted; static long lastChunkTableAt;

    // The byte of the table's cipher at position i (an encrypted table is stored XORed with this stream).
    public static byte Key(int i) { uint m = 0x655F; for (int k = 0; k < i; k++) m *= 0x4115; return (byte)m; }

    static List<Row> ParseUtf(byte[] t)
    {
        var rows = new List<Row>();
        lastEncrypted = false;
        if (t.Length < 32) return rows;
        if (!(t[0] == '@' && t[1] == 'U' && t[2] == 'T' && t[3] == 'F'))
        {
            uint m = 0x655F;
            for (int i = 0; i < t.Length; i++) { t[i] ^= (byte)m; m *= 0x4115; }
            lastEncrypted = true;
            if (!(t[0] == '@' && t[1] == 'U' && t[2] == 'T' && t[3] == 'F')) return rows;
        }
        int rowsO = (int)Be.U32(t, 8) + 8, strO = (int)Be.U32(t, 12) + 8;
        int ncol = Be.U16(t, 24), rowLen = Be.U16(t, 26); uint nrow = Be.U32(t, 28);
        Func<uint, string> str = off =>
        {
            int s = strO + (int)off, e = s;
            while (e < t.Length && t[e] != 0) e++;
            return Encoding.UTF8.GetString(t, s, e - s);
        };
        Func<byte, int, Tuple<ulong, string, int>> read = (type, o) =>
        {
            switch (type & 0x0F)
            {
                case 0: case 1: return Tuple.Create((ulong)t[o], (string)null, 1);
                case 2: case 3: return Tuple.Create((ulong)Be.U16(t, o), (string)null, 2);
                case 4: case 5: return Tuple.Create((ulong)Be.U32(t, o), (string)null, 4);
                case 6: case 7: return Tuple.Create(Be.U64(t, o), (string)null, 8);
                case 8: return Tuple.Create((ulong)Be.U32(t, o), (string)null, 4);
                case 0xA: return Tuple.Create(0UL, str(Be.U32(t, o)), 4);
                case 0xB: return Tuple.Create(0UL, (string)null, 8);
            }
            return Tuple.Create(0UL, (string)null, 0);
        };
        var cols = new List<Tuple<byte, string, ulong, string>>();
        int p = 32;
        for (int c = 0; c < ncol && p + 5 <= t.Length; c++)
        {
            byte flags = t[p]; string name = str(Be.U32(t, p + 1)); p += 5;
            ulong num = 0; string s = null;
            if ((flags & 0xF0) == 0x30) { var r = read(flags, p); num = r.Item1; s = r.Item2; p += r.Item3; }
            cols.Add(Tuple.Create(flags, name, num, s));
        }
        for (uint r = 0; r < nrow; r++)
        {
            int q = rowsO + (int)r * rowLen;
            var row = new Row();
            foreach (var col in cols)
            {
                ulong num = col.Item3; string s = col.Item4;
                if ((col.Item1 & 0xF0) == 0x50)
                {
                    if (q + 8 > t.Length) break;
                    var rv = read(col.Item1, q); num = rv.Item1; s = rv.Item2; row.Pos[col.Item2] = new[] { q, rv.Item3 }; q += rv.Item3;
                }
                else if ((col.Item1 & 0xF0) != 0x30) continue;
                if ((col.Item1 & 0x0F) == 0xA) row.Str[col.Item2] = s; else row.Num[col.Item2] = num;
            }
            rows.Add(row);
        }
        return rows;
    }

    [ThreadStatic] static long cpkBase;
    static List<Row> ReadChunk(FileStream f, long offset)
    {
        var head = new byte[16];
        f.Position = cpkBase + offset; f.Read(head, 0, 16);
        long size = BitConverter.ToInt64(head, 8);
        if (size <= 0 || size > (64 << 20)) return new List<Row>();
        var table = new byte[size];
        f.Read(table, 0, (int)size);
        var rows = ParseUtf(table);
        lastChunkEncrypted = lastEncrypted; lastChunkTableAt = offset + 16;
        return rows;
    }

    public static Cpk Open(string path) { return Open(path, 0, new FileInfo(path).Length, System.IO.Path.GetFileName(path)); }

    public static Cpk Open(string path, long at, long length, string name)
    {
        var cpk = new Cpk { FilePath = path, Base = at, Length = length, Name = name };
        cpkBase = at;
        using (var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            var h = ReadChunk(f, 0);
            if (h.Count == 0 || !h[0].Num.ContainsKey("TocOffset")) throw new Exception("CPK sem TocOffset: " + path);
            ulong toc = h[0].Num["TocOffset"];
            ulong content = h[0].Num.ContainsKey("ContentOffset") ? h[0].Num["ContentOffset"] : toc;
            long bas = (long)Math.Min(toc, content);
            var tocRows = ReadChunk(f, (long)toc);
            bool tocEnc = lastChunkEncrypted; long tocAt = lastChunkTableAt;
            foreach (var r in tocRows)
            {
                var e = new CpkEntry();
                e.TocEncrypted = tocEnc; e.TocTableAt = tocAt;
                int[] ps;
                if (r.Pos.TryGetValue("FileSize", out ps)) { e.SizeAt = tocAt + ps[0]; e.SizeLen = ps[1]; }
                if (r.Pos.TryGetValue("ExtractSize", out ps)) { e.ExtractAt = tocAt + ps[0]; e.ExtractLen = ps[1]; }
                if (r.Pos.TryGetValue("FileOffset", out ps)) { e.OffsetAt = tocAt + ps[0]; e.OffsetLen = ps[1]; }
                e.OffsetBase = bas;
                e.Dir = r.Str.ContainsKey("DirName") ? r.Str["DirName"] : "";
                e.Name = r.Str.ContainsKey("FileName") ? r.Str["FileName"] : "";
                e.Offset = (long)r.Num["FileOffset"] + bas;
                e.Size = (long)r.Num["FileSize"];
                e.ExtractSize = r.Num.ContainsKey("ExtractSize") ? (long)r.Num["ExtractSize"] : e.Size;
                cpk.Files.Add(e);
            }
        }
        return cpk;
    }

    // CRILAYLA: the stream is read backwards, the last 0x100 bytes of the output
    // are stored raw after the packed data.
    public static byte[] Crilayla(byte[] src)
    {
        if (src.Length < 0x110 || Encoding.ASCII.GetString(src, 0, 8) != "CRILAYLA") throw new Exception("nao e CRILAYLA");
        int size = BitConverter.ToInt32(src, 8), hdr = BitConverter.ToInt32(src, 12);
        var res = new byte[size + 0x100];
        Array.Copy(src, 0x10 + hdr, res, 0, 0x100);
        int inPos = src.Length - 0x100 - 1, outEnd = 0x100 + size - 1;
        int pool = 0, left = 0, produced = 0;
        Func<int, int> bits = n =>
        {
            int v = 0, got = 0;
            while (got < n)
            {
                if (left == 0) { pool = src[inPos--]; left = 8; }
                int take = Math.Min(left, n - got);
                v = (v << take) | ((pool >> (left - take)) & ((1 << take) - 1));
                left -= take; got += take;
            }
            return v;
        };
        int[] vle = { 2, 3, 5, 8 };
        while (produced < size)
        {
            if (bits(1) != 0)
            {
                int off = bits(13) + 3, len = 3, lv = 0;
                bool done = false;
                for (int k = 0; k < 4 && !done; k++) { lv = bits(vle[k]); len += lv; if (lv != (1 << vle[k]) - 1) done = true; }
                if (!done) { do { lv = bits(8); len += lv; } while (lv == 255); }
                for (int i = 0; i < len && produced < size; i++) { res[outEnd - produced] = res[outEnd - produced + off]; produced++; }
            }
            else res[outEnd - produced++] = (byte)bits(8);
        }
        return res;
    }

    public byte[] Read(CpkEntry e)
    {
        if (e.Size != e.ExtractSize) return Crilayla(ReadRaw(e));
        return ReadRaw(e);
    }

    public byte[] ReadRaw(CpkEntry e)
    {
        using (var f = new FileStream(FilePath, FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            var d = new byte[e.Size];
            f.Position = Base + e.Offset;
            int got = 0;
            while (got < d.Length) { int n = f.Read(d, got, d.Length - got); if (n <= 0) break; got += n; }
            return d;
        }
    }
}
