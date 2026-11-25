// The Builder's work, off the window's thread: the tools (fetched into toolchain\ when missing), the game's code
// built from the player's disc (build.bat), and the headset (adb: install, the disc copied to the app's folder).
// Nothing system-wide but Visual Studio's C++ build tools, whose own installer asks first.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Net;
using System.Text;
using System.Threading;

namespace BlackQuestBuilder {

public sealed class Pipeline {
    public const string Package = "com.slimybirch.blackquest";
    const string kNdk = "28.2.13676358", kCmake = "3.31.6";
    const string kJdkUrl = "https://api.adoptium.net/v3/binary/latest/17/ga/windows/x64/jdk/hotspot/normal/eclipse";
    const string kSdkToolsUrl = "https://dl.google.com/android/repository/commandlinetools-win-13114758_latest.zip";
    const string kPlatformToolsUrl = "https://dl.google.com/android/repository/platform-tools-latest-windows.zip";
    const string kPythonUrl = "https://www.python.org/ftp/python/3.11.9/python-3.11.9-embed-amd64.zip";
    const string kGitUrl = "https://github.com/git-for-windows/git/releases/download/v2.47.1.windows.1/MinGit-2.47.1-64-bit.zip";
    const string kVsUrl = "https://aka.ms/vs/17/release/vs_BuildTools.exe";

    public readonly string Root;  // the BlackQuest folder (where build.bat is)
    public string ToolsDir;       // where downloaded tools go (the player's choice; toolchain\ by default)
    public string JdkOverride, SdkOverride;  // the player's own Java 17+ and Android SDK, when pointed to
    public bool CleanupAfter;     // the downloaded tools removed once the whole job succeeds
    public Func<string, bool> Ask = _ => true;  // a yes/no question to the player (the window's)
    readonly List<string> downloaded = new List<string>();  // this run's downloads, for the cleanup
    bool vsInstalled;
    public Action<string> Log = _ => { };
    public Action<int, string> Progress = (_, __) => { };  // percent, the stage's name
    public volatile bool Cancel;

    public Pipeline(string root) { Root = root; ToolsDir = Path.Combine(root, "toolchain"); }

    string Toolchain => ToolsDir;
    string Sdk => !string.IsNullOrEmpty(SdkOverride) ? SdkOverride : Path.Combine(Toolchain, "android-sdk");
    string Jdk => !string.IsNullOrEmpty(JdkOverride) ? JdkOverride : Path.Combine(Toolchain, "jdk17");
    public string HdOut => Path.Combine(Root, "out", "textures_hd");
    public string Adb => Path.Combine(Sdk, "platform-tools", "adb.exe");
    public string Apk => Path.Combine(Root, "app", "build", "outputs", "apk", "release", "app-release.apk");

    // ---- the whole job: tools, build, then the headset when one is picked
    public bool Run(string iso, string hdPack, string device) {
        try {
            Progress(2, "Checking the tools");
            if (!Tools()) return false;
            Progress(15, "Reading the disc");
            if (!PrepareDisc(iso)) return false;
            if (!Build(iso)) return false;
            bool hd = !string.IsNullOrEmpty(hdPack);
            if (hd) {
                Progress(86, "Converting the HD textures");
                string map = Path.Combine(Root, "tools", "hd_map.txt");
                if (HdPack.Convert(hdPack, map, HdOut, 1024, Log, () => Cancel) < 0) return false;
            }
            bool ok = true;
            if (string.IsNullOrEmpty(device)) {
                Progress(100, "Built");
                Log("Built: " + Apk);
            } else {
                ok = Install(device, iso, hd);
            }
            if (ok && CleanupAfter) Cleanup();
            return ok;
        } catch (Exception e) {
            Log("Stopped: " + e.Message);
            return false;
        }
    }

