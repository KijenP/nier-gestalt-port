// Reads the two things setup needs from the user's copy of the game, without
// extracting the whole disc: default.xex out of an Xbox 360 disc image
// (XDVDFS/GDFX), and the title/media IDs from an XEX2 header.
using System;
using System.IO;
using System.Text;

public static class GameImage {
    const int Sector = 2048;
    // Byte offsets of the game partition in the known disc layouts (same list
    // as ReXGlue's DiscImageDevice).
    static readonly long[] GameOffsets = { 0x00000000, 0x0000FB20, 0x00020600, 0x02080000, 0x0FD90000 };

    public static byte[] ReadDefaultXex(string isoPath) {
        return ReadRootFile(isoPath, "default.xex");
    }

    public static byte[] ReadRootFile(string isoPath, string fileName) {
        using (var f = new FileStream(isoPath, FileMode.Open, FileAccess.Read, FileShare.Read)) {
            foreach (long gameOffset in GameOffsets) {
                byte[] head = ReadAt(f, gameOffset + 32L * Sector, 28);
                if (head == null || Encoding.ASCII.GetString(head, 0, 20) != "MICROSOFT*XBOX*MEDIA") continue;
                uint rootSector = BitConverter.ToUInt32(head, 20);
                uint rootSize = BitConverter.ToUInt32(head, 24);
                if (rootSize < 13 || rootSize > 32 * 1024 * 1024) throw new InvalidDataException("Diretório raiz da imagem danificado.");
                byte[] root = ReadAt(f, gameOffset + (long)rootSector * Sector, (int)rootSize);
                if (root == null) throw new InvalidDataException("Imagem truncada.");
                long sector, length;
                if (!FindEntry(root, 0, fileName, out sector, out length))
                    throw new FileNotFoundException(fileName + " não está na raiz do disco.");
                byte[] xex = ReadAt(f, gameOffset + sector * Sector, (int)length);
                if (xex == null) throw new InvalidDataException("Imagem truncada.");
                return xex;
            }
        }
        throw new InvalidDataException("Não parece uma imagem de disco de Xbox 360.");
    }

    // Directory entries form a binary tree: left/right child offsets in 4-byte units.
    static bool FindEntry(byte[] dir, int ordinal, string name, out long sector, out long length) {
        sector = length = 0;
        int p = ordinal * 4;
        if (p + 14 > dir.Length) return false;
        ushort left = BitConverter.ToUInt16(dir, p), right = BitConverter.ToUInt16(dir, p + 2);
        int nameLen = dir[p + 13];
        if (p + 14 + nameLen > dir.Length) return false;
        if (string.Equals(Encoding.ASCII.GetString(dir, p + 14, nameLen), name, StringComparison.OrdinalIgnoreCase)) {
            sector = BitConverter.ToUInt32(dir, p + 4);
            length = BitConverter.ToUInt32(dir, p + 8);
            return true;
        }
        return (left != 0 && left != 0xFFFF && FindEntry(dir, left, name, out sector, out length)) ||
               (right != 0 && right != 0xFFFF && FindEntry(dir, right, name, out sector, out length));
    }

    // XEX2: big-endian; optional header 0x00040006 is the execution info
    // (media_id, version, base_version, title_id, ...).
    public static string TitleInfo(byte[] xex) {
        if (xex.Length < 0x18 || Encoding.ASCII.GetString(xex, 0, 4) != "XEX2")
            throw new InvalidDataException("default.xex não é um XEX2 válido.");
        uint count = Be32(xex, 0x14);
        for (uint i = 0; i < count && 0x18 + i * 8 + 8 <= xex.Length; i++) {
            uint key = Be32(xex, (int)(0x18 + i * 8));
            uint value = Be32(xex, (int)(0x18 + i * 8 + 4));
            if (key == 0x00040006 && value + 16 <= xex.Length)
                return string.Format("{0:X8},{1:X8}", Be32(xex, (int)value + 12), Be32(xex, (int)value));
        }
        throw new InvalidDataException("default.xex sem informação de título.");
    }

    // nxeart is an STFS package (CON/LIVE/PIRS); its header carries the title
    // tile as a PNG: size at 0x1712, data at 0x171A.
    public static byte[] TileFromNxeArt(byte[] stfs) {
        string magic = stfs.Length > 4 ? Encoding.ASCII.GetString(stfs, 0, 4) : "";
        if (magic != "PIRS" && magic != "LIVE" && magic != "CON ") return null;
        int size = (int)Be32(stfs, 0x1712);
        if (size <= 8 || 0x171A + size > stfs.Length || stfs[0x171A] != 0x89) return null;
        byte[] png = new byte[size];
        Array.Copy(stfs, 0x171A, png, 0, size);
        return png;
    }

    // Windows .ico with PNG-compressed images at the usual sizes.
    public static byte[] IcoFromPng(byte[] png) {
        int[] sizes = { 16, 24, 32, 48, 64, 128, 256 };
        var images = new byte[sizes.Length][];
        using (var src = System.Drawing.Image.FromStream(new MemoryStream(png))) {
            for (int i = 0; i < sizes.Length; i++) {
                using (var bmp = new System.Drawing.Bitmap(sizes[i], sizes[i]))
                using (var g = System.Drawing.Graphics.FromImage(bmp))
                using (var ms = new MemoryStream()) {
                    g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                    g.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
                    g.DrawImage(src, 0, 0, sizes[i], sizes[i]);
                    bmp.Save(ms, System.Drawing.Imaging.ImageFormat.Png);
                    images[i] = ms.ToArray();
                }
            }
        }
        using (var ms = new MemoryStream())
        using (var w = new BinaryWriter(ms)) {
            w.Write((ushort)0); w.Write((ushort)1); w.Write((ushort)sizes.Length);
            int offset = 6 + 16 * sizes.Length;
            for (int i = 0; i < sizes.Length; i++) {
                w.Write((byte)(sizes[i] >= 256 ? 0 : sizes[i]));
                w.Write((byte)(sizes[i] >= 256 ? 0 : sizes[i]));
                w.Write((byte)0); w.Write((byte)0);
                w.Write((ushort)1); w.Write((ushort)32);
                w.Write(images[i].Length); w.Write(offset);
                offset += images[i].Length;
            }
            foreach (var img in images) w.Write(img);
            return ms.ToArray();
        }
    }

    static uint Be32(byte[] b, int o) {
        return (uint)(b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]);
    }

    static byte[] ReadAt(FileStream f, long offset, int count) {
        if (offset < 0 || offset + count > f.Length) return null;
        f.Seek(offset, SeekOrigin.Begin);
        byte[] buf = new byte[count];
        int read = 0;
        while (read < count) {
            int n = f.Read(buf, read, count - read);
            if (n <= 0) return null;
            read += n;
        }
        return buf;
    }
}
