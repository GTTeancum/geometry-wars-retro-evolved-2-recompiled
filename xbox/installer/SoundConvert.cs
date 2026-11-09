// Converts the game's sound banks (audio\*.baf) from XMA to Xbox ADPCM for
// the Xbox, which has no XMA (see xbox\tools\convert_banks.py, the same
// conversion for development, for the formats). Each wave's XMA is wrapped as
// XMA2 RIFF and converted by ffmpeg (adpcm_ima_wav, trellis search); waves of
// 16-bit big-endian PCM (data type 3 at record +8) go in as raw PCM; the
// IMA ADPCM then becomes Xbox ADPCM (Reblock, as xbox/src/xadpcm.cpp), the
// format the Xbox's sound chip plays. Next to each bank, <bank>.adpcm holds
// the converted waves found by their place in the original bank:
//   'GWX1', wave count, then per wave (little-endian words): original data
//   offset, original size, sample rate, channels, samples, group size (28
//   mono or 14 stereo blocks: 1008 bytes), data offset, data size; then the
//   data: 36-byte blocks per channel of 64 samples, interleaved per block.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;

public static class SoundConvert
{
    // --- IMA ADPCM (Microsoft blocks) into Xbox ADPCM, as xbox/src/xadpcm.cpp ---

    static readonly int[] Steps = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
        5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
        27086, 29794, 32767 };
    static readonly int[] IndexTable = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

    static void Decode(ref int pred, ref int idx, int nib)
    {
        int step = Steps[idx], diff = step >> 3;
        if ((nib & 1) != 0) diff += step >> 2;
        if ((nib & 2) != 0) diff += step >> 1;
        if ((nib & 4) != 0) diff += step;
        pred += (nib & 8) != 0 ? -diff : diff;
        pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
        idx += IndexTable[nib];
        idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
    }

    // The code bringing (pred, idx) nearest to target, of all 16; among equal
    // ones, the one whose step index comes nearest to targetIdx.
    static int Encode(int pred, int idx, int target, int targetIdx)
    {
        int best = 0, bestErr = int.MaxValue, bestGap = int.MaxValue;
        for (int nib = 0; nib < 16; nib++)
        {
            int p = pred, i = idx;
            Decode(ref p, ref i, nib);
            int err = Math.Abs(p - target), gap = Math.Abs(i - targetIdx);
            if (err < bestErr || (err == bestErr && gap < bestGap)) { best = nib; bestErr = err; bestGap = gap; }
        }
        return best;
    }

    // The samples IMA ADPCM data in blocks of 'block' bytes holds.
    static int HeldSamples(int size, int block, int ch)
    {
        int per = (block - 4 * ch) * 2 / ch + 1;
        int n = (size / block) * per;
        if (size % block > 4 * ch) n += (size % block - 4 * ch) * 2 / ch + 1;
        return n;
    }

    // Xbox ADPCM: 36-byte blocks per channel of 64 samples (a header sample
    // and step index, 63 played codes, an unplayed one), interleaved per
    // block for stereo. Where the source's own block boundaries fall between
    // ours, its samples are encoded from the output decoder's state until our
    // next header joins the two again.
    public static byte[] Reblock(byte[] src, int srcBlock, int ch, int samples)
    {
        int srcPer = (srcBlock - 4 * ch) * 2 / ch + 1;
        int blocks = (samples + 63) / 64;
        var dst = new byte[blocks * 36 * ch];
        for (int c = 0; c < ch; c++)
        {
            int pred = 0, idx = 0, opred = 0, oidx = 0;
            for (int s = 0; s < samples; s++)
            {
                int b = s / srcPer, k = s % srcPer, blk = b * srcBlock, nib = -1;
                bool same = pred == opred && idx == oidx;
                if (k == 0)
                {
                    pred = (short)(src[blk + c * 4] | src[blk + c * 4 + 1] << 8);
                    idx = Math.Min((int)src[blk + c * 4 + 2], 88);
                }
                else
                {
                    int code = k - 1;
                    int q = blk + 4 * ch + (code / 8) * 4 * ch + 4 * c;
                    nib = (src[q + (code % 8) / 2] >> ((code & 1) * 4)) & 15;
                    Decode(ref pred, ref idx, nib);
                }
                int o = (s / 64) * 36 * ch, ok = s % 64;
                if (ok == 0)
                {
                    dst[o + 4 * c] = (byte)(pred & 0xFF);
                    dst[o + 4 * c + 1] = (byte)((pred >> 8) & 0xFF);
                    dst[o + 4 * c + 2] = (byte)idx;
                    opred = pred; oidx = idx;
                    continue;
                }
                int outNib = nib >= 0 && same ? nib : Encode(opred, oidx, pred, idx);
                Decode(ref opred, ref oidx, outNib);
                int oc = ok - 1;
                int oq = o + 4 * ch + (oc / 8) * 4 * ch + 4 * c;
                dst[oq + (oc % 8) / 2] |= (byte)(outNib << ((oc & 1) * 4));
            }
        }
        return dst;
    }

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
                    byte[] ima = ToAdpcm(ffmpeg, payload, rate, channels, pcm, work, out block, out samples);
                    samples = Math.Min(samples, HeldSamples(ima.Length, block, channels));
                    byte[] adpcm = Reblock(ima, block, channels, samples);
                    int group = (channels == 1 ? 28 : 14) * 36 * channels;  // one stream slot
                    foreach (int v in new[] { offset, length, rate, channels, samples, group, tableSize + (int)blob.Length, adpcm.Length })
                        tw.Write(v);
                    blob.Write(adpcm, 0, adpcm.Length);
                    while (blob.Length % 4 != 0) blob.WriteByte(0);
                    at += size;
                    waves++;
                }
                using (var f = File.Create(Path.Combine(outDir, Path.GetFileNameWithoutExtension(bank) + ".adpcm")))
                {
                    var fw = new BinaryWriter(f);
                    fw.Write(Encoding.ASCII.GetBytes("GWX1")); fw.Write(count);
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
