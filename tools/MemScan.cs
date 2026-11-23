// Searches a guest memory dump written by NIER_DUMP_MEMORY
// (records of [u32 guest address][u32 size][data], little-endian header).
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

public static class MemScan {
    public class Chunk { public uint Addr; public byte[] Data; }

    public static List<Chunk> Load(string path) {
        var chunks = new List<Chunk>();
        using (var f = new BinaryReader(File.OpenRead(path))) {
            while (f.BaseStream.Position + 8 <= f.BaseStream.Length) {
                uint addr = f.ReadUInt32(), size = f.ReadUInt32();
                chunks.Add(new Chunk { Addr = addr, Data = f.ReadBytes((int)size) });
            }
        }
        return chunks;
    }

    // All guest addresses where the byte pattern occurs (chunk-local search).
    public static List<uint> Find(List<Chunk> chunks, byte[] pattern, int limit) {
        var hits = new List<uint>();
        foreach (var c in chunks) {
            byte[] d = c.Data;
            for (int i = 0; i + pattern.Length <= d.Length; i++) {
                if (d[i] != pattern[0]) continue;
                int j = 1;
                while (j < pattern.Length && d[i + j] == pattern[j]) j++;
                if (j == pattern.Length) {
                    hits.Add(c.Addr + (uint)i);
                    if (hits.Count >= limit) return hits;
                }
            }
        }
        return hits;
    }

    public static byte[] Utf16Be(string s) { return Encoding.BigEndianUnicode.GetBytes(s); }
    public static byte[] Utf16Le(string s) { return Encoding.Unicode.GetBytes(s); }
    public static byte[] Ascii(string s) { return Encoding.ASCII.GetBytes(s); }
    public static byte[] Be32(uint v) { return new[] { (byte)(v >> 24), (byte)(v >> 16), (byte)(v >> 8), (byte)v }; }

    public static byte[] Read(List<Chunk> chunks, uint addr, int len) {
        foreach (var c in chunks) {
            if (addr >= c.Addr && (ulong)addr + (ulong)len <= (ulong)c.Addr + (ulong)c.Data.Length) {
                var r = new byte[len];
                Array.Copy(c.Data, (int)(addr - c.Addr), r, 0, len);
                return r;
            }
        }
        return null;
    }

    public static string AsciiAround(List<Chunk> chunks, uint addr, int before, int after) {
        byte[] b = Read(chunks, addr - (uint)before, before + after);
        if (b == null) return "(fora do dump)";
        var sb = new StringBuilder();
        foreach (byte x in b) sb.Append(x >= 0x20 && x < 0x7F ? (char)x : (x == 0 ? '|' : '.'));
        return sb.ToString();
    }

    public static string Hex(List<Chunk> chunks, uint addr, int len) {
        foreach (var c in chunks) {
            if (addr >= c.Addr && addr + len <= c.Addr + (uint)c.Data.Length) {
                var sb = new StringBuilder();
                for (int i = 0; i < len; i++) sb.Append(c.Data[addr - c.Addr + i].ToString("X2")).Append(i % 4 == 3 ? " " : "");
                return sb.ToString();
            }
        }
        return "(fora do dump)";
    }
}
