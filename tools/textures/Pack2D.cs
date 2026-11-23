// NieR 2D pack: <name>.2DV ("KPKy" container whose first part is a HEAP of "TX2D"
// records) + <name>.2DP (the texture bytes, Xbox 360 tiled).
using System;
using System.Collections.Generic;
using System.Text;

public class Tex2D
{
    public string Name;
    public int EntryOffset;   // in the .2DV
    public int HeaderOffset;  // 0x40-byte header in the .2DV (fetch constant at +0x1C)
    public int DataOffset, DataSize;
    public uint F0, F1, F2, F3, F4, F5;
    public int MaxMip { get { return (int)((F4 >> 6) & 0xF); } }
    public bool PackedMips { get { return ((F5 >> 11) & 1) != 0; } }
    // With packed mips, a texture 16 texels or less on a side (rounded up to a power of two) is stored
    // as a packed mip tail: the base level starts 16 texels along the shorter side of the storage
    // (the console's GetMipTailLevelOffsetCoords for level 0). Offset in blocks (4x4 for DXT).
    public void TailOffset(out int xBlocks, out int yBlocks)
    {
        xBlocks = yBlocks = 0;
        int pw = 1, ph = 1;
        while (pw < Width) pw <<= 1;
        while (ph < Height) ph <<= 1;
        if (!PackedMips || (pw > 16 && ph > 16)) return;
        int block = Format == 0x06 ? 1 : 4;
        if (ph < pw) yBlocks = 16 / block; else xBlocks = 16 / block;
    }
    public int Format, Endian, Width, Height, Pitch;
    public bool Tiled;
    public string FormatName
    {
        get
        {
            switch (Format) { case 0x12: return "DXT1"; case 0x13: return "DXT3"; case 0x14: return "DXT5"; case 0x3C: return "CTX1"; case 0x06: return "ARGB8888"; default: return "0x" + Format.ToString("X"); }
        }
    }
    public bool Supported { get { return Format == 0x12 || Format == 0x13 || Format == 0x14 || Format == 0x06 || Format == 0x3C; } }
    public int BaseSize
    {
        get
        {
            int bpb = Format == 0x12 || Format == 0x3C ? 8 : (Format == 0x06 ? 4 : 16);
            if (Format == 0x06) return Pitch32() * Height * 4;
            int bw = (Width + 3) / 4, bh = (Height + 3) / 4;
            return bw * bh * bpb;
        }
    }
    int Pitch32() { return Math.Max(Pitch, Width); }
    public bool HasMips { get { return DataSize > BaseSize * 5 / 4 + 4096 && false; } }
}

public class Pack2D
{
    public string Name;
    public byte[] Vir, Pix;
    public List<Tex2D> Textures = new List<Tex2D>();

    public static Pack2D Load(string name, byte[] vir, byte[] pix)
    {
        if (vir.Length < 0x20 || vir[0] != 'K' || vir[1] != 'P' || vir[2] != 'K' || vir[3] != 'y') throw new Exception("2DV invalido: " + name);
        var p = new Pack2D { Name = name, Vir = vir, Pix = pix };
        int gpr = (int)Be.U32(vir, 0x0C);
        int count = (int)Be.U32(vir, gpr + 0x1C);
        int entries = gpr + (int)Be.U32(vir, gpr + 0x08), nameBase = gpr + (int)Be.U32(vir, gpr + 0x0C), headers = gpr + (int)Be.U32(vir, gpr + 0x10);
        for (int i = 0; i < count; i++)
        {
            int e = entries + i * 0x20;
            if (e + 0x20 > vir.Length || !(vir[e] == 'T' && vir[e + 1] == 'X' && vir[e + 2] == '2' && vir[e + 3] == 'D')) break;
            var t = new Tex2D();
            int n = nameBase + (int)Be.U32(vir, e + 4), end = n;
            while (vir[end] != 0) end++;
            t.Name = Encoding.ASCII.GetString(vir, n, end - n);
            t.EntryOffset = e;
            t.HeaderOffset = headers + (int)Be.U32(vir, e + 0xC);
            t.DataOffset = (int)Be.U32(vir, e + 0x14);
            t.DataSize = (int)Be.U32(vir, e + 0x18);
            int f = t.HeaderOffset + 0x1C;
            t.F0 = Be.U32(vir, f); t.F1 = Be.U32(vir, f + 4); t.F2 = Be.U32(vir, f + 8);
            t.F3 = Be.U32(vir, f + 12); t.F4 = Be.U32(vir, f + 16); t.F5 = Be.U32(vir, f + 20);
            t.Format = (int)(t.F1 & 0x3F); t.Endian = (int)((t.F1 >> 6) & 3);
            t.Tiled = (t.F0 & 0x80000000u) != 0;
            t.Pitch = (int)((t.F0 >> 22) & 0x1FF) * 32;
            t.Width = (int)(t.F2 & 0x1FFF) + 1; t.Height = (int)((t.F2 >> 13) & 0x1FFF) + 1;
            p.Textures.Add(t);
        }
        return p;
    }

