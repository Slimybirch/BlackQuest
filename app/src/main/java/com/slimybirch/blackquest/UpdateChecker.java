package com.slimybirch.blackquest;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.net.ConnectivityManager;
import android.net.NetworkCapabilities;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

import org.json.JSONArray;
import org.json.JSONObject;

// Asks GitHub for the newest BlackQuest release, off the main thread: at each launch, handing it over when it is
// newer than the one installed and the player has not skipped it, and from the options menu, where the answer is
// given whatever it is.
final class UpdateChecker {
    static final String REPO = "Slimybirch/BlackQuest";
    private static final String LATEST = "https://api.github.com/repos/" + REPO + "/releases/latest";
    private static final String TAG = "BlackUpdate";
    private static final int TIMEOUT_MS = 8000;

    private static final String PREFS = "updates";
    private static final String SKIPPED = "skipped_tag";
    private static final String STARTUP = "check_at_startup";

    static final class Release {
        String tag;      // v1.2.3
        String name;     // BlackQuest v1.2.3
        String page;     // the release on GitHub
        String apk;      // the APK's download, or null
        String changes;  // the "What's Changed" list as plain text, or ""
        boolean full;    // "Rebuild: full" in its notes: the PC recompiles the game (30-90 minutes)
        boolean hdRedo;  // "HD textures: redo": the Builder converts and copies them again

        // how the Builder's update goes, for the notice
        String howLong() {
            String s = full ? "a full rebuild on your PC, 30 to 90 minutes" : "a quick rebuild on your PC, a few minutes";
            return hdRedo ? s + ", HD textures redone" : s;
        }
    }

    interface Listener {
        // the newer release or null; failed when GitHub could not be asked
        void onResult(Release newer, boolean failed);
    }

    private UpdateChecker() {}

    // manual: asked from the menu, a skipped release counts too; the listener runs on the main thread
    static void check(Context context, boolean manual, Listener listener) {
        Context app = context.getApplicationContext();
        SharedPreferences prefs = app.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        String installed = installedVersion(app);
        Handler main = new Handler(Looper.getMainLooper());
        new Thread(() -> {
            Release newer = null;
            boolean failed = false;
            // at launch only over wi-fi (user): no mobile data spent, no wait on a slow link
            if (!manual && !onWifi(app)) {
                Log.i(TAG, "no update check: not on wi-fi");
                return;
            }
            try {
                Release r = fetch();
                boolean skipped = r != null && !manual && r.tag.equals(prefs.getString(SKIPPED, null));
                if (r != null && !skipped && compare(r.tag, installed) > 0) {
                    Log.i(TAG, "update " + r.tag + " over " + installed);
                    newer = r;
                }
            } catch (Exception e) {
                Log.w(TAG, "no update check: " + e);  // offline, rate limited: try again next launch
                failed = true;
            }
            Release found = newer;
            boolean f = failed;
            main.post(() -> listener.onResult(found, f));
        }, "UpdateCheck").start();
    }

    static void skip(Context context, String tag) {
        context.getApplicationContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .edit().putString(SKIPPED, tag).apply();
    }

    // STARTUP UPDATE CHECK in the options menu: whether each launch asks GitHub, on unless turned off
    static boolean startupCheck(Context context) {
        return context.getApplicationContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .getBoolean(STARTUP, true);
    }

    static void setStartupCheck(Context context, boolean on) {
        context.getApplicationContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .edit().putBoolean(STARTUP, on).apply();
    }

    // a line of the release's notes starting so (case aside)
    private static boolean marker(String body, String regex) {
        return java.util.regex.Pattern.compile("^" + regex, java.util.regex.Pattern.MULTILINE
                | java.util.regex.Pattern.CASE_INSENSITIVE).matcher(body).find();
    }

