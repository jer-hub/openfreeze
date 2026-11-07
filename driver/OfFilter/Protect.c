/*++

Module Name:

    Protect.c

Abstract:

    Phase 5 self-protection — Ob (anti-kill) + Cm (registry while frozen).

--*/

#include <ntifs.h>
#include <wdf.h>

#include "Protect.h"
#include "Device.h"
#include "Driver.h"

extern PDEVICE_CONTEXT g_OfTargetDeviceContext;

#ifndef PROCESS_TERMINATE
#define PROCESS_TERMINATE     (0x0001)
#endif
#ifndef PROCESS_VM_OPERATION
#define PROCESS_VM_OPERATION  (0x0008)
#endif
#ifndef PROCESS_VM_WRITE
#define PROCESS_VM_WRITE      (0x0020)
#endif

#define OF_PROTECT_STRIP_ACCESS \
    (PROCESS_TERMINATE | PROCESS_VM_OPERATION | PROCESS_VM_WRITE)

static WDFDRIVER g_ProtectDriver = NULL;
static PVOID g_ObHandle = NULL;
static LARGE_INTEGER g_CmCookie;
static BOOLEAN g_CmRegistered = FALSE;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, OfFilterProtectStart)
#pragma alloc_text(PAGE, OfFilterProtectStop)
#endif

static
BOOLEAN
OfProtectCallerIsLocalSystem(
    VOID
    )
{
    SECURITY_SUBJECT_CONTEXT subj;
    PACCESS_TOKEN token;
    PTOKEN_USER userInfo = NULL;
    NTSTATUS status;
    BOOLEAN isSystem = FALSE;

    SeCaptureSubjectContext(&subj);
    token = SeQuerySubjectContextToken(&subj);
    status = SeQueryInformationToken(token, TokenUser, &userInfo);
    if (NT_SUCCESS(status) && userInfo != NULL) {
        isSystem = RtlEqualSid(userInfo->User.Sid, SeExports->SeLocalSystemSid);
        ExFreePool(userInfo);
    }
    SeReleaseSubjectContext(&subj);
    return isSystem;
}

static
BOOLEAN
OfProtectIsFrozen(
    VOID
    )
{
    PDEVICE_CONTEXT ctx = g_OfTargetDeviceContext;
    return (ctx != NULL && ctx->IsTargetVolume && ctx->CowEnabled) ? TRUE : FALSE;
}

static
BOOLEAN
OfProtectUnicodeContainsI(
    _In_ PCUNICODE_STRING Haystack,
    _In_ PCWSTR Needle
    )
{
    UNICODE_STRING n;
    USHORT i;
    USHORT nLen;

    if (Haystack == NULL || Haystack->Buffer == NULL || Needle == NULL) {
        return FALSE;
    }
    RtlInitUnicodeString(&n, Needle);
    nLen = n.Length;
    if (nLen == 0 || Haystack->Length < nLen) {
        return FALSE;
    }
    for (i = 0; i + nLen <= Haystack->Length; i = (USHORT)(i + sizeof(WCHAR))) {
        UNICODE_STRING slice;
        slice.Buffer = (PWCH)((PUCHAR)Haystack->Buffer + i);
        slice.Length = nLen;
        slice.MaximumLength = nLen;
        if (RtlEqualUnicodeString(&slice, &n, TRUE)) {
            return TRUE;
        }
    }
    return FALSE;
}

static
BOOLEAN
OfProtectKeyIsProtected(
    _In_ PCUNICODE_STRING KeyName
    )
{
    if (OfProtectUnicodeContainsI(KeyName, L"\\SERVICES\\OFFILTER")) {
        return TRUE;
    }
    if (OfProtectUnicodeContainsI(KeyName, L"\\SERVICES\\OPENFREEZESVC")) {
        return TRUE;
    }
    if (OfProtectUnicodeContainsI(KeyName, L"{71A27CDD-812A-11D0-BEC7-08002BE2092F}")) {
        return TRUE;
    }
    return FALSE;
}

static
OB_PREOP_CALLBACK_STATUS
OfProtectObPre(
    _In_ PVOID RegistrationContext,
    _Inout_ POB_PRE_OPERATION_INFORMATION OpInfo
    )
{
    PDRIVER_CONTEXT driverCtx;
    PEPROCESS target;
    LONG targetPid;
    LONG ownerPid;
    ACCESS_MASK *desired;

    UNREFERENCED_PARAMETER(RegistrationContext);

    if (OpInfo == NULL || OpInfo->ObjectType != *PsProcessType) {
        return OB_PREOP_SUCCESS;
    }
    if (g_ProtectDriver == NULL) {
        return OB_PREOP_SUCCESS;
    }
    driverCtx = OfFilterGetDriverContext(g_ProtectDriver);
    ownerPid = driverCtx->ControlOwnerPid;
    if (ownerPid == 0) {
        return OB_PREOP_SUCCESS;
    }

    target = (PEPROCESS)OpInfo->Object;
    targetPid = (LONG)(ULONG_PTR)PsGetProcessId(target);
    if (targetPid != ownerPid) {
        return OB_PREOP_SUCCESS;
    }
    if (OfProtectCallerIsLocalSystem()) {
        return OB_PREOP_SUCCESS;
    }

    if (OpInfo->Operation == OB_OPERATION_HANDLE_CREATE) {
        desired = &OpInfo->Parameters->CreateHandleInformation.DesiredAccess;
    } else if (OpInfo->Operation == OB_OPERATION_HANDLE_DUPLICATE) {
        desired = &OpInfo->Parameters->DuplicateHandleInformation.DesiredAccess;
    } else {
        return OB_PREOP_SUCCESS;
    }
    *desired &= ~OF_PROTECT_STRIP_ACCESS;
    return OB_PREOP_SUCCESS;
}

