# Find likely function entry points the static analysis missed: 32-bit words in
# the image's data that point into code but are not a registered function.
#
#   find-vcall-targets.ps1 -Image work\image.bin -Generated generated\default -Out work\candidates.csv
#
# The image comes from running nier.exe with NIER_DUMP_IMAGE=<file>.
param(
    [Parameter(Mandatory)][string]$Image,
    [Parameter(Mandatory)][string]$Generated,
    [Parameter(Mandatory)][string]$Out,
    # Strings: PowerShell 5.1 parses 0x8xxxxxxx literals as negative Int32.
    [string]$ImageBase = '82000000',
    [string]$CodeStart = '82100000',
    [string]$CodeEnd = '8262AA08'
)

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Text.RegularExpressions;

public static class VcallScan {
    static uint Be(byte[] b, long o) {
        return (uint)(b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]);
    }

    // blr, bctr, or an unconditional relative branch without link.
    static bool IsTerminator(uint ins) {
        if (ins == 0x4E800020 || ins == 0x4E800420) return true;
        return (ins >> 26) == 18 && (ins & 3) == 0;
    }

    public static List<string> Run(string image, string generated, uint imageBase, uint codeStart, uint codeEnd) {
        byte[] img = File.ReadAllBytes(image);

        var registered = new HashSet<uint>();
        var reg = new Regex(@"SetFunction\(0x([0-9A-F]{8})");
        foreach (var f in Directory.GetFiles(generated, "*_register.cpp"))
            foreach (Match m in reg.Matches(File.ReadAllText(f)))
                registered.Add(Convert.ToUInt32(m.Groups[1].Value, 16));

        var labels = new HashSet<uint>();
        var lbl = new Regex(@"^loc_([0-9A-F]{8}):", RegexOptions.Multiline);
        foreach (var f in Directory.GetFiles(generated, "*_recomp.*.cpp"))
            foreach (Match m in lbl.Matches(File.ReadAllText(f)))
                labels.Add(Convert.ToUInt32(m.Groups[1].Value, 16));

        var refs = new Dictionary<uint, List<uint>>();
        long codeLo = codeStart - imageBase, codeHi = codeEnd - imageBase;
        for (long o = 0; o + 4 <= img.Length; o += 4) {
            if (o >= codeLo && o < codeHi) continue;  // data only
            uint w = Be(img, o);
            if (w < codeStart || w >= codeEnd || (w & 3) != 0 || registered.Contains(w)) continue;
            List<uint> l;
            if (!refs.TryGetValue(w, out l)) refs[w] = l = new List<uint>();
            l.Add((uint)(imageBase + o));
        }

        // Addresses materialized in code: lis rD,hi then addi/ori rX,rD,lo shortly after.
        var codeRefs = new Dictionary<uint, uint>();
        for (long o = codeLo; o + 4 <= codeHi; o += 4) {
            uint ins = Be(img, o);
            if ((ins >> 26) != 15 || ((ins >> 16) & 31) != 0) continue;  // lis = addis rD,0,imm
            uint rd = (ins >> 21) & 31, hi = ins & 0xFFFF;
            for (long k = 1; k <= 8 && o + 4 * k + 4 <= codeHi; k++) {
                uint n = Be(img, o + 4 * k);
                uint op = n >> 26, ra = (n >> 16) & 31, rs = (n >> 21) & 31, lo = n & 0xFFFF;
                uint v;
                if (op == 14 && ra == rd) v = (hi << 16) + (uint)(short)lo;   // addi
                else if (op == 24 && rs == rd) v = (hi << 16) | lo;           // ori
                else continue;
                if (v >= codeStart && v < codeEnd && (v & 3) == 0 && !registered.Contains(v)) {
                    List<uint> l;
                    if (!refs.TryGetValue(v, out l)) refs[v] = l = new List<uint>();
                    l.Add((uint)(imageBase + o));
                    codeRefs[v] = 1;
                }
                break;
            }
        }

        var rows = new List<string> { "target,refs,first_ref,prev_insn,prev_is_terminator,is_label,insn,from_code" };
        var keys = new List<uint>(refs.Keys);
        keys.Sort();
        foreach (uint t in keys) {
            long to = t - imageBase;
            uint prev = Be(img, to - 4), ins = Be(img, to);
            rows.Add(string.Format("0x{0:X8},{1},0x{2:X8},0x{3:X8},{4},{5},0x{6:X8},{7}",
                t, refs[t].Count, refs[t][0], prev, IsTerminator(prev) ? 1 : 0, labels.Contains(t) ? 1 : 0, ins,
                codeRefs.ContainsKey(t) ? 1 : 0));
        }
        Console.Error.WriteLine("registered=" + registered.Count + " labels=" + labels.Count + " candidates=" + keys.Count);
        return rows;
    }
}
'@

$hex = { param($s) [Convert]::ToUInt32($s, 16) }
$rows = [VcallScan]::Run((Resolve-Path $Image), (Resolve-Path $Generated),
    (& $hex $ImageBase), (& $hex $CodeStart), (& $hex $CodeEnd))
Set-Content -LiteralPath $Out -Value $rows -Encoding utf8