    // ---- tools
    // what's missing, and about how much it takes (download, then on disk)
    List<string> Missing(out double gb) {
        var list = new List<string>();
        gb = 0;
        if (!File.Exists(Path.Combine(Jdk, "lib", "jvm.cfg"))) { list.Add("Java 17 (0.2 GB, 0.3 GB on disk)"); gb += 0.3; }
        if (!SdkReady()) { list.Add("the Android SDK, NDK and CMake (1.2 GB, 3.5 GB on disk)"); gb += 3.5; }
        if (FindOnPath("python.exe") == null && !File.Exists(Path.Combine(Toolchain, "python", "python.exe"))) { list.Add("Python (0.01 GB)"); gb += 0.03; }
        if (FindOnPath("git.exe") == null && !File.Exists(Path.Combine(Toolchain, "git", "cmd", "git.exe"))) { list.Add("git (0.05 GB, 0.3 GB on disk)"); gb += 0.3; }
        if (!HasVsCpp()) { list.Add("Visual Studio's C++ build tools (2 GB, 6 GB on disk; Windows asks to allow them)"); gb += 6; }
        return list;
    }

    bool SdkReady() =>
        Directory.Exists(Path.Combine(Sdk, "platforms", "android-34")) &&
        File.Exists(Path.Combine(Sdk, "ndk", kNdk, "toolchains", "llvm", "prebuilt", "windows-x86_64", "bin", "clang-cl.exe")) &&
        File.Exists(Path.Combine(Sdk, "cmake", kCmake, "bin", "cmake.exe")) &&
        Directory.Exists(Path.Combine(Sdk, "build-tools", "34.0.0")) && File.Exists(Adb);

    bool Tools() {
        ServicePointManager.SecurityProtocol = SecurityProtocolType.Tls12;
        Directory.CreateDirectory(Toolchain);
        var missing = Missing(out double gb);
        if (missing.Count > 0) {
            string text = "The Builder will download these into\n" + Toolchain + "\n\n- " + string.Join("\n- ", missing) +
                          "\n\nAbout " + gb.ToString("0.0") + " GB in all, plus about 8 GB for the build itself in the BlackQuest folder." +
                          (CleanupAfter ? "\n\nThey are removed again once the build and install succeed." : "") +
                          "\n\nGo ahead?";
            if (!Ask(text)) { Log("Stopped: the tools weren't downloaded."); return false; }
        }
        // Java 17 (Gradle's)
        if (!File.Exists(Path.Combine(Jdk, "lib", "jvm.cfg"))) {
            Log("Java 17: downloading (Eclipse Temurin)...");
            if (!FetchZip(kJdkUrl, Path.Combine(Toolchain, "jdk17"), true)) return false;
            downloaded.Add(Path.Combine(Toolchain, "jdk17"));
        }
        // the Android SDK's parts
        string sdkmanager = Path.Combine(Sdk, "cmdline-tools", "latest", "bin", "sdkmanager.bat");
        if (!SdkReady()) {
            if (string.IsNullOrEmpty(SdkOverride)) downloaded.Add(Sdk);
            if (!File.Exists(sdkmanager)) {
                Log("Android SDK tools: downloading...");
                string tmp = Path.Combine(Toolchain, "cmdline-tools-tmp");
                if (!FetchZip(kSdkToolsUrl, tmp, false)) return false;
                string dest = Path.Combine(Sdk, "cmdline-tools", "latest");
                Directory.CreateDirectory(Path.GetDirectoryName(dest));
                if (Directory.Exists(dest)) Directory.Delete(dest, true);
                Directory.Move(Path.Combine(tmp, "cmdline-tools"), dest);
                Directory.Delete(tmp, true);
            }
            Log("Android SDK: installing platform 34, build tools, the NDK and CMake (a few GB, once)...");
            var env = JavaEnv();
            // (the licences: Google's Android SDK terms, accepted by building, as Android Studio asks)
            if (Exec(sdkmanager, "--sdk_root=\"" + Sdk + "\" --licenses", Root, env, "y\ny\ny\ny\ny\ny\ny\ny\ny\ny\n") != 0) return false;
            if (Exec(sdkmanager, "--sdk_root=\"" + Sdk + "\" \"platforms;android-34\" \"build-tools;34.0.0\" \"platform-tools\" \"ndk;" + kNdk +
                     "\" \"cmake;" + kCmake + "\"", Root, env, null) != 0) return false;
        }
        // Python (the disc's reader, the stocks' generator)
        if (FindOnPath("python.exe") == null && !File.Exists(Path.Combine(Toolchain, "python", "python.exe"))) {
            Log("Python: downloading (embeddable)...");
            if (!FetchZip(kPythonUrl, Path.Combine(Toolchain, "python"), false)) return false;
            downloaded.Add(Path.Combine(Toolchain, "python"));
        }
        // git (the build fetches the recompiler's runtime and SDL)
        if (FindOnPath("git.exe") == null && !File.Exists(Path.Combine(Toolchain, "git", "cmd", "git.exe"))) {
            Log("git: downloading (MinGit)...");
            if (!FetchZip(kGitUrl, Path.Combine(Toolchain, "git"), false)) return false;
            downloaded.Add(Path.Combine(Toolchain, "git"));
        }
        // Visual Studio's C++ tools (the recompiler is a Windows program); its installer asks for permission itself
        if (!HasVsCpp()) {
            Log("Visual Studio C++ Build Tools: not found. Downloading its installer; Windows will ask to allow it.");
            string vs = Path.Combine(Toolchain, "vs_BuildTools.exe");
            if (!Download(kVsUrl, vs)) return false;
            Log("Installing the C++ build tools (about 20 minutes, once)...");
            // (into the tools folder, so the cleanup can take them out again)
            if (Exec(vs, "--installPath \"" + Path.Combine(Toolchain, "vs") + "\" --passive --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --includeRecommended",
                     Root, null, null, true) is int code && code != 0 && code != 3010) {
                Log("The build tools' installer ended with code " + code + ".");
                return false;
            }
            if (!HasVsCpp()) { Log("The C++ build tools still aren't found."); return false; }
            vsInstalled = true;
        }
        Log("Tools ready.");
        return true;
    }

