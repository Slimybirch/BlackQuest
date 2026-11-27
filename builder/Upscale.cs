// The textures the HD pack has no match for, upscaled 4x by Real-ESRGAN (realesrgan-ncnn-vulkan, MIT: run on the PC's
// GPU through Vulkan) from the player's own texture dump, under the dump's own hash, so the converter and the loader
// take them as they take the pack's. Each is padded by a wrap of itself before (tiling textures keep no seam at
// their edges) and cropped after; its alpha, if it has one, upscaled on its own as a grey image. Never in git: the
// game's pictures and anything made from them stay on the player's PC and headset.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;

namespace BlackQuestBuilder {

public static class Upscaler {
    public const int Cap = 1024;  // the converter's size cap

    sealed class Item {
        public string Hash;
        public int W, H, Pad;
        public bool Alpha;
    }

    // dumps: textures_dump (index.csv and <hash>.png); map: the HD pairs (their textures left out); outDir: <hash>.png
    // upscaled; sample: at most this many, spread over the dump (0: all); sheet: review sheets' path prefix (or null)
    // the candidates in order (the sorting's numbers): without an HD pair, 32 px and up, not flat, not huge
    static List<Item> Candidates(string dumps, string map) {
        var matched = new HashSet<string>(File.ReadAllLines(map).Where(l => !l.StartsWith("#") && l.Trim().Length > 0)
                                              .Select(l => l.Split(' ')[0]));
        var items = new List<Item>();
        var seen = new HashSet<string>();
        foreach (string line in File.ReadAllLines(Path.Combine(dumps, "index.csv"))) {
            string[] f = line.Split(',');
            if (f.Length < 4 || matched.Contains(f[0]) || !seen.Add(f[0])) continue;
            string png = Path.Combine(dumps, f[0] + ".png");
            if (!File.Exists(png)) continue;
            var px = Read(png, out int w, out int h);
            if (Math.Min(w, h) < 32) continue;              // (tiny: glyphs, dots)
            if (Spread(px) < 0.03f) continue;               // (flat colours: nothing to gain)
            if (w * 4 > Cap * 2 && h * 4 > Cap * 2) continue;  // (big already: the front end's 640x480 screens)
            int a = 0;
            for (int i = 3; i < px.Length; i += 4) if (px[i] < 250) a++;
            items.Add(new Item { Hash = f[0], W = w, H = h, Alpha = a > px.Length / 4 / 50, Pad = Math.Max(4, Math.Min(w, h) / 8) });
        }
        return items.OrderBy(i => i.Hash, StringComparer.Ordinal).ToList();
    }

    // contact sheets of the candidates (their numbers, an "a" for alpha), 8 x 8 a sheet: for sorting them by eye
    public static void Contact(string dumps, string map, string prefix) {
        var items = Candidates(dumps, map);
        const int T = 120, cols = 8, per = 64;
        for (int s = 0; s * per < items.Count; s++) {
            var page = items.Skip(s * per).Take(per).ToList();
            using (var bmp = new Bitmap(cols * (T + 4), (page.Count + cols - 1) / cols * (T + 14)))
            using (var g = Graphics.FromImage(bmp)) {
                g.Clear(Color.Black);
                g.InterpolationMode = InterpolationMode.HighQualityBilinear;
                for (int i = 0; i < page.Count; i++) {
                    var px = Read(Path.Combine(dumps, page[i].Hash + ".png"), out int w, out int h);
                    int x = i % cols * (T + 4), y = i / cols * (T + 14);
                    using (var b = ToBitmap(px, w, h)) g.DrawImage(b, new Rectangle(x, y, T, T));
                    g.DrawString((s * per + i) + (page[i].Alpha ? " a" : ""), new Font("Consolas", 8), Brushes.Yellow, x, y + T);
                }
                bmp.Save(prefix + s + ".png", ImageFormat.Png);
            }
        }
    }

