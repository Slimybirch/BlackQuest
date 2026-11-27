// Phase 8's converter: the HD Reimagined pack (a PCSX2 replacement pack: DDS DXT5/DXT1 and PNG named by PCSX2's
// own hashes of the PS2's textures) to the headset's textures_hd/<xbox hash>.ktx2 (the loader's, native/vulkan.cpp).
// The PS2's names mean nothing on the Xbox, so tools\hd_map.txt (made once by --match from the Xbox dump, names and
// hashes only) pairs each Xbox texture with its pack image by picture. The conversion keeps the pack's own DXT
// blocks and mips (the Quest 3 samples BC1/BC3) from the size cap down; images without mips, and PNGs, are shrunk,
// given mips and encoded to BC3 here
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;

namespace BlackQuestBuilder {

public static class HdPack {
    const int kBc1 = 133, kBc3 = 137;  // VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK

    // ---- images: a DDS's levels, or RGBA pixels
    sealed class Dds {
        public int W, H, Format;  // Format kBc1 / kBc3
        public List<byte[]> Levels = new List<byte[]>();
    }

    static int Blocks(int n) => Math.Max(1, (n + 3) / 4);

    static Dds ReadDds(string path) {
        byte[] b = File.ReadAllBytes(path);
        if (b.Length < 128 || b[0] != 'D' || b[1] != 'D' || b[2] != 'S') return null;
        var d = new Dds { H = BitConverter.ToInt32(b, 12), W = BitConverter.ToInt32(b, 16) };
        string four = Encoding.ASCII.GetString(b, 84, 4);
        if (four == "DXT5") d.Format = kBc3;
        else if (four == "DXT1") d.Format = kBc1;
        else return null;
        int mips = Math.Max(1, BitConverter.ToInt32(b, 28));
        int block = d.Format == kBc1 ? 8 : 16, at = 128;
        for (int l = 0; l < mips; l++) {
            int w = Math.Max(1, d.W >> l), h = Math.Max(1, d.H >> l);
            int n = Blocks(w) * Blocks(h) * block;
            if (at + n > b.Length) break;
            var lv = new byte[n];
            Buffer.BlockCopy(b, at, lv, 0, n);
            d.Levels.Add(lv);
            at += n;
        }
        return d.Levels.Count > 0 ? d : null;
    }

    // a DXT level to RGBA
    static byte[] Decode(byte[] blocks, int w, int h, int format) {
        var px = new byte[w * h * 4];
        int bw = Blocks(w), bh = Blocks(h), size = format == kBc1 ? 8 : 16;
        var pal = new byte[16];
        var alpha = new byte[8];
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                int o = (by * bw + bx) * size;
                int c = format == kBc3 ? o + 8 : o;
                ushort c0 = BitConverter.ToUInt16(blocks, c), c1 = BitConverter.ToUInt16(blocks, c + 2);
                Rgb565(c0, pal, 0);
                Rgb565(c1, pal, 4);
                bool four = c0 > c1 || format == kBc3;
                for (int k = 0; k < 3; k++) {
                    pal[8 + k] = (byte)(four ? (2 * pal[k] + pal[4 + k]) / 3 : (pal[k] + pal[4 + k]) / 2);
                    pal[12 + k] = (byte)(four ? (pal[k] + 2 * pal[4 + k]) / 3 : 0);
                }
                pal[3] = pal[7] = pal[11] = 255;
                pal[15] = (byte)(four ? 255 : 0);
                uint idx = BitConverter.ToUInt32(blocks, c + 4);
                ulong aidx = 0;
                if (format == kBc3) {
                    alpha[0] = blocks[o];
                    alpha[1] = blocks[o + 1];
                    for (int k = 2; k < 8; k++)
                        alpha[k] = (byte)(alpha[0] > alpha[1] ? ((8 - k) * alpha[0] + (k - 1) * alpha[1]) / 7
                                          : k < 6 ? ((6 - k) * alpha[0] + (k - 1) * alpha[1]) / 5 : k == 6 ? 0 : 255);
                    for (int k = 0; k < 6; k++) aidx |= (ulong)blocks[o + 2 + k] << (8 * k);
                }
                for (int i = 0; i < 16; i++) {
                    int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    if (x >= w || y >= h) continue;
                    int p = (y * w + x) * 4, s = (int)((idx >> (2 * i)) & 3) * 4;
                    px[p] = pal[s];
                    px[p + 1] = pal[s + 1];
                    px[p + 2] = pal[s + 2];
                    px[p + 3] = format == kBc3 ? alpha[(aidx >> (3 * i)) & 7] : pal[s + 3];
                }
            }
        return px;
    }

