/*++
Module Name: Control.h
Abstract: SYSTEM-only control device (\\.\OfFilter) for Phase 4.
--*/

#pragma once

#include "Driver.h"

NTSTATUS
OfFilterCreateControlDevice(
    _In_ WDFDRIVER Driver
    );

VOID
OfFilterDeleteControlDevice(
    _In_ WDFDRIVER Driver
    );