    // only: the candidates' numbers to do (null: all but alpha ones); method "esrgan" (the model) or "bicubic" (4x
    // bicubic then an unsharp mask: no invented detail, for fine regular patterns and text)
    public static int Run(string dumps, string map, string outDir, string exe, string model, int sample, string sheet,
                          Action<string> log, HashSet<int> only = null, string method = "esrgan",
                          HashSet<int> skip = null) {
        var all = Candidates(dumps, map);
        var items = new List<Item>();
        for (int k = 0; k < all.Count; k++)
            if (only != null ? only.Contains(k) : !all[k].Alpha && (skip == null || !skip.Contains(k))) items.Add(all[k]);
        log(all.Count + " textures without an HD match; " + items.Count + " to upscale (" + method + ")");
        if (sample > 0 && items.Count > sample) {
            var pick = new List<Item>();
            for (int k = 0; k < sample; k++) pick.Add(items[k * items.Count / sample]);
            items = pick;
        }
        string work = Path.Combine(Path.GetTempPath(), "bq_upscale");
        string inDir = Path.Combine(work, "in"), upDir = Path.Combine(work, "out");
        foreach (string d in new[] { inDir, upDir }) {
            if (Directory.Exists(d)) Directory.Delete(d, true);
            Directory.CreateDirectory(d);
        }
        Directory.CreateDirectory(outDir);
        foreach (var it in items) {
            var px = Read(Path.Combine(dumps, it.Hash + ".png"), out int w, out int h);
            int p = it.Pad, pw = w + 2 * p, ph = h + 2 * p;
            var rgb = new byte[pw * ph * 4];
            var al = it.Alpha ? new byte[pw * ph * 4] : null;
            for (int y = 0; y < ph; y++)
                for (int x = 0; x < pw; x++) {
                    int sx = ((x - p) % w + w) % w, sy = ((y - p) % h + h) % h, s = (sy * w + sx) * 4, o = (y * pw + x) * 4;
                    rgb[o] = px[s]; rgb[o + 1] = px[s + 1]; rgb[o + 2] = px[s + 2]; rgb[o + 3] = 255;
                    if (al != null) { al[o] = al[o + 1] = al[o + 2] = px[s + 3]; al[o + 3] = 255; }
                }
            Write(Path.Combine(inDir, it.Hash + ".png"), rgb, pw, ph);
            if (al != null) Write(Path.Combine(inDir, it.Hash + "_a.png"), al, pw, ph);
        }
        if (method == "bicubic") {
            foreach (var it in items) {
                var src = Read(Path.Combine(inDir, it.Hash + ".png"), out int iw, out int ih);
                Write(Path.Combine(upDir, it.Hash + ".png"), Sharpen(Scale(src, iw, ih, iw * 4, ih * 4, out _, out _), iw * 4, ih * 4), iw * 4, ih * 4);
                string ap = Path.Combine(inDir, it.Hash + "_a.png");
                if (File.Exists(ap)) {
                    var a = Read(ap, out _, out _);
                    Write(Path.Combine(upDir, it.Hash + "_a.png"), Scale(a, iw, ih, iw * 4, ih * 4, out _, out _), iw * 4, ih * 4);
                }
            }
        } else {
        log("upscaling " + items.Count + " with " + model + "...");
        var sw = Stopwatch.StartNew();
        var psi = new ProcessStartInfo(exe, "-i \"" + inDir + "\" -o \"" + upDir + "\" -n " + model + " -s 4 -f png") {
            UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true, RedirectStandardOutput = true,
            WorkingDirectory = Path.GetDirectoryName(exe)
        };
        using (var proc = Process.Start(psi)) {
            proc.ErrorDataReceived += (s, e) => { };
            proc.BeginErrorReadLine();
            proc.StandardOutput.ReadToEnd();
            proc.WaitForExit();
            log("realesrgan exit " + proc.ExitCode + " in " + sw.Elapsed.TotalSeconds.ToString("0") + " s");
        }
        }
        int done = 0;
        var pairs = new List<KeyValuePair<Item, byte[]>>();
        foreach (var it in items) {
            string up = Path.Combine(upDir, it.Hash + ".png");
            if (!File.Exists(up)) { log(it.Hash + ": not upscaled"); continue; }
            var big = Read(up, out int bw, out int bh);
            int p = it.Pad * 4, w = it.W * 4, h = it.H * 4;
            var o = new byte[w * h * 4];
            byte[] abig = null;
            if (it.Alpha && File.Exists(Path.Combine(upDir, it.Hash + "_a.png"))) abig = Read(Path.Combine(upDir, it.Hash + "_a.png"), out _, out _);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    int s = ((y + p) * bw + x + p) * 4, d = (y * w + x) * 4;
                    o[d] = big[s]; o[d + 1] = big[s + 1]; o[d + 2] = big[s + 2];
                    o[d + 3] = abig != null ? abig[s] : (byte)255;
                }
            if (Math.Max(w, h) > Cap) o = Scale(o, w, h, Cap * w / Math.Max(w, h), Cap * h / Math.Max(w, h), out w, out h);
            Write(Path.Combine(outDir, it.Hash + ".png"), o, w, h);
            pairs.Add(new KeyValuePair<Item, byte[]>(it, o));
            done++;
        }
        if (sheet != null) Sheets(dumps, pairs, sheet);
        log(done + " upscaled into " + outDir);
        return done;
    }

