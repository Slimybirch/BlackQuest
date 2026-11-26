// BlackQuest Builder (phase 10): the player's one-button way from their disc to BlackQuest on their headset.
// It sits in the BlackQuest folder (beside build.bat) and does its work there.
using System;
using System.Collections.Generic;
using System.IO;
using System.Windows.Forms;

namespace BlackQuestBuilder {

static class Program {
    [STAThread]
    static void Main() {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        string root = FindRoot(AppDomain.CurrentDomain.BaseDirectory);
        if (root == null) {
            MessageBox.Show("Put BlackQuestBuilder.exe in the BlackQuest folder (the one with build.bat) and start it from there.",
                            "BlackQuest Builder");
            return;
        }
        // an update's old Builder, renamed while it ran
        try { File.Delete(Path.Combine(root, "BlackQuestBuilder.old.exe")); } catch { }
        // --match <pack> <dumps> <map>: the HD pack's map from an Xbox texture dump (a developer's step, once)
        {
            string[] a = Environment.GetCommandLineArgs();
            int m = Array.IndexOf(a, "--match");
            if (m > 0 && m + 3 < a.Length) {
                var w = new StreamWriter(a[m + 3] + ".log", false) { AutoFlush = true };
                HdPack.Match(a[m + 1], a[m + 2], a[m + 3], line => w.WriteLine(line));
                w.Close();
                Environment.Exit(0);
            }
        }
        // --sheet <pack> <dumps> <map> <png> <from> <step> <count>: a review sheet of the map's pairs
        {
            string[] a = Environment.GetCommandLineArgs();
            int m = Array.IndexOf(a, "--sheet");
            if (m > 0 && m + 7 < a.Length) {
                HdPack.Sheet(a[m + 1], a[m + 2], a[m + 3], a[m + 4], int.Parse(a[m + 5]), int.Parse(a[m + 6]), int.Parse(a[m + 7]));
                Environment.Exit(0);
            }
        }
        // --build <iso> [--hd <dir>] [--device <serial>] --log <file>: the whole job with no window (tests; the exit code
        // says how it went)
        {
            string[] a = Environment.GetCommandLineArgs();
            int b = Array.IndexOf(a, "--build"), l = Array.IndexOf(a, "--log");
            int hi = Array.IndexOf(a, "--hd"), di = Array.IndexOf(a, "--device");  // (the HD pack; a headset to install on)
            string hdArg = hi > 0 && hi + 1 < a.Length ? a[hi + 1] : "", devArg = di > 0 && di + 1 < a.Length ? a[di + 1] : null;
            if (b > 0 && b + 1 < a.Length) {
                string logFile = l > 0 && l + 1 < a.Length ? a[l + 1] : Path.Combine(root, "out", "builder.log");
                var pipe = new Pipeline(root);
                var w = new StreamWriter(logFile, false) { AutoFlush = true };
                pipe.Log = line => { lock (w) w.WriteLine(line); };
                pipe.Progress = (pc, what) => { lock (w) w.WriteLine("== " + pc + "% " + what); };
                bool ok = pipe.Run(a[b + 1], hdArg, devArg);
                w.WriteLine(ok ? "== OK" : "== FAILED");
                w.Close();
                Environment.Exit(ok ? 0 : 1);
            }
        }
        // --apply <zip or url> <tag> --log <file>: an update's apply alone, no window (tests)
        {
            string[] a = Environment.GetCommandLineArgs();
            int u = Array.IndexOf(a, "--apply"), l = Array.IndexOf(a, "--log");
            if (u > 0 && u + 2 < a.Length) {
                var w = new StreamWriter(l > 0 && l + 1 < a.Length ? a[l + 1] : Path.Combine(root, "out", "update.log"), false) { AutoFlush = true };
                string src = a[u + 1];
                var r = new Release { Tag = a[u + 2], Name = "BlackQuest " + a[u + 2], Zip = src.Contains("://") ? src : new Uri(Path.GetFullPath(src)).AbsoluteUri };
                bool ok = Updater.Apply(root, r, line => w.WriteLine(line), out bool changed);
                w.WriteLine((ok ? "== OK" : "== FAILED") + (changed ? ", the Builder changed" : ""));
                w.Close();
                Environment.Exit(ok ? 0 : 1);
            }
        }
        string[] cl = Environment.GetCommandLineArgs();
        var form = new MainForm(root, Array.IndexOf(cl, "--after-update") > 0, Array.IndexOf(cl, "--hd-redo") > 0);
        // --shot <png>: the window drawn to a file once shown, then closed (screenshots for the docs and tests)
        string[] args = Environment.GetCommandLineArgs();
        int at = Array.IndexOf(args, "--shot");
        if (at > 0 && at + 1 < args.Length) {
            string file = args[at + 1];
            var t = new Timer { Interval = 4000 };
            t.Tick += (s, e) => {
                t.Stop();
                using (var bmp = new System.Drawing.Bitmap(form.Width, form.Height)) {
                    form.DrawToBitmap(bmp, new System.Drawing.Rectangle(0, 0, form.Width, form.Height));
                    bmp.Save(file, System.Drawing.Imaging.ImageFormat.Png);
                }
                form.Close();
            };
            form.Shown += (s, e) => t.Start();
        }
        Application.Run(form);
    }

    // the folder with build.bat: this one or one above (out\builder\bin while developing)
    static string FindRoot(string dir) {
        for (var d = new DirectoryInfo(dir); d != null; d = d.Parent)
            if (File.Exists(Path.Combine(d.FullName, "build.bat"))) return d.FullName;
        return null;
    }
}

// the choices kept between runs, in %LOCALAPPDATA%\BlackQuest\builder.txt (key=value)
public sealed class Settings {
    public string Iso = "", HdPack = "", Wifi = "", Device = "", Tools = "", Jdk = "", Sdk = "";
    public bool Cleanup;
    static string FilePath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "BlackQuest", "builder.txt");

    public static Settings Load() {
        var s = new Settings();
        try {
            var kv = new Dictionary<string, string>();
            foreach (string line in File.ReadAllLines(FilePath)) {
                int eq = line.IndexOf('=');
                if (eq > 0) kv[line.Substring(0, eq)] = line.Substring(eq + 1);
            }
            kv.TryGetValue("iso", out s.Iso);
            kv.TryGetValue("hd", out s.HdPack);
            kv.TryGetValue("wifi", out s.Wifi);
            kv.TryGetValue("device", out s.Device);
            kv.TryGetValue("tools", out s.Tools);
            kv.TryGetValue("jdk", out s.Jdk);
            kv.TryGetValue("sdk", out s.Sdk);
            s.Cleanup = kv.TryGetValue("cleanup", out string c) && c == "1";
            s.Tools = s.Tools ?? ""; s.Jdk = s.Jdk ?? ""; s.Sdk = s.Sdk ?? "";
        } catch { }
        return s;
    }

    public void Save() {
        try {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath));
            File.WriteAllLines(FilePath, new[] { "iso=" + Iso, "hd=" + HdPack, "wifi=" + Wifi, "device=" + Device, "tools=" + Tools,
                                                 "jdk=" + Jdk, "sdk=" + Sdk, "cleanup=" + (Cleanup ? "1" : "0") });
        } catch { }
    }
}

}  // namespace BlackQuestBuilder
