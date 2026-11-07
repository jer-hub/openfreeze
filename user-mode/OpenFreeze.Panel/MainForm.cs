using System;
using System.Drawing;
using System.Windows.Forms;
using OpenFreeze.Core;

namespace OpenFreeze.Panel
{
    public sealed class MainForm : Form
    {
        readonly Label _state;
        readonly Label _detail;
        readonly ProgressBar _overlay;
        readonly RadioButton _rbFrozen;
        readonly RadioButton _rbThawed;
        readonly RadioButton _rbThawN;
        readonly NumericUpDown _nReboots;
        readonly TextBox _password;
        readonly Button _btnApply;
        readonly Button _btnRefresh;
        readonly Button _btnSetPassword;
        readonly NotifyIcon _tray;
        readonly Timer _timer;

        public MainForm()
        {
            Text = "OpenFreeze";
            Width = 420;
            Height = 360;
            FormBorderStyle = FormBorderStyle.FixedSingle;
            MaximizeBox = false;
            StartPosition = FormStartPosition.CenterScreen;

            _state = new Label { Left = 16, Top = 16, Width = 370, Height = 28, Font = new Font(Font.FontFamily, 14, FontStyle.Bold), Text = "…" };
            _detail = new Label { Left = 16, Top = 48, Width = 370, Height = 40, Text = "" };
            _overlay = new ProgressBar { Left = 16, Top = 96, Width = 370, Height = 18, Minimum = 0, Maximum = 100 };

            var modeBox = new GroupBox { Left = 16, Top = 126, Width = 370, Height = 100, Text = "Next boot mode" };
            _rbFrozen = new RadioButton { Left = 12, Top = 24, Width = 100, Text = "Frozen", Checked = true };
            _rbThawed = new RadioButton { Left = 120, Top = 24, Width = 100, Text = "Thawed" };
            _rbThawN = new RadioButton { Left = 230, Top = 24, Width = 120, Text = "Thawed for N" };
            _nReboots = new NumericUpDown { Left = 230, Top = 54, Width = 60, Minimum = 1, Maximum = 99, Value = 2, Enabled = false };
            _rbThawN.CheckedChanged += (s, e) => { _nReboots.Enabled = _rbThawN.Checked; };
            modeBox.Controls.AddRange(new Control[] { _rbFrozen, _rbThawed, _rbThawN, _nReboots });

            var lblPw = new Label { Left = 16, Top = 236, Width = 70, Text = "Password" };
            _password = new TextBox { Left = 90, Top = 232, Width = 180, UseSystemPasswordChar = true };
            _btnApply = new Button { Left = 280, Top = 230, Width = 100, Text = "Apply" };
            _btnRefresh = new Button { Left = 16, Top = 270, Width = 100, Text = "Refresh" };
            _btnSetPassword = new Button { Left = 130, Top = 270, Width = 140, Text = "Set / Change PW" };

            _btnApply.Click += OnApply;
            _btnRefresh.Click += (s, e) => RefreshStatus();
            _btnSetPassword.Click += OnSetPassword;

            Controls.AddRange(new Control[] { _state, _detail, _overlay, modeBox, lblPw, _password, _btnApply, _btnRefresh, _btnSetPassword });

            _tray = new NotifyIcon
            {
                Text = "OpenFreeze",
                Visible = true,
                Icon = SystemIcons.Application
            };
            _tray.DoubleClick += (s, e) => { Show(); WindowState = FormWindowState.Normal; Activate(); };
            var menu = new ContextMenuStrip();
            menu.Items.Add("Open", null, (s, e) => { Show(); WindowState = FormWindowState.Normal; });
            menu.Items.Add("Refresh", null, (s, e) => RefreshStatus());
            menu.Items.Add("Exit", null, (s, e) => { _tray.Visible = false; Application.Exit(); });
            _tray.ContextMenuStrip = menu;

            Resize += (s, e) =>
            {
                if (WindowState == FormWindowState.Minimized)
                {
                    Hide();
                }
            };

            _timer = new Timer { Interval = 5000 };
            _timer.Tick += (s, e) => RefreshStatus();
            _timer.Start();

            Load += (s, e) => RefreshStatus();
            FormClosed += (s, e) => { _timer.Stop(); _tray.Visible = false; _tray.Dispose(); };
        }

