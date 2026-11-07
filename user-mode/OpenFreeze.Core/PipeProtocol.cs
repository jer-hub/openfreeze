using System;
using System.IO;
using System.IO.Pipes;
using System.Runtime.Serialization.Json;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace OpenFreeze.Core
{
    public static class PipeProtocol
    {
        public const string PipeName = "OpenFreeze.Control";
        public static readonly string PipePath = @"\\.\pipe\" + PipeName;

        public static byte[] Serialize<T>(T obj)
        {
            var ser = new DataContractJsonSerializer(typeof(T));
            using (var ms = new MemoryStream())
            {
                ser.WriteObject(ms, obj);
                return ms.ToArray();
            }
        }

        public static T Deserialize<T>(byte[] data)
        {
            var ser = new DataContractJsonSerializer(typeof(T));
            using (var ms = new MemoryStream(data))
                return (T)ser.ReadObject(ms);
        }

        public static void WriteMessage(Stream stream, byte[] payload)
        {
            var len = BitConverter.GetBytes(payload.Length);
            stream.Write(len, 0, 4);
            stream.Write(payload, 0, payload.Length);
            stream.Flush();
        }

        public static byte[] ReadMessage(Stream stream)
        {
            var lenBuf = new byte[4];
            ReadExact(stream, lenBuf, 4);
            var len = BitConverter.ToInt32(lenBuf, 0);
            if (len < 0 || len > 1024 * 1024) throw new InvalidDataException("bad frame length");
            var payload = new byte[len];
            if (len > 0) ReadExact(stream, payload, len);
            return payload;
        }

        static void ReadExact(Stream stream, byte[] buf, int count)
        {
            var off = 0;
            while (off < count)
            {
                var n = stream.Read(buf, off, count - off);
                if (n <= 0) throw new EndOfStreamException();
                off += n;
            }
        }

        public static PipeResponse Transact(PipeRequest request, int timeoutMs = 15000)
        {
            using (var pipe = new NamedPipeClientStream(".", PipeName, PipeDirection.InOut, PipeOptions.None))
            {
                pipe.Connect(timeoutMs);
                pipe.ReadMode = PipeTransmissionMode.Byte;
                WriteMessage(pipe, Serialize(request));
                var respBytes = ReadMessage(pipe);
                return Deserialize<PipeResponse>(respBytes);
            }
        }

        public static Task PipeServerLoopAsync(Func<PipeRequest, PipeResponse> handler, CancellationToken ct)
        {
            return Task.Run(() =>
            {
                while (!ct.IsCancellationRequested)
                {
                    // Authenticated Users can connect; SECURITY_ATTRIBUTES via PipeSecurity.
                    var security = CreatePipeSecurity();
                    using (var server = new NamedPipeServerStream(
                        PipeName,
                        PipeDirection.InOut,
                        1,
                        PipeTransmissionMode.Byte,
                        PipeOptions.Asynchronous,
                        4096,
                        4096,
                        security))
                    {
                        try
                        {
                            server.WaitForConnection();
                            var reqBytes = ReadMessage(server);
                            var req = Deserialize<PipeRequest>(reqBytes);
                            PipeResponse resp;
                            try { resp = handler(req) ?? new PipeResponse { Code = PipeStatusCode.InternalError, Message = "null" }; }
                            catch (Exception ex)
                            {
                                resp = new PipeResponse { Code = PipeStatusCode.InternalError, Message = ex.Message };
                            }
                            WriteMessage(server, Serialize(resp));
                        }
                        catch (Exception)
                        {
                            // Client disconnect / cancel: loop for next client.
                        }
                    }
                }
            }, ct);
        }

        public static PipeSecurity CreatePipeSecurity()
        {
            var ps = new PipeSecurity();
            // SYSTEM + Authenticated Users (connect/read/write). World denied by omission.
            ps.AddAccessRule(new PipeAccessRule(
                new System.Security.Principal.SecurityIdentifier(System.Security.Principal.WellKnownSidType.LocalSystemSid, null),
                PipeAccessRights.FullControl,
                System.Security.AccessControl.AccessControlType.Allow));
            ps.AddAccessRule(new PipeAccessRule(
                new System.Security.Principal.SecurityIdentifier(System.Security.Principal.WellKnownSidType.AuthenticatedUserSid, null),
                PipeAccessRights.ReadWrite | PipeAccessRights.CreateNewInstance,
                System.Security.AccessControl.AccessControlType.Allow));
            return ps;
        }
    }
}