    // the tools this run downloaded, removed (the APK and the build stay; adb goes with the SDK)
    void Cleanup() {
        Progress(99, "Removing the downloaded tools");
        if (vsInstalled) {
            string vs = Path.Combine(Toolchain, "vs_BuildTools.exe");
            Log("Uninstalling the C++ build tools...");
            Exec(vs, "uninstall --installPath \"" + Path.Combine(Toolchain, "vs") + "\" --passive --wait --norestart", Root, null, null, true);
        }
        foreach (string d in downloaded) {
            try { if (Directory.Exists(d)) Directory.Delete(d, true); Log("Removed " + d); } catch (Exception e) { Log("Couldn't remove " + d + ": " + e.Message); }
        }
        Progress(100, "Done, the downloaded tools removed");
    }

    bool HasVsCpp() {
        string vswhere = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Microsoft Visual Studio", "Installer", "vswhere.exe");
        if (!File.Exists(vswhere)) return false;
        var sb = new StringBuilder();
        Exec(vswhere, "-latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath", Root, null, null, false, sb, true);
        return sb.ToString().Trim().Length > 0;
    }

    // ---- the disc: a new one means its xbe is taken again
    bool PrepareDisc(string iso) {
        if (string.IsNullOrEmpty(iso) || !File.Exists(iso)) { Log("Pick the game's disc image (.iso) first."); return false; }
        string xbe = Path.Combine(Root, "assets", "default.xbe");
        string mark = Path.Combine(Root, "assets", "disc.txt");
        string stamp = iso + "|" + new FileInfo(iso).Length;
        if (!File.Exists(mark) || File.ReadAllText(mark) != stamp) {
            if (File.Exists(xbe)) File.Delete(xbe);
            Directory.CreateDirectory(Path.Combine(Root, "assets"));
            File.WriteAllText(mark, stamp);
        }
        return true;
    }

