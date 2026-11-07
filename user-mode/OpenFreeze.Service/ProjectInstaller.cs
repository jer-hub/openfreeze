using System.ComponentModel;
using System.Configuration.Install;
using System.ServiceProcess;

namespace OpenFreeze.Service
{
    [RunInstaller(true)]
    public sealed class ProjectInstaller : Installer
    {
        public ProjectInstaller()
        {
            var process = new ServiceProcessInstaller
            {
                Account = ServiceAccount.LocalSystem
            };
            var service = new ServiceInstaller
            {
                ServiceName = OpenFreezeSvc.ServiceNameConst,
                DisplayName = "OpenFreeze Control Service",
                Description = "Password-gated OpenFreeze freeze/thaw control plane.",
                StartType = ServiceStartMode.Automatic
            };
            Installers.Add(process);
            Installers.Add(service);
        }
    }
}
