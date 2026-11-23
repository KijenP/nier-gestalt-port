// NieR Gestalt translation tool: a small local server + window for the editor page.
// Build: tools\translator\build.ps1  ->  NierTradutor.exe (next to nier.exe)
// It only listens on 127.0.0.1, needs a random token on every request, and
// only touches files under the TEXT folder next to the exe.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Reflection;
using System.Text;
using System.Threading;

static class Translator
{
    static string textRoot;
    static string token;
    static long lastPing = 0;
    static bool pinged = false;

    [STAThread]
    static int Main(string[] args)
    {
        string exeDir = AppDomain.CurrentDomain.BaseDirectory;
        textRoot = Path.Combine(exeDir, "TEXT");
        if (args.Length > 0 && Directory.Exists(args[0])) textRoot = Path.GetFullPath(args[0]);
        if (!Directory.Exists(textRoot))
        {
            try { Directory.CreateDirectory(textRoot); } catch { }
        }
        token = Guid.NewGuid().ToString("N");

        var rnd = new Random();
        HttpListener http = null;
        int port = 0;
        for (int i = 0; i < 40 && http == null; i++)
        {
            port = rnd.Next(20000, 60000);
            var h = new HttpListener();
            h.Prefixes.Add("http://127.0.0.1:" + port + "/");
            try { h.Start(); http = h; } catch { }
        }
        if (http == null) return 1;

        string url = "http://127.0.0.1:" + port + "/?t=" + token;
        OpenWindow(url);
        lastPing = Environment.TickCount;

        var watchdog = new Thread(() =>
        {
            while (true)
            {
                Thread.Sleep(2000);
                if (pinged && Environment.TickCount - lastPing > 25000) Environment.Exit(0);
                if (!pinged && Environment.TickCount - lastPing > 120000) Environment.Exit(0);
            }
        });
        watchdog.IsBackground = true;
        watchdog.Start();

        while (true)
        {
            HttpListenerContext ctx;
            try { ctx = http.GetContext(); } catch { break; }
            try { Handle(ctx); }
            catch (Exception e) { Reply(ctx, 500, "text/plain", Encoding.UTF8.GetBytes(e.Message)); }
        }
        return 0;
    }

    static void OpenWindow(string url)
    {
        string[] edge = {
            Environment.ExpandEnvironmentVariables(@"%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe"),
            Environment.ExpandEnvironmentVariables(@"%ProgramFiles%\Microsoft\Edge\Application\msedge.exe"),
            Environment.ExpandEnvironmentVariables(@"%ProgramFiles%\Google\Chrome\Application\chrome.exe"),
            Environment.ExpandEnvironmentVariables(@"%ProgramFiles(x86)%\Google\Chrome\Application\chrome.exe"),
        };
        foreach (var p in edge)
        {
            if (!File.Exists(p)) continue;
            try
            {
                string profile = Path.Combine(Path.GetTempPath(), "nier-tradutor-perfil");
                Process.Start(p, "--app=" + url + " --user-data-dir=\"" + profile + "\" --window-size=1200,850");
                return;
            }
            catch { }
        }
        Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
    }

    static void Reply(HttpListenerContext ctx, int code, string type, byte[] body)
    {
        ctx.Response.StatusCode = code;
        ctx.Response.ContentType = type;
        ctx.Response.ContentLength64 = body.Length;
        ctx.Response.OutputStream.Write(body, 0, body.Length);
        ctx.Response.OutputStream.Close();
    }

    static string Query(HttpListenerContext ctx, string key)
    {
        return ctx.Request.QueryString[key];
    }

    static bool ValidName(string s)
    {
        if (string.IsNullOrEmpty(s)) return false;
        if (s.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0) return false;
        return s != "." && s != ".." && !s.StartsWith(".");
    }

    // A path under TEXT\<lang>\ ; null when it escapes the folder.
    static string SafePath(string lang, string rel)
    {
        if (!ValidName(lang)) return null;
        string root = Path.GetFullPath(Path.Combine(textRoot, lang));
        string full = Path.GetFullPath(Path.Combine(root, rel.Replace('/', '\\')));
        if (!full.StartsWith(root + "\\", StringComparison.OrdinalIgnoreCase)) return null;
        return full;
    }