    // ---- the build: build.bat, its stages as progress
    bool Build(string iso) {
        var env = JavaEnv();
        env["ANDROID_HOME"] = Sdk;
        env["BLACK_BUILDER"] = "1";
        env["BLACK_ISO"] = iso;
        env["CI"] = "1";  // (no pause at the end)
        var extra = new List<string>();
        if (File.Exists(Path.Combine(Toolchain, "python", "python.exe"))) extra.Add(Path.Combine(Toolchain, "python"));
        if (File.Exists(Path.Combine(Toolchain, "git", "cmd", "git.exe"))) extra.Add(Path.Combine(Toolchain, "git", "cmd"));
        if (extra.Count > 0) env["PATH"] = string.Join(";", extra) + ";" + Environment.GetEnvironmentVariable("PATH");
        Progress(18, "Building (the first time: 30 to 90 minutes)");
        var shown = new HashSet<int>();  // (each stage once: Gradle prints many tasks)
        Action<int, string> step = (pc, what) => { if (shown.Add(pc)) Progress(pc, what); };
        int code = Exec(Environment.GetEnvironmentVariable("ComSpec") ?? "cmd.exe", "/c \"\"" + Path.Combine(Root, "build.bat") + "\"\"", Root, env, null, false, null, false, line => {
            if (line.Contains("[1/5]")) step(20, "Checking the build tools");
            else if (line.Contains("[2/5]")) step(25, "Taking the game from the disc");
            else if (line.Contains("[3/5]")) step(30, "Building the recompiler");
            else if (line.Contains("[4/5]")) step(38, "Recompiling the game");
            else if (line.Contains("[5/5]")) step(45, "Building the APK (the first time: 30 to 90 minutes)");
            else if (line.Contains("> Task :app:")) step(80, "Packing the APK");
        });
        if (code != 0 || !File.Exists(Apk)) { Log("The build failed: the lines above say why."); return false; }
        Progress(85, "Built");
        return true;
    }

    // ---- the headset
    // a shell command's output on the headset
    string Shell(string device, string cmd) {
        var sb = new StringBuilder();
        Exec(Adb, "-s " + device + " shell \"" + cmd + "\"", Root, null, null, false, sb, true);
        return sb.ToString().Trim();
    }

    public bool Install(string device, string iso, bool hd = false) {
        if (!File.Exists(Adb) && !EnsureAdb()) return false;
        if (!File.Exists(Apk)) { Log("No APK built yet."); return false; }
        Progress(88, "Installing on the headset");
        var sb = new StringBuilder();
        if (Exec(Adb, "-s " + device + " install -r \"" + Apk + "\"", Root, null, null, false, sb) != 0 || !sb.ToString().Contains("Success")) {
            if (sb.ToString().Contains("offline") || sb.ToString().Contains("not found"))
                Log("The headset isn't answering (asleep, or its Wi-Fi debugging dropped): put it on to wake it, press Refresh, and try again.");
            if (sb.ToString().Contains("INSTALL_FAILED_UPDATE_INCOMPATIBLE"))
                Log("The headset has BlackQuest signed by another PC. Uninstall it there first (your saves go with it).");
            Log("The install failed.");
            return false;
        }
        Log("Installed.");
        // the disc: looked for on the headset first (the app's own folder, then the shared Black folder)
        if (!string.IsNullOrEmpty(iso) && File.Exists(iso)) {
            string files = "/sdcard/Android/data/" + Package + "/files";
            string dir = files + "/assets";
            string remote = dir + "/" + Path.GetFileName(iso).Replace(" ", "_");
            long size = new FileInfo(iso).Length;
            // (folders adb makes are the shell user's, closed to the app: opened to it, or the game finds no disc)
            Shell(device, "mkdir -p " + dir + " && chmod 777 " + files + " " + dir + " 2>/dev/null");
            string own = Shell(device, "for f in " + dir + "/*.iso; do [ -f \\\"$f\\\" ] && stat -c '%s %n' \\\"$f\\\"; done");
            string shared = Shell(device, "for f in /storage/emulated/0/Black/*.iso; do [ -f \\\"$f\\\" ] && stat -c '%s %n' \\\"$f\\\"; done");
            bool ownSame = own.Split('\n').Any(l => l.StartsWith(size + " "));
            bool sharedSame = shared.Split('\n').Any(l => l.StartsWith(size + " "));
            bool copy;
            if (ownSame) {
                Log("The disc is in the app's folder on the headset already.");
                copy = false;
            } else if (own.Trim().Length > 0) {
                copy = Ask("The headset has a different disc image in BlackQuest's folder:\n" + own.Trim() + "\n\nReplace it with " + Path.GetFileName(iso) + "?");
                if (copy) Shell(device, "rm -f " + dir + "/*.iso");
            } else if (sharedSame) {
                copy = Ask("The disc is on the headset already, in its Black folder (BlackQuest reads it there once you allow its storage access when it starts).\n\nCopy it into BlackQuest's own folder as well (1.7 GB more, no permission needed)?");
            } else {
                copy = true;
            }
            if (copy) {
                Progress(92, "Copying the disc to the headset (a few minutes)");
                if (Exec(Adb, "-s " + device + " push \"" + iso + "\" " + remote, Root, null, null) != 0) { Log("Copying the disc failed."); return false; }
            }
            Shell(device, "chmod 666 " + dir + "/*.iso 2>/dev/null");
        }
        // the HD textures: textures_hd beside the settings (the loader lists it at start)
        if (hd && Directory.Exists(HdOut) && Directory.EnumerateFiles(HdOut, "*.ktx2").Any()) {
            string remoteHd = "/sdcard/Android/data/" + Package + "/files/textures_hd";
            string count = Shell(device, "ls " + remoteHd + " 2>/dev/null | wc -l");
            bool push = true;
            if (int.TryParse(count, out int n) && n > 0) {
                push = Ask("The headset has " + n + " HD textures already.\n\nReplace them with this build's?");
                if (push) Shell(device, "rm -rf " + remoteHd);
            }
            if (push) {
                Progress(96, "Copying the HD textures to the headset");
                // adb's push into the app's folder fails its own chown there (the Quest's storage refuses it), so it goes
                // to the shell's /data/local/tmp first and the headset's cp (no chown) puts it in place, opened to the app
                const string stage = "/data/local/tmp/bq_textures_hd";
                Shell(device, "rm -rf " + stage);
                if (Exec(Adb, "-s " + device + " push \"" + HdOut + "\" " + stage, Root, null, null) != 0) { Log("Copying the HD textures failed."); return false; }
                string done = Shell(device, "rm -rf " + remoteHd + "; mkdir -p " + remoteHd + " && cp " + stage + "/* " + remoteHd +
                                            "/ && chmod 777 " + remoteHd + " && chmod 666 " + remoteHd + "/* && rm -rf " + stage + " && echo ok");
                if (!done.EndsWith("ok")) { Log("Putting the HD textures in place failed: " + done); return false; }
                Log("HD textures on the headset: turn them on in VR SETTINGS, GRAPHICS, TEXTURES: HD PACK.");
            }
        }
        Progress(100, "Done: start BlackQuest on the headset (Library, Unknown Sources)");
        return true;
    }

