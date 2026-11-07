using System;
using System.Runtime.InteropServices;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;

namespace OpenFreeze.Core
{
    /// <summary>
    /// Talks to \\.\OfFilter (SYSTEM) when possible, else \\.\C: volume IOCTLs.
    /// </summary>
    public sealed class DriverClient : IDisposable
    {
        const uint IOCTL_OF_DEBUG_QUERY_COW = 2404328456u;
        const uint IOCTL_OF_SET_THAW_NEXT_BOOT = 0x8F4FA410u;
        const uint IOCTL_OF_GET_STATUS = 0x8F4F2804u; // CTL_CODE(0x8F4F, 0xA01, BUFFERED, ANY)
        const uint IOCTL_OF_SET_NEXT_BOOT_MODE = 0x8F4FA808u; // CTL_CODE(0x8F4F, 0xA02, BUFFERED, WRITE)
        const uint IOCTL_OF_GET_OVERLAY_EVENT = 0x8F4F280Cu; // CTL_CODE(0x8F4F, 0xA03, BUFFERED, ANY)

        public const uint OverlayEventNone = 0;
        public const uint OverlayEventNearFull = 1;
        public const uint OverlayEventFull = 2;

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern SafeFileHandle CreateFile(string n, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool DeviceIoControl(SafeFileHandle h, uint code, IntPtr i, uint il, byte[] o, uint ol, out uint r, IntPtr ov);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool DeviceIoControl(SafeFileHandle h, uint code, byte[] i, uint il, byte[] o, uint ol, out uint r, IntPtr ov);

        SafeFileHandle _control;
        SafeFileHandle _volume;
        bool _disposed;

        public bool HasControlDevice { get { return _control != null && !_control.IsInvalid; } }

        public static DriverClient Open()
        {
            var c = new DriverClient();
            c._control = CreateFile(@"\\.\OfFilter", 0xC0000000u, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
            c._volume = CreateFile(@"\\.\C:", 0xC0000000u, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
            if ((c._control == null || c._control.IsInvalid) && (c._volume == null || c._volume.IsInvalid))
                throw new InvalidOperationException("Cannot open \\\\.\\OfFilter or \\\\.\\C: err=" + Marshal.GetLastWin32Error());
            return c;
        }

        public DriverStatus QueryStatus()
        {
            if (HasControlDevice)
            {
                var st = new byte[48];
                uint r;
                if (DeviceIoControl(_control, IOCTL_OF_GET_STATUS, IntPtr.Zero, 0, st, (uint)st.Length, out r, IntPtr.Zero) && r >= 48)
                    return ParseStatusV1(st);
            }
            if (_volume != null && !_volume.IsInvalid)
            {
                var b = new byte[104];
                uint r;
                if (!DeviceIoControl(_volume, IOCTL_OF_DEBUG_QUERY_COW, IntPtr.Zero, 0, b, (uint)b.Length, out r, IntPtr.Zero))
                {
                    var err = Marshal.GetLastWin32Error();
                    // ERROR_INVALID_FUNCTION (1): OfFilter not on the volume stack (no reboot / testsigning / not installed).
                    if (err == 1)
                        throw new InvalidOperationException(
                            "query failed err=1 (OfFilter not attached to C:). Reboot after install; require testsigning; run Verify-OpenFreeze.ps1. If the driver was never installed, remove the orphan service.");
                    throw new InvalidOperationException("query failed err=" + err);
                }
                return ParseQueryCow(b, r);
            }
            throw new InvalidOperationException("no driver handle");
        }

        public void SetThawNextBoot(bool set)
        {
            var input = BitConverter.GetBytes(set ? 1u : 0u);
            uint r;
            if (HasControlDevice)
            {
                if (!DeviceIoControl(_control, IOCTL_OF_SET_THAW_NEXT_BOOT, input, (uint)input.Length, null, 0, out r, IntPtr.Zero))
                    throw new InvalidOperationException("thaw ioctl failed err=" + Marshal.GetLastWin32Error());
                return;
            }
            if (_volume == null || _volume.IsInvalid)
                throw new InvalidOperationException("no driver handle");
            if (!DeviceIoControl(_volume, IOCTL_OF_SET_THAW_NEXT_BOOT, input, (uint)input.Length, null, 0, out r, IntPtr.Zero))
                throw new InvalidOperationException("thaw ioctl failed err=" + Marshal.GetLastWin32Error());
        }

        public void SetNextBootMode(FreezeMode mode, uint thawedForN)
        {
            // Prefer \\.\OfFilter; fall back to \\.\C: (volume now handles SET_NEXT_BOOT_MODE).
            // Do not use SET_THAW alone for ThawedForN — that drops N.
            var input = new byte[8];
            BitConverter.GetBytes((uint)mode).CopyTo(input, 0);
            BitConverter.GetBytes(thawedForN).CopyTo(input, 4);
            uint r;
            var ok = false;
            var err = 0;
            if (HasControlDevice)
            {
                ok = DeviceIoControl(_control, IOCTL_OF_SET_NEXT_BOOT_MODE, input, (uint)input.Length, null, 0, out r, IntPtr.Zero);
                if (!ok) err = Marshal.GetLastWin32Error();
            }
            if (!ok && _volume != null && !_volume.IsInvalid)
            {
                ok = DeviceIoControl(_volume, IOCTL_OF_SET_NEXT_BOOT_MODE, input, (uint)input.Length, null, 0, out r, IntPtr.Zero);
                if (!ok) err = Marshal.GetLastWin32Error();
            }
            if (!ok)
                throw new InvalidOperationException("SET_NEXT_BOOT_MODE failed err=" + err);

            if (mode == FreezeMode.Frozen)
                SetEnableCow(true);
            else if (mode == FreezeMode.Thawed)
                SetEnableCow(false);
            // ThawedForN: leave EnableCow=1 so auto re-freeze after countdown.
        }

        /// <summary>
        /// Blocking inverted call: returns when overlay is near-full or full.
        /// Requires \\.\OfFilter. timeoutMs maps to CancelIo after wait (best-effort).
        /// </summary>
        public uint WaitOverlayEvent(out uint slotsUsed, out uint slotCount, int timeoutMs = 60000)
        {
            slotsUsed = 0;
            slotCount = 0;
            if (!HasControlDevice)
                throw new InvalidOperationException("WaitOverlayEvent needs \\\\.\\OfFilter");
            var output = new byte[12];
            uint r;
            // Synchronous DeviceIoControl; caller should use a dedicated thread.
            // For timeout, rely on service cancellation / process stop.
            if (!DeviceIoControl(_control, IOCTL_OF_GET_OVERLAY_EVENT, IntPtr.Zero, 0, output, (uint)output.Length, out r, IntPtr.Zero))
                throw new InvalidOperationException("GET_OVERLAY_EVENT failed err=" + Marshal.GetLastWin32Error());
            if (r < 12) throw new InvalidOperationException("GET_OVERLAY_EVENT short read");
            var ev = BitConverter.ToUInt32(output, 0);
            slotsUsed = BitConverter.ToUInt32(output, 4);
            slotCount = BitConverter.ToUInt32(output, 8);
            return ev;
        }

        public static void SetEnableCow(bool enable)
        {
            using (var k = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Services\OfFilter\Parameters", true))
            {
                if (k == null) throw new InvalidOperationException("OfFilter Parameters key missing");
                k.SetValue("EnableCow", enable ? 1 : 0, RegistryValueKind.DWord);
            }
        }

        static DriverStatus ParseStatusV1(byte[] st)
        {
            return new DriverStatus
            {
                CowEnabled = BitConverter.ToUInt32(st, 0) != 0,
                CowTornDown = BitConverter.ToUInt32(st, 4) != 0,
                SlotsUsed = BitConverter.ToUInt32(st, 8),
                SlotCount = BitConverter.ToUInt32(st, 12),
                OverlayBackend = BitConverter.ToUInt32(st, 16),
                RefuseReason = (RefuseReason)BitConverter.ToUInt32(st, 20),
                CfgValid = BitConverter.ToUInt32(st, 24) != 0,
                ThawNext = BitConverter.ToUInt32(st, 28) != 0,
                CfgSeq = BitConverter.ToUInt32(st, 32),
                ThawedForN = BitConverter.ToUInt32(st, 36),
                InitStep = BitConverter.ToUInt32(st, 40),
                InitStatus = BitConverter.ToUInt32(st, 44)
            };
        }

        static DriverStatus ParseQueryCow(byte[] b, uint r)
        {
            var s = new DriverStatus
            {
                CowEnabled = BitConverter.ToUInt32(b, 0) != 0,
                CowTornDown = BitConverter.ToUInt32(b, 4) != 0,
                SlotsUsed = BitConverter.ToUInt32(b, 8)
            };
            if (r >= 80) s.RefuseReason = (RefuseReason)BitConverter.ToUInt32(b, 76);
            if (r >= 88)
            {
                s.InitStep = BitConverter.ToUInt32(b, 80);
                s.InitStatus = BitConverter.ToUInt32(b, 84);
            }
            if (r >= 100)
            {
                s.CfgValid = BitConverter.ToUInt32(b, 88) != 0;
                s.ThawNext = BitConverter.ToUInt32(b, 92) != 0;
                s.CfgSeq = BitConverter.ToUInt32(b, 96);
            }
            try
            {
                var be = Registry.GetValue(@"HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters", "OverlayBackend", 0);
                var sc = Registry.GetValue(@"HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Services\OfFilter\Parameters", "SlotCount", 0);
                s.OverlayBackend = Convert.ToUInt32(be);
                s.SlotCount = Convert.ToUInt32(sc);
            }
            catch { }
            return s;
        }

        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            if (_control != null) _control.Dispose();
            if (_volume != null) _volume.Dispose();
        }
    }
}
