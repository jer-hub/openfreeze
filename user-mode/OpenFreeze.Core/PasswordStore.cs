using System;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using Microsoft.Win32;

namespace OpenFreeze.Core
{
    /// <summary>
    /// PBKDF2-SHA256 password verifier. Hash lives under
    /// HKLM\SOFTWARE\OpenFreeze\Auth (installer should keep SYSTEM-only ACL).
    /// </summary>
    public static class PasswordStore
    {
        public const string AuthKeyPath = @"SOFTWARE\OpenFreeze\Auth";
        public const int Iterations = 600000;
        public const int SaltBytes = 16;
        public const int HashBytes = 32;
        public const int MaxFailedAttempts = 5;
        public const int LockoutSeconds = 300;

        public static bool HasPassword()
        {
            using (var k = Registry.LocalMachine.OpenSubKey(AuthKeyPath, false))
            {
                if (k == null) return false;
                var hash = k.GetValue("PasswordHash") as byte[];
                var salt = k.GetValue("PasswordSalt") as byte[];
                return hash != null && salt != null && hash.Length == HashBytes && salt.Length == SaltBytes;
            }
        }

        public static void EnsureKeyAcl()
        {
            // Best-effort SYSTEM + Administrators only. Service runs as SYSTEM.
            using (var baseKey = Registry.LocalMachine.CreateSubKey(@"SOFTWARE\OpenFreeze"))
            using (var auth = Registry.LocalMachine.CreateSubKey(AuthKeyPath))
            {
                try
                {
                    var sec = auth.GetAccessControl();
                    sec.SetAccessRuleProtection(true, false);
                    var sys = new SecurityIdentifier(WellKnownSidType.LocalSystemSid, null);
                    var admins = new SecurityIdentifier(WellKnownSidType.BuiltinAdministratorsSid, null);
                    sec.AddAccessRule(new RegistryAccessRule(sys, RegistryRights.FullControl, AccessControlType.Allow));
                    sec.AddAccessRule(new RegistryAccessRule(admins, RegistryRights.FullControl, AccessControlType.Allow));
                    auth.SetAccessControl(sec);
                }
                catch
                {
                    // Non-elevated or ACL denied: leave default.
                }
            }
        }

        public static void SetPassword(string password)
        {
            if (string.IsNullOrEmpty(password) || password.Length < 4)
                throw new ArgumentException("Password must be at least 4 characters.");

            EnsureKeyAcl();
            var salt = new byte[SaltBytes];
            using (var rng = RandomNumberGenerator.Create())
                rng.GetBytes(salt);

            var hash = Derive(password, salt);
            using (var k = Registry.LocalMachine.CreateSubKey(AuthKeyPath))
            {
                k.SetValue("PasswordSalt", salt, RegistryValueKind.Binary);
                k.SetValue("PasswordHash", hash, RegistryValueKind.Binary);
                k.SetValue("Iterations", Iterations, RegistryValueKind.DWord);
                k.SetValue("FailedCount", 0, RegistryValueKind.DWord);
                k.SetValue("LockoutUtcTicks", 0L, RegistryValueKind.QWord);
            }
        }

        public static bool Verify(string password, out int lockoutRemainingSeconds)
        {
            lockoutRemainingSeconds = 0;
            using (var k = Registry.LocalMachine.OpenSubKey(AuthKeyPath, true))
            {
                if (k == null) return false;

                var lockTicks = Convert.ToInt64(k.GetValue("LockoutUtcTicks", 0L));
                if (lockTicks > 0)
                {
                    var until = new DateTime(lockTicks, DateTimeKind.Utc);
                    var rem = (int)Math.Ceiling((until - DateTime.UtcNow).TotalSeconds);
                    if (rem > 0)
                    {
                        lockoutRemainingSeconds = rem;
                        return false;
                    }
                    k.SetValue("LockoutUtcTicks", 0L, RegistryValueKind.QWord);
                    k.SetValue("FailedCount", 0, RegistryValueKind.DWord);
                }

                var salt = k.GetValue("PasswordSalt") as byte[];
                var hash = k.GetValue("PasswordHash") as byte[];
                if (salt == null || hash == null) return false;

                var attempt = Derive(password ?? string.Empty, salt);
                if (FixedTimeEquals(attempt, hash))
                {
                    k.SetValue("FailedCount", 0, RegistryValueKind.DWord);
                    return true;
                }

                var fails = Convert.ToInt32(k.GetValue("FailedCount", 0)) + 1;
                k.SetValue("FailedCount", fails, RegistryValueKind.DWord);
                if (fails >= MaxFailedAttempts)
                {
                    var until = DateTime.UtcNow.AddSeconds(LockoutSeconds);
                    k.SetValue("LockoutUtcTicks", until.Ticks, RegistryValueKind.QWord);
                    lockoutRemainingSeconds = LockoutSeconds;
                }
                return false;
            }
        }

        static byte[] Derive(string password, byte[] salt)
        {
            using (var pbkdf2 = new Rfc2898DeriveBytes(Encoding.UTF8.GetBytes(password), salt, Iterations, HashAlgorithmName.SHA256))
                return pbkdf2.GetBytes(HashBytes);
        }

        static bool FixedTimeEquals(byte[] a, byte[] b)
        {
            if (a == null || b == null || a.Length != b.Length) return false;
            var diff = 0;
            for (var i = 0; i < a.Length; i++) diff |= a[i] ^ b[i];
            return diff == 0;
        }
    }
}
