// LZO1X decompression (the character .MDP files are LZO streams after a small header).
using System;
using System.Collections.Generic;

public static class Lzo
{
    // Decompresses one LZO1X stream from src[pos..]; returns the output and the
    // input position after the end marker.
    public static byte[] Decompress1X(byte[] src, int pos, out int endPos, int expected = 0)
    {
        var o = new List<byte>(expected > 0 ? expected : 1 << 16);
        int ip = pos;
        int state = 0;  // 0: after a match, 1: after a match + 1..3 literals, 2: after a literal run
        int t = src[ip];
        if (t > 17)
        {
            ip++;
            t -= 17;
            for (int i = 0; i < t; i++) o.Add(src[ip++]);
            state = t < 4 ? 1 : 2;
        }
        while (true)
        {
            t = src[ip++];
            int trailing;
            if (t >= 64)  // M2
            {
                int dist = 1 + ((t >> 2) & 7) + (src[ip++] << 3);
                CopyBack(o, dist, (t >> 5) - 1 + 2);
                trailing = t & 3;
            }
            else if (t >= 32)  // M3
            {
                int len = t & 31;
                if (len == 0) { while (src[ip] == 0) { len += 255; ip++; } len += 31 + src[ip++]; }
                len += 2;
                int dist = 1 + ((src[ip] >> 2) | (src[ip + 1] << 6));
                trailing = src[ip] & 3;
                ip += 2;
                CopyBack(o, dist, len);
            }
            else if (t >= 16)  // M4 / end of stream
            {
                int len = t & 7;
                if (len == 0) { while (src[ip] == 0) { len += 255; ip++; } len += 7 + src[ip++]; }
                len += 2;
                int dist = (t & 8) << 11;
                dist += (src[ip] >> 2) | (src[ip + 1] << 6);
                trailing = src[ip] & 3;
                ip += 2;
                if (dist == 0) { endPos = ip; return o.ToArray(); }
                dist += 0x4000;
                CopyBack(o, dist, len);
            }
            else if (state == 0)  // literal run
            {
                int len = t;
                if (len == 0) { while (src[ip] == 0) { len += 255; ip++; } len += 15 + src[ip++]; }
                len += 3;
                for (int i = 0; i < len; i++) o.Add(src[ip++]);
                state = 2;
                continue;
            }
            else if (state == 1)  // M1 after 1..3 trailing literals: length 2
            {
                int dist = 1 + (t >> 2) + (src[ip++] << 2);
                CopyBack(o, dist, 2);
                trailing = t & 3;
            }
            else  // M1 after a literal run: length 3, distance 2049+
            {
                int dist = 1 + 0x800 + (t >> 2) + (src[ip++] << 2);
                CopyBack(o, dist, 3);
                trailing = t & 3;
            }
            for (int i = 0; i < trailing; i++) o.Add(src[ip++]);
            state = trailing > 0 ? 1 : 0;
        }
    }

    // "lzo\0" container: header [magic][?][slot size][slot count][total size]..., then one
    // slot per slot-size bytes: [output offset][size][packed size] + an LZO1X stream.
    public static bool IsContainer(byte[] d) { return d.Length > 0x30 && d[0] == 0x6C && d[1] == 0x7A && d[2] == 0x6F && d[3] == 0; }

    public static byte[] Unpack(byte[] d)
    {
        int slot = (int)Be.U32(d, 8), count = (int)Be.U32(d, 0xC), total = (int)Be.U32(d, 0x10);
        var res = new byte[total];
        for (int k = 0; k < count; k++)
        {
            int h = k == 0 ? 0x20 : k * slot;
            if (h + 12 > d.Length) break;
            int off = (int)Be.U32(d, h), size = (int)Be.U32(d, h + 4), packed = (int)Be.U32(d, h + 8);
            if (size <= 0 || off + size > total) continue;
            if (packed >= size)  // a stream that did not shrink (our packer) or stored bytes
            {
                try { int e2; var o2 = Decompress1X(d, h + 12, out e2, size); if (o2.Length == size) { Array.Copy(o2, 0, res, off, size); continue; } } catch (Exception) { }
                Array.Copy(d, h + 12, res, off, Math.Min(size, d.Length - h - 12)); continue;
            }
            int end;
            var o = Decompress1X(d, h + 12, out end, size);
            Array.Copy(o, 0, res, off, Math.Min(o.Length, size));
        }
        return res;
    }

    static void CopyBack(List<byte> o, int dist, int len)
    {
        int s = o.Count - dist;
        if (s < 0) throw new Exception("LZO: distancia invalida");
        for (int i = 0; i < len; i++) o.Add(o[s + i]);
    }

