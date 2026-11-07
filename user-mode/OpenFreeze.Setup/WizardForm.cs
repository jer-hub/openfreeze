using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace OpenFreeze.Setup
{
    public sealed class WizardForm : Form
    {
        enum Page { Welcome, Options, Progress, Done }

        readonly Panel _host;
        readonly Button _btnBack;
        readonly Button _btnNext;
        readonly Button _btnCancel;

        // Welcome
        readonly Panel _pageWelcome;
        // Options
        readonly Panel _pageOptions;
        readonly TextBox _tbPassword;
        readonly TextBox _tbConfirm;
        readonly NumericUpDown _nudOverlayGb;
        readonly CheckBox _chkAllowNonVm;
        readonly CheckBox _chkFreezeAfter;
        // Progress
        readonly Panel _pageProgress;
        readonly TextBox _tbLog;
        readonly ProgressBar _bar;
        // Done
        readonly Panel _pageDone;
        readonly Label _lblDone;
        readonly TextBox _tbDoneLog;
        readonly Button _btnReboot;
        readonly Button _btnOpenLog;

        Page _page = Page.Welcome;
        bool _installOk;
        string _extractDir;
        string _logPath;
        readonly StringBuilder _logBuf = new StringBuilder();

        public WizardForm()
        {
            Text = "OpenFreeze Lab Setup";
            Width = 640;
            Height = 520;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;

            _host = new Panel { Left = 12, Top = 12, Width = 600, Height = 400 };

            _pageWelcome = BuildWelcome();
            _pageOptions = BuildOptions(out _tbPassword, out _tbConfirm, out _nudOverlayGb, out _chkAllowNonVm, out _chkFreezeAfter);
            _pageProgress = BuildProgress(out _tbLog, out _bar);
            _pageDone = BuildDone(out _lblDone, out _tbDoneLog, out _btnReboot, out _btnOpenLog);

            foreach (var p in new[] { _pageWelcome, _pageOptions, _pageProgress, _pageDone })
            {
                p.Dock = DockStyle.Fill;
                p.Visible = false;
                _host.Controls.Add(p);
            }

            _btnBack = new Button { Text = "Back", Left = 280, Top = 430, Width = 90 };
            _btnNext = new Button { Text = "Next", Left = 380, Top = 430, Width = 90 };
            _btnCancel = new Button { Text = "Cancel", Left = 480, Top = 430, Width = 90 };
            _btnBack.Click += (_, __) => GoBack();
            _btnNext.Click += async (_, __) => await GoNextAsync();
            _btnCancel.Click += (_, __) => Close();
            _btnReboot.Click += (_, __) =>
            {
                try { Process.Start(new ProcessStartInfo("shutdown.exe", "/r /t 0") { UseShellExecute = true }); }
                catch (Exception ex) { MessageBox.Show(this, ex.Message, Text); }
            };
            _btnOpenLog.Click += (_, __) =>
            {
                try
                {
                    if (!string.IsNullOrEmpty(_logPath) && File.Exists(_logPath))
                        Process.Start(new ProcessStartInfo(_logPath) { UseShellExecute = true });
                }
                catch (Exception ex) { MessageBox.Show(this, ex.Message, Text); }
            };

            Controls.AddRange(new Control[] { _host, _btnBack, _btnNext, _btnCancel });
            ShowPage(Page.Welcome);
        }

        static Panel BuildWelcome()
        {
            var p = new Panel();
            var t = new TextBox
            {
                Multiline = true,
                ReadOnly = true,
                BorderStyle = BorderStyle.None,
                Dock = DockStyle.Fill,
                Font = new Font(FontFamily.GenericSansSerif, 10f),
                Text =
@"OpenFreeze Lab Installer (managed lab images)

This installs a TEST-SIGNED kernel volume filter (OfFilter).

Requirements:
• Lab VM or dedicated lab PC image (not your daily work PC / Hyper-V host)
• bcdedit /set testsigning on  (then reboot) — Secure Boot usually off
• Administrator rights (this EXE elevates)
• ~4+ GB free on C: (default 2 GB overlay)

After install + reboot, run:  C:\OpenFreeze\Verify-OpenFreeze.ps1

Hyper-V *hosts* are blocked unless you check Allow non-VM (bare-metal lab PCs).
Not for production Secure Boot / WHQL — see docs/production-signing.md.

Silent: OpenFreeze-Setup.exe /S -Password '...' -AllowNonVm

Click Next to continue."
            };
            p.Controls.Add(t);
            return p;
        }

        static Panel BuildOptions(out TextBox tbPw, out TextBox tbConfirm, out NumericUpDown nudGb,
            out CheckBox chkAllow, out CheckBox chkFreeze)
        {
            var p = new Panel();
            int y = 16;
            p.Controls.Add(new Label { Text = "Bootstrap password (or existing OpenFreeze password):", Left = 8, Top = y, Width = 560 });
            y += 24;
            tbPw = new TextBox { Left = 8, Top = y, Width = 280, UseSystemPasswordChar = true };
            p.Controls.Add(tbPw);
            y += 36;
            p.Controls.Add(new Label { Text = "Confirm password:", Left = 8, Top = y, Width = 200 });
            y += 24;
            tbConfirm = new TextBox { Left = 8, Top = y, Width = 280, UseSystemPasswordChar = true };
            p.Controls.Add(tbConfirm);
            y += 40;
            p.Controls.Add(new Label { Text = "Overlay size (GB):", Left = 8, Top = y, Width = 140 });
            nudGb = new NumericUpDown { Left = 150, Top = y - 2, Width = 60, Minimum = 1, Maximum = 64, Value = 2 };
            p.Controls.Add(nudGb);
            y += 36;
            chkAllow = new CheckBox
            {
                Left = 8,
                Top = y,
                Width = 560,
                Text = "Allow install on non-VM hardware (-AllowNonVm). Required for physical lab PCs."
            };
            p.Controls.Add(chkAllow);
            y += 28;
            chkFreeze = new CheckBox
            {
                Left = 8,
                Top = y,
                Width = 560,
                Text = "Arm Frozen for next boot after install (-FreezeAfterInstall). Needs password above."
            };
            p.Controls.Add(chkFreeze);
            return p;
        }

        static Panel BuildProgress(out TextBox log, out ProgressBar bar)
        {
            var p = new Panel();
            bar = new ProgressBar { Left = 8, Top = 8, Width = 580, Height = 18, Style = ProgressBarStyle.Marquee };
            log = new TextBox
            {
                Left = 8,
                Top = 36,
                Width = 580,
                Height = 350,
                Multiline = true,
                ScrollBars = ScrollBars.Vertical,
                ReadOnly = true,
                Font = new Font(FontFamily.GenericMonospace, 8.5f)
            };
            p.Controls.Add(bar);
            p.Controls.Add(log);
            return p;
        }

        static Panel BuildDone(out Label lbl, out TextBox doneLog, out Button reboot, out Button openLog)
        {
            var p = new Panel();
            lbl = new Label { Left = 8, Top = 4, Width = 580, Height = 56 };
            doneLog = new TextBox
            {
                Left = 8,
                Top = 64,
                Width = 580,
                Height = 280,
                Multiline = true,
                ScrollBars = ScrollBars.Vertical,
                ReadOnly = true,
                Font = new Font(FontFamily.GenericMonospace, 8.5f)
            };
            reboot = new Button { Text = "Reboot now", Left = 8, Top = 354, Width = 140, Enabled = false };
            openLog = new Button { Text = "Open log file", Left = 160, Top = 354, Width = 140 };
            p.Controls.Add(lbl);
            p.Controls.Add(doneLog);
            p.Controls.Add(reboot);
            p.Controls.Add(openLog);
            return p;
        }

        void ShowPage(Page page)
        {
            _page = page;
            _pageWelcome.Visible = page == Page.Welcome;
            _pageOptions.Visible = page == Page.Options;
            _pageProgress.Visible = page == Page.Progress;
            _pageDone.Visible = page == Page.Done;

            _btnBack.Enabled = page == Page.Options;
            _btnCancel.Enabled = page != Page.Progress;
            if (page == Page.Welcome) _btnNext.Text = "Next";
            else if (page == Page.Options) _btnNext.Text = "Install";
            else if (page == Page.Progress) { _btnNext.Enabled = false; _btnNext.Text = "Install"; }
            else
            {
                _btnNext.Enabled = true;
                _btnNext.Text = "Close";
                _btnBack.Enabled = false;
            }
        }

        void GoBack()
        {
            if (_page == Page.Options) ShowPage(Page.Welcome);
        }

        async Task GoNextAsync()
        {
            if (_page == Page.Welcome)
            {
                ShowPage(Page.Options);
                return;
            }
            if (_page == Page.Done)
            {
                Close();
                return;
            }
            if (_page != Page.Options) return;

            if (_tbPassword.Text != _tbConfirm.Text)
            {
                MessageBox.Show(this, "Passwords do not match.", Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            if (_chkFreezeAfter.Checked && string.IsNullOrEmpty(_tbPassword.Text))
            {
                MessageBox.Show(this, "Freeze after install requires a password.", Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
            if (!_chkAllowNonVm.Checked && LooksLikeHyperVHost())
            {
                var r = MessageBox.Show(this,
                    "This PC looks like a Hyper-V host (not a guest VM).\n\n" +
                    "OfFilter must not be installed on the build/dev host.\n" +
                    "Run Setup inside DriverTest (or another lab VM), or check Allow non-VM only for a dedicated lab PC image.\n\n" +
                    "Continue anyway and let the installer refuse/confirm?",
                    Text, MessageBoxButtons.OKCancel, MessageBoxIcon.Warning);
                if (r != DialogResult.OK) return;
            }

            ShowPage(Page.Progress);
            _btnNext.Enabled = false;
            _btnBack.Enabled = false;
            _installOk = false;
            _tbLog.Clear();
            _logBuf.Clear();
            _logPath = Path.Combine(Path.GetTempPath(), "OpenFreeze-Setup.log");
            try { File.WriteAllText(_logPath, ""); } catch { /* ignore */ }
            try
            {
                await Task.Run(() => RunInstall());
            }
            catch (Exception ex)
            {
                AppendLog("FATAL: " + ex);
                _installOk = false;
            }

            _bar.Style = ProgressBarStyle.Continuous;
            _bar.Value = 100;
            _btnReboot.Enabled = _installOk;
            var summary = _installOk
                ? "Install finished. Reboot is required for the volume filter to attach cleanly.\nAfter reboot, freeze via Panel or ofpipe if you did not arm freeze."
                : "Install failed. Scroll the log below (also saved to temp). Typical causes: Hyper-V host / non-VM without Allow non-VM, testsigning off, free space, thaw if frozen.";
            if (!string.IsNullOrEmpty(_logPath))
                summary += "\nLog: " + _logPath;
            _lblDone.Text = summary;
            _tbDoneLog.Text = _logBuf.ToString();
            if (_tbDoneLog.TextLength > 0)
            {
                _tbDoneLog.SelectionStart = _tbDoneLog.TextLength;
                _tbDoneLog.ScrollToCaret();
            }
            ShowPage(Page.Done);
            _btnNext.Enabled = true;
        }

        static bool LooksLikeHyperVHost()
        {
            // HypervisorPresent is true on Hyper-V hosts; guests usually report a "Virtual Machine" model.
            try
            {
                var psi = new ProcessStartInfo
                {
                    FileName = Path.Combine(Environment.SystemDirectory, "WindowsPowerShell\\v1.0\\powershell.exe"),
                    Arguments = "-NoProfile -Command \"$c=Get-CimInstance Win32_ComputerSystem; if($c.HypervisorPresent -and $c.Model -notmatch 'Virtual|VMware|VirtualBox|HVM|KVM|QEMU|Xen'){'HOST'}else{'OK'}\"",
                    UseShellExecute = false,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    CreateNoWindow = true
                };
                using (var p = Process.Start(psi))
                {
                    if (p == null) return false;
                    var o = p.StandardOutput.ReadToEnd();
                    p.WaitForExit(8000);
                    return o != null && o.IndexOf("HOST", StringComparison.OrdinalIgnoreCase) >= 0;
                }
            }
            catch { return false; }
        }

        void AppendLog(string line)
        {
            if (InvokeRequired)
            {
                BeginInvoke(new Action<string>(AppendLog), line);
                return;
            }
            _tbLog.AppendText(line + Environment.NewLine);
            _logBuf.AppendLine(line);
            if (!string.IsNullOrEmpty(_logPath))
            {
                try { File.AppendAllText(_logPath, line + Environment.NewLine); } catch { /* ignore */ }
            }
        }

        void RunInstall()
        {
            AppendLog("Extracting payload...");
            _extractDir = Path.Combine(Path.GetTempPath(), "OpenFreeze-lab-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(_extractDir);
            PayloadExtract.ExtractTo(_extractDir);
            AppendLog("Extracted to " + _extractDir);

            var ps1 = Path.Combine(_extractDir, "Install-OpenFreeze.ps1");
            if (!File.Exists(ps1))
                throw new FileNotFoundException("Install-OpenFreeze.ps1 missing from payload", ps1);

            var args = new StringBuilder();
            args.Append("-NoProfile -ExecutionPolicy Bypass -File \"").Append(ps1).Append("\"");
            args.Append(" -OverlaySizeGB ").Append((int)_nudOverlayGb.Value);
            args.Append(" -SkipRebootPrompt");
            if (!string.IsNullOrEmpty(_tbPassword.Text))
            {
                // PowerShell single-quoted literal; double embedded single quotes.
                var pw = _tbPassword.Text.Replace("'", "''");
                args.Append(" -BootstrapPassword '").Append(pw).Append("'");
            }
            if (_chkAllowNonVm.Checked) args.Append(" -AllowNonVm");
            if (_chkFreezeAfter.Checked) args.Append(" -FreezeAfterInstall");

            AppendLog("Running Install-OpenFreeze.ps1 ...");
            var psi = new ProcessStartInfo
            {
                FileName = Path.Combine(Environment.SystemDirectory, "WindowsPowerShell\\v1.0\\powershell.exe"),
                Arguments = args.ToString(),
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                CreateNoWindow = true,
                WorkingDirectory = _extractDir
            };
            using (var proc = new Process { StartInfo = psi, EnableRaisingEvents = true })
            {
                proc.OutputDataReceived += (_, e) => { if (e.Data != null) AppendLog(e.Data); };
                proc.ErrorDataReceived += (_, e) => { if (e.Data != null) AppendLog(e.Data); };
                proc.Start();
                proc.BeginOutputReadLine();
                proc.BeginErrorReadLine();
                proc.WaitForExit();
                AppendLog("Exit code: " + proc.ExitCode);
                _installOk = proc.ExitCode == 0;
            }
        }

    }
}
