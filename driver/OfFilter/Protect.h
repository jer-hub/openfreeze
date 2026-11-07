/*++
Module Name: Protect.h
Abstract: Phase 5 ObRegisterCallbacks + CmRegisterCallbackEx (prototype).
--*/

#pragma once

#include <ntddk.h>
#include <wdf.h>

NTSTATUS
OfFilterProtectStart(
    _In_ WDFDRIVER Driver
    );

VOID
OfFilterProtectStop(
    _In_ WDFDRIVER Driver
    );
