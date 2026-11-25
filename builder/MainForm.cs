// The Builder's window: the disc, the HD pack (later), the headset (USB or Wi-Fi), one button. Dark, like the game.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Threading;
using System.Windows.Forms;

namespace BlackQuestBuilder {

public sealed class MainForm : Form {
    static readonly Color Bg = Color.Black, Panel = Color.FromArgb(26, 26, 26), Field = Color.FromArgb(36, 36, 36);
    static readonly Color Gold = Color.FromArgb(214, 168, 58), Fg = Color.FromArgb(225, 225, 225), Dim = Color.FromArgb(140, 140, 140);
    // the buttons: the bluish green of the game's main menu (its PRESS A, sampled from a headset shot) (user)
    static readonly Color Teal = Color.FromArgb(90, 140, 120), TealEdge = Color.FromArgb(120, 175, 152);

    readonly Pipeline pipe;
    readonly TextBox iso = new TextBox(), hd = new TextBox(), wifi = new TextBox(), log = new TextBox(), tools = new TextBox();
    readonly CheckBox cleanup = new CheckBox();
    readonly ComboBox device = new ComboBox();
    readonly Button go = new Button(), installOnly = new Button();
    readonly ProgressBar bar = new ProgressBar();
    readonly Label stage = new Label();
    readonly Settings settings;
    List<KeyValuePair<string, string>> devices = new List<KeyValuePair<string, string>>();
    Thread worker;

    public MainForm(string folder) {
        pipe = new Pipeline(folder);
        settings = Settings.Load();
        Text = "BlackQuest Builder";
        BackColor = Bg;
        ForeColor = Fg;
        Font = new Font("Segoe UI", 10f);
        AutoScaleMode = AutoScaleMode.Dpi;
        ClientSize = new Size(760, 760);
        MinimumSize = new Size(640, 560);
        StartPosition = FormStartPosition.CenterScreen;
        try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

        var root = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, Padding = new Padding(22, 16, 22, 16), BackColor = Bg };
        Controls.Add(root);

        var title = new Label { Text = "BLACKQUEST", Font = new Font("Segoe UI Black", 26f, FontStyle.Bold), ForeColor = Gold, AutoSize = true };
        var sub = new Label { Text = "Builds BlackQuest for your Quest from your own copy of Black, and installs it.", ForeColor = Dim, AutoSize = true, Margin = new Padding(3, 0, 3, 14) };
        root.Controls.Add(title);
        root.Controls.Add(sub);

        root.Controls.Add(Section("1   GAME DISC", "Your Black disc image (USA, .iso)."));
        root.Controls.Add(PathRow(iso, settings.Iso, () => {
            using (var d = new OpenFileDialog { Filter = "Disc image (*.iso)|*.iso|All files|*.*", Title = "Your Black disc image" })
                if (d.ShowDialog(this) == DialogResult.OK) iso.Text = d.FileName;
        }));

        root.Controls.Add(Section("2   HD TEXTURES  (optional)", "The HD Reimagined texture pack's folder: its textures converted for the headset (turn them on in VR SETTINGS, TEXTURES)."));
        root.Controls.Add(PathRow(hd, settings.HdPack, () => {
            using (var d = new FolderBrowserDialog { Description = "The HD texture pack's folder" })
                if (d.ShowDialog(this) == DialogResult.OK) hd.Text = d.SelectedPath;
        }));