    static void Handle(HttpListenerContext ctx)
    {
        string path = ctx.Request.Url.AbsolutePath;
        if (path == "/")
        {
            if (Query(ctx, "t") != token) { Reply(ctx, 403, "text/plain", Encoding.UTF8.GetBytes("negado")); return; }
            var asm = Assembly.GetExecutingAssembly();
            using (var s = asm.GetManifestResourceStream("translator.html"))
            using (var ms = new MemoryStream())
            {
                s.CopyTo(ms);
                string html = Encoding.UTF8.GetString(ms.ToArray()).Replace("__TOKEN__", token);
                Reply(ctx, 200, "text/html; charset=utf-8", Encoding.UTF8.GetBytes(html));
            }
            return;
        }
        if (!path.StartsWith("/api/") || ctx.Request.Headers["X-Token"] != token)
        {
            Reply(ctx, 403, "text/plain", Encoding.UTF8.GetBytes("negado"));
            return;
        }

        switch (path)
        {
            case "/api/ping":
                pinged = true; lastPing = Environment.TickCount;
                Reply(ctx, 200, "text/plain", Encoding.UTF8.GetBytes("ok"));
                return;

            case "/api/langs":
            {
                var sb = new StringBuilder();
                if (Directory.Exists(textRoot))
                    foreach (var d in Directory.GetDirectories(textRoot))
                        sb.Append(Path.GetFileName(d)).Append('\n');
                Reply(ctx, 200, "text/plain; charset=utf-8", Encoding.UTF8.GetBytes(sb.ToString()));
                return;
            }

            case "/api/files":
            {
                string root = SafePath(Query(ctx, "lang"), "x");
                if (root == null) { Reply(ctx, 400, "text/plain", Encoding.UTF8.GetBytes("idioma invalido")); return; }
                root = Path.GetDirectoryName(root);
                var sb = new StringBuilder();
                if (Directory.Exists(root))
                    foreach (var f in Directory.GetFiles(root, "*", SearchOption.AllDirectories))
                    {
                        string ext = Path.GetExtension(f).ToLowerInvariant();
                        if (ext == ".html" || ext == ".txt" || ext == ".bak" || ext == ".png") continue;
                        sb.Append(f.Substring(root.Length + 1).Replace('\\', '/')).Append('\n');
                    }
                Reply(ctx, 200, "text/plain; charset=utf-8", Encoding.UTF8.GetBytes(sb.ToString()));
                return;
            }

            case "/api/file":
            {
                string full = SafePath(Query(ctx, "lang"), Query(ctx, "path") ?? "");
                if (full == null || !File.Exists(full)) { Reply(ctx, 404, "text/plain", Encoding.UTF8.GetBytes("nao achei")); return; }
                Reply(ctx, 200, "application/octet-stream", File.ReadAllBytes(full));
                return;
            }

            case "/api/save":
            {
                string full = SafePath(Query(ctx, "lang"), Query(ctx, "path") ?? "");
                if (full == null || !File.Exists(full)) { Reply(ctx, 404, "text/plain", Encoding.UTF8.GetBytes("nao achei")); return; }
                byte[] data;
                using (var ms = new MemoryStream()) { ctx.Request.InputStream.CopyTo(ms); data = ms.ToArray(); }
                string bak = full + ".bak";
                if (!File.Exists(bak)) File.Copy(full, bak);
                File.WriteAllBytes(full, data);
                Reply(ctx, 200, "text/plain", Encoding.UTF8.GetBytes("salvo"));
                return;
            }

            case "/api/newlang":
            {
                string name = Query(ctx, "name");
                string from = Query(ctx, "from") ?? "EXAMPLE";
                if (!ValidName(name) || !ValidName(from)) { Reply(ctx, 400, "text/plain", Encoding.UTF8.GetBytes("nome invalido")); return; }
                string src = Path.Combine(textRoot, from), dst = Path.Combine(textRoot, name);
                if (!Directory.Exists(src)) { Reply(ctx, 404, "text/plain", Encoding.UTF8.GetBytes("pasta modelo nao existe")); return; }
                if (Directory.Exists(dst)) { Reply(ctx, 409, "text/plain", Encoding.UTF8.GetBytes("ja existe")); return; }
                CopyDir(src, dst);
                Reply(ctx, 200, "text/plain", Encoding.UTF8.GetBytes("criado"));
                return;
            }

            case "/api/openfolder":
                Process.Start("explorer.exe", "\"" + textRoot + "\"");
                Reply(ctx, 200, "text/plain", Encoding.UTF8.GetBytes("ok"));
                return;
        }
        Reply(ctx, 404, "text/plain", Encoding.UTF8.GetBytes("?"));
    }

    static void CopyDir(string src, string dst)
    {
        Directory.CreateDirectory(dst);
        foreach (var f in Directory.GetFiles(src))
        {
            if (Path.GetExtension(f).ToLowerInvariant() == ".html") continue;
            File.Copy(f, Path.Combine(dst, Path.GetFileName(f)));
        }
        foreach (var d in Directory.GetDirectories(src))
            CopyDir(d, Path.Combine(dst, Path.GetFileName(d)));
    }
}
