# Unpacks an MT Framework (Xbox 360) .arc archive.
# Layout: "\0CRA", u16 version, u16 count, then 0x50-byte entries
# (char path[64], u32 type hash, u32 compressed size, u32 size | flags << 24, u32 offset), all big-endian.
# Entry data is a zlib stream.
param([Parameter(Mandatory)][string]$Arc, [Parameter(Mandatory)][string]$OutDir)

Add-Type -TypeDefinition @"
using System;
using System.IO;
using System.IO.Compression;
using System.Text;

public static class MtArc
{
    static uint BE32(byte[] b, int o) { return (uint)(b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]); }

    public static string Unpack(string arc, string outDir)
    {
        byte[] data = File.ReadAllBytes(arc);
        int count = data[6] << 8 | data[7];
        Directory.CreateDirectory(outDir);
        var log = new StringBuilder();

        for (int i = 0; i < count; i++)
        {
            int e = 8 + i * 0x50;
            string name = Encoding.ASCII.GetString(data, e, 64).TrimEnd('\0');
            uint type = BE32(data, e + 64), csize = BE32(data, e + 68), size = BE32(data, e + 72), offset = BE32(data, e + 76);

            byte[] bytes;
            using (var ms = new MemoryStream(data, (int)offset + 2, (int)csize - 2)) // skip the zlib header
            using (var ds = new DeflateStream(ms, CompressionMode.Decompress))
            using (var o = new MemoryStream())
            {
                ds.CopyTo(o);
                bytes = o.ToArray();
            }

            string file = Path.Combine(outDir, name.Replace('\\', '_').Replace('/', '_') + "." + type.ToString("X8"));
            File.WriteAllBytes(file, bytes);
            log.AppendLine(string.Format("{0,-44} type {1:X8} {2,8} -> {3,8} bytes", name, type, csize, bytes.Length));
        }

        return log.ToString();
    }
}
"@

[MtArc]::Unpack($Arc, $OutDir)
