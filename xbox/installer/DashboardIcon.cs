// The dashboard's title name and the game's icon for the installed XBE, made
// from the user's own copy of the game (the download carries none of the
// game's art). The same steps, to the byte, as xbox\tools\xbe_meta.py
// "dashboard" (the development build's): the 360 game's dashboard icon (the
// XDBF image 0x8000 in the XEX's resource, a 64x64 PNG, read from the
// decrypted image), laid on black and made DXT1 in the XPR0 resources the
// Xbox dashboard reads, TitleImage.xbx (128x128, each pixel doubled) and
// SaveImage.xbx (64x64), with TitleMeta.xbx; written next to the XBE (the
// game copies them to its save folder on start) and inserted into it as the
// sections an XDK build has ($$XTINFO, $$XTIMAGE, $$XSIMAGE).
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;

public static class DashboardIcon
{
    const uint ImageBase = 0x82000000, XdbfAddress = 0x821FB480, XdbfSize = 0x17465, IconId = 0x8000;
    const string TitleName = "Geometry Wars Retro Evolved 2";

    static uint BE32(byte[] d, int o) { return (uint)(d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3]); }
    static uint LE32(byte[] d, int o) { return BitConverter.ToUInt32(d, o); }
    static void PutLE32(byte[] d, int o, uint v) { BitConverter.GetBytes(v).CopyTo(d, o); }
    static int Align(int v, int a) { return (v + a - 1) & ~(a - 1); }

    // The XDBF's image 0x8000 (PNG bytes).
    static byte[] GameIcon(string imageBin)
    {
        byte[] image = File.ReadAllBytes(imageBin);
        int x = (int)(XdbfAddress - ImageBase);
        if (Encoding.ASCII.GetString(image, x, 4) != "XDBF") throw new Exception("The game image has no XDBF resource.");
        int tableLen = (int)BE32(image, x + 8), count = (int)BE32(image, x + 12), freeLen = (int)BE32(image, x + 16);
        int dataAt = x + 24 + tableLen * 18 + freeLen * 8;
        for (int i = 0; i < count; ++i)
        {
            int e = x + 24 + i * 18;
            int ns = image[e] << 8 | image[e + 1];
            ulong id = (ulong)BE32(image, e + 2) << 32 | BE32(image, e + 6);
            int off = (int)BE32(image, e + 10), length = (int)BE32(image, e + 14);
            if (ns == 2 && id == IconId)
            {
                var png = new byte[length];
                Array.Copy(image, dataAt + off, png, 0, length);
                return png;
            }
        }
        throw new Exception("The game image's XDBF has no icon.");
    }

    // A 64x64 8-bit RGBA PNG's pixels as stored (the file's gamma not
    // applied, as the build's Pillow reads them).
    static byte[] IconPixels(byte[] png)
    {
        int at = 8, width = 0, height = 0;
        var idat = new MemoryStream();
        while (at + 8 <= png.Length)
        {
            int n = (int)BE32(png, at);
            string type = Encoding.ASCII.GetString(png, at + 4, 4);
            if (type == "IHDR")
            {
                width = (int)BE32(png, at + 8);
                height = (int)BE32(png, at + 12);
                if (png[at + 16] != 8 || png[at + 17] != 6 || png[at + 20] != 0)
                    throw new Exception("The icon is not an 8-bit RGBA PNG.");
            }
            else if (type == "IDAT") idat.Write(png, at + 8, n);
            at += 12 + n;
        }
        if (width != 64 || height != 64) throw new Exception("The icon is not 64x64.");
        byte[] z = idat.ToArray();
        var raw = new MemoryStream();
        using (var inflate = new DeflateStream(new MemoryStream(z, 2, z.Length - 2), CompressionMode.Decompress))
            inflate.CopyTo(raw);
        byte[] f = raw.ToArray();
        const int bpp = 4, stride = 64 * 4;
        var pixels = new byte[64 * stride];
        for (int y = 0; y < 64; ++y)
        {
            int filter = f[y * (stride + 1)];
            for (int i = 0; i < stride; ++i)
            {
                int v = f[y * (stride + 1) + 1 + i];
                int a = i >= bpp ? pixels[y * stride + i - bpp] : 0;
                int b = y > 0 ? pixels[(y - 1) * stride + i] : 0;
                int c = i >= bpp && y > 0 ? pixels[(y - 1) * stride + i - bpp] : 0;
                int p;
                switch (filter)
                {
                    case 0: p = 0; break;
                    case 1: p = a; break;
                    case 2: p = b; break;
                    case 3: p = (a + b) / 2; break;
                    case 4:
                        int e = a + b - c, pa = Math.Abs(e - a), pb = Math.Abs(e - b), pc = Math.Abs(e - c);
                        p = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
                        break;
                    default: throw new Exception("The icon's PNG has an unknown filter.");
                }
                pixels[y * stride + i] = (byte)(v + p);
            }
        }
        return pixels;
    }

    static int Pack(int[] c)
    {
        return ((c[0] * 31 + 127) / 255) << 11 | ((c[1] * 63 + 127) / 255) << 5 | (c[2] * 31 + 127) / 255;
    }

    static int[] Unpack(int v)
    {
        int r = v >> 11, g = (v >> 5) & 63, b = v & 31;
        return new[] { (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2) };
    }

    // DXT1, as xbe_meta.py's dxt1: end colours the block's darkest and
    // brightest pixels (2R + 5G + B, first on a tie), each pixel the nearest
    // of the four colours (lowest index on a tie).
    static byte[] Dxt1(int[][][] rgb, int size)
    {
        var out_ = new MemoryStream();
        var w = new BinaryWriter(out_);
        for (int by = 0; by < size; by += 4)
            for (int bx = 0; bx < size; bx += 4)
            {
                var px = new int[16][];
                for (int k = 0; k < 16; ++k) px[k] = rgb[by + k / 4][bx + k % 4];
                int lo = 0, hi = 0;
                for (int k = 1; k < 16; ++k)
                {
                    int l = 2 * px[k][0] + 5 * px[k][1] + px[k][2];
                    if (l < 2 * px[lo][0] + 5 * px[lo][1] + px[lo][2]) lo = k;
                    if (l > 2 * px[hi][0] + 5 * px[hi][1] + px[hi][2]) hi = k;
                }
                int c0 = Pack(px[hi]), c1 = Pack(px[lo]);
                if (c0 < c1) { int t = c0; c0 = c1; c1 = t; }
                if (c0 == c1)
                {
                    w.Write((ushort)c0); w.Write((ushort)c1); w.Write(0u);
                    continue;
                }
                int[] e0 = Unpack(c0), e1 = Unpack(c1);
                var palette = new int[4][];
                palette[0] = e0;
                palette[1] = e1;
                palette[2] = new int[3];
                palette[3] = new int[3];
                for (int i = 0; i < 3; ++i)
                {
                    palette[2][i] = (2 * e0[i] + e1[i]) / 3;
                    palette[3][i] = (e0[i] + 2 * e1[i]) / 3;
                }
                uint bits = 0;
                for (int k = 0; k < 16; ++k)
                {
                    int best = 0, bestD = int.MaxValue;
                    for (int q = 0; q < 4; ++q)
                    {
                        int d = 0;
                        for (int i = 0; i < 3; ++i) d += (px[k][i] - palette[q][i]) * (px[k][i] - palette[q][i]);
                        if (d < bestD) { bestD = d; best = q; }
                    }
                    bits |= (uint)best << (2 * k);
                }
                w.Write((ushort)c0); w.Write((ushort)c1); w.Write(bits);
            }
        return out_.ToArray();
    }

    // The XPR0 resource for one size x size DXT1 texture (as xbe_meta.py's xpr0).
    static byte[] Xpr0(byte[] rgba, int size)
    {
        int scale = size / 64;
        var rgb = new int[size][][];
        for (int y = 0; y < size; ++y)
        {
            rgb[y] = new int[size][];
            for (int x = 0; x < size; ++x)
            {
                int at = ((y / scale) * 64 + x / scale) * 4, a = rgba[at + 3];
                rgb[y][x] = new[] { (rgba[at] * a + 127) / 255, (rgba[at + 1] * a + 127) / 255, (rgba[at + 2] * a + 127) / 255 };
            }
        }
        byte[] blocks = Dxt1(rgb, size);
        int log2 = size == 128 ? 7 : 6;
        uint fmt = 0x29u | 0x0Cu << 8 | 1u << 16 | (uint)log2 << 20 | (uint)log2 << 24;
        var header = new byte[0x800];
        for (int i = 0; i < header.Length; ++i) header[i] = 0xAD;
        Encoding.ASCII.GetBytes("XPR0").CopyTo(header, 0);
        uint[] words = { (uint)(0x800 + blocks.Length), 0x800, 0x00040001, 0, 0, fmt, 0, 0xFFFFFFFF };
        for (int i = 0; i < words.Length; ++i) PutLE32(header, 4 + i * 4, words[i]);
        var result = new byte[0x800 + blocks.Length];
        header.CopyTo(result, 0);
        blocks.CopyTo(result, 0x800);
        return result;
    }

    // Appends inserted-file sections to the XBE (as xbe_meta.py's
    // insert_sections); an XBE that has them already is left.
    static void InsertSections(string xbe, string[] names, byte[][] payloads)
    {
        byte[] data = File.ReadAllBytes(xbe);
        if (Encoding.ASCII.GetString(data, 0, 4) != "XBEH") throw new Exception(xbe + " is not an XBE.");
        int baseAddr = (int)LE32(data, 0x104), headerSize = (int)LE32(data, 0x108), imageSize = (int)LE32(data, 0x10C);
        int count = (int)LE32(data, 0x11C), table = (int)LE32(data, 0x120) - baseAddr;
        int firstRaw = int.MaxValue;
        for (int i = 0; i < count; ++i)
        {
            int nameAt = (int)LE32(data, table + i * 56 + 20) - baseAddr;
            int end = Array.IndexOf(data, (byte)0, nameAt);
            if (Array.IndexOf(names, Encoding.ASCII.GetString(data, nameAt, end - nameAt)) >= 0) return;
            int raw = (int)LE32(data, table + i * 56 + 12);
            if (raw != 0 && raw < firstRaw) firstRaw = raw;
        }
        var old = new byte[count * 56];
        Array.Copy(data, table, old, 0, old.Length);
        int newTable = Align(headerSize, 16);
        int cursor = newTable + (count + names.Length) * 56;
        var nameOffsets = new int[names.Length];
        for (int i = 0; i < names.Length; ++i) { nameOffsets[i] = cursor; cursor += names[i].Length + 1; }
        int refs = Align(cursor, 2);
        int newHeader = Align(refs + (names.Length + 1) * 2, 4);
        if (newHeader > firstRaw) throw new Exception("No room in the XBE's header for the dashboard sections.");
        for (int i = newTable; i < newHeader; ++i)
            if (data[i] != 0) throw new Exception("The XBE header's spare room is not empty.");
        old.CopyTo(data, newTable);
        for (int i = 0; i < names.Length; ++i)
        {
            Encoding.ASCII.GetBytes(names[i]).CopyTo(data, nameOffsets[i]);
            data[nameOffsets[i] + names[i].Length] = 0;
        }
        var file = new MemoryStream();
        file.Write(data, 0, data.Length);
        while (file.Length % 0x1000 != 0) file.WriteByte(0);
        int virt = baseAddr + Align(imageSize, 0x1000);
        var headers = new byte[names.Length * 56];
        using (var sha1 = SHA1.Create())
            for (int i = 0; i < names.Length; ++i)
            {
                int h = i * 56;
                uint[] v = { 0x38, (uint)virt, (uint)payloads[i].Length, (uint)file.Length, (uint)payloads[i].Length,
                             (uint)(baseAddr + nameOffsets[i]), 0, (uint)(baseAddr + refs + i * 2),
                             (uint)(baseAddr + refs + (i + 1) * 2) };
                for (int k = 0; k < 9; ++k) PutLE32(headers, h + k * 4, v[k]);
                sha1.ComputeHash(payloads[i]).CopyTo(headers, h + 36);
                file.Write(payloads[i], 0, payloads[i].Length);
                while (file.Length % 0x1000 != 0) file.WriteByte(0);
                virt = Align(virt + payloads[i].Length, 0x20);
            }
        byte[] result = file.ToArray();
        headers.CopyTo(result, newTable + count * 56);
        PutLE32(result, 0x108, (uint)newHeader);
        PutLE32(result, 0x10C, (uint)(virt - baseAddr));
        PutLE32(result, 0x11C, (uint)(count + names.Length));
        PutLE32(result, 0x120, (uint)(baseAddr + newTable));
        File.WriteAllBytes(xbe, result);
    }

    public static void Run(string imageBin, string xbe, string outDir)
    {
        byte[] rgba = IconPixels(GameIcon(imageBin));
        byte[] titleImage = Xpr0(rgba, 128), saveImage = Xpr0(rgba, 64);
        string meta = "[default]\r\nTitleName=" + TitleName + "\r\n";
        var bom = new byte[] { 0xFF, 0xFE };
        byte[] metaUtf16 = new byte[2 + meta.Length * 2];
        bom.CopyTo(metaUtf16, 0);
        Encoding.Unicode.GetBytes(meta).CopyTo(metaUtf16, 2);
        File.WriteAllBytes(Path.Combine(outDir, "TitleImage.xbx"), titleImage);
        File.WriteAllBytes(Path.Combine(outDir, "SaveImage.xbx"), saveImage);
        File.WriteAllBytes(Path.Combine(outDir, "TitleMeta.xbx"), metaUtf16);
        InsertSections(xbe, new[] { "$$XTINFO", "$$XTIMAGE", "$$XSIMAGE" },
                       new[] { Encoding.ASCII.GetBytes(meta), titleImage, saveImage });
    }
}
