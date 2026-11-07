// ofoverlay: create C:\OpenFreeze\overlay.bin while thawed, protect clusters,
// capture retrieval pointers, and write Services\OfFilter\Parameters for the
// file overlay backend. Modes: create [sizeGB] [path] | show | clear
// Run elevated (SeManageVolumePrivilege + SeBackupPrivilege). Never prints secrets.
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;

class OfOverlay {
  [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern SafeFileHandle CreateFile(string n, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);
  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] i, uint il, byte[] o, uint ol, out uint r, IntPtr ov);
  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool SetFileValidData(SafeFileHandle h, long len);
  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool SetFilePointerEx(SafeFileHandle h, long d, out long np, uint m);
  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool SetEndOfFile(SafeFileHandle h);
  [DllImport("advapi32.dll", SetLastError = true)]
  static extern bool OpenProcessToken(IntPtr p, uint a, out IntPtr t);
  [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern bool LookupPrivilegeValue(string s, string n, out long luid);
  [DllImport("advapi32.dll", SetLastError = true)]
  static extern bool AdjustTokenPrivileges(IntPtr t, bool d, byte[] n, int l, IntPtr p, IntPtr r);
  [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();

  const uint FSCTL_GET_RETRIEVAL_POINTERS = 0x00090073;
  const uint FSCTL_GET_NTFS_VOLUME_DATA = 0x00090064;
  const uint FSCTL_MARK_HANDLE = 0x000900FC;
  const uint MARK_HANDLE_PROTECT_CLUSTERS = 0x00000001;
  const string Key = @"SYSTEM\CurrentControlSet\Services\OfFilter\Parameters";
  const int ConfigSector = 4096;
  const int Block = 4096;

  static void EnablePriv(string name) {
    IntPtr t; long luid;
    if (!OpenProcessToken(GetCurrentProcess(), 0x28, out t) || !LookupPrivilegeValue(null, name, out luid)) return;
    var b = new byte[16];
    BitConverter.GetBytes(1).CopyTo(b, 0);
    BitConverter.GetBytes(luid).CopyTo(b, 4);
    BitConverter.GetBytes(2).CopyTo(b, 12);
    AdjustTokenPrivileges(t, false, b, 16, IntPtr.Zero, IntPtr.Zero);
  }

  static List<ulong[]> Retrieval(SafeFileHandle h, ulong cluster, out string err) {
    err = null; var list = new List<ulong[]>();
    long vcn = 0; var outb = new byte[64 * 1024];
    for (;;) {
      uint r; bool ok = DeviceIoControl(h, FSCTL_GET_RETRIEVAL_POINTERS, BitConverter.GetBytes(vcn), 8, outb, (uint)outb.Length, out r, IntPtr.Zero);
      int e = ok ? 0 : Marshal.GetLastWin32Error();
      if (!ok && e != 234) { if (e == 38) return list; err = "FSCTL_GET_RETRIEVAL_POINTERS err " + e; return null; }
      uint n = BitConverter.ToUInt32(outb, 0); long prev = BitConverter.ToInt64(outb, 8);
      for (int i = 0; i < n; i++) {
        long next = BitConverter.ToInt64(outb, 16 + i * 16), lcn = BitConverter.ToInt64(outb, 24 + i * 16);
        if (lcn >= 0) list.Add(new[] { (ulong)lcn * cluster, (ulong)(next - prev) * cluster });
        prev = next;
      }
      if (ok) return list; vcn = prev;
    }
  }

  static byte[] VolumeUniqueId() {
    using (var k = Registry.LocalMachine.OpenSubKey(@"SYSTEM\MountedDevices")) {
      if (k == null) throw new Exception("MountedDevices missing");
      var v = k.GetValue(@"\DosDevices\C:") as byte[];
      if (v == null || v.Length == 0) throw new Exception("No MountedDevices entry for C:");
      return v;
    }
  }

  static int Create(long sizeBytes, string path) {
    if (sizeBytes < ConfigSector + Block || (sizeBytes % Block) != 0) {
      Console.WriteLine("RESULT FAIL size must be 4KB-aligned and > 4KB"); return 1;
    }
    Directory.CreateDirectory(Path.GetDirectoryName(path));
    if (File.Exists(path)) File.Delete(path);

    var h = CreateFile(path, 0xC0000000 /*GENERIC_READ|WRITE*/, 0, IntPtr.Zero, 2 /*CREATE_ALWAYS*/, 0x80, IntPtr.Zero);
    if (h.IsInvalid) { Console.WriteLine("RESULT FAIL create err " + Marshal.GetLastWin32Error()); return 1; }
    using (h) {
      long np;
      if (!SetFilePointerEx(h, sizeBytes, out np, 0) || !SetEndOfFile(h)) {
        Console.WriteLine("RESULT FAIL SetEndOfFile err " + Marshal.GetLastWin32Error()); return 1;
      }
      if (!SetFileValidData(h, sizeBytes)) {
        Console.WriteLine("RESULT FAIL SetFileValidData err " + Marshal.GetLastWin32Error() + " (need SeManageVolumePrivilege)"); return 1;
      }
      // Zero the reserved Phase 3 config sector via normal write.
      var zero = new byte[ConfigSector];
      
    }
    using (var fs = new FileStream(path, FileMode.Open, FileAccess.ReadWrite, FileShare.ReadWrite)) {
      fs.Write(new byte[ConfigSector], 0, ConfigSector);
      fs.Flush(true);
    }

    // Re-open for mark-handle + retrieval.
    var hf = CreateFile(path, 0x80000000 /*GENERIC_READ*/, 7, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero);
    if (hf.IsInvalid) { Console.WriteLine("RESULT FAIL reopen err " + Marshal.GetLastWin32Error()); return 1; }

    var vol = CreateFile(@"\\.\C:", 0x80000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
    if (vol.IsInvalid) { Console.WriteLine("RESULT FAIL open volume err " + Marshal.GetLastWin32Error()); return 1; }

    using (hf)
    using (vol) {
      // MARK_HANDLE_PROTECT_CLUSTERS
      // MARK_HANDLE_INFO (x64): ULONG UsnSourceInfo; pad4; HANDLE VolumeHandle; ULONG HandleInfo = 20 bytes
      var mhi2 = new byte[20];
      BitConverter.GetBytes(0).CopyTo(mhi2, 0); // UsnSourceInfo
      BitConverter.GetBytes(vol.DangerousGetHandle().ToInt64()).CopyTo(mhi2, 8);
      BitConverter.GetBytes(MARK_HANDLE_PROTECT_CLUSTERS).CopyTo(mhi2, 16);
      uint rr;
      if (!DeviceIoControl(hf, FSCTL_MARK_HANDLE, mhi2, (uint)mhi2.Length, null, 0, out rr, IntPtr.Zero)) {
        Console.WriteLine("WARN FSCTL_MARK_HANDLE err " + Marshal.GetLastWin32Error() + " (continuing)");
      } else {
        Console.WriteLine("MARK_HANDLE_PROTECT_CLUSTERS ok");
      }

      var vd = new byte[128];
      if (!DeviceIoControl(vol, FSCTL_GET_NTFS_VOLUME_DATA, null, 0, vd, (uint)vd.Length, out rr, IntPtr.Zero)) {
        Console.WriteLine("RESULT FAIL FSCTL_GET_NTFS_VOLUME_DATA err " + Marshal.GetLastWin32Error()); return 1;
      }
      ulong cluster = BitConverter.ToUInt64(vd, 36); // BytesPerCluster at offset 36 in NTFS_VOLUME_DATA_BUFFER
      // Actually: LONGLONG VolumeSerialNumber(8), LONGLONG NumberSectors(8), LONGLONG TotalClusters(8),
      // LONGLONG FreeClusters(8), LONGLONG TotalReserved(8), ULONG BytesPerSector(4), ULONG BytesPerCluster(4)...
      // offsets: 0,8,16,24,32,40,44 - BytesPerCluster at 44
      cluster = BitConverter.ToUInt32(vd, 44);
      if (cluster == 0) cluster = 4096;

      string err;
      var runs = Retrieval(hf, cluster, out err);
      if (runs == null) { Console.WriteLine("RESULT FAIL " + err); return 1; }
      if (runs.Count == 0) { Console.WriteLine("RESULT FAIL no extents"); return 1; }

      // Build OverlayExtents: FileOffset walks VCN order
      var extents = new List<byte>();
      ulong fileOff = 0;
      foreach (var run in runs) {
        // run[0]=volume offset, run[1]=length
        extents.AddRange(BitConverter.GetBytes(fileOff));
        extents.AddRange(BitConverter.GetBytes(run[0]));
        extents.AddRange(BitConverter.GetBytes(run[1]));
        Console.WriteLine(string.Format("extent file=0x{0:X} vol=0x{1:X} len=0x{2:X}", fileOff, run[0], run[1]));
        fileOff += run[1];
      }
      if (fileOff < (ulong)sizeBytes) {
        Console.WriteLine("RESULT FAIL extents cover 0x" + fileOff.ToString("X") + " < file size 0x" + sizeBytes.ToString("X")); return 1;
      }

      ulong slotCount = ((ulong)sizeBytes - (ulong)ConfigSector) / (ulong)Block;
      if (slotCount > 262144) slotCount = 262144;
      var uid = VolumeUniqueId();
      uint gen = (uint)(DateTime.UtcNow.Ticks & 0xFFFFFFFF);

      using (var rk = Registry.LocalMachine.CreateSubKey(Key)) {
        rk.SetValue("OverlayBackend", 1, RegistryValueKind.DWord);
        rk.SetValue("SlotCount", (int)slotCount, RegistryValueKind.DWord);
        rk.SetValue("OverlayGeneration", unchecked((int)gen), RegistryValueKind.DWord);
        rk.SetValue("OverlayFileSize", BitConverter.GetBytes((ulong)sizeBytes), RegistryValueKind.Binary);
        rk.SetValue("OverlayExtents", extents.ToArray(), RegistryValueKind.Binary);
        rk.SetValue("VolumeUniqueId", uid, RegistryValueKind.Binary);
        rk.SetValue("OverlayPath", path, RegistryValueKind.String);
        // Leave EnableCow alone (creator does not freeze).
      }
      Console.WriteLine("path=" + path);
      Console.WriteLine("size=" + sizeBytes + " slots=" + slotCount + " extents=" + runs.Count + " gen=" + gen);
      Console.WriteLine("VolumeUniqueId bytes=" + uid.Length);
      Console.WriteLine("RESULT OK");
      return 0;
    }
  }

  static int Show() {
    using (var rk = Registry.LocalMachine.OpenSubKey(Key)) {
      if (rk == null) { Console.WriteLine("RESULT FAIL no Parameters key"); return 1; }
      Console.WriteLine("OverlayBackend=" + rk.GetValue("OverlayBackend"));
      Console.WriteLine("SlotCount=" + rk.GetValue("SlotCount"));
      Console.WriteLine("OverlayGeneration=" + rk.GetValue("OverlayGeneration"));
      Console.WriteLine("OverlayPath=" + rk.GetValue("OverlayPath"));
      var fs = rk.GetValue("OverlayFileSize") as byte[];
      if (fs != null && fs.Length >= 8) Console.WriteLine("OverlayFileSize=" + BitConverter.ToUInt64(fs, 0));
      var ex = rk.GetValue("OverlayExtents") as byte[];
      Console.WriteLine("OverlayExtents bytes=" + (ex == null ? 0 : ex.Length));
      if (ex != null) {
        for (int o = 0; o + 24 <= ex.Length; o += 24) {
          Console.WriteLine(string.Format("  file=0x{0:X} vol=0x{1:X} len=0x{2:X}",
            BitConverter.ToUInt64(ex, o), BitConverter.ToUInt64(ex, o + 8), BitConverter.ToUInt64(ex, o + 16)));
        }
      }
      var uid = rk.GetValue("VolumeUniqueId") as byte[];
      Console.WriteLine("VolumeUniqueId bytes=" + (uid == null ? 0 : uid.Length));
    }
    Console.WriteLine("RESULT OK");
    return 0;
  }

  static int Clear() {
    using (var rk = Registry.LocalMachine.CreateSubKey(Key)) {
      foreach (var n in new[] { "OverlayExtents", "OverlayFileSize", "OverlayGeneration", "OverlayPath" }) {
        try { rk.DeleteValue(n, false); } catch { }
      }
      rk.SetValue("OverlayBackend", 0, RegistryValueKind.DWord);
    }
    Console.WriteLine("RESULT OK cleared file-backend parameters (OverlayBackend=0)");
    return 0;
  }

  static int Main(string[] a) {
    EnablePriv("SeManageVolumePrivilege");
    EnablePriv("SeBackupPrivilege");
    string mode = a.Length > 0 ? a[0].ToLowerInvariant() : "create";
    if (mode == "show") return Show();
    if (mode == "clear") return Clear();
    if (mode != "create") { Console.WriteLine("usage: ofoverlay create [sizeGB=8] [path] | show | clear"); return 1; }
    double gb = 8;
    string path = @"C:\OpenFreeze\overlay.bin";
    if (a.Length > 1) double.TryParse(a[1], out gb);
    if (a.Length > 2) path = a[2];
    long size = (long)(gb * 1024L * 1024L * 1024L);
    size = (size / Block) * Block;
    return Create(size, path);
  }
}