    public bool EnsureAdb() {
        if (File.Exists(Adb)) return true;
        ServicePointManager.SecurityProtocol = SecurityProtocolType.Tls12;
        Log("adb: downloading Android platform tools...");
        string tmp = Path.Combine(Toolchain, "platform-tools-tmp");
        if (!FetchZip(kPlatformToolsUrl, tmp, false)) return false;
        Directory.CreateDirectory(Sdk);
        string dest = Path.Combine(Sdk, "platform-tools");
        if (Directory.Exists(dest)) Directory.Delete(dest, true);
        Directory.Move(Path.Combine(tmp, "platform-tools"), dest);
        Directory.Delete(tmp, true);
        return File.Exists(Adb);
    }

    // the headsets adb sees: serial and a readable name
    public List<KeyValuePair<string, string>> Devices() {
        var list = new List<KeyValuePair<string, string>>();
        if (!File.Exists(Adb)) return list;
        var sb = new StringBuilder();
        Exec(Adb, "devices -l", Root, null, null, false, sb, true);
        foreach (string raw in sb.ToString().Split('\n')) {
            string line = raw.Trim();
            if (line.Length == 0 || line.StartsWith("List of") || line.StartsWith("*")) continue;
            string[] parts = line.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length < 2) continue;
            string serial = parts[0], state = parts[1];
            string model = parts.FirstOrDefault(p => p.StartsWith("model:"))?.Substring(6).Replace('_', ' ') ?? "device";
            string how = serial.Contains(":") || serial.Contains("._adb-tls") ? "Wi-Fi" : "USB";
            string name = state == "device" ? model + " (" + how + ")"
                        : state == "unauthorized" ? model + " (" + how + ": allow USB debugging in the headset)"
                        : model + " (" + state + ")";
            list.Add(new KeyValuePair<string, string>(serial, name));
        }
        return list;
    }

    public string Connect(string address) {
        if (!File.Exists(Adb) && !EnsureAdb()) return "adb isn't available";
        if (!address.Contains(":")) address += ":5555";
        var sb = new StringBuilder();
        Exec(Adb, "connect " + address, Root, null, null, false, sb, true);
        return sb.ToString().Trim();
    }

