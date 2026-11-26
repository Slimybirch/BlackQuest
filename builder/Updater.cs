// The Builder's updater: BlackQuest's newest release on GitHub over the one in this folder (VERSION), its zip
// downloaded and laid over the folder (the files the old release had and the new one dropped removed, by each
// zip's MANIFEST.txt), the Builder itself swapped (a running exe can be renamed, not overwritten). The rebuild and
// install that follow are the Builder's own. Never touches what isn't the release's: the disc, toolchain\, out\,
// generated\, assets\ or the build's caches.
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;

namespace BlackQuestBuilder {

public sealed class Release {
    public string Tag = "", Name = "", Zip = "", Changes = "";
    public bool Full;    // the game's code is recompiled (Rebuild: full): the first build's 30-90 minutes again
    public bool HdRedo;  // the HD textures converted and copied again (HD textures: redo)
}

public static class Updater {
    const string Latest = "https://api.github.com/repos/Slimybirch/BlackQuest/releases/latest";
    public const string ExeName = "BlackQuestBuilder.exe";

    public static string Installed(string root) {
        try { return File.ReadAllText(Path.Combine(root, "VERSION")).Trim(); } catch { return "0"; }
    }

    // a clone updates with git, not over itself
    public static bool IsGitCheckout(string root) => Directory.Exists(Path.Combine(root, ".git"));

    // the newest release when it's newer than this folder's, else null (no release yet, offline: null too)
    public static Release Check(string root, out string error) {
        error = null;
        try {
            ServicePointManager.SecurityProtocol = SecurityProtocolType.Tls12;
            string json;
            using (var w = new WebClient()) {
                w.Headers[HttpRequestHeader.UserAgent] = "BlackQuestBuilder";
                w.Headers[HttpRequestHeader.Accept] = "application/vnd.github+json";
                json = w.DownloadString(Latest);
            }
            var j = new JavaScriptSerializer().Deserialize<Dictionary<string, object>>(json);
            var r = new Release { Tag = j["tag_name"] as string ?? "" };
            r.Name = j.TryGetValue("name", out object n) && n is string s && s.Length > 0 ? s : r.Tag;
            string body = j.TryGetValue("body", out object b) ? b as string ?? "" : "";
            if (j.TryGetValue("assets", out object a) && a is System.Collections.ArrayList list)
                foreach (Dictionary<string, object> asset in list)
                    if (((string)asset["name"]).EndsWith(".zip", StringComparison.OrdinalIgnoreCase)) {
                        r.Zip = (string)asset["browser_download_url"];
                        break;
                    }
            r.Full = Regex.IsMatch(body, @"^Rebuild:\s*full", RegexOptions.Multiline | RegexOptions.IgnoreCase);
            r.HdRedo = Regex.IsMatch(body, @"^HD textures:\s*redo", RegexOptions.Multiline | RegexOptions.IgnoreCase);
            r.Changes = Changes(body);
            if (r.Zip.Length == 0 || Compare(r.Tag, Installed(root)) <= 0) return null;
            return r;
        } catch (WebException e) when (e.Response is HttpWebResponse h && h.StatusCode == HttpStatusCode.NotFound) {
            return null;  // no release yet
        } catch (Exception e) {
            error = e.Message;
            return null;
        }
    }

    // the "What's Changed" list's lines
    static string Changes(string body) {
        int at = body.IndexOf("## What's Changed", StringComparison.Ordinal);
        if (at < 0) return "";
        var lines = new List<string>();
        foreach (string raw in body.Substring(at).Split('\n').Skip(1)) {
            string line = raw.Trim();
            if (line.StartsWith("## ") || line.StartsWith("**Full Changelog**")) break;
            if (line.StartsWith("- ") || line.StartsWith("* ")) lines.Add("- " + line.Substring(2));
        }
        return string.Join(Environment.NewLine, lines.Take(15));
    }

    // dotted numbers, a leading v and anything after "-" left out
    public static int Compare(string a, string b) {
        int[] x = Parts(a), y = Parts(b);
        for (int i = 0; i < Math.Max(x.Length, y.Length); i++) {
            int p = i < x.Length ? x[i] : 0, q = i < y.Length ? y[i] : 0;
            if (p != q) return p.CompareTo(q);
        }
        return 0;
    }

    static int[] Parts(string v) {
        v = (v ?? "").Trim().TrimStart('v', 'V');
        int dash = v.IndexOf('-');
        if (dash >= 0) v = v.Substring(0, dash);
        return v.Split('.').Select(p => int.TryParse(Regex.Match(p, @"^\d+").Value, out int n) ? n : 0).ToArray();
    }

