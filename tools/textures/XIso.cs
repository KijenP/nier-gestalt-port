// Xbox 360 disc image (XDVDFS) reader: finds files inside the game's .iso without extracting it.
// Volume descriptor at partition + 0x10000 ("MICROSOFT*XBOX*MEDIA", root directory sector and size).
// A directory is a binary tree of entries: [u16 left][u16 right] (in 4-byte units from the start of the
// directory, 0xFFFF = none), [u32 sector][u32 size][u8 attributes][u8 name length][name]. Little-endian,
// sectors of 2048 bytes counted from the partition start.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

public static class XIso
{
    // Where the game partition starts in the known image layouts (extracted XISO, XGD2, XGD3, XGD1).
    static readonly long[] kPartitions = { 0, 0xFD90000, 0x2080000, 0x18300000 };

    public static long FindPartition(FileStream f)
    {
        var magic = new byte[20];
        foreach (long p in kPartitions)
        {
            if (p + 0x10000 + 20 > f.Length) continue;
            f.Position = p + 0x10000; f.Read(magic, 0, 20);
            if (Encoding.ASCII.GetString(magic) == "MICROSOFT*XBOX*MEDIA") return p;
        }
        return -1;
    }

    // Offset and size of a file inside the image ("media/layer1.cpk"); false when not there.
    public static bool Find(string iso, string path, out long offset, out long size)
    {
        offset = size = 0;
        using (var f = new FileStream(iso, FileMode.Open, FileAccess.Read, FileShare.Read))
        {
            long part = FindPartition(f);
            if (part < 0) return false;
            var vd = new byte[28];
            f.Position = part + 0x10000; f.Read(vd, 0, 28);
            long dirSector = BitConverter.ToUInt32(vd, 20), dirSize = BitConverter.ToUInt32(vd, 24);
            var parts = path.Replace('\\', '/').Split(new[] { '/' }, StringSplitOptions.RemoveEmptyEntries);
            for (int i = 0; i < parts.Length; i++)
            {
                if (dirSize <= 0 || dirSize > (16 << 20)) return false;
                var dir = new byte[dirSize];
                f.Position = part + dirSector * 2048; f.Read(dir, 0, dir.Length);
                long sector, len; bool isDir;
                if (!Lookup(dir, 0, parts[i], out sector, out len, out isDir, 0)) return false;
                if (i == parts.Length - 1) { offset = part + sector * 2048; size = len; return !isDir; }
                if (!isDir) return false;
                dirSector = sector; dirSize = len;
            }
        }
        return false;
    }

    static bool Lookup(byte[] dir, int at, string name, out long sector, out long size, out bool isDir, int depth)
    {
        sector = size = 0; isDir = false;
        if (at < 0 || at + 14 > dir.Length || depth > 64) return false;
        int left = BitConverter.ToUInt16(dir, at), right = BitConverter.ToUInt16(dir, at + 2);
        if (left == 0xFFFF && right == 0xFFFF && dir[at + 13] == 0xFF) return false;  // padding
        int n = dir[at + 13];
        if (at + 14 + n > dir.Length) return false;
        string here = Encoding.ASCII.GetString(dir, at + 14, n);
        int c = string.Compare(name, here, StringComparison.OrdinalIgnoreCase);
        if (c == 0)
        {
            sector = BitConverter.ToUInt32(dir, at + 4); size = BitConverter.ToUInt32(dir, at + 8);
            isDir = (dir[at + 12] & 0x10) != 0;
            return true;
        }
        // The tree is ordered (case-insensitively), but walk both sides: cheap, and safe with any order.
        if (left != 0xFFFF && left != 0 && Lookup(dir, left * 4, name, out sector, out size, out isDir, depth + 1)) return true;
        if (right != 0xFFFF && right != 0 && Lookup(dir, right * 4, name, out sector, out size, out isDir, depth + 1)) return true;
        return false;
    }

    // The game's .iso for a path given as the image itself or a folder holding exactly one .iso.
    public static string ImageAt(string path)
    {
        if (string.IsNullOrEmpty(path)) return null;
        if (File.Exists(path) && path.EndsWith(".iso", StringComparison.OrdinalIgnoreCase)) return path;
        if (Directory.Exists(path))
        {
            var isos = Directory.GetFiles(path, "*.iso");
            if (isos.Length == 1) return isos[0];
        }
        return null;
    }
}
