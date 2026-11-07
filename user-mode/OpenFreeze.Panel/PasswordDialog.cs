using System;
using System.Windows.Forms;

namespace OpenFreeze.Panel
{
    sealed class PasswordDialog : Form
    {
        readonly TextBox _current;
        readonly TextBox _nw;
        readonly TextBox _confirm;

        public string CurrentPassword { get { return _current.Text; } }
        public string NewPassword { get { return _nw.Text; } }

        public PasswordDialog()
        {
            Text = "Set password";
            Width = 360;
            Height = 200;
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = false;
            MinimizeBox = false;
            StartPosition = FormStartPosition.CenterParent;

            var l1 = new Label { Left = 12, Top = 16, Width = 100, Text = "Current" };
            _current = new TextBox { Left = 120, Top = 12, Width = 200, UseSystemPasswordChar = true };
            var l2 = new Label { Left = 12, Top = 48, Width = 100, Text = "New" };
            _nw = new TextBox { Left = 120, Top = 44, Width = 200, UseSystemPasswordChar = true };
            var l3 = new Label { Left = 12, Top = 80, Width = 100, Text = "Confirm" };
            _confirm = new TextBox { Left = 120, Top = 76, Width = 200, UseSystemPasswordChar = true };
            var ok = new Button { Left = 160, Top = 120, Width = 75, Text = "OK", DialogResult = DialogResult.OK };
            var cancel = new Button { Left = 245, Top = 120, Width = 75, Text = "Cancel", DialogResult = DialogResult.Cancel };
            ok.Click += (s, e) =>
            {
                if (_nw.Text != _confirm.Text)
                {
                    MessageBox.Show(this, "New passwords do not match.", Text);
                    DialogResult = DialogResult.None;
                }
            };
            AcceptButton = ok;
            CancelButton = cancel;
            Controls.AddRange(new Control[] { l1, _current, l2, _nw, l3, _confirm, ok, cancel });
        }
    }
}