    static int TiledOffset(int x, int y, int width, int log2Bpb)
    {
        int alignedWidth = (width + 31) & ~31;
        int macro = ((x >> 5) + (y >> 5) * (alignedWidth >> 5)) << (log2Bpb + 7);
        int micro = ((x & 7) + ((y & 0xE) << 2)) << log2Bpb;
        int offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
        return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
    }

    static void Layout(Tex2D t, out int bpb, out int log2, out int pitchBlocks, out int bw, out int bh)
    {
        bool dxt = t.Format == 0x12 || t.Format == 0x13 || t.Format == 0x14 || t.Format == 0x3C;
        bpb = t.Format == 0x12 || t.Format == 0x3C ? 8 : (dxt ? 16 : 4);
        log2 = bpb == 8 ? 3 : (bpb == 16 ? 4 : 2);
        pitchBlocks = dxt ? Math.Max(t.Pitch, t.Width) / 4 : Math.Max(t.Pitch, t.Width);
        bw = dxt ? (t.Width + 3) / 4 : t.Width;
        bh = dxt ? (t.Height + 3) / 4 : t.Height;
    }

    static void SwapEndian(byte[] b, int len, int endian)
    {
        if (endian == 1) for (int i = 0; i + 1 < len; i += 2) { byte x = b[i]; b[i] = b[i + 1]; b[i + 1] = x; }
        else if (endian == 2) for (int i = 0; i + 3 < len; i += 4) { Array.Reverse(b, i, 4); }
    }

