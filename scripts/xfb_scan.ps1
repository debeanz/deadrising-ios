# Lists every Xbox 360 shader container (flags 0x102A0Exx, the 2006 compiler's layout) found in
# MT Framework XFB shader packages, with the container header and the shader header words.
param([Parameter(Mandatory)][string]$Dir, [int]$Limit = 0)

Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.Text;

public static class XfbScan
{
    static uint BE(byte[] b, long o) { return o + 4 <= b.Length ? (uint)(b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]) : 0; }

    public static string Scan(string dir, int limit)
    {
        var sb = new StringBuilder();
        int total = 0, ps = 0, vs = 0;
        foreach (var file in Directory.GetFiles(dir, "*.4E32817C"))
        {
            byte[] d = File.ReadAllBytes(file);
            for (long o = 0; o + 0x24 <= d.Length; o += 4)
            {
                uint flags = BE(d, o);
                if ((flags & 0xFFFFFF00) != 0x102A0E00)
                    continue;

                uint virtualSize = BE(d, o + 4), physicalSize = BE(d, o + 8);
                uint f0C = BE(d, o + 0xC), ctab = BE(d, o + 0x10), shader = BE(d, o + 0x14);
                if (o + virtualSize + physicalSize > d.Length || ctab == 0 || shader == 0)
                    continue;

                bool isPs = (flags & 1) == 0;
                if (isPs) ps++; else vs++;
                total++;
                if (limit > 0 && total > limit)
                    continue;

                var words = new StringBuilder();
                for (int i = 0; i < 14; i++)
                    words.Append(BE(d, o + shader + i * 4).ToString("X8")).Append(' ');

                sb.AppendLine(string.Format("{0} @{1:X5} {2} vsize={3:X} psize={4:X} f0C={5:X} f18={6:X} f1C={7:X} f20={8:X} | hdr {9}",
                    Path.GetFileName(file).Replace("system_xfshader_", "").Replace(".4E32817C", ""), o, isPs ? "PS" : "VS",
                    virtualSize, physicalSize, f0C, BE(d, o + 0x18), BE(d, o + 0x1C), BE(d, o + 0x20), words));
            }
        }
        sb.AppendLine(string.Format("containers: {0} (pixel {1}, vertex {2})", total, ps, vs));
        return sb.ToString();
    }
}
"@

[XfbScan]::Scan($Dir, $Limit)
