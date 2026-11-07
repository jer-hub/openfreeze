using System;
using System.IO;
using System.IO.Compression;
using System.Reflection;

namespace OpenFreeze.Setup
{
    /// <summary>
    /// Extracts the embedded lab payload.zip into a destination directory.
    /// </summary>
    static class PayloadExtract
    {
        public static void ExtractTo(string destDir)
        {
            var asm = Assembly.GetExecutingAssembly();
            string resourceName = null;
            foreach (var n in asm.GetManifestResourceNames())
            {
                if (n.EndsWith("payload.zip", StringComparison.OrdinalIgnoreCase))
                {
                    resourceName = n;
                    break;
                }
            }
            if (resourceName == null)
                throw new InvalidOperationException("Embedded payload.zip not found. Rebuild with New-LabPackage.ps1.");

            var zipPath = Path.Combine(destDir, "_payload.zip");
            using (var src = asm.GetManifestResourceStream(resourceName))
            {
                if (src == null) throw new InvalidOperationException("Cannot open payload.zip stream.");
                using (var fs = File.Create(zipPath))
                    src.CopyTo(fs);
            }
            ZipFile.ExtractToDirectory(zipPath, destDir);
            try { File.Delete(zipPath); } catch { /* ignore */ }
        }
    }
}
