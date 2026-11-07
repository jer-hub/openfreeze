using System;
using System.ServiceProcess;

namespace OpenFreeze.Service
{
    static class Program
    {
        static void Main(string[] args)
        {
            if (args != null && args.Length > 0 &&
                string.Equals(args[0], "--console", StringComparison.OrdinalIgnoreCase))
            {
                var svc = new OpenFreezeSvc();
                svc.StartConsole();
                Console.WriteLine("OpenFreeze.Service running (console). Ctrl+C to stop.");
                var exit = new System.Threading.ManualResetEvent(false);
                Console.CancelKeyPress += (s, e) => { e.Cancel = true; exit.Set(); };
                exit.WaitOne();
                svc.StopConsole();
                return;
            }

            ServiceBase.Run(new ServiceBase[] { new OpenFreezeSvc() });
        }
    }
}