static
NTSTATUS
OfProtectCmCallback(
    _In_ PVOID CallbackContext,
    _In_opt_ PVOID Argument1,
    _In_opt_ PVOID Argument2
    )
{
    REG_NOTIFY_CLASS notifyClass;
    PUNICODE_STRING keyName = NULL;
    PVOID object = NULL;
    NTSTATUS status;
    PCUNICODE_STRING nameFromId = NULL;
    ULONG_PTR objectId = 0;
    BOOLEAN gotId = FALSE;

    UNREFERENCED_PARAMETER(CallbackContext);

    if (!OfProtectIsFrozen()) {
        return STATUS_SUCCESS;
    }
    if (OfProtectCallerIsLocalSystem()) {
        return STATUS_SUCCESS;
    }
    if (Argument1 == NULL) {
        return STATUS_SUCCESS;
    }

    notifyClass = (REG_NOTIFY_CLASS)(ULONG_PTR)Argument1;
    switch (notifyClass) {
    case RegNtPreSetValueKey:
        object = ((PREG_SET_VALUE_KEY_INFORMATION)Argument2)->Object;
        break;
    case RegNtPreDeleteValueKey:
        object = ((PREG_DELETE_VALUE_KEY_INFORMATION)Argument2)->Object;
        break;
    case RegNtPreDeleteKey:
        object = ((PREG_DELETE_KEY_INFORMATION)Argument2)->Object;
        break;
    case RegNtPreRenameKey:
        object = ((PREG_RENAME_KEY_INFORMATION)Argument2)->Object;
        break;
    case RegNtPreCreateKeyEx:
        keyName = ((PREG_CREATE_KEY_INFORMATION)Argument2)->CompleteName;
        break;
    case RegNtPreOpenKeyEx:
        return STATUS_SUCCESS;
    default:
        return STATUS_SUCCESS;
    }

    if (keyName == NULL && object != NULL) {
        status = CmCallbackGetKeyObjectIDEx(&g_CmCookie, object, &objectId, &nameFromId, 0);
        if (NT_SUCCESS(status) && nameFromId != NULL) {
            keyName = (PUNICODE_STRING)nameFromId;
            gotId = TRUE;
        }
    }

    if (keyName != NULL && OfProtectKeyIsProtected(keyName)) {
        if (gotId) {
            CmCallbackReleaseKeyObjectIDEx(nameFromId);
        }
        return STATUS_ACCESS_DENIED;
    }
    if (gotId) {
        CmCallbackReleaseKeyObjectIDEx(nameFromId);
    }
    return STATUS_SUCCESS;
}

NTSTATUS
OfFilterProtectStart(
    _In_ WDFDRIVER Driver
    )
{
    NTSTATUS status;
    OB_CALLBACK_REGISTRATION obReg;
    OB_OPERATION_REGISTRATION opReg;
    UNICODE_STRING obAltitude;
    UNICODE_STRING cmAltitude;
    PDRIVER_OBJECT driverObject;

    PAGED_CODE();

    g_ProtectDriver = Driver;
    driverObject = WdfDriverWdmGetDriverObject(Driver);

    RtlZeroMemory(&opReg, sizeof(opReg));
    opReg.ObjectType = PsProcessType;
    opReg.Operations = OB_OPERATION_HANDLE_CREATE | OB_OPERATION_HANDLE_DUPLICATE;
    opReg.PreOperation = OfProtectObPre;
    opReg.PostOperation = NULL;

    RtlInitUnicodeString(&obAltitude, L"385100");
    RtlZeroMemory(&obReg, sizeof(obReg));
    obReg.Version = OB_FLT_REGISTRATION_VERSION;
    obReg.OperationRegistrationCount = 1;
    obReg.Altitude = obAltitude;
    obReg.RegistrationContext = NULL;
    obReg.OperationRegistration = &opReg;

    status = ObRegisterCallbacks(&obReg, &g_ObHandle);
    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "OfFilter: ObRegisterCallbacks failed 0x%08X (need /INTEGRITYCHECK?)\n",
                   status);
        g_ObHandle = NULL;
    } else {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "OfFilter: ObRegisterCallbacks OK\n");
    }

    RtlInitUnicodeString(&cmAltitude, L"385110");
    status = CmRegisterCallbackEx(OfProtectCmCallback,
                                  &cmAltitude,
                                  driverObject,
                                  NULL,
                                  &g_CmCookie,
                                  NULL);
    if (!NT_SUCCESS(status)) {
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "OfFilter: CmRegisterCallbackEx failed 0x%08X\n", status);
        g_CmRegistered = FALSE;
    } else {
        g_CmRegistered = TRUE;
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "OfFilter: CmRegisterCallbackEx OK\n");
    }

    return STATUS_SUCCESS;
}

VOID
OfFilterProtectStop(
    _In_ WDFDRIVER Driver
    )
{
    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    if (g_ObHandle != NULL) {
        ObUnRegisterCallbacks(g_ObHandle);
        g_ObHandle = NULL;
    }
    if (g_CmRegistered) {
        CmUnRegisterCallback(g_CmCookie);
        g_CmRegistered = FALSE;
    }
    g_ProtectDriver = NULL;
}
