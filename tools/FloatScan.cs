// Searches a guest memory dump (NIER_DUMP_MEMORY records) for big-endian
// floats close to given values, as consecutive components (stride 4 bytes,
// e.g. a position x, y, z) - to find game structures holding a known value.
using System;
using System.Collections.Generic;

public static class FloatScan {
    static float At(byte[] d, int i) {
        return BitConverter.ToSingle(new byte[] { d[i + 3], d[i + 2], d[i + 1], d[i] }, 0);
    }

    // Addresses where values[0..n) appear as consecutive floats within tolerance.
    public static List<uint> Find(List<MemScan.Chunk> chunks, float[] values, float tolerance,
                                  int limit) {
        var hits = new List<uint>();
        foreach (var c in chunks) {
            byte[] d = c.Data;
            for (int i = 0; i + 4 * values.Length <= d.Length; i += 4) {
                int k = 0;
                while (k < values.Length && Math.Abs(At(d, i + 4 * k) - values[k]) <= tolerance) k++;
                if (k == values.Length) {
                    hits.Add(c.Addr + (uint)i);
                    if (hits.Count >= limit) return hits;
                }
            }
        }
        return hits;
    }

    // Addresses of x, y, z float triples with x and z within tolerance of the
    // given values and y in [yMin, yMax] (a position whose height is unsure).
    public static List<uint> FindXZ(List<MemScan.Chunk> chunks, float x, float z, float tolerance,
                                    float yMin, float yMax, int limit) {
        var hits = new List<uint>();
        foreach (var c in chunks) {
            byte[] d = c.Data;
            for (int i = 0; i + 12 <= d.Length; i += 4) {
                // Written as !(within) so NaN never matches.
                if (!(Math.Abs(At(d, i) - x) <= tolerance)) continue;
                if (!(Math.Abs(At(d, i + 8) - z) <= tolerance)) continue;
                float y = At(d, i + 4);
                if (!(y >= yMin && y <= yMax)) continue;
                hits.Add(c.Addr + (uint)i);
                if (hits.Count >= limit) return hits;
            }
        }
        return hits;
    }

    // 4x4 world matrices (3 unit rows with w = 0, then x y z 1, not at the
    // origin) in [lo, hi): "address x y z" lines.
    public static List<string> WorldMatrices(List<MemScan.Chunk> chunks, uint lo, uint hi) {
        var lines = new List<string>();
        foreach (var c in chunks) {
            if (c.Addr + c.Data.Length <= lo || c.Addr >= hi) continue;
            byte[] d = c.Data;
            for (int i = 0; i + 64 <= d.Length; i += 16) {
                uint a = c.Addr + (uint)i;
                if (a < lo || a >= hi) continue;
                bool ok = true;
                for (int r = 0; r < 3 && ok; r++) {
                    float x = At(d, i + 16 * r), y = At(d, i + 16 * r + 4), z = At(d, i + 16 * r + 8);
                    double len = Math.Sqrt(x * x + y * y + z * z);
                    ok = Math.Abs(len - 1) < 0.02 && At(d, i + 16 * r + 12) == 0;
                }
                if (!ok || !(Math.Abs(At(d, i + 60) - 1) < 1e-4)) continue;
                float px = At(d, i + 48), py = At(d, i + 52), pz = At(d, i + 56);
                if (!(Math.Abs(px) < 5000 && Math.Abs(pz) < 5000) || (px == 0 && py == 0 && pz == 0)) continue;
                lines.Add(string.Format("{0:X8} {1:F2} {2:F2} {3:F2}", a, px, py, pz));
            }
        }
        return lines;
    }

    // count floats starting at address (for printing what surrounds a hit).
    public static float[] Read(List<MemScan.Chunk> chunks, uint addr, int count) {
        foreach (var c in chunks) {
            if (addr >= c.Addr && addr + 4 * count <= c.Addr + c.Data.Length) {
                var r = new float[count];
                for (int k = 0; k < count; k++) r[k] = At(c.Data, (int)(addr - c.Addr) + 4 * k);
                return r;
            }
        }
        return null;
    }
}