        void RefreshStatus()
        {
            try
            {
                var resp = PipeProtocol.Transact(new PipeRequest { Command = PipeCommand.GetStatus }, 3000);
                if (resp.Code != PipeStatusCode.Ok || resp.Status == null)
                {
                    _state.Text = "Service error";
                    _detail.Text = resp.Message ?? resp.Code.ToString();
                    UpdateTray(false);
                    return;
                }
                var st = resp.Status;
                st.RefreshStateName();
                _state.Text = st.StateName;
                _state.ForeColor = st.CowEnabled ? Color.SteelBlue : Color.DarkGreen;
                _detail.Text = string.Format("slots {0}/{1}  backend={2}  thawNext={3}  reason={4}",
                    st.SlotsUsed, st.SlotCount, st.OverlayBackend, st.ThawNext ? 1 : 0, st.RefuseReason);
                if (st.SlotCount > 0)
                    _overlay.Value = (int)Math.Min(100, (st.SlotsUsed * 100.0 / st.SlotCount));
                else
                    _overlay.Value = 0;
                UpdateTray(st.CowEnabled);
            }
            catch (Exception ex)
            {
                _state.Text = "No service";
                _detail.Text = ex.Message;
                UpdateTray(false);
            }
        }

        void UpdateTray(bool frozen)
        {
            _tray.Text = frozen ? "OpenFreeze: Frozen" : "OpenFreeze: Thawed";
            // Snowflake vs open: use system icons as placeholders until custom icons ship.
            _tray.Icon = frozen ? SystemIcons.Shield : SystemIcons.Application;
        }

        void OnApply(object sender, EventArgs e)
        {
            var mode = _rbFrozen.Checked ? FreezeMode.Frozen
                : _rbThawed.Checked ? FreezeMode.Thawed
                : FreezeMode.ThawedForN;
            try
            {
                var resp = PipeProtocol.Transact(new PipeRequest
                {
                    Command = PipeCommand.SetNextBootMode,
                    Password = _password.Text,
                    Mode = mode,
                    ThawedForN = (uint)_nReboots.Value
                });
                MessageBox.Show(this, resp.Message ?? resp.Code.ToString(), "OpenFreeze",
                    MessageBoxButtons.OK,
                    resp.Code == PipeStatusCode.Ok ? MessageBoxIcon.Information : MessageBoxIcon.Warning);
                RefreshStatus();
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, ex.Message, "OpenFreeze", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
        }

        void OnSetPassword(object sender, EventArgs e)
        {
            using (var dlg = new PasswordDialog())
            {
                if (dlg.ShowDialog(this) != DialogResult.OK) return;
                try
                {
                    // Auth key is SYSTEM-scoped; ask the service whether a password exists.
                    var st = PipeProtocol.Transact(new PipeRequest { Command = PipeCommand.GetStatus }, 3000);
                    PipeRequest req;
                    if (!st.HasPassword)
                    {
                        req = new PipeRequest
                        {
                            Command = PipeCommand.SetPasswordBootstrap,
                            NewPassword = dlg.NewPassword
                        };
                    }
                    else
                    {
                        req = new PipeRequest
                        {
                            Command = PipeCommand.ChangePassword,
                            Password = dlg.CurrentPassword,
                            NewPassword = dlg.NewPassword
                        };
                    }
                    var resp = PipeProtocol.Transact(req);
                    MessageBox.Show(this, resp.Message ?? resp.Code.ToString(), "OpenFreeze",
                        MessageBoxButtons.OK,
                        resp.Code == PipeStatusCode.Ok ? MessageBoxIcon.Information : MessageBoxIcon.Warning);
                }
                catch (Exception ex)
                {
                    MessageBox.Show(this, ex.Message, "OpenFreeze", MessageBoxButtons.OK, MessageBoxIcon.Error);
                }
            }
        }
    }
}
