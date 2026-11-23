// Finds big-endian dwords in a guest memory dump (NIER_DUMP_MEMORY records)
// whose value lies in [lo, hi) - pointers into a known structure.
using System.Collections.Generic;

public static class PtrScan {
    public static List<uint[]> Find(List<MemScan.Chunk> chunks, uint lo, uint hi, int limit) {
        var hits = new List<uint[]>();
        foreach (var c in chunks) {
            byte[] d = c.Data;
            for (int i = 0; i + 4 <= d.Length; i += 4) {
                uint v = (uint)(d[i] << 24 | d[i + 1] << 16 | d[i + 2] << 8 | d[i + 3]);
                if (v >= lo && v < hi) {
                    hits.Add(new uint[] { c.Addr + (uint)i, v });
                    if (hits.Count >= limit) return hits;
                }
            }
        }
        return hits;
    }

    public static uint U32(List<MemScan.Chunk> chunks, uint addr) {
        foreach (var c in chunks) {
            if (addr >= c.Addr && addr + 4 <= c.Addr + c.Data.Length) {
                int i = (int)(addr - c.Addr);
                byte[] d = c.Data;
                return (uint)(d[i] << 24 | d[i + 1] << 16 | d[i + 2] << 8 | d[i + 3]);
            }
        }
        return 0xFFFFFFFF;
    }
}