    static void Rgb565(ushort c, byte[] o, int at) {
        int r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
        o[at] = (byte)(r << 3 | r >> 2);
        o[at + 1] = (byte)(g << 2 | g >> 4);
        o[at + 2] = (byte)(b << 3 | b >> 2);
    }

    // RGBA from a PNG (or any image System.Drawing reads)
    static byte[] ReadImage(string path, out int w, out int h) {
        using (var bmp = new Bitmap(path)) {
            w = bmp.Width;
            h = bmp.Height;
            var data = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            var bgra = new byte[w * h * 4];
            for (int y = 0; y < h; y++) Marshal.Copy(data.Scan0 + y * data.Stride, bgra, y * w * 4, w * 4);
            bmp.UnlockBits(data);
            for (int i = 0; i < bgra.Length; i += 4) { byte t = bgra[i]; bgra[i] = bgra[i + 2]; bgra[i + 2] = t; }
            return bgra;
        }
    }

    // a 2x2 box filter's half
    static byte[] Half(byte[] px, int w, int h, out int nw, out int nh) {
        nw = Math.Max(1, w / 2);
        nh = Math.Max(1, h / 2);
        var o = new byte[nw * nh * 4];
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++)
                for (int c = 0; c < 4; c++) {
                    int x0 = Math.Min(2 * x, w - 1), x1 = Math.Min(2 * x + 1, w - 1), y0 = Math.Min(2 * y, h - 1), y1 = Math.Min(2 * y + 1, h - 1);
                    o[(y * nw + x) * 4 + c] = (byte)((px[(y0 * w + x0) * 4 + c] + px[(y0 * w + x1) * 4 + c] + px[(y1 * w + x0) * 4 + c] + px[(y1 * w + x1) * 4 + c] + 2) / 4);
                }
        return o;
    }

    // RGBA to BC3 (endpoints from the block's colour bounding box, inset; alpha from its range): plain and quick
    static byte[] EncodeBc3(byte[] px, int w, int h) {
        int bw = Blocks(w), bh = Blocks(h);
        var o = new byte[bw * bh * 16];
        var blk = new int[64];
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                for (int i = 0; i < 16; i++) {
                    int x = Math.Min(bx * 4 + (i & 3), w - 1), y = Math.Min(by * 4 + (i >> 2), h - 1);
                    for (int c = 0; c < 4; c++) blk[i * 4 + c] = px[(y * w + x) * 4 + c];
                }
                int at = (by * bw + bx) * 16;
                // alpha: 8 levels between its min and max
                int amin = 255, amax = 0;
                for (int i = 0; i < 16; i++) { amin = Math.Min(amin, blk[i * 4 + 3]); amax = Math.Max(amax, blk[i * 4 + 3]); }
                o[at] = (byte)amax;
                o[at + 1] = (byte)amin;
                ulong abits = 0;
                for (int i = 0; i < 16; i++) {
                    int a = blk[i * 4 + 3], code;
                    if (amax == amin) code = 0;
                    else {
                        int step = (int)Math.Round((amax - a) * 7.0 / (amax - amin));  // 0 = max .. 7 = min
                        code = step == 0 ? 0 : step == 7 ? 1 : step + 1;
                    }
                    abits |= (ulong)code << (3 * i);
                }
                for (int k = 0; k < 6; k++) o[at + 2 + k] = (byte)(abits >> (8 * k));
                // colour: the bounding box's corners, inset by 1/16
                int[] mn = { 255, 255, 255 }, mx = { 0, 0, 0 };
                for (int i = 0; i < 16; i++)
                    for (int c = 0; c < 3; c++) { mn[c] = Math.Min(mn[c], blk[i * 4 + c]); mx[c] = Math.Max(mx[c], blk[i * 4 + c]); }
                for (int c = 0; c < 3; c++) { int inset = (mx[c] - mn[c]) / 16; mn[c] += inset; mx[c] -= inset; }
                ushort c0 = To565(mx), c1 = To565(mn);
                if (c0 < c1) { var t = c0; c0 = c1; c1 = t; }
                var pal = new byte[16];
                Rgb565(c0, pal, 0);
                Rgb565(c1, pal, 4);
                for (int k = 0; k < 3; k++) { pal[8 + k] = (byte)((2 * pal[k] + pal[4 + k]) / 3); pal[12 + k] = (byte)((pal[k] + 2 * pal[4 + k]) / 3); }
                uint idx = 0;
                for (int i = 0; i < 16; i++) {
                    int best = 0, bd = int.MaxValue;
                    for (int s = 0; s < 4; s++) {
                        int d = 0;
                        for (int c = 0; c < 3; c++) { int e = blk[i * 4 + c] - pal[s * 4 + c]; d += e * e; }
                        if (d < bd) { bd = d; best = s; }
                    }
                    idx |= (uint)best << (2 * i);
                }
                BitConverter.GetBytes(c0).CopyTo(o, at + 8);
                BitConverter.GetBytes(c1).CopyTo(o, at + 10);
                BitConverter.GetBytes(idx).CopyTo(o, at + 12);
            }
        return o;
    }

    static ushort To565(int[] c) => (ushort)((c[0] * 31 + 127) / 255 << 11 | (c[1] * 63 + 127) / 255 << 5 | (c[2] * 31 + 127) / 255);

    // ---- KTX2: the 80-byte header, the level index, the levels (16-aligned), no DFD (the loader doesn't read it)
    static void WriteKtx2(string path, int format, int w, int h, List<byte[]> levels) {
        int n = levels.Count, at = 80 + 24 * n;
        var offs = new long[n];
        for (int i = 0; i < n; i++) { at = (at + 15) & ~15; offs[i] = at; at += levels[i].Length; }
        var b = new byte[at];
        byte[] id = { 0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };
        id.CopyTo(b, 0);
        int[] head = { format, 1, w, h, 0, 0, 1, n, 0 };
        for (int i = 0; i < head.Length; i++) BitConverter.GetBytes(head[i]).CopyTo(b, 12 + 4 * i);
        for (int i = 0; i < n; i++) {
            BitConverter.GetBytes(offs[i]).CopyTo(b, 80 + 24 * i);
            BitConverter.GetBytes((long)levels[i].Length).CopyTo(b, 88 + 24 * i);
            BitConverter.GetBytes((long)levels[i].Length).CopyTo(b, 96 + 24 * i);
            Buffer.BlockCopy(levels[i], 0, b, (int)offs[i], levels[i].Length);
        }
        File.WriteAllBytes(path, b);
    }

    // ---- the conversion (the player's PC): every pair in the map to dest\<xbox hash>.ktx2, at most cap pixels a side
    public static int Convert(string pack, string map, string dest, int cap, Action<string> log, Func<bool> cancel) {
        Directory.CreateDirectory(dest);
        string dir = FindReplacements(pack);
        if (dir == null) { log("HD textures: no SLUS-21376\\replacements folder in " + pack); return -1; }
        var pairs = ReadMap(map);
        int done = 0, missing = 0;
        foreach (var pair in pairs) {
            if (cancel()) return -1;
            string src = Path.Combine(dir, pair.Value);
            string outFile = Path.Combine(dest, pair.Key + ".ktx2");
            if (!File.Exists(src)) { missing++; continue; }
            if (File.Exists(outFile) && File.GetLastWriteTimeUtc(outFile) >= File.GetLastWriteTimeUtc(src)) { done++; continue; }
            try {
                ConvertOne(src, outFile, cap);
                done++;
                if (done % 50 == 0) log("  " + done + " / " + pairs.Count);
            } catch (Exception e) {
                log("  " + pair.Value + ": " + e.Message);
            }
        }
        // the upscaled ones beside it (out	extures_upscaled, <xbox hash>.png: Upscale.cs, the dump's textures the pack
        // has no match for), after the pack's: a pack image wins
        string up = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(dest)), "textures_upscaled");
        int ups = 0;
        if (Directory.Exists(up)) {
            var paired = new HashSet<string>(pairs.Select(p => p.Key));
            foreach (string png in Directory.EnumerateFiles(up, "*.png")) {
                if (cancel()) return -1;
                string hash = Path.GetFileNameWithoutExtension(png);
                if (paired.Contains(hash)) continue;
                string outFile = Path.Combine(dest, hash + ".ktx2");
                if (!File.Exists(outFile) || File.GetLastWriteTimeUtc(outFile) < File.GetLastWriteTimeUtc(png)) {
                    try { ConvertOne(png, outFile, cap); } catch (Exception e) { log("  " + hash + ": " + e.Message); continue; }
                }
                ups++;
            }
        }
        log("HD textures: " + done + " converted" + (missing > 0 ? ", " + missing + " pack images missing" : "") +
            (ups > 0 ? ", and " + ups + " upscaled" : ""));
        return done + ups;
    }

    static void ConvertOne(string src, string outFile, int cap) {
        if (src.EndsWith(".dds", StringComparison.OrdinalIgnoreCase)) {
            var d = ReadDds(src);
            if (d == null) throw new Exception("not a DXT1/DXT5 DDS");
            // the pack's own mips from the cap down: nothing re-encoded
            int start = 0;
            while (start + 1 < d.Levels.Count && Math.Max(d.W >> start, d.H >> start) > cap) start++;
            int w = Math.Max(1, d.W >> start), h = Math.Max(1, d.H >> start);
            if (d.Levels.Count - start >= 2 || (w <= 4 && h <= 4)) {
                WriteKtx2(outFile, d.Format, w, h, d.Levels.Skip(start).ToList());
                return;
            }
            // one level: decoded, shrunk, mipped, encoded
            var px0 = Decode(d.Levels[start], w, h, d.Format);
            WriteEncoded(outFile, px0, w, h, cap);
            return;
        }
        var px = ReadImage(src, out int iw, out int ih);
        WriteEncoded(outFile, px, iw, ih, cap);
    }

    static void WriteEncoded(string outFile, byte[] px, int w, int h, int cap) {
        while (Math.Max(w, h) > cap) px = Half(px, w, h, out w, out h);
        int w0 = w, h0 = h;
        var levels = new List<byte[]> { EncodeBc3(px, w, h) };
        while (w > 1 || h > 1) {
            px = Half(px, w, h, out w, out h);
            levels.Add(EncodeBc3(px, w, h));
        }
        WriteKtx2(outFile, kBc3, w0, h0, levels);
    }

    static string FindReplacements(string pack) {
        foreach (string d in new[] { Path.Combine(pack, "SLUS-21376", "replacements"), Path.Combine(pack, "replacements"), pack })
            if (Directory.Exists(d) && Directory.EnumerateFiles(d, "*.dds").Any()) return d;
        return null;
    }

    // xbox hash -> pack file
    static List<KeyValuePair<string, string>> ReadMap(string map) {
        var list = new List<KeyValuePair<string, string>>();
        foreach (string line in File.ReadAllLines(map)) {
            if (line.StartsWith("#") || line.Trim().Length == 0) continue;
            string[] f = line.Split(' ');
            if (f.Length >= 2) list.Add(new KeyValuePair<string, string>(f[0], f[1]));
        }
        return list;
    }

    // ---- the matching (a developer's PC, once): each Xbox dump texture's best pack image by picture
    // 16x16 thumbnails, compared on luminance's shape (zero mean, unit spread), colour and alpha; aspect must agree
    sealed class Sig {
        public string Name;
        public float Aspect;
        public float[] Luma = new float[1024], Grad = new float[2048], Rgb = new float[768], Alpha = new float[256];
        public float Spread;
    }

    const int G = 32;

    static Sig MakeSig(string name, byte[] px, int w, int h) {
        // shrink by halves to near 128, then 32x32 area averages
        while (Math.Max(w, h) > 128) px = Half(px, w, h, out w, out h);
        var s = new Sig { Name = name, Aspect = (float)w / h };
        var rgb = new float[G * G * 3];
        var alpha = new float[G * G];
        for (int ty = 0; ty < G; ty++)
            for (int tx = 0; tx < G; tx++) {
                int x0 = tx * w / G, x1 = Math.Max(x0 + 1, (tx + 1) * w / G), y0 = ty * h / G, y1 = Math.Max(y0 + 1, (ty + 1) * h / G);
                float r = 0, g = 0, b = 0, a = 0;
                int n = 0;
                for (int y = y0; y < Math.Min(y1, h); y++)
                    for (int x = x0; x < Math.Min(x1, w); x++) {
                        int p = (y * w + x) * 4;
                        r += px[p]; g += px[p + 1]; b += px[p + 2]; a += px[p + 3]; n++;
                    }
                n = Math.Max(n, 1);
                int i = ty * G + tx;
                rgb[i * 3] = r / n / 255; rgb[i * 3 + 1] = g / n / 255; rgb[i * 3 + 2] = b / n / 255;
                alpha[i] = a / n / 255;
                s.Luma[i] = 0.299f * rgb[i * 3] + 0.587f * rgb[i * 3 + 1] + 0.114f * rgb[i * 3 + 2];
            }
        for (int ty = 0; ty < 16; ty++)
            for (int tx = 0; tx < 16; tx++) {
                int i = ty * 16 + tx;
                for (int c = 0; c < 3; c++) {
                    float v = 0;
                    for (int k = 0; k < 4; k++) v += rgb[((ty * 2 + k / 2) * G + tx * 2 + k % 2) * 3 + c];
                    s.Rgb[i * 3 + c] = v / 4;
                }
                float al = 0;
                for (int k = 0; k < 4; k++) al += alpha[(ty * 2 + k / 2) * G + tx * 2 + k % 2];
                s.Alpha[i] = al / 4;
            }
        float mean = s.Luma.Average();
        s.Spread = (float)Math.Sqrt(s.Luma.Sum(v => (v - mean) * (v - mean)) / s.Luma.Length);
        for (int i = 0; i < s.Luma.Length; i++) s.Luma[i] = (s.Luma[i] - mean) / Math.Max(s.Spread, 0.02f);
        // the edges: neighbours' differences of the normalised luminance (wrapping: textures tile)
        for (int y = 0; y < G; y++)
            for (int x = 0; x < G; x++) {
                int i = y * G + x;
                s.Grad[i * 2] = s.Luma[y * G + (x + 1) % G] - s.Luma[i];
                s.Grad[i * 2 + 1] = s.Luma[(y + 1) % G * G + x] - s.Luma[i];
            }
        return s;
    }

    static float Distance(Sig a, Sig b) {
        float shape = 0, edge = 0, colour = 0, alpha = 0;
        for (int i = 0; i < a.Luma.Length; i++) shape += Math.Abs(a.Luma[i] - b.Luma[i]);
        for (int i = 0; i < a.Grad.Length; i++) edge += Math.Abs(a.Grad[i] - b.Grad[i]);
        for (int i = 0; i < 768; i++) colour += Math.Abs(a.Rgb[i] - b.Rgb[i]);
        for (int i = 0; i < 256; i++) alpha += Math.Abs(a.Alpha[i] - b.Alpha[i]);
        return shape / a.Luma.Length * 0.4f + edge / a.Grad.Length * 0.4f + colour / 768 * 2.0f + alpha / 256 * 1.0f;
    }

    // every xbox texture's best pack image under the raw distance and under CSLS (cross-domain similarity local
    // scaling: 2d - rx - rp, r each side's mean distance to its k nearest on the other side), which marks down the
    // hubs: a plain grey panel near everything. Columns: xbox, raw best, raw d, csls best, csls, its runner-up,
    // csls-mutual (the pack image's own csls best is this texture), d of the csls best
    // a pair kept: mutual, CSLS this low or lower, the runner-up this much higher (on the eye-checked pairs: ~95% right;
    // its new pairs past -0.25 ran 67-84% right by score, so they want an eye before going in)
    const float kCsls = -0.25f, kCslsMargin = 0.03f;
    static int WriteAll(string file, string map, List<Sig> xs, List<Sig> ps, float[,] D) {
        const int K = 10;
        int nx = xs.Count, np = ps.Count;
        var rx = new float[nx];
        var rp = new float[np];
        var tmp = new List<float>();
        for (int i = 0; i < nx; i++) {
            tmp.Clear();
            for (int j = 0; j < np; j++) if (!float.IsNaN(D[i, j])) tmp.Add(D[i, j]);
            tmp.Sort();
            rx[i] = tmp.Count == 0 ? 0 : tmp.Take(K).Average();
        }
        for (int j = 0; j < np; j++) {
            tmp.Clear();
            for (int i = 0; i < nx; i++) if (!float.IsNaN(D[i, j])) tmp.Add(D[i, j]);
            tmp.Sort();
            rp[j] = tmp.Count == 0 ? 0 : tmp.Take(K).Average();
        }
        var pBest = new int[np];
        var pBestC = new float[np];
        for (int j = 0; j < np; j++) { pBest[j] = -1; pBestC[j] = float.MaxValue; }
        var lines = new List<string>();
        var bestJ = new int[nx];
        var bestC = new float[nx];
        var secC = new float[nx];
        var rawJ = new int[nx];
        for (int i = 0; i < nx; i++) {
            float bc = float.MaxValue, sc = float.MaxValue, bd = float.MaxValue;
            int bj = -1, rj = -1;
            for (int j = 0; j < np; j++) {
                float d = D[i, j];
                if (float.IsNaN(d)) continue;
                if (d < bd) { bd = d; rj = j; }
                float c = 2 * d - rx[i] - rp[j];
                if (c < bc) { sc = bc; bc = c; bj = j; } else if (c < sc) sc = c;
                if (c < pBestC[j]) { pBestC[j] = c; pBest[j] = i; }
            }
            bestJ[i] = bj; bestC[i] = bc; secC[i] = sc; rawJ[i] = rj;
        }
        var inv = CultureInfo.InvariantCulture;
        for (int i = 0; i < nx; i++) {
            if (bestJ[i] < 0) continue;
            int j = bestJ[i];
            bool mutual = pBest[j] == i || bestC[i] <= pBestC[j] + 0.02f;
            lines.Add(xs[i].Name + " " + ps[rawJ[i]].Name + " " + D[i, rawJ[i]].ToString("0.000", inv) + " " + ps[j].Name + " " +
                      bestC[i].ToString("0.000", inv) + " " + (secC[i] == float.MaxValue ? "9" : secC[i].ToString("0.000", inv)) + " " +
                      (mutual ? 1 : 0) + " " + D[i, j].ToString("0.000", inv));
        }
        File.WriteAllLines(file, lines);
        var kept = new List<string> { "# xbox_hash pack_file csls runner_up (BlackQuestBuilder --match: CSLS mutual best matches; lower is closer)" };
        for (int i = 0; i < nx; i++) {
            int j = bestJ[i];
            if (j < 0) continue;
            bool mutual = pBest[j] == i || bestC[i] <= pBestC[j] + 0.02f;
            if (!mutual || bestC[i] > kCsls || secC[i] - bestC[i] < kCslsMargin) continue;
            kept.Add(xs[i].Name + " " + ps[j].Name + " " + bestC[i].ToString("0.000", inv) + " " +
                     (secC[i] == float.MaxValue ? "-" : secC[i].ToString("0.000", inv)));
        }
        File.WriteAllLines(map, kept);
        return kept.Count - 1;
    }

    // a review sheet: pairs (the Xbox texture, then its pack image), 96 px each, rows of 6 pairs, the score on each;
    // every step-th line of the map sorted by score (from 'from' on)
    public static void Sheet(string pack, string dumps, string map, string png, int from, int step, int count) {
        string dir = FindReplacements(pack);
        var rows = File.ReadAllLines(map).Where(l => !l.StartsWith("#")).Select(l => l.Split(' '))
                       .OrderBy(f => float.Parse(f[2], CultureInfo.InvariantCulture)).ToList();
        var pick = new List<string[]>();
        for (int i = from; i < rows.Count && pick.Count < count; i += step) pick.Add(rows[i]);
        const int T = 96, cols = 6;
        int rowsN = (pick.Count + cols - 1) / cols;
        using (var bmp = new Bitmap(cols * (2 * T + 12), rowsN * (T + 16)))
        using (var g = Graphics.FromImage(bmp)) {
            g.Clear(Color.Black);
            g.InterpolationMode = InterpolationMode.HighQualityBilinear;
            for (int i = 0; i < pick.Count; i++) {
                int x = i % cols * (2 * T + 12), y = i / cols * (T + 16);
                using (var a = Thumb(Path.Combine(dumps, pick[i][0] + ".png"))) if (a != null) g.DrawImage(a, x, y, T, T);
                using (var b = Thumb(Path.Combine(dir, pick[i][1]))) if (b != null) g.DrawImage(b, x + T + 2, y, T, T);
                g.DrawString(pick[i][2], new Font("Consolas", 8), Brushes.Yellow, x, y + T);
            }
            bmp.Save(png, ImageFormat.Png);
        }
    }

    static Bitmap Thumb(string path) {
        try {
            byte[] px;
            int w, h;
            if (path.EndsWith(".dds", StringComparison.OrdinalIgnoreCase)) {
                var d = ReadDds(path);
                int l = 0;
                while (l + 1 < d.Levels.Count && Math.Max(d.W >> l, d.H >> l) > 128) l++;
                w = Math.Max(1, d.W >> l); h = Math.Max(1, d.H >> l);
                px = Decode(d.Levels[l], w, h, d.Format);
            } else {
                px = ReadImage(path, out w, out h);
            }
            var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb);
            var data = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
            var bgra = new byte[w * h * 4];
            for (int i = 0; i < bgra.Length; i += 4) { bgra[i] = px[i + 2]; bgra[i + 1] = px[i + 1]; bgra[i + 2] = px[i]; bgra[i + 3] = 255; }
            for (int y = 0; y < h; y++) Marshal.Copy(bgra, y * w * 4, data.Scan0 + y * data.Stride, w * 4);
            bmp.UnlockBits(data);
            return bmp;
        } catch { return null; }
    }

    // dumps: the Xbox dump (textures_dump with index.csv); writes map (xbox hash, pack file, score, runner-up)
    public static void Match(string pack, string dumps, string map, Action<string> log) {
        string dir = FindReplacements(pack);
        var packSigs = new List<Sig>();
        foreach (string f in Directory.EnumerateFiles(dir)) {
            try {
                byte[] px;
                int w, h;
                if (f.EndsWith(".dds", StringComparison.OrdinalIgnoreCase)) {
                    var d = ReadDds(f);
                    if (d == null) continue;
                    int l = 0;  // a small level to decode
                    while (l + 1 < d.Levels.Count && Math.Max(d.W >> l, d.H >> l) > 128) l++;
                    w = Math.Max(1, d.W >> l); h = Math.Max(1, d.H >> l);
                    px = Decode(d.Levels[l], w, h, d.Format);
                } else if (f.EndsWith(".png", StringComparison.OrdinalIgnoreCase)) {
                    px = ReadImage(f, out w, out h);
                } else continue;
                packSigs.Add(MakeSig(Path.GetFileName(f), px, w, h));
            } catch (Exception e) { log(Path.GetFileName(f) + ": " + e.Message); }
        }
        log("pack: " + packSigs.Count + " images");
        var xsigs = new List<Sig>();
        foreach (string line in File.ReadAllLines(Path.Combine(dumps, "index.csv"))) {
            string[] f = line.Split(',');
            if (f.Length < 4) continue;
            string png = Path.Combine(dumps, f[0] + ".png");
            if (!File.Exists(png) || xsigs.Any(x => x.Name == f[0])) continue;
            var px = ReadImage(png, out int w, out int h);
            if (w < 16 || h < 16) continue;  // (tiny ones: nothing to sharpen)
            var s = MakeSig(f[0], px, w, h);
            if (s.Spread < 0.02f) continue;  // (flat colours: anything matches them)
            xsigs.Add(s);
        }
        // every pair of agreeing aspect; each side's best
        var best = new Dictionary<Sig, KeyValuePair<Sig, float>>();      // xbox -> its best pack image
        var packBest = new Dictionary<Sig, float>();                       // pack image -> its best distance to any xbox
        var second = new Dictionary<Sig, float>();
        // every distance kept (NaN: aspects disagree), for the hub-aware scoring below
        var D = new float[xsigs.Count, packSigs.Count];
        int xi = -1;
        foreach (var s in xsigs) {
            xi++;
            float bd = float.MaxValue, sd = float.MaxValue;
            Sig bp = null;
            int pi = -1;
            foreach (var p in packSigs) {
                pi++;
                D[xi, pi] = float.NaN;
                if (Math.Abs(p.Aspect / s.Aspect - 1) > 0.03f) continue;
                float d = Distance(s, p);
                D[xi, pi] = d;
                if (d < bd) { sd = bd; bd = d; bp = p; } else if (d < sd) sd = d;
                if (!packBest.TryGetValue(p, out float pb) || d < pb) packBest[p] = d;
            }
            if (bp != null) { best[s] = new KeyValuePair<Sig, float>(bp, bd); second[s] = sd; }
        }
        // kept: mutual (the pack image's own best is this one, or within 10% of it: the same picture under another
        // hash), and clear of the runner-up
        var lines = new List<string> { "# xbox_hash pack_file score runner_up (BlackQuestBuilder --match: mutual best matches; lower is closer)" };
        // the near misses beside it (map.near, the same columns and why each was left: for review sheets)
        var near = new List<string> { "# xbox_hash pack_file score runner_up why (left out by --match: for review)" };
        int n = 0;
        foreach (var kv in best.OrderBy(k => k.Value.Value)) {
            var p = kv.Value.Key;
            float d = kv.Value.Value;
            bool mutual = d <= packBest[p] * 1.10f + 0.01f;
            bool clear = second[kv.Key] > d * 1.08f;
            // (past 0.68 the review sheet showed wrong pairs: a phone for a curtain, a blue panel for a blueprint)
            if (!mutual || !clear || d > 0.68f) {
                if (d <= 0.95f)
                    near.Add(kv.Key.Name + " " + p.Name + " " + d.ToString("0.000", CultureInfo.InvariantCulture) + " " +
                             (second[kv.Key] == float.MaxValue ? "-" : second[kv.Key].ToString("0.000", CultureInfo.InvariantCulture)) +
                             " " + (!mutual ? "not-mutual" : !clear ? "unclear" : "far"));
                continue;
            }
            lines.Add(kv.Key.Name + " " + p.Name + " " + d.ToString("0.000", CultureInfo.InvariantCulture) + " " +
                      (second[kv.Key] == float.MaxValue ? "-" : second[kv.Key].ToString("0.000", CultureInfo.InvariantCulture)));
            n++;
        }
        // (the raw rule's pairs beside it, to compare; the map is CSLS's: graded against 300 pairs checked by eye and
        // 215 rejected, 2026-11-26, it kept 259 right for 15 wrong where the raw rule kept 196)
        File.WriteAllLines(map + ".raw", lines);
        File.WriteAllLines(map + ".near", near);
        int kept = WriteAll(map + ".all", map, xsigs, packSigs, D);
        log("matched " + kept + " of " + xsigs.Count + " xbox textures (CSLS: mutual, <= " + kCsls + ", clear by " + kCslsMargin +
            "; the raw rule's " + n + " in " + Path.GetFileName(map) + ".raw)");
    }
}

}  // namespace BlackQuestBuilder
