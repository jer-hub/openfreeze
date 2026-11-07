using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace OpenFreeze.Setup
{
    static class SilentInstaller
    {
        [DllImport("kernel32.dll")]
        static extern bool AllocConsole();

        public static int Run(SilentOptions opts)
        {
            AllocConsole();
            var logPath = Path.Combine(Path.GetTempPath(), "OpenFreeze-Setup-silent.log");
            Action<string> log = m =>
            {
                var line = DateTime.Now.ToString("HH:mm:ss") + " " + m;
                try { File.AppendAllText(logPath, line + Environment.NewLine); } catch { }
                Console.WriteLine(line);
            };
            log("[OpenFreeze.Setup] silent install starting log=" + logPath);
            var extractDir = Path.Combine(Path.GetTempPath(), "OpenFreeze-lab-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(extractDir);
            PayloadExtract.ExtractTo(extractDir);
            var ps1 = Path.Combine(extractDir, "Install-OpenFreeze.ps1");
            if (!File.Exists(ps1))
                throw new FileNotFoundException("Install-OpenFreeze.ps1 missing from payload", ps1);

            var args = new StringBuilder();
            args.Append("-NoProfile -ExecutionPolicy Bypass -File \"").Append(ps1).Append("\"");
            args.Append(" -OverlaySizeGB ").Append(opts.OverlayGb);
            args.Append(" -Silent -SkipRebootPrompt");
            if (!string.IsNullOrEmpty(opts.Password))
            {
                var pw = opts.Password.Replace("'", "''");
                args.Append(" -BootstrapPassword '").Append(pw).Append("'");
            }
            if (opts.AllowNonVm) args.Append(" -AllowNonVm");
            if (opts.FreezeAfterInstall) args.Append(" -FreezeAfterInstall");

            var psi = new ProcessStartInfo
            {
                FileName = Path.Combine(Environment.SystemDirectory, "WindowsPowerShell\\v1.0\\powershell.exe"),
                Arguments = args.ToString(),
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
                WorkingDirectory = extractDir
            };
            using (var proc = Process.Start(psi))
            {
                if (proc == null) throw new InvalidOperationException("failed to start powershell");
                var stdout = proc.StandardOutput.ReadToEnd();
                var stderr = proc.StandardError.ReadToEnd();
                if (!string.IsNullOrEmpty(stdout)) log(stdout.TrimEnd());
                if (!string.IsNullOrEmpty(stderr)) log("ERR " + stderr.TrimEnd());
                proc.WaitForExit();
                log("[OpenFreeze.Setup] exit=" + proc.ExitCode);
                return proc.ExitCode;
            }
        }
    }
}