    // ---- helpers
    Dictionary<string, string> JavaEnv() {
        var env = new Dictionary<string, string>();
        if (File.Exists(Path.Combine(Jdk, "lib", "jvm.cfg"))) env["JAVA_HOME"] = Jdk;
        return env;
    }

    static string FindOnPath(string exe) {
        foreach (string dir in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(';')) {
            try {
                string p = Path.Combine(dir.Trim(), exe);
                if (File.Exists(p) && !p.Contains("WindowsApps")) return p;  // (not the Store's python stub)
            } catch { }
        }
        return null;
    }

    bool Download(string url, string file) {
        try {
            using (var wc = new WebClient()) {
                wc.Headers.Add("User-Agent", "BlackQuestBuilder");
                int last = -1;
                wc.DownloadProgressChanged += (s, e) => {
                    if (e.ProgressPercentage / 10 != last) { last = e.ProgressPercentage / 10; Log("  " + e.ProgressPercentage + "%"); }
                    if (Cancel) wc.CancelAsync();
                };
                var done = new ManualResetEvent(false);
                Exception err = null;
                wc.DownloadFileCompleted += (s, e) => { err = e.Error ?? (e.Cancelled ? new Exception("cancelled") : null); done.Set(); };
                wc.DownloadFileAsync(new Uri(url), file);
                done.WaitOne();
                if (err != null) { Log("Download failed: " + err.Message); return false; }
            }
            return true;
        } catch (Exception e) {
            Log("Download failed: " + e.Message);
            return false;
        }
    }

    // a zip into dest; strip: its single top folder taken off (the JDK's)
    bool FetchZip(string url, string dest, bool strip) {
        string zip = Path.Combine(Toolchain, "download.zip");
        if (!Download(url, zip)) return false;
        string tmp = dest + "-unzip";
        if (Directory.Exists(tmp)) Directory.Delete(tmp, true);
        ZipFile.ExtractToDirectory(zip, tmp);
        File.Delete(zip);
        if (Directory.Exists(dest)) Directory.Delete(dest, true);
        string[] top = Directory.GetDirectories(tmp);
        if (strip && top.Length == 1 && Directory.GetFiles(tmp).Length == 0) {
            Directory.Move(top[0], dest);
            Directory.Delete(tmp, true);
        } else {
            Directory.Move(tmp, dest);
        }
        return true;
    }

    // a program, its output to the log (or into sb); stdin: text fed to it; elevated: its own window, as the VS
    // installer needs; quiet: nothing logged
    int Exec(string exe, string args, string dir, Dictionary<string, string> env, string stdin, bool elevated = false,
             StringBuilder sb = null, bool quiet = false, Action<string> onLine = null) {
        var psi = new ProcessStartInfo(exe, args) { WorkingDirectory = dir };
        if (elevated) {
            psi.UseShellExecute = true;
        } else {
            psi.UseShellExecute = false;
            psi.CreateNoWindow = true;
            psi.RedirectStandardOutput = true;
            psi.RedirectStandardError = true;
            psi.RedirectStandardInput = stdin != null;
            psi.StandardOutputEncoding = Encoding.UTF8;
            if (env != null) foreach (var kv in env) psi.EnvironmentVariables[kv.Key] = kv.Value;
        }
        using (var p = Process.Start(psi)) {
            if (!elevated) {
                DataReceivedEventHandler h = (s, e) => {
                    if (e.Data == null) return;
                    if (sb != null) sb.AppendLine(e.Data);
                    if (!quiet) Log(e.Data);
                    onLine?.Invoke(e.Data);
                };
                p.OutputDataReceived += h;
                p.ErrorDataReceived += h;
                p.BeginOutputReadLine();
                p.BeginErrorReadLine();
                if (stdin != null) { p.StandardInput.Write(stdin); p.StandardInput.Close(); }
            }
            while (!p.WaitForExit(500)) {
                if (Cancel) { try { p.Kill(); } catch { } return -1; }
            }
            p.WaitForExit();
            return p.ExitCode;
        }
    }
}

}  // namespace BlackQuestBuilder
