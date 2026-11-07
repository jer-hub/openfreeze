using System;
using System.Diagnostics;
using System.ServiceProcess;
using System.Threading;
using System.Threading.Tasks;
using OpenFreeze.Core;

namespace OpenFreeze.Service
{
    public sealed class OpenFreezeSvc : ServiceBase
    {
        public const string ServiceNameConst = "OpenFreezeSvc";

        CancellationTokenSource _cts;
        DriverClient _driver;
        DriverClient _overlayDriver; // dedicated handle; must not share with pipe IOCTLs
        Task _overlayWatch;
        int _nearFullLogged;

        public OpenFreezeSvc()
        {
            ServiceName = ServiceNameConst;
            CanStop = true;
            CanShutdown = true;
            AutoLog = true;
        }

        protected override void OnStart(string[] args)
        {
            StartCore();
        }

        protected override void OnStop()
        {
            StopCore();
        }

        public void StartConsole() { StartCore(); }
        public void StopConsole() { StopCore(); }

        void StartCore()
        {
            try
            {
                PasswordStore.EnsureKeyAcl();
                _driver = DriverClient.Open();
            }
            catch (Exception ex)
            {
                EventLog.WriteEntry(ServiceNameConst,
                    "Driver open failed (will retry on request): " + ex.Message,
                    EventLogEntryType.Warning);
            }

            _cts = new CancellationTokenSource();
            PipeProtocol.PipeServerLoopAsync(HandleRequest, _cts.Token);
            // Pended GET_OVERLAY_EVENT needs parallel control queue (0.8.7.4+).
            _overlayWatch = Task.Run(() => OverlayWatchLoop(_cts.Token));
            EventLog.WriteEntry(ServiceNameConst, "Pipe server listening on " + PipeProtocol.PipeName,
                EventLogEntryType.Information);
        }

        void StopCore()
        {
            try { if (_cts != null) _cts.Cancel(); } catch { }
            // Dispose overlay handle first so a pended GET_OVERLAY_EVENT unblocks.
            try { if (_overlayDriver != null) _overlayDriver.Dispose(); } catch { }
            _overlayDriver = null;
            try { if (_driver != null) _driver.Dispose(); } catch { }
            _driver = null;
            try { if (_overlayWatch != null) _overlayWatch.Wait(3000); } catch { }
            _cts = null;
            _overlayWatch = null;
        }

        void OverlayWatchLoop(CancellationToken ct)
        {
            while (!ct.IsCancellationRequested)
            {
                try
                {
                    // Own handle: a pended WaitOverlayEvent on the pipe client's
                    // handle would block all GetStatus/SetMode DeviceIoControls.
                    if (_overlayDriver == null)
                        _overlayDriver = DriverClient.Open();
                    uint used;
                    uint total;
                    var ev = _overlayDriver.WaitOverlayEvent(out used, out total);
                    if (ct.IsCancellationRequested) break;
                    if (ev == DriverClient.OverlayEventNearFull)
                    {
                        if (Interlocked.Exchange(ref _nearFullLogged, 1) == 0)
                        {
                            EventLog.WriteEntry(ServiceNameConst,
                                string.Format("Overlay near-full: {0}/{1} slots.", used, total),
                                EventLogEntryType.Warning);
                        }
                    }
                    else if (ev == DriverClient.OverlayEventFull)
                    {
                        EventLog.WriteEntry(ServiceNameConst,
                            string.Format("Overlay FULL: {0}/{1} slots. Forcing reboot.", used, total),
                            EventLogEntryType.Error);
                        try
                        {
                            Process.Start(new ProcessStartInfo
                            {
                                FileName = "shutdown.exe",
                                Arguments = "/r /t 30 /c \"OpenFreeze: overlay full\" /d p:2:17",
                                CreateNoWindow = true,
                                UseShellExecute = false
                            });
                        }
                        catch (Exception ex)
                        {
                            EventLog.WriteEntry(ServiceNameConst, "reboot failed: " + ex.Message, EventLogEntryType.Error);
                        }
                        break;
                    }
                }
                catch
                {
                    // Driver not ready, handle disposed on stop, or IOCTL cancelled.
                    if (ct.IsCancellationRequested) break;
                    try { ct.WaitHandle.WaitOne(2000); } catch { break; }
                }
            }
        }