    // downloads and lays the release over the folder (false: it failed); builderChanged: the Builder's exe was
    // replaced, so the new one is started to carry on
    public static bool Apply(string root, Release r, Action<string> log, out bool builderChanged) {
        builderChanged = false;
        string work = Path.Combine(root, "out", "update");
        string zip = Path.Combine(work, "BlackQuest-" + r.Tag.TrimStart('v') + ".zip");
        try {
            Directory.CreateDirectory(work);
            ServicePointManager.SecurityProtocol = SecurityProtocolType.Tls12;
            log("Downloading " + r.Name + "...");
            using (var w = new WebClient()) {
                w.Headers[HttpRequestHeader.UserAgent] = "BlackQuestBuilder";
                w.DownloadFile(r.Zip, zip);
            }
            using (var z = ZipFile.OpenRead(zip)) {
                // every entry under BlackQuest\, its manifest listing them
                const string top = "BlackQuest/";
                var entries = z.Entries.Where(e => e.FullName.StartsWith(top) && e.Name.Length > 0).ToList();
                var manifest = z.GetEntry(top + "MANIFEST.txt");
                if (manifest == null || entries.Count == 0) { log("The release's zip has no MANIFEST.txt: not applied."); return false; }
                HashSet<string> next;
                using (var s = new StreamReader(manifest.Open())) next = Lines(s.ReadToEnd());
                string oldManifest = Path.Combine(root, "MANIFEST.txt");
                var old = File.Exists(oldManifest) ? Lines(File.ReadAllText(oldManifest)) : new HashSet<string>();
                // files the old release had and the new one doesn't (a stale source would still be built)
                foreach (string f in old.Except(next)) {
                    string p = Local(root, f);
                    if (p != null && File.Exists(p)) { File.Delete(p); log("Removed " + f); }
                }
                int written = 0;
                foreach (var e in entries) {
                    string rel = e.FullName.Substring(top.Length);
                    string dest = Local(root, rel);
                    if (dest == null) continue;
                    if (rel.Equals(ExeName, StringComparison.OrdinalIgnoreCase)) {
                        string fresh = Path.Combine(work, ExeName);
                        e.ExtractToFile(fresh, true);
                        builderChanged = !SameBytes(fresh, dest);
                        if (builderChanged) {
                            // the running exe renamed out of the way (deleted at the next start), the new one in
                            string aside = Path.Combine(root, "BlackQuestBuilder.old.exe");
                            if (File.Exists(aside)) File.Delete(aside);
                            File.Move(dest, aside);
                            File.Copy(fresh, dest);
                        }
                        continue;
                    }
                    // only what changed is written, stamped now: the zip's own times can be older than the player's
                    // last build, which would then skip it; an unchanged file keeps its time and isn't rebuilt
                    byte[] data;
                    using (var src = e.Open()) using (var ms = new MemoryStream()) { src.CopyTo(ms); data = ms.ToArray(); }
                    if (File.Exists(dest) && new FileInfo(dest).Length == data.Length && File.ReadAllBytes(dest).SequenceEqual(data)) continue;
                    Directory.CreateDirectory(Path.GetDirectoryName(dest));
                    File.WriteAllBytes(dest, data);
                    written++;
                }
                log("Updated " + written + " files to " + r.Name + ".");
            }
            File.Delete(zip);
            return true;
        } catch (Exception e) {
            log("The update failed: " + e.Message + ". The folder may be part updated: run the update again.");
            return false;
        }
    }

    static HashSet<string> Lines(string text) =>
        new HashSet<string>(text.Split('\n').Select(l => l.Trim().Replace('\\', '/')).Where(l => l.Length > 0), StringComparer.OrdinalIgnoreCase);

    // a release path inside the folder, or null (nothing outside it, nothing in the player's own places)
    static string Local(string root, string rel) {
        rel = rel.Replace('/', Path.DirectorySeparatorChar);
        string full = Path.GetFullPath(Path.Combine(root, rel));
        string top = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        if (!full.StartsWith(top, StringComparison.OrdinalIgnoreCase)) return null;
        string first = rel.Split(Path.DirectorySeparatorChar)[0].ToLowerInvariant();
        if (new[] { "toolchain", "out", "generated", "assets", "iso", "sdk", "ref", ".git", "app\\build", "third_party" }.Contains(first)) return null;
        return full;
    }

    static bool SameBytes(string a, string b) {
        if (!File.Exists(a) || !File.Exists(b)) return false;
        var x = new FileInfo(a);
        var y = new FileInfo(b);
        return x.Length == y.Length && File.ReadAllBytes(a).SequenceEqual(File.ReadAllBytes(b));
    }
}

}  // namespace BlackQuestBuilder