    // RGBA (rows of R,G,B,A) of one texture.
    public byte[] Decode(Tex2D t)
    {
        int bpb, log2, pitchBlocks, bw, bh;
        Layout(t, out bpb, out log2, out pitchBlocks, out bw, out bh);
        int w = t.Width, h = t.Height;
        var rgba = new byte[w * h * 4];
        var blk = new byte[16];
        bool dxt = bpb != 4;
        int tx, ty;
        t.TailOffset(out tx, out ty);
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++)
            {
                int src = t.DataOffset + (t.Tiled ? TiledOffset(bx + tx, by + ty, pitchBlocks, log2) : ((by + ty) * pitchBlocks + bx + tx) * bpb);
                if (src + bpb > Pix.Length) continue;
                Array.Copy(Pix, src, blk, 0, bpb);
                SwapEndian(blk, bpb, t.Endian);
                if (!dxt)
                {
                    int q = (by * w + bx) * 4;
                    rgba[q] = blk[1]; rgba[q + 1] = blk[2]; rgba[q + 2] = blk[3]; rgba[q + 3] = blk[0];
                    continue;
                }
                if (t.Format == 0x3C)
                {   // CTX1 (2026-11-16; it was read as DXN, 16-byte blocks: rewriting a normal map wrote over the next texture
                    // and 2B's body vanished): 8 bytes, X0 Y0 X1 Y1, then 2 bits a texel picking X0Y0, X1Y1, 2/3+1/3, 1/3+2/3
                    // (Xenia's ConvertTexelCTX1ToR8G8).
                    var xs = new int[16]; var ys = new int[16];
                    int[] cx = Ctx1Palette(blk[0], blk[2]), cy = Ctx1Palette(blk[1], blk[3]);
                    uint bits = (uint)(blk[4] | blk[5] << 8 | blk[6] << 16 | blk[7] << 24);
                    for (int i = 0; i < 16; i++) { int k = (int)(bits >> (2 * i)) & 3; xs[i] = cx[k]; ys[i] = cy[k]; }
                    for (int py = 0; py < 4; py++)
                        for (int px = 0; px < 4; px++)
                        {
                            int x = bx * 4 + px, y = by * 4 + py;
                            if (x >= w || y >= h) continue;
                            int i = py * 4 + px;
                            double nx = xs[i] / 127.5 - 1, ny = ys[i] / 127.5 - 1, nz = Math.Sqrt(Math.Max(0, 1 - nx * nx - ny * ny));
                            int q = (y * w + x) * 4;
                            rgba[q] = (byte)xs[i]; rgba[q + 1] = (byte)ys[i]; rgba[q + 2] = (byte)Math.Round((nz * 0.5 + 0.5) * 255); rgba[q + 3] = 255;
                        }
                    continue;
                }
                int co = bpb == 16 ? 8 : 0;
                int[] c = { blk[co] | blk[co + 1] << 8, blk[co + 2] | blk[co + 3] << 8 };
                var pal = new int[4, 4];
                for (int k = 0; k < 2; k++)
                {
                    pal[k, 0] = ((c[k] >> 11) & 31) * 255 / 31; pal[k, 1] = ((c[k] >> 5) & 63) * 255 / 63; pal[k, 2] = (c[k] & 31) * 255 / 31; pal[k, 3] = 255;
                }
                if (c[0] > c[1] || bpb == 16)
                {
                    for (int k = 0; k < 3; k++) { pal[2, k] = (2 * pal[0, k] + pal[1, k]) / 3; pal[3, k] = (pal[0, k] + 2 * pal[1, k]) / 3; }
                    pal[2, 3] = pal[3, 3] = 255;
                }
                else
                {
                    for (int k = 0; k < 3; k++) { pal[2, k] = (pal[0, k] + pal[1, k]) / 2; pal[3, k] = 0; }
                    pal[2, 3] = 255; pal[3, 3] = 0;
                }
                uint idx = (uint)(blk[co + 4] | blk[co + 5] << 8 | blk[co + 6] << 16 | blk[co + 7] << 24);
                var alpha = new int[16];
                if (t.Format == 0x13) { for (int i = 0; i < 16; i++) alpha[i] = ((blk[i / 2] >> ((i & 1) * 4)) & 15) * 17; }
                else if (t.Format == 0x14)
                {
                    var ap = new int[8]; ap[0] = blk[0]; ap[1] = blk[1];
                    if (ap[0] > ap[1]) for (int i = 1; i < 7; i++) ap[i + 1] = ((7 - i) * ap[0] + i * ap[1]) / 7;
                    else { for (int i = 1; i < 5; i++) ap[i + 1] = ((5 - i) * ap[0] + i * ap[1]) / 5; ap[6] = 0; ap[7] = 255; }
                    ulong bits = 0;
                    for (int i = 0; i < 6; i++) bits |= (ulong)blk[2 + i] << (8 * i);
                    for (int i = 0; i < 16; i++) alpha[i] = ap[(bits >> (3 * i)) & 7];
                }
                else for (int i = 0; i < 16; i++) alpha[i] = -1;
                for (int py = 0; py < 4; py++)
                    for (int px = 0; px < 4; px++)
                    {
                        int x = bx * 4 + px, y = by * 4 + py;
                        if (x >= w || y >= h) continue;
                        int i = py * 4 + px, ci = (int)((idx >> (2 * i)) & 3);
                        int q = (y * w + x) * 4;
                        rgba[q] = (byte)pal[ci, 0]; rgba[q + 1] = (byte)pal[ci, 1]; rgba[q + 2] = (byte)pal[ci, 2];
                        rgba[q + 3] = (byte)(alpha[i] >= 0 ? alpha[i] : pal[ci, 3]);
                    }
            }
        return rgba;
    }

    // ---------- encoding (RGBA of the texture's own size -> tiled Xenos bytes) ----------

    static int To565(int r, int g, int b) { return ((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255); }
    static void From565(int c, out int r, out int g, out int b) { r = ((c >> 11) & 31) * 255 / 31; g = ((c >> 5) & 63) * 255 / 63; b = (c & 31) * 255 / 31; }

    // 8-byte colour block (DXT1 style, 4-colour mode) of a 4x4 pixel block.
    static void ColorBlock(byte[] px, byte[] outb, int o, bool allowAlpha)
    {
        int minR = 255, minG = 255, minB = 255, maxR = 0, maxG = 0, maxB = 0;
        int opaque = 0;
        for (int i = 0; i < 16; i++)
        {
            if (allowAlpha && px[i * 4 + 3] < 128) continue;
            opaque++;
            int r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2];
            if (r < minR) minR = r; if (g < minG) minG = g; if (b < minB) minB = b;
            if (r > maxR) maxR = r; if (g > maxG) maxG = g; if (b > maxB) maxB = b;
        }
        if (opaque == 0) { for (int i = 0; i < 8; i++) outb[o + i] = 0; outb[o + 4] = 0xFF; outb[o + 5] = 0xFF; outb[o + 6] = 0xFF; outb[o + 7] = 0xFF; return; }
        // inset the bounding box a little (better average error)
        int ir = (maxR - minR) >> 4, ig = (maxG - minG) >> 4, ib = (maxB - minB) >> 4;
        minR = Math.Min(255, minR + ir); minG = Math.Min(255, minG + ig); minB = Math.Min(255, minB + ib);
        maxR = Math.Max(0, maxR - ir); maxG = Math.Max(0, maxG - ig); maxB = Math.Max(0, maxB - ib);
        int c0 = To565(maxR, maxG, maxB), c1 = To565(minR, minG, minB);
        if (!allowAlpha || true)
        {
            if (c0 < c1) { int s = c0; c0 = c1; c1 = s; }
            if (c0 == c1 && c0 < 0xFFFF) c0++;  // keep 4-colour mode for DXT3/5; DXT1 below
        }
        int[,] pal = new int[4, 3];
        From565(c0, out pal[0, 0], out pal[0, 1], out pal[0, 2]);
        From565(c1, out pal[1, 0], out pal[1, 1], out pal[1, 2]);
        for (int k = 0; k < 3; k++) { pal[2, k] = (2 * pal[0, k] + pal[1, k]) / 3; pal[3, k] = (pal[0, k] + 2 * pal[1, k]) / 3; }
        uint idx = 0;
        for (int i = 0; i < 16; i++)
        {
            if (allowAlpha && px[i * 4 + 3] < 128) { continue; }
            int best = 0, bd = int.MaxValue;
            for (int k = 0; k < 4; k++)
            {
                int dr = px[i * 4] - pal[k, 0], dg = px[i * 4 + 1] - pal[k, 1], db = px[i * 4 + 2] - pal[k, 2];
                int d = dr * dr + dg * dg + db * db;
                if (d < bd) { bd = d; best = k; }
            }
            idx |= (uint)best << (2 * i);
        }
        if (allowAlpha)
        {
            // DXT1 with 1-bit alpha: c0 <= c1 selects the 3-colour + transparent mode
            int cc0 = Math.Min(c0, c1), cc1 = Math.Max(c0, c1);
            int[,] p3 = new int[3, 3];
            From565(cc0, out p3[0, 0], out p3[0, 1], out p3[0, 2]);
            From565(cc1, out p3[1, 0], out p3[1, 1], out p3[1, 2]);
            for (int k = 0; k < 3; k++) p3[2, k] = (p3[0, k] + p3[1, k]) / 2;
            bool anyTransparent = opaque < 16;
            if (anyTransparent)
            {
                idx = 0;
                for (int i = 0; i < 16; i++)
                {
                    if (px[i * 4 + 3] < 128) { idx |= 3u << (2 * i); continue; }
                    int best = 0, bd = int.MaxValue;
                    for (int k = 0; k < 3; k++)
                    {
                        int dr = px[i * 4] - p3[k, 0], dg = px[i * 4 + 1] - p3[k, 1], db = px[i * 4 + 2] - p3[k, 2];
                        int d = dr * dr + dg * dg + db * db;
                        if (d < bd) { bd = d; best = k; }
                    }
                    idx |= (uint)best << (2 * i);
                }
                c0 = cc0; c1 = cc1;
            }
        }
        outb[o] = (byte)c0; outb[o + 1] = (byte)(c0 >> 8); outb[o + 2] = (byte)c1; outb[o + 3] = (byte)(c1 >> 8);
        outb[o + 4] = (byte)idx; outb[o + 5] = (byte)(idx >> 8); outb[o + 6] = (byte)(idx >> 16); outb[o + 7] = (byte)(idx >> 24);
    }

    static void Alpha5Block(byte[] px, byte[] outb, int o)
    {
        int mn = 255, mx = 0;
        for (int i = 0; i < 16; i++) { int a = px[i * 4 + 3]; if (a < mn) mn = a; if (a > mx) mx = a; }
        outb[o] = (byte)mx; outb[o + 1] = (byte)mn;  // a0 > a1: 8-value mode
        var ap = new int[8]; ap[0] = mx; ap[1] = mn;
        for (int i = 1; i < 7; i++) ap[i + 1] = ((7 - i) * mx + i * mn) / 7;
        ulong bits = 0;
        for (int i = 0; i < 16; i++)
        {
            int best = 0, bd = int.MaxValue, a = px[i * 4 + 3];
            for (int k = 0; k < 8; k++) { int d = Math.Abs(a - ap[k]); if (d < bd) { bd = d; best = k; } }
            bits |= (ulong)best << (3 * i);
        }
        for (int i = 0; i < 6; i++) outb[o + 2 + i] = (byte)(bits >> (8 * i));
    }

    static int[] Ctx1Palette(int a, int b) { return new[] { a, b, (2 * a + b) / 3, (a + 2 * b) / 3 }; }

    // CTX1: one line through the block's (X, Y) points, its two ends the endpoints. Both diagonals of the bounding box
    // are tried (X and Y rising together, or one falling), the one with less error kept.
    static void Ctx1Block(byte[] px, byte[] outb)
    {
        int x0 = 255, x1 = 0, y0 = 255, y1 = 0;
        for (int i = 0; i < 16; i++) { int x = px[i * 4], y = px[i * 4 + 1]; x0 = Math.Min(x0, x); x1 = Math.Max(x1, x); y0 = Math.Min(y0, y); y1 = Math.Max(y1, y); }
        long bestErr = long.MaxValue;
        for (int d = 0; d < 2; d++)
        {
            int ax = x1, ay = d == 0 ? y1 : y0, bx = x0, by = d == 0 ? y0 : y1;
            int[] cx = Ctx1Palette(ax, bx), cy = Ctx1Palette(ay, by);
            uint bits = 0; long err = 0;
            for (int i = 0; i < 16; i++)
            {
                int x = px[i * 4], y = px[i * 4 + 1], best = 0, bd = int.MaxValue;
                for (int k = 0; k < 4; k++) { int e = (x - cx[k]) * (x - cx[k]) + (y - cy[k]) * (y - cy[k]); if (e < bd) { bd = e; best = k; } }
                bits |= (uint)best << (2 * i); err += bd;
            }
            if (err >= bestErr) continue;
            bestErr = err;
            outb[0] = (byte)ax; outb[1] = (byte)ay; outb[2] = (byte)bx; outb[3] = (byte)by;
            outb[4] = (byte)bits; outb[5] = (byte)(bits >> 8); outb[6] = (byte)(bits >> 16); outb[7] = (byte)(bits >> 24);
        }
    }

    static int Align(int v, int a) { return (v + a - 1) / a * a; }

    // Puts a new image in the texture's place. Same size: in place. Another size
    // (an HD pack): the texture moves to the end of the .2DP and its header in the
    // .2DV gets the new size, pitch and offset. Single mip level, same format.
    public void Replace(Tex2D t, byte[] rgba, int w, int h)
    {
        if (w == t.Width && h == t.Height) { Encode(t, rgba); return; }
        bool dxt = t.Format != 0x06;
        int bpb = t.Format == 0x12 || t.Format == 0x3C ? 8 : (dxt ? 16 : 4);
        int pitch = dxt ? Align(w, 128) : Align(w, 32);
        int bw = dxt ? pitch / 4 : pitch, bh = Align(dxt ? (h + 3) / 4 : h, 32);
        int size = bw * bh * bpb;
        int at = Align(Pix.Length, 0x1000);
        var grown = new byte[at + Align(size, 0x1000)];
        Array.Copy(Pix, grown, Pix.Length);
        Pix = grown;
        t.DataOffset = at; t.DataSize = size;
        t.Width = w; t.Height = h; t.Pitch = pitch;
        t.F0 = (t.F0 & ~(0x1FFu << 22)) | (uint)(pitch / 32) << 22;
        t.F2 = (t.F2 & ~0x03FFFFFFu) | (uint)(w - 1) | (uint)(h - 1) << 13;
        int f = t.HeaderOffset + 0x1C;
        Be.Put32(Vir, f, t.F0); Be.Put32(Vir, f + 8, t.F2);
        Be.Put32(Vir, t.EntryOffset + 0x14, (uint)at); Be.Put32(Vir, t.EntryOffset + 0x18, (uint)size);
        Encode(t, rgba);
    }

    // Encodes RGBA (w*h*4, same size as the texture) back into Pix at the texture's slot.
    public void Encode(Tex2D t, byte[] rgba)
    {
        int bpb, log2, pitchBlocks, bw, bh;
        Layout(t, out bpb, out log2, out pitchBlocks, out bw, out bh);
        int w = t.Width, h = t.Height;
        var blk = new byte[16];
        var px = new byte[64];
        int tx, ty;
        t.TailOffset(out tx, out ty);
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++)
            {
                Array.Clear(blk, 0, 16);
                if (bpb == 4)
                {
                    int q = (by * w + bx) * 4;
                    blk[0] = rgba[q + 3]; blk[1] = rgba[q]; blk[2] = rgba[q + 1]; blk[3] = rgba[q + 2];
                }
                else
                {
                    for (int py = 0; py < 4; py++)
                        for (int pxx = 0; pxx < 4; pxx++)
                        {
                            int x = Math.Min(w - 1, bx * 4 + pxx), y = Math.Min(h - 1, by * 4 + py);
                            Array.Copy(rgba, (y * w + x) * 4, px, (py * 4 + pxx) * 4, 4);
                        }
                    if (t.Format == 0x12) ColorBlock(px, blk, 0, true);
                    else if (t.Format == 0x14) { Alpha5Block(px, blk, 0); ColorBlock(px, blk, 8, false); }
                    else if (t.Format == 0x3C)
                        Ctx1Block(px, blk);
                    else
                    {
                        for (int i = 0; i < 16; i += 2) blk[i / 2] = (byte)((px[i * 4 + 3] >> 4) | (px[(i + 1) * 4 + 3] & 0xF0));
                        ColorBlock(px, blk, 8, false);
                    }
                }
                SwapEndian(blk, bpb, t.Endian);  // the swap is its own inverse
                int dst = t.DataOffset + (t.Tiled ? TiledOffset(bx + tx, by + ty, pitchBlocks, log2) : ((by + ty) * pitchBlocks + bx + tx) * bpb);
                if (dst + bpb > Pix.Length) continue;
                Array.Copy(blk, 0, Pix, dst, bpb);
            }
    }
}
