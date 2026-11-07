// Converts the game's sound banks (audio\*.baf) from XMA to IMA ADPCM for
// the Xbox, which has no XMA (see xbox\tools\convert_banks.py, the same
// conversion for development, for the formats). Each wave's XMA is wrapped as
// XMA2 RIFF and converted by ffmpeg (adpcm_ima_wav, trellis search); waves of
// 16-bit big-endian PCM (data type 3 at record +8) go in as raw PCM; next to
// each bank, <bank>.adpcm holds the converted waves found by their place in
// the original bank:
//   'GWAD', wave count, then per wave (little-endian words): original data
//   offset, original size, sample rate, channels, samples, ADPCM block size,
//   ADPCM offset, ADPCM size; then the ADPCM data.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;

public static class SoundConvert
{
    const int Packet = 2048;

    static uint BE32(byte[] d, int o) { return (uint)(d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3]); }

    static byte[] Xma2Riff(byte[] payload, int rate, int channels)
    {
        var fmt = new MemoryStream();
        var w = new BinaryWriter(fmt);
        w.Write((ushort)0x166); w.Write((ushort)channels); w.Write(rate); w.Write(rate * 2 * channels);
        w.Write((ushort)Packet); w.Write((ushort)16); w.Write((ushort)34);
        w.Write((ushort)1); w.Write(channels == 1 ? 4u : 3u); w.Write(0u); w.Write((uint)payload.Length);
        w.Write(0u); w.Write(0u); w.Write(0u); w.Write(0u); w.Write((byte)0); w.Write((byte)4);
        w.Write((ushort)Math.Max(1, payload.Length / Packet));
        var body = new MemoryStream();
        var b = new BinaryWriter(body);
        b.Write(Encoding.ASCII.GetBytes("WAVEfmt ")); b.Write((uint)fmt.Length); b.Write(fmt.ToArray());
        b.Write(Encoding.ASCII.GetBytes("data")); b.Write((uint)payload.Length); b.Write(payload);
        var riff = new MemoryStream();
        var r = new BinaryWriter(riff);
        r.Write(Encoding.ASCII.GetBytes("RIFF")); r.Write((uint)body.Length); r.Write(body.ToArray());
        return riff.ToArray();
    }

    // -> ADPCM data; block size and sample count from the RIFF ffmpeg writes.
    static byte[] ToAdpcm(string ffmpeg, byte[] payload, int rate, int channels, bool pcm, string work, out int block, out int samples)
    {
        string dst = Path.Combine(work, "out.wav");
        string source;
        if (pcm)
        {
            string raw = Path.Combine(work, "in.raw");
            File.WriteAllBytes(raw, payload);
            source = "-f s16be -ar " + rate + " -ac " + channels + " -i \"" + raw + "\"";
        }
        else
        {
            string src = Path.Combine(work, "in.wav");
            File.WriteAllBytes(src, Xma2Riff(payload, rate, channels));
            source = "-i \"" + src + "\"";
        }
        var info = new ProcessStartInfo(ffmpeg, "-hide_banner -loglevel error -y " + source +
                                        " -acodec adpcm_ima_wav -trellis 8 \"" + dst + "\"");
        info.UseShellExecute = false;
        info.RedirectStandardError = true;
        info.CreateNoWindow = true;
        using (var p = Process.Start(info))
        {
            string err = p.StandardError.ReadToEnd();
            p.WaitForExit();
            if (p.ExitCode != 0) throw new Exception("ffmpeg: " + err.Trim());
        }
        byte[] riff = File.ReadAllBytes(dst);
        block = -1; samples = -1;
        byte[] data = null;
        for (int at = 12; at + 8 <= riff.Length;)
        {
            string tag = Encoding.ASCII.GetString(riff, at, 4);
            int size = BitConverter.ToInt32(riff, at + 4);
            if (tag == "fmt ") block = BitConverter.ToUInt16(riff, at + 8 + 12);
            else if (tag == "fact") samples = BitConverter.ToInt32(riff, at + 8);
            else if (tag == "data") { data = new byte[size]; Array.Copy(riff, at + 8, data, 0, size); }
            at += 8 + size + (size & 1);
        }
        if (data == null || block < 0 || samples < 0) throw new Exception("ffmpeg wrote no ADPCM");
        return data;
    }

    // Converts every *.baf in audioDir into <bank>.adpcm in outDir; returns
    // the number of waves.
    public static int Run(string ffmpeg, string audioDir, string outDir)
    {
        string work = Path.Combine(Path.GetTempPath(), "gw2-sound-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(work);
        int waves = 0;
        try
        {
            foreach (string bank in Directory.GetFiles(audioDir, "*.baf"))
            {
                byte[] d = File.ReadAllBytes(bank);
                if (Encoding.ASCII.GetString(d, 0, 4) != "BANK") throw new Exception(bank + " is not a sound bank");
                int count = (int)BE32(d, 12);
                var table = new MemoryStream();
                var tw = new BinaryWriter(table);
                var blob = new MemoryStream();
                int at = 0x34;
                int tableSize = 8 + 32 * count;
                for (int i = 0; i < count; i++)
                {
                    if (Encoding.ASCII.GetString(d, at, 4) != "WAVE") throw new Exception(bank + ": record " + i + " is not a WAVE");
                    int size = (int)BE32(d, at + 4);
                    int offset = (int)BE32(d, at + 0x2C), length = (int)BE32(d, at + 0x30);
                    int rate = (int)BE32(d, at + 0x3C), channels = (int)BE32(d, at + 0x44);
                    bool pcm = BE32(d, at + 8) == 3;
                    var payload = new byte[length];
                    Array.Copy(d, offset, payload, 0, length);
                    int block, samples;
                    byte[] adpcm = ToAdpcm(ffmpeg, payload, rate, channels, pcm, work, out block, out samples);
                    foreach (int v in new[] { offset, length, rate, channels, samples, block, tableSize + (int)blob.Length, adpcm.Length })
                        tw.Write(v);
                    blob.Write(adpcm, 0, adpcm.Length);
                    while (blob.Length % 4 != 0) blob.WriteByte(0);
                    at += size;
                    waves++;
                }
                using (var f = File.Create(Path.Combine(outDir, Path.GetFileNameWithoutExtension(bank) + ".adpcm")))
                {
                    var fw = new BinaryWriter(f);
                    fw.Write(Encoding.ASCII.GetBytes("GWAD")); fw.Write(count);
                    fw.Write(table.ToArray()); fw.Write(blob.ToArray());
                }
                Console.WriteLine("  " + Path.GetFileName(bank) + ": " + count + " sounds");
            }
        }
        finally
        {
            try { Directory.Delete(work, true); } catch { }
        }
        return waves;
    }
}
