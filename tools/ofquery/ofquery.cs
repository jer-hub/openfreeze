using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

class P {
  [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern SafeFileHandle CreateFile(string n, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);
  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool DeviceIoControl(SafeFileHandle h, uint code, IntPtr i, uint il, byte[] o, uint ol, out uint r, IntPtr ov);
  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] i, uint il, byte[] o, uint ol, out uint r, IntPtr ov);

  // CTL_CODE(0x8F4F, 0x902, METHOD_BUFFERED, FILE_ANY_ACCESS)
  const uint IOCTL_OF_DEBUG_QUERY_COW = 2404328456u;
  // CTL_CODE(0x8F4F, 0x904, METHOD_BUFFERED, FILE_WRITE_ACCESS)
  // NOTE: FILE_READ_ACCESS form 0x8F4F6410 was hardcoded as 2404344848 and never matched the driver.
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

  static int Query(SafeFileHandle h) {
    // OF_DEBUG_COW_STATE_V4 fields end at 100; MSVC sizeof may pad to 104.
    var b = new byte[104];
    uint r;
    if (!DeviceIoControl(h, IOCTL_OF_DEBUG_QUERY_COW, IntPtr.Zero, 0, b, (uint)b.Length, out r, IntPtr.Zero)) {
      Console.WriteLine("query failed err=" + Marshal.GetLastWin32Error());
      return 2;
    }
    uint cow = BitConverter.ToUInt32(b, 0);
    uint torn = BitConverter.ToUInt32(b, 4);
    uint slots = BitConverter.ToUInt32(b, 8);
    uint pending = BitConverter.ToUInt32(b, 12);
    long bypass = BitConverter.ToInt64(b, 16);
    long worker = BitConverter.ToInt64(b, 24);
    ulong full = BitConverter.ToUInt64(b, 32);
    string state = (torn != 0) ? "TORN" : (cow != 0) ? "FROZEN" : "PASSTHROUGH";
    Console.Write("cow={0} torn={1} slotsUsed={2} pendingW={3} bypassR={4} workerR={5} full={6} state={7}",
      cow, torn, slots, pending, bypass, worker, full, state);
    if (r >= 72) {
      uint ptRanges = BitConverter.ToUInt32(b, 40);
      uint l2 = BitConverter.ToUInt32(b, 44);
      long ptW = BitConverter.ToInt64(b, 48);
      long ptR = BitConverter.ToInt64(b, 56);
      ulong ptBytes = BitConverter.ToUInt64(b, 64);
      Console.Write(" ptRanges={0} l2Res={1} ptW={2} ptR={3} ptBytes={4}", ptRanges, l2, ptW, ptR, ptBytes);
    }
    if (r >= 80) {
      uint target = BitConverter.ToUInt32(b, 72);
      uint reasonCode = BitConverter.ToUInt32(b, 76);
      Console.Write(" target={0} reason={1}", target, ReasonName(reasonCode));
    }
    if (r >= 88) {
      uint initStep = BitConverter.ToUInt32(b, 80);
      uint initStatus = BitConverter.ToUInt32(b, 84);
      Console.Write(" initStep={0} initStatus=0x{1:X8}", initStep, initStatus);
    }
    if (r >= 100) {
      uint cfgValid = BitConverter.ToUInt32(b, 88);
      uint thawNext = BitConverter.ToUInt32(b, 92);
      uint cfgSeq = BitConverter.ToUInt32(b, 96);
      Console.Write(" cfgValid={0} thawNext={1} cfgSeq={2}", cfgValid, thawNext, cfgSeq);
    }
    Console.WriteLine();
    return 0;
  }

  static int SetThaw(SafeFileHandle h, bool set) {
    var input = BitConverter.GetBytes(set ? 1u : 0u);
    uint r;
    if (!DeviceIoControl(h, IOCTL_OF_SET_THAW_NEXT_BOOT, input, (uint)input.Length, null, 0, out r, IntPtr.Zero)) {
      Console.WriteLine("thaw-{0} failed err={1}", set ? "set" : "clear", Marshal.GetLastWin32Error());
      return 3;
    }
    Console.WriteLine("RESULT OK thaw-{0}", set ? "set" : "clear");
    return 0;
  }

  static int Main(string[] a) {
    string mode = a.Length > 0 ? a[0].ToLowerInvariant() : "query";
    // GENERIC_READ|GENERIC_WRITE for thaw IOCTL (FILE_WRITE_ACCESS); query also works
    uint access = 0xC0000000u;
    var h = CreateFile(@"\\.\C:", access, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
    if (h.IsInvalid) {
      Console.WriteLine("open failed err=" + Marshal.GetLastWin32Error());
      return 1;
    }
    using (h) {
      if (mode == "thaw-set" || mode == "thawset" || mode == "set-thaw") return SetThaw(h, true);
      if (mode == "thaw-clear" || mode == "thawclear" || mode == "clear-thaw") return SetThaw(h, false);
      if (mode == "query" || mode == "q") return Query(h);
      if (a.Length == 0) return Query(h);
      Console.WriteLine("usage: ofquery [query|thaw-set|thaw-clear]");
      return 1;
    }
  }
}