    // LZO1X-1 compression (the reference algorithm: hash of 4 bytes, M2/M3/M4 matches, literal runs).
    public static byte[] Compress1X(byte[] src, int start, int len)
    {
        var op = new List<byte>(len / 2 + 64);
        var dict = new int[1 << 14];
        for (int i = 0; i < dict.Length; i++) dict[i] = -1;
        int end = start + len, ipEnd = end - 20, ip = start + 4, ii = start;
        Action<int, int> literals = (from, n) =>
        {
            if (n == 0) return;
            if (op.Count == 0 && n <= 238) op.Add((byte)(17 + n));
            else if (n <= 3) op[op.Count - 2] |= (byte)n;
            else if (n <= 18) op.Add((byte)(n - 3));
            else { int tt = n - 18; op.Add(0); while (tt > 255) { tt -= 255; op.Add(0); } op.Add((byte)tt); }
            for (int k = 0; k < n; k++) op.Add(src[from + k]);
        };
        if (len > 20)
        {
            while (ip < ipEnd)
            {
                uint dv = (uint)(src[ip] | src[ip + 1] << 8 | src[ip + 2] << 16 | src[ip + 3] << 24);
                int di = (int)(((dv * 0x1824429Du) >> 18) & 0x3FFF);
                int mp = dict[di]; dict[di] = ip;
                if (mp < 0 || ip - mp > 0xBFFF || src[mp] != src[ip] || src[mp + 1] != src[ip + 1] || src[mp + 2] != src[ip + 2] || src[mp + 3] != src[ip + 3])
                {
                    ip += 1 + ((ip - ii) >> 5);
                    continue;
                }
                literals(ii, ip - ii);
                int mlen = 4;
                while (ip + mlen < end && src[mp + mlen] == src[ip + mlen]) mlen++;
                int moff = ip - mp;
                ip += mlen; ii = ip;
                if (mlen <= 8 && moff <= 0x800)
                {
                    moff -= 1;
                    op.Add((byte)(((mlen - 1) << 5) | ((moff & 7) << 2)));
                    op.Add((byte)(moff >> 3));
                }
                else if (moff <= 0x4000)
                {
                    moff -= 1;
                    if (mlen <= 33) op.Add((byte)(32 | (mlen - 2)));
                    else { int m = mlen - 33; op.Add(32); while (m > 255) { m -= 255; op.Add(0); } op.Add((byte)m); }
                    op.Add((byte)((moff << 2) & 0xFF)); op.Add((byte)(moff >> 6));
                }
                else
                {
                    moff -= 0x4000;
                    if (mlen <= 9) op.Add((byte)(16 | ((moff >> 11) & 8) | (mlen - 2)));
                    else { int m = mlen - 9; op.Add((byte)(16 | ((moff >> 11) & 8))); while (m > 255) { m -= 255; op.Add(0); } op.Add((byte)m); }
                    op.Add((byte)((moff << 2) & 0xFF)); op.Add((byte)((moff >> 6) & 0xFF));
                }
            }
        }
        literals(ii, end - ii);
        op.Add(0x11); op.Add(0); op.Add(0);
        return op.ToArray();
    }

    // A "lzo\0" container for data, laid out like 'like' (same header words, same slot size): each slot
    // holds as many 0x2000-byte steps of output as its packed stream fits in.
    public static byte[] Pack(byte[] data, byte[] like)
    {
        int slot = (int)Be.U32(like, 8);
        var slots = new List<byte[]>(); var offs = new List<int>(); var sizes = new List<int>();
        int at = 0;
        while (at < data.Length)
        {
            int room = (slots.Count == 0 ? slot - 0x2C : slot - 12);
            int n = Math.Min(data.Length - at, 0x40000);
            byte[] packed;
            while (true)
            {
                packed = Compress1X(data, at, n);
                if (packed.Length <= room && (packed.Length < n || n <= 0x2000)) break;  // a stream that does not shrink is still a valid stream
                if (n <= 0x2000) throw new Exception("LZO: bloco nao cabe");
                n = Math.Max(0x2000, (n * 3 / 4) & ~0x1FFF);
            }
            slots.Add(packed); offs.Add(at); sizes.Add(n);
            if (Environment.GetEnvironmentVariable("NT_LZODBG") != null) Console.WriteLine("  bloco " + slots.Count + " @" + at.ToString("X") + " " + n.ToString("X") + " -> " + packed.Length.ToString("X"));
            at += n;
        }
        var res = new byte[slots.Count * slot];
        Array.Copy(like, 0, res, 0, 0x20);
        Be.Put32(res, 0xC, (uint)slots.Count); Be.Put32(res, 0x10, (uint)data.Length);
        int most = 0; foreach (int z in sizes) most = Math.Max(most, z);
        Be.Put32(res, 0x14, (uint)most);  // the largest slot output: the game unpacks each slot into a buffer this big
        for (int k = 0; k < slots.Count; k++)
        {
            int h = k == 0 ? 0x20 : k * slot;
            Be.Put32(res, h, (uint)offs[k]); Be.Put32(res, h + 4, (uint)sizes[k]); Be.Put32(res, h + 8, (uint)slots[k].Length);
            slots[k].CopyTo(res, h + 12);
        }
        return res;
    }
}