        root.Controls.Add(Section("3   HEADSET", "Plug it in by USB (allow USB debugging in the headset), or connect over Wi-Fi by its address."));
        var devRow = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0, 0, 0, 4) };
        Style(device);
        device.DropDownStyle = ComboBoxStyle.DropDownList;
        device.Width = 430;
        var refresh = Small("Refresh", (s, e) => RefreshDevices());
        devRow.Controls.Add(device);
        devRow.Controls.Add(refresh);
        root.Controls.Add(devRow);
        var wifiRow = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0, 0, 0, 12) };
        wifiRow.Controls.Add(new Label { Text = "Wi-Fi:", ForeColor = Dim, AutoSize = true, Margin = new Padding(3, 7, 6, 0) });
        Style(wifi);
        wifi.Width = 250;
        wifi.Text = settings.Wifi;
        wifiRow.Controls.Add(wifi);
        wifiRow.Controls.Add(Small("Connect", (s, e) => ConnectWifi()));
        root.Controls.Add(wifiRow);

        root.Controls.Add(Section("4   TOOLS", "Where the Builder downloads what it needs (it says how much first), or use tools you already have."));
        var toolRow = PathRow(tools, string.IsNullOrEmpty(settings.Tools) ? Path.Combine(folder, "toolchain") : settings.Tools, () => {
            using (var d = new FolderBrowserDialog { Description = "Where the Builder puts its tools" })
                if (d.ShowDialog(this) == DialogResult.OK) tools.Text = d.SelectedPath;
        });
        ((FlowLayoutPanel)toolRow).Controls.Add(Small("My own tools...", (s, e) => OwnTools()));
        tools.Width = 440;
        root.Controls.Add(toolRow);
        cleanup.Text = "Remove the downloaded tools once the build and install succeed";
        cleanup.ForeColor = Dim;
        cleanup.AutoSize = true;
        cleanup.Checked = settings.Cleanup;
        cleanup.Margin = new Padding(3, 0, 3, 10);
        root.Controls.Add(cleanup);

        go.Text = "BUILD && INSTALL";
        go.Font = new Font("Segoe UI Black", 15f, FontStyle.Bold);
        go.BackColor = Teal;
        go.ForeColor = Color.Black;
        go.FlatStyle = FlatStyle.Flat;
        go.FlatAppearance.BorderSize = 0;
        go.Height = 58;
        go.Dock = DockStyle.Top;
        go.Margin = new Padding(0, 4, 0, 6);
        go.Cursor = Cursors.Hand;
        go.Click += (s, e) => Start(false);
        root.Controls.Add(go);
        installOnly.Text = "Install the last build only";
        Flat(installOnly);
        installOnly.AutoSize = true;
        installOnly.Click += (s, e) => Start(true);
        root.Controls.Add(installOnly);

        bar.Dock = DockStyle.Top;
        bar.Height = 10;
        bar.Margin = new Padding(0, 10, 0, 4);
        root.Controls.Add(bar);
        stage.AutoSize = true;
        stage.ForeColor = Gold;
        stage.Text = "Ready.";
        root.Controls.Add(stage);

        log.Multiline = true;
        log.ReadOnly = true;
        log.ScrollBars = ScrollBars.Vertical;
        log.BackColor = Panel;
        log.ForeColor = Dim;
        log.BorderStyle = BorderStyle.None;
        log.Font = new Font("Consolas", 9f);
        log.Dock = DockStyle.Fill;
        root.Controls.Add(log);
        root.RowStyles.Clear();
        for (int i = 0; i < root.Controls.Count - 1; i++) root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));

        pipe.Log = line => BeginInvoke((Action)(() => { log.AppendText(line + Environment.NewLine); }));
        pipe.Progress = (p, what) => BeginInvoke((Action)(() => { bar.Value = Math.Max(0, Math.Min(100, p)); stage.Text = what; }));
        Shown += (s, e) => new Thread(() => {
            pipe.EnsureAdb();
            BeginInvoke((Action)RefreshDevices);
        }) { IsBackground = true }.Start();
        FormClosing += (s, e) => {
            Save();
            if (worker != null && worker.IsAlive) {
                if (MessageBox.Show(this, "A build is running. Stop it and close?", "BlackQuest Builder", MessageBoxButtons.YesNo) != DialogResult.Yes) { e.Cancel = true; return; }
                pipe.Cancel = true;
            }
        };
    }

    Control Section(string head, string help) {
        var p = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, AutoSize = true, WrapContents = false, Margin = new Padding(0, 6, 0, 2) };
        p.Controls.Add(new Label { Text = head, Font = new Font("Segoe UI Semibold", 11f, FontStyle.Bold), ForeColor = Gold, AutoSize = true });
        p.Controls.Add(new Label { Text = help, ForeColor = Dim, AutoSize = true, MaximumSize = new Size(700, 0) });
        return p;
    }

    Control PathRow(TextBox box, string value, Action browse) {
        var row = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0, 2, 0, 8) };
        Style(box);
        box.Width = 560;
        box.Text = value ?? "";
        row.Controls.Add(box);
        row.Controls.Add(Small("Browse...", (s, e) => browse()));
        return row;
    }

    static void Style(Control c) {
        c.BackColor = Field;
        c.ForeColor = Fg;
        if (c is TextBox t) t.BorderStyle = BorderStyle.FixedSingle;
        if (c is ComboBox cb) cb.FlatStyle = FlatStyle.Flat;
    }

    static void Flat(Button b) {
        b.FlatStyle = FlatStyle.Flat;
        b.BackColor = Teal;
        b.ForeColor = Color.Black;
        b.FlatAppearance.BorderColor = TealEdge;
        b.Cursor = Cursors.Hand;
    }

    static Button Small(string text, EventHandler click) {
        var b = new Button { Text = text, AutoSize = true, Margin = new Padding(8, 0, 0, 0) };
        Flat(b);
        b.Click += click;
        return b;
    }

    // the player's own Java 17+ and Android SDK (found where Android Studio puts them, or picked)
    void OwnTools() {
        string studioJbr = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "Android", "Android Studio", "jbr");
        string studioSdk = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Android", "Sdk");
        string jdk = settings.Jdk, sdk = settings.Sdk;
        if (string.IsNullOrEmpty(jdk) && File.Exists(Path.Combine(studioJbr, "lib", "jvm.cfg"))) jdk = studioJbr;
        if (string.IsNullOrEmpty(sdk) && Directory.Exists(Path.Combine(studioSdk, "platforms"))) sdk = studioSdk;
        using (var f = new Form { Text = "My own tools", BackColor = Bg, ForeColor = Fg, Font = Font, ClientSize = new Size(640, 230),
                                  FormBorderStyle = FormBorderStyle.FixedDialog, StartPosition = FormStartPosition.CenterParent, MaximizeBox = false, MinimizeBox = false }) {
            var p = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, Padding = new Padding(14), WrapContents = false };
            var j = new TextBox { Text = jdk ?? "" };
            var s = new TextBox { Text = sdk ?? "" };
            p.Controls.Add(new Label { Text = "Java 17 or newer (the folder with bin\\java.exe): empty to download", AutoSize = true, ForeColor = Dim });
            p.Controls.Add(PathRow(j, j.Text, () => { using (var d = new FolderBrowserDialog()) if (d.ShowDialog(f) == DialogResult.OK) j.Text = d.SelectedPath; }));
            p.Controls.Add(new Label { Text = "Android SDK (missing parts are added to it): empty to download", AutoSize = true, ForeColor = Dim });
            p.Controls.Add(PathRow(s, s.Text, () => { using (var d = new FolderBrowserDialog()) if (d.ShowDialog(f) == DialogResult.OK) s.Text = d.SelectedPath; }));
            j.Width = s.Width = 470;
            var ok = Small("OK", (o, e) => f.DialogResult = DialogResult.OK);
            p.Controls.Add(ok);
            f.Controls.Add(p);
            f.AcceptButton = ok;
            if (f.ShowDialog(this) != DialogResult.OK) return;
            settings.Jdk = j.Text.Trim();
            settings.Sdk = s.Text.Trim();
            log.AppendText("Own tools: Java " + (settings.Jdk.Length > 0 ? settings.Jdk : "(download)") + ", Android SDK " +
                           (settings.Sdk.Length > 0 ? settings.Sdk : "(download)") + Environment.NewLine);
        }
    }

    void RefreshDevices() {
        devices = pipe.Devices();
        device.Items.Clear();
        device.Items.Add("No headset: build only");
        foreach (var d in devices) device.Items.Add(d.Value);
        int pick = devices.FindIndex(d => d.Key == settings.Device);
        device.SelectedIndex = pick >= 0 ? pick + 1 : devices.Count > 0 ? 1 : 0;
        go.Text = device.SelectedIndex > 0 ? "BUILD && INSTALL" : "BUILD";
        device.SelectedIndexChanged -= DeviceChanged;
        device.SelectedIndexChanged += DeviceChanged;
    }

    void DeviceChanged(object s, EventArgs e) => go.Text = device.SelectedIndex > 0 ? "BUILD && INSTALL" : "BUILD";

    void ConnectWifi() {
        string address = wifi.Text.Trim();
        if (address.Length == 0) { MessageBox.Show(this, "Type the headset's address (Settings, Wi-Fi, its network's details), like 192.168.1.20.", "Wi-Fi"); return; }
        Cursor = Cursors.WaitCursor;
        string answer = pipe.Connect(address);
        Cursor = Cursors.Default;
        log.AppendText(answer + Environment.NewLine);
        RefreshDevices();
    }

    void Start(bool installOnlyRun) {
        if (worker != null && worker.IsAlive) return;
        string serial = device.SelectedIndex > 0 ? devices[device.SelectedIndex - 1].Key : null;
        if (installOnlyRun && serial == null) { MessageBox.Show(this, "Pick a headset first.", "Install"); return; }
        if (!installOnlyRun && !File.Exists(iso.Text)) { MessageBox.Show(this, "Pick your Black disc image (.iso) first.", "Game disc"); return; }
        Save();
        go.Enabled = installOnly.Enabled = false;
        bar.Value = 0;
        log.Clear();
        string isoPath = iso.Text, hdPath = hd.Text.Trim();
        pipe.Cancel = false;
        pipe.ToolsDir = tools.Text.Trim().Length > 0 ? tools.Text.Trim() : Path.Combine(pipe.Root, "toolchain");
        pipe.JdkOverride = settings.Jdk;
        pipe.SdkOverride = settings.Sdk;
        pipe.CleanupAfter = cleanup.Checked;
        pipe.Ask = question => (bool)Invoke((Func<bool>)(() =>
            MessageBox.Show(this, question, "BlackQuest Builder", MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes));
        worker = new Thread(() => {
            bool ok = installOnlyRun ? pipe.Install(serial, isoPath, hdPath.Length > 0) : pipe.Run(isoPath, hdPath, serial);
            BeginInvoke((Action)(() => {
                go.Enabled = installOnly.Enabled = true;
                if (!ok) stage.Text = "Stopped: see the log.";
                else if (serial != null) MessageBox.Show(this, "BlackQuest is on your headset.\nFind it in the Library under Unknown Sources.", "Done");
            }));
        }) { IsBackground = true };
        worker.Start();
    }

    void Save() {
        settings.Iso = iso.Text;
        settings.HdPack = hd.Text;
        settings.Wifi = wifi.Text;
        settings.Tools = tools.Text;
        settings.Cleanup = cleanup.Checked;
        if (device.SelectedIndex > 0 && device.SelectedIndex - 1 < devices.Count) settings.Device = devices[device.SelectedIndex - 1].Key;
        settings.Save();
    }
}

}  // namespace BlackQuestBuilder