    static boolean onWifi(Context app) {
        try {
            ConnectivityManager cm = (ConnectivityManager) app.getSystemService(Context.CONNECTIVITY_SERVICE);
            NetworkCapabilities nc = cm == null ? null : cm.getNetworkCapabilities(cm.getActiveNetwork());
            return nc != null && (nc.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)
                    || nc.hasTransport(NetworkCapabilities.TRANSPORT_ETHERNET));
        } catch (Exception e) {
            return false;
        }
    }

    static String installedVersion(Context app) {
        try {
            return app.getPackageManager().getPackageInfo(app.getPackageName(), 0).versionName;
        } catch (PackageManager.NameNotFoundException e) {
            return "0";
        }
    }

    private static Release fetch() throws Exception {
        HttpURLConnection c = (HttpURLConnection) new URL(LATEST).openConnection();
        c.setConnectTimeout(TIMEOUT_MS);
        c.setReadTimeout(TIMEOUT_MS);
        c.setRequestProperty("Accept", "application/vnd.github+json");
        c.setRequestProperty("User-Agent", "BlackQuest");
        try {
            if (c.getResponseCode() != 200) return null;  // 404 when there is no release yet
            JSONObject j = new JSONObject(read(c.getInputStream()));
            Release r = new Release();
            r.tag = j.getString("tag_name");
            r.name = j.optString("name", r.tag);
            if (r.name.isEmpty()) r.name = r.tag;
            r.page = j.optString("html_url", "https://github.com/" + REPO + "/releases/latest");
            JSONArray assets = j.optJSONArray("assets");
            for (int i = 0; assets != null && i < assets.length(); i++) {
                JSONObject a = assets.getJSONObject(i);
                if (a.optString("name").toLowerCase().endsWith(".apk")) {
                    r.apk = a.optString("browser_download_url", null);
                    break;
                }
            }
            String body = j.optString("body", "");
            r.changes = changes(body);
            r.full = marker(body, "Rebuild:\\s*full");
            r.hdRedo = marker(body, "HD textures:\\s*redo");
            return r;
        } finally {
            c.disconnect();
        }
    }

    private static String read(InputStream in) throws Exception {
        try (in) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[8192];
            for (int n; (n = in.read(buf)) > 0;) out.write(buf, 0, n);
            return out.toString(StandardCharsets.UTF_8.name());
        }
    }

    // GitHub's generated "What's Changed" list, without the authors' links: "* fix: x by @a in https://…" → "• fix: x"
    private static String changes(String body) {
        int at = body.indexOf("## What's Changed");
        if (at < 0) return "";
        StringBuilder sb = new StringBuilder();
        int lines = 0;
        for (String line : body.substring(at).split("\r?\n")) {
            line = line.trim();
            if (line.startsWith("**Full Changelog**") || (line.startsWith("## ") && sb.length() > 0)) break;
            if (!line.startsWith("* ") && !line.startsWith("- ")) continue;
            line = line.substring(2).replaceAll(" by @\\S+ in https://\\S+$", "").replaceAll(" in https://\\S+$", "");
            if (++lines > 10) {
                sb.append("• …\n");
                break;
            }
            sb.append("• ").append(line).append('\n');
        }
        return sb.toString().trim();
    }

    // dotted numbers, "v" and anything after "-" left out: v1.2.10 > 1.2.9, 1.0 == 1.0.0
    static int compare(String a, String b) {
        int[] x = parts(a), y = parts(b);
        for (int i = 0; i < Math.max(x.length, y.length); i++) {
            int p = i < x.length ? x[i] : 0, q = i < y.length ? y[i] : 0;
            if (p != q) return Integer.compare(p, q);
        }
        return 0;
    }

    private static int[] parts(String v) {
        if (v == null) return new int[0];
        v = v.trim();
        if (v.startsWith("v") || v.startsWith("V")) v = v.substring(1);
        int dash = v.indexOf('-');
        if (dash >= 0) v = v.substring(0, dash);
        String[] s = v.split("\\.");
        int[] n = new int[s.length];
        for (int i = 0; i < s.length; i++) {
            try {
                n[i] = Integer.parseInt(s[i].replaceAll("\\D.*", ""));
            } catch (NumberFormatException e) {
                n[i] = 0;
            }
        }
        return n;
    }
}
