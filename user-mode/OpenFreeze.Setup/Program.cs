using System;
using System.Windows.Forms;

namespace OpenFreeze.Setup
{
    static class Program
    {
        [STAThread]
        static int Main(string[] args)
        {
            var opts = SilentOptions.Parse(args);
            if (opts.Silent)
            {
                try
                {
                    return SilentInstaller.Run(opts);
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine("FATAL: " + ex.Message);
                    return 2;
                }
            }

            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new WizardForm());
            return 0;
        }
    }

    sealed class SilentOptions
    {
        public bool Silent;
        public string Password = "";
        public int OverlayGb = 2;
        public bool AllowNonVm;
        public bool FreezeAfterInstall;

        public static SilentOptions Parse(string[] args)
        {
            var o = new SilentOptions();
            if (args == null) return o;
            for (int i = 0; i < args.Length; i++)
            {
                var a = args[i];
                if (string.Equals(a, "/S", StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(a, "-Silent", StringComparison.OrdinalIgnoreCase) ||
                    string.Equals(a, "--silent", StringComparison.OrdinalIgnoreCase))
                    o.Silent = true;
                else if (string.Equals(a, "-Password", StringComparison.OrdinalIgnoreCase) ||
                         string.Equals(a, "/Password", StringComparison.OrdinalIgnoreCase))
                {
                    if (i + 1 < args.Length) o.Password = args[++i];
                }
                else if (string.Equals(a, "-OverlaySizeGB", StringComparison.OrdinalIgnoreCase) ||
                         string.Equals(a, "/OverlaySizeGB", StringComparison.OrdinalIgnoreCase))
                {
                    int g;
                    if (i + 1 < args.Length && int.TryParse(args[++i], out g)) o.OverlayGb = g;
                }
                else if (string.Equals(a, "-AllowNonVm", StringComparison.OrdinalIgnoreCase) ||
                         string.Equals(a, "/AllowNonVm", StringComparison.OrdinalIgnoreCase))
                    o.AllowNonVm = true;
                else if (string.Equals(a, "-FreezeAfterInstall", StringComparison.OrdinalIgnoreCase) ||
                         string.Equals(a, "/FreezeAfterInstall", StringComparison.OrdinalIgnoreCase))
                    o.FreezeAfterInstall = true;
            }
            return o;
        }
    }
}
