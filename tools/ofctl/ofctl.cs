using System;
using System.Runtime.InteropServices;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;

// Phase 4 step 1: ofctl over existing \\.\C: IOCTLs (control device comes next).
class OfCtl {
  [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern SafeFileHandle CreateFile(string n, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);
  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool DeviceIoControl(SafeFileHandle h, uint code, IntPtr i, uint il, byte[] o, uint ol, out uint r, IntPtr ov);
  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] i, uint il, byte[] o, uint ol, out uint r, IntPtr ov);

  const uint IOCTL_OF_DEBUG_QUERY_COW = 2404328456u;       // 0x8F4F2408
  const uint IOCTL_OF_SET_THAW_NEXT_BOOT = 0x8F4FA410u;

  static string ReasonName(uint code) {
    switch (code) {
      case 1: return "NotTarget";
      case 2: return "ConfigRefuse";
      case 3: return "ThawRequested";
      case 4: return "ConfigCorrupt";
      default: return "None";
    }
  }

  // CTL_CODE(0x8F4F, 0xA01, METHOD_BUFFERED, FILE_ANY_ACCESS)
  const uint IOCTL_OF_GET_STATUS = 0x8F4F2804u;

  static SafeFileHandle OpenVol() {
    var h = CreateFile(@"\\.\C:", 0xC0000000u, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
    if (h.IsInvalid) throw new InvalidOperationException("open \\\\.\\C: failed err=" + Marshal.GetLastWin32Error());
    return h;
  }

  static SafeFileHandle OpenControl() {
    // SYSTEM SDDL: may fail for normal admin; fall back to volume path.
    return CreateFile(@"\\.\OfFilter", 0xC0000000u, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
  }

  static int Status() {
    using (var ctrl = OpenControl()) {
      if (!ctrl.IsInvalid) {
        var st = new byte[48]; // OF_STATUS_V1
        uint r;
        if (DeviceIoControl(ctrl, IOCTL_OF_GET_STATUS, IntPtr.Zero, 0, st, (uint)st.Length, out r, IntPtr.Zero) && r >= 48) {
          uint cow = BitConverter.ToUInt32(st, 0);
          uint torn = BitConverter.ToUInt32(st, 4);
          uint slots = BitConverter.ToUInt32(st, 8);
          uint slotCount = BitConverter.ToUInt32(st, 12);
          uint backend = BitConverter.ToUInt32(st, 16);
          uint reason = BitConverter.ToUInt32(st, 20);
          uint cfgValid = BitConverter.ToUInt32(st, 24);
          uint thawNext = BitConverter.ToUInt32(st, 28);
          uint cfgSeq = BitConverter.ToUInt32(st, 32);
          uint initStep = BitConverter.ToUInt32(st, 40);
          uint initStatus = BitConverter.ToUInt32(st, 44);
          string state = (torn != 0) ? "TORN" : (cow != 0) ? "FROZEN" : "PASSTHROUGH";
          Console.WriteLine("via=OfFilter state={0} cow={1} slotsUsed={2}/{3} backend={4} reason={5} cfgValid={6} thawNext={7} cfgSeq={8} initStep={9} initStatus=0x{10:X8}",
            state, cow, slots, slotCount, backend, ReasonName(reason), cfgValid, thawNext, cfgSeq, initStep, initStatus);
          return 0;
        }
      }
    }
    using (var h = OpenVol()) {
      var b = new byte[104];
      uint r;
      if (!DeviceIoControl(h, IOCTL_OF_DEBUG_QUERY_COW, IntPtr.Zero, 0, b, (uint)b.Length, out r, IntPtr.Zero)) {
        Console.WriteLine("status failed err=" + Marshal.GetLastWin32Error());
        return 2;
      }
      uint cow = BitConverter.ToUInt32(b, 0);
      uint torn = BitConverter.ToUInt32(b, 4);
      uint slots = BitConverter.ToUInt32(b, 8);
      string state = (torn != 0) ? "TORN" : (cow != 0) ? "FROZEN" : "PASSTHROUGH";
      Console.Write("via=C: state={0} cow={1} slotsUsed={2}", state, cow, slots);
      if (r >= 80) {
        uint reason = BitConverter.ToUInt32(b, 76);
        Console.Write(" reason={0}", ReasonName(reason));
      }
      if (r >= 100) {
        Console.Write(" cfgValid={0} thawNext={1} cfgSeq={2}",
          BitConverter.ToUInt32(b, 88), BitConverter.ToUInt32(b, 92), BitConverter.ToUInt32(b, 96));
      }
      try {
        var p = Registry.GetValue(@"HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters", "EnableCow", 0);
        var be = Registry.GetValue(@"HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters", "OverlayBackend", 0);
        Console.Write(" EnableCow={0} Backend={1}", p, be);
      } catch { }
      Console.WriteLine();
      return 0;
    }
  }

  static int Thaw(bool set) {
    using (var h = OpenVol()) {
      var input = BitConverter.GetBytes(set ? 1u : 0u);
      uint r;
      if (!DeviceIoControl(h, IOCTL_OF_SET_THAW_NEXT_BOOT, input, (uint)input.Length, null, 0, out r, IntPtr.Zero)) {
        Console.WriteLine("thaw-{0} failed err={1}", set ? "set" : "clear", Marshal.GetLastWin32Error());
        return 3;
      }
      Console.WriteLine("RESULT OK thaw-{0}", set ? "set" : "clear");
      return 0;
    }
  }

  static int FreezeArm(bool enable) {
    // Registry only — takes effect next boot (and only sticks if thawed / sticky).
    const string key = @"SYSTEM\CurrentControlSet\Services\OfFilter\Parameters";
    using (var k = Registry.LocalMachine.OpenSubKey(key, true)) {
      if (k == null) { Console.WriteLine("Parameters key missing"); return 4; }
      k.SetValue("EnableCow", enable ? 1 : 0, RegistryValueKind.DWord);
    }
    Console.WriteLine("RESULT OK EnableCow={0} (next boot)", enable ? 1 : 0);
    return 0;
  }

  static int Main(string[] a) {
    string cmd = a.Length > 0 ? a[0].ToLowerInvariant() : "status";
    try {
      if (cmd == "status" || cmd == "query") return Status();
      if (cmd == "thaw-set" || cmd == "thaw") return Thaw(true);
      if (cmd == "thaw-clear") return Thaw(false);
      if (cmd == "freeze") return FreezeArm(true);
      if (cmd == "unfreeze" || cmd == "thaw-persist") {
        // Convenience: EnableCow=0 + clear sector flag (call while sticky/thawed).
        int e = FreezeArm(false);
        if (e != 0) return e;
        return Thaw(false);
      }
      Console.WriteLine("usage: ofctl [status|thaw-set|thaw-clear|freeze|unfreeze]");
      return 1;
    } catch (Exception ex) {
      Console.WriteLine("error: " + ex.Message);
      return 5;
    }
  }
}