        PipeResponse HandleRequest(PipeRequest req)
        {
            if (req == null)
                return Fail(PipeStatusCode.InvalidRequest, "null request");

            switch (req.Command)
            {
                case PipeCommand.Ping:
                    return new PipeResponse
                    {
                        Code = PipeStatusCode.Ok,
                        Message = "pong",
                        HasPassword = PasswordStore.HasPassword()
                    };

                case PipeCommand.GetStatus:
                    return WithStatus(PipeStatusCode.Ok, "ok");

                case PipeCommand.SetPasswordBootstrap:
                    if (PasswordStore.HasPassword())
                        return Fail(PipeStatusCode.AlreadyHasPassword, "password already set");
                    try
                    {
                        PasswordStore.SetPassword(req.NewPassword ?? req.Password);
                        return new PipeResponse
                        {
                            Code = PipeStatusCode.Ok,
                            Message = "password set",
                            HasPassword = true
                        };
                    }
                    catch (Exception ex)
                    {
                        return Fail(PipeStatusCode.InvalidRequest, ex.Message);
                    }

                case PipeCommand.ChangePassword:
                case PipeCommand.SetNextBootMode:
                    return HandleAuthed(req);

                default:
                    return Fail(PipeStatusCode.InvalidRequest, "unknown command");
            }
        }

        PipeResponse HandleAuthed(PipeRequest req)
        {
            if (!PasswordStore.HasPassword())
                return Fail(PipeStatusCode.NoPasswordSet, "no password configured; use SetPasswordBootstrap");

            int lockRem;
            if (!PasswordStore.Verify(req.Password, out lockRem))
            {
                if (lockRem > 0)
                    return new PipeResponse
                    {
                        Code = PipeStatusCode.LockedOut,
                        Message = "locked out",
                        LockoutRemainingSeconds = lockRem
                    };
                return Fail(PipeStatusCode.BadPassword, "bad password");
            }

            DriverStatus st;
            try { st = EnsureDriver().QueryStatus(); }
            catch (Exception ex)
            {
                return Fail(PipeStatusCode.DriverError, ex.Message);
            }

            if (req.Command == PipeCommand.ChangePassword)
            {
                if (!st.IsThawedForPasswordChange)
                    return WithStatus(PipeStatusCode.NotThawed, "password change only while thawed", st);
                try
                {
                    PasswordStore.SetPassword(req.NewPassword);
                    return WithStatus(PipeStatusCode.Ok, "password changed", st);
                }
                catch (Exception ex)
                {
                    return Fail(PipeStatusCode.InvalidRequest, ex.Message);
                }
            }

            // SetNextBootMode (DriverClient persists config sector + EnableCow for Frozen/Thawed)
            try
            {
                EnsureDriver().SetNextBootMode(req.Mode, req.ThawedForN);
                st = EnsureDriver().QueryStatus();
                return WithStatus(PipeStatusCode.Ok,
                    string.Format("mode armed for next boot: {0} n={1} ctrl={2}",
                        req.Mode, req.ThawedForN, EnsureDriver().HasControlDevice ? 1 : 0),
                    st);
            }
            catch (Exception ex)
            {
                return Fail(PipeStatusCode.DriverError, ex.Message);
            }
        }

        DriverClient EnsureDriver()
        {
            if (_driver == null)
                _driver = DriverClient.Open();
            return _driver;
        }

        static PipeResponse Fail(PipeStatusCode code, string msg)
        {
            return new PipeResponse
            {
                Code = code,
                Message = msg,
                HasPassword = PasswordStore.HasPassword()
            };
        }

        PipeResponse WithStatus(PipeStatusCode code, string msg, DriverStatus st = null)
        {
            try
            {
                if (st == null) st = EnsureDriver().QueryStatus();
                st.RefreshStateName();
            }
            catch (Exception ex)
            {
                return new PipeResponse
                {
                    Code = PipeStatusCode.DriverError,
                    Message = ex.Message,
                    HasPassword = PasswordStore.HasPassword()
                };
            }
            return new PipeResponse
            {
                Code = code,
                Message = msg,
                Status = st,
                HasPassword = PasswordStore.HasPassword()
            };
        }
    }
}
