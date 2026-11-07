using System;
using OpenFreeze.Core;

namespace OpenFreeze.Cli
{
    static class Program
    {
        static int Main(string[] args)
        {
            if (args == null || args.Length == 0)
            {
                Usage();
                return 2;
            }

            try
            {
                var cmd = args[0].ToLowerInvariant();
                PipeRequest req;
                switch (cmd)
                {
                    case "ping":
                        req = new PipeRequest { Command = PipeCommand.Ping };
                        break;
                    case "status":
                        req = new PipeRequest { Command = PipeCommand.GetStatus };
                        break;
                    case "bootstrap":
                        if (args.Length < 2) { Console.Error.WriteLine("usage: ofpipe bootstrap <newPassword>"); return 2; }
                        req = new PipeRequest { Command = PipeCommand.SetPasswordBootstrap, NewPassword = args[1] };
                        break;
                    case "changepw":
                        if (args.Length < 3) { Console.Error.WriteLine("usage: ofpipe changepw <old> <new>"); return 2; }
                        req = new PipeRequest
                        {
                            Command = PipeCommand.ChangePassword,
                            Password = args[1],
                            NewPassword = args[2]
                        };
                        break;
                    case "setmode":
                        // ofpipe setmode <password> frozen|thawed|thawedn [n]
                        if (args.Length < 3) { Console.Error.WriteLine("usage: ofpipe setmode <password> frozen|thawed|thawedn [n]"); return 2; }
                        FreezeMode mode;
                        uint n = 1;
                        switch (args[2].ToLowerInvariant())
                        {
                            case "frozen": mode = FreezeMode.Frozen; break;
                            case "thawed": mode = FreezeMode.Thawed; break;
                            case "thawedn":
                                mode = FreezeMode.ThawedForN;
                                if (args.Length >= 4) n = uint.Parse(args[3]);
                                break;
                            default:
                                Console.Error.WriteLine("mode must be frozen|thawed|thawedn");
                                return 2;
                        }
                        req = new PipeRequest
                        {
                            Command = PipeCommand.SetNextBootMode,
                            Password = args[1],
                            Mode = mode,
                            ThawedForN = n
                        };
                        Console.Error.WriteLine("dbg setmode mode={0} n={1}", mode, n);
                        break;
                    default:
                        Usage();
                        return 2;
                }

                var resp = PipeProtocol.Transact(req);
                Print(resp);
                return resp.Code == PipeStatusCode.Ok ? 0 : 1;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("ERR " + ex.Message);
                return 3;
            }
        }

        static void Print(PipeResponse resp)
        {
            Console.WriteLine("code={0} hasPassword={1} lockout={2} msg={3}",
                resp.Code, resp.HasPassword ? 1 : 0, resp.LockoutRemainingSeconds, resp.Message ?? "");
            if (resp.Status != null)
            {
                var st = resp.Status;
                st.RefreshStateName();
                Console.WriteLine("state={0} cow={1} torn={2} slots={3}/{4} backend={5} reason={6} thawNext={7} thawedForN={8} cfgValid={9} seq={10} init={11}/{12:X8}",
                    st.StateName,
                    st.CowEnabled ? 1 : 0,
                    st.CowTornDown ? 1 : 0,
                    st.SlotsUsed,
                    st.SlotCount,
                    st.OverlayBackend,
                    st.RefuseReason,
                    st.ThawNext ? 1 : 0,
                    st.ThawedForN,
                    st.CfgValid ? 1 : 0,
                    st.CfgSeq,
                    st.InitStep,
                    st.InitStatus);
            }
        }

        static void Usage()
        {
            Console.WriteLine("ofpipe — OpenFreeze named-pipe client");
            Console.WriteLine("  ofpipe ping");
            Console.WriteLine("  ofpipe status");
            Console.WriteLine("  ofpipe bootstrap <newPassword>");
            Console.WriteLine("  ofpipe changepw <old> <new>");
            Console.WriteLine("  ofpipe setmode <password> frozen|thawed|thawedn [n]");
        }
    }
}