    // review sheets: each texture whole (the original left, upscaled right), then a quarter-size crop of its middle
    // magnified the same (the original smoothed as the game filters it), 4 a row, 24 a sheet
    static void Sheets(string dumps, List<KeyValuePair<Item, byte[]>> pairs, string prefix) {
        const int T = 160, per = 24, cols = 4, cellW = 4 * T + 18, cellH = T + 16;
        for (int s = 0; s * per < pairs.Count; s++) {
            var page = pairs.Skip(s * per).Take(per).ToList();
            int rows = (page.Count + cols - 1) / cols;
            using (var bmp = new Bitmap(cols * cellW, rows * cellH))
            using (var g = Graphics.FromImage(bmp)) {
                g.Clear(Color.Black);
                for (int i = 0; i < page.Count; i++) {
                    var it = page[i].Key;
                    int x = i % cols * cellW, y = i / cols * cellH;
                    var src = Read(Path.Combine(dumps, it.Hash + ".png"), out int w, out int h);
                    int uw = Math.Min(it.W * 4, Cap), uh = uw * it.H / it.W;
                    using (var a = ToBitmap(src, w, h))
                    using (var b = ToBitmap(page[i].Value, uw, uh)) {
                        g.InterpolationMode = InterpolationMode.HighQualityBilinear;
                        g.DrawImage(a, new Rectangle(x, y, T, T));
                        g.DrawImage(b, new Rectangle(x + T + 2, y, T, T));
                        // the middle quarter of each, magnified
                        g.DrawImage(a, new Rectangle(x + 2 * T + 10, y, T, T), new Rectangle(w * 3 / 8, h * 3 / 8, w / 4, h / 4), GraphicsUnit.Pixel);
                        g.DrawImage(b, new Rectangle(x + 3 * T + 12, y, T, T), new Rectangle(uw * 3 / 8, uh * 3 / 8, uw / 4, uh / 4), GraphicsUnit.Pixel);
                    }
                    g.DrawString(i + " " + it.Hash.Substring(0, 8) + (it.Alpha ? " a" : ""), new Font("Consolas", 8), Brushes.Yellow, x, y + T);
                }
                bmp.Save(prefix + s + ".png", ImageFormat.Png);
            }
        }
    }

    // an unsharp mask (radius 2, amount 0.8): the bicubic's softness taken off its edges
    static byte[] Sharpen(byte[] px, int w, int h) {
        var o = (byte[])px.Clone();
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                for (int c = 0; c < 3; c++) {
                    int sum = 0, n = 0;
                    for (int dy = -2; dy <= 2; dy++)
                        for (int dx = -2; dx <= 2; dx++) {
                            int xx = (x + dx + w) % w, yy = (y + dy + h) % h;
                            sum += px[(yy * w + xx) * 4 + c]; n++;
                        }
                    int i = (y * w + x) * 4 + c;
                    o[i] = (byte)Math.Max(0, Math.Min(255, px[i] + 0.8f * (px[i] - sum / (float)n)));
                }
        return o;
    }

    static float Spread(byte[] px) {
        double sum = 0, sq = 0;
        int n = px.Length / 4;
        for (int i = 0; i < px.Length; i += 4) {
            double l = (0.299 * px[i] + 0.587 * px[i + 1] + 0.114 * px[i + 2]) / 255;
            sum += l; sq += l * l;
        }
        double m = sum / n;
        return (float)Math.Sqrt(Math.Max(0, sq / n - m * m));
    }

    static byte[] Scale(byte[] px, int w, int h, int nw, int nh, out int ow, out int oh) {
        ow = nw; oh = nh;
        using (var a = ToBitmap(px, w, h))
        using (var b = new Bitmap(nw, nh, PixelFormat.Format32bppArgb))
        using (var g = Graphics.FromImage(b)) {
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.CompositingMode = CompositingMode.SourceCopy;
            g.DrawImage(a, new Rectangle(0, 0, nw, nh));
            return FromBitmap(b);
        }
    }

    // RGBA bytes <-> bitmaps
    static byte[] Read(string path, out int w, out int h) {
        using (var b = new Bitmap(path)) {
            w = b.Width; h = b.Height;
            return FromBitmap(b);
        }
    }
    static byte[] FromBitmap(Bitmap bmp) {
        int w = bmp.Width, h = bmp.Height;
        var data = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        var px = new byte[w * h * 4];
        for (int y = 0; y < h; y++) Marshal.Copy(data.Scan0 + y * data.Stride, px, y * w * 4, w * 4);
        bmp.UnlockBits(data);
        for (int i = 0; i < px.Length; i += 4) { byte t = px[i]; px[i] = px[i + 2]; px[i + 2] = t; }
        return px;
    }
    static Bitmap ToBitmap(byte[] rgba, int w, int h) {
        var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb);
        var data = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
        var bgra = (byte[])rgba.Clone();
        for (int i = 0; i < bgra.Length; i += 4) { byte t = bgra[i]; bgra[i] = bgra[i + 2]; bgra[i + 2] = t; }
        for (int y = 0; y < h; y++) Marshal.Copy(bgra, y * w * 4, data.Scan0 + y * data.Stride, w * 4);
        bmp.UnlockBits(data);
        return bmp;
    }
    static void Write(string path, byte[] rgba, int w, int h) {
        using (var b = ToBitmap(rgba, w, h)) b.Save(path, ImageFormat.Png);
    }
}

}  // namespace BlackQuestBuilder
