using System;
using System.Runtime.Serialization;

namespace OpenFreeze.Core
{
    public enum FreezeMode : uint
    {
        Frozen = 0,
        Thawed = 1,
        ThawedForN = 2
    }

    public enum RefuseReason : uint
    {
        None = 0,
        NotTarget = 1,
        ConfigRefuse = 2,
        ThawRequested = 3,
        ConfigCorrupt = 4
    }

    [DataContract]
    public sealed class DriverStatus
    {
        [DataMember] public bool CowEnabled { get; set; }
        [DataMember] public bool CowTornDown { get; set; }
        [DataMember] public uint SlotsUsed { get; set; }
        [DataMember] public uint SlotCount { get; set; }
        [DataMember] public uint OverlayBackend { get; set; }
        [DataMember] public RefuseReason RefuseReason { get; set; }
        [DataMember] public bool CfgValid { get; set; }
        [DataMember] public bool ThawNext { get; set; }
        [DataMember] public uint CfgSeq { get; set; }
        [DataMember] public uint ThawedForN { get; set; }
        [DataMember] public uint InitStep { get; set; }
        [DataMember] public uint InitStatus { get; set; }
        [DataMember] public string StateName { get; set; }

        public void RefreshStateName()
        {
            if (CowTornDown) StateName = "TORN";
            else if (CowEnabled) StateName = "FROZEN";
            else if (RefuseReason == RefuseReason.ThawRequested) StateName = "THAWED_STICKY";
            else StateName = "PASSTHROUGH";
        }

        public bool IsThawedForPasswordChange
        {
            get { return !CowEnabled && !CowTornDown; }
        }
    }

    public enum PipeCommand : byte
    {
        Ping = 1,
        GetStatus = 2,
        SetNextBootMode = 3,
        ChangePassword = 4,
        SetPasswordBootstrap = 5
    }

    public enum PipeStatusCode : byte
    {
        Ok = 0,
        BadPassword = 1,
        LockedOut = 2,
        NotThawed = 3,
        DriverError = 4,
        InvalidRequest = 5,
        NoPasswordSet = 6,
        AlreadyHasPassword = 7,
        InternalError = 8
    }

    [DataContract]
    public sealed class PipeRequest
    {
        [DataMember] public PipeCommand Command { get; set; }
        [DataMember] public string Password { get; set; }
        [DataMember] public string NewPassword { get; set; }
        [DataMember] public FreezeMode Mode { get; set; }
        [DataMember] public uint ThawedForN { get; set; }
    }

    [DataContract]
    public sealed class PipeResponse
    {
        [DataMember] public PipeStatusCode Code { get; set; }
        [DataMember] public string Message { get; set; }
        [DataMember] public DriverStatus Status { get; set; }
        [DataMember] public int LockoutRemainingSeconds { get; set; }
        [DataMember] public bool HasPassword { get; set; }
    }
}
