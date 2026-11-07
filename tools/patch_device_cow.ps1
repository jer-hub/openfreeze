# Surgical Device.c patch: derive file-backend SlotCount before BlockMapInitialize;
# create file overlay (or refuse) instead of silent RAM fallback.
$ErrorActionPreference = 'Stop'
$path = 'C:\Users\ADMIN\openfreeze\driver\OfFilter\Device.c'
$c = Get-Content $path -Raw
if ($c -match 'OfOverlayComputeSlotCount') { Write-Host 'Device.c already patched'; return }

# Insert file-backend precheck just before mapStatus = OfBlockMapInitialize
$anchor = @'
    mapStatus = OfBlockMapInitialize(&deviceCtx->BlockMap,
                                     maxBlocks,
                                     driverCtx->SlotCount,
                                     OfFilterBlockMapAlloc,
                                     OfFilterBlockMapFree,
                                     deviceCtx);
'@

$pre = @'
    //
    // File backend: validate extents + volume unique ID and derive SlotCount
    // before sizing the block map. Invalid config => passthrough (refuse freeze).
    //
    if (driverCtx->OverlayBackend == (ULONG)OfOverlayFile) {
        OF_U32 derived;
        OF_STATUS vs;

        if (!driverCtx->OverlayExtentsLoaded ||
            !driverCtx->UniqueIdConfigured ||
            !OfOverlayUniqueIdEqual(driverCtx->UniqueId,
                                    driverCtx->UniqueIdLength,
                                    deviceCtx->UniqueId,
                                    deviceCtx->UniqueIdLength)) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                        "File overlay rejected (extents/unique id) - passthrough");
            DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                       "OfFilter: file overlay invalid - passthrough\n");
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }

        derived = OfOverlayComputeSlotCount(driverCtx->OverlayFileSize);
        if (derived == 0) {
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }

        vs = OfOverlayValidateExtents(driverCtx->OverlayExtents,
                                      driverCtx->OverlayExtentCount,
                                      driverCtx->OverlayFileSize,
                                      derived,
                                      volumeSize);
        if (vs != OfStatusOk) {
            TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                        "OverlayExtents validate failed (%d) - passthrough", (int)vs);
            return STATUS_DEVICE_CONFIGURATION_ERROR;
        }

        driverCtx->SlotCount = derived;

        if (!deviceCtx->PassthroughSetByIoctl) {
            OF_RANGE combined[OF_MAX_PASSTHROUGH_RANGES];
            OF_U32 combinedCount = 0;
            OF_U32 i;
            for (i = 0; i < driverCtx->PassthroughRawCount && combinedCount < OF_MAX_PASSTHROUGH_RANGES; ++i) {
                combined[combinedCount++] = driverCtx->PassthroughRaw[i];
            }
            for (i = 0; i < driverCtx->OverlayExtentCount && combinedCount < OF_MAX_PASSTHROUGH_RANGES; ++i) {
                combined[combinedCount].Offset = driverCtx->OverlayExtents[i].VolumeOffset;
                combined[combinedCount].Length = driverCtx->OverlayExtents[i].Length;
                combinedCount++;
            }
            {
                OF_STATUS rs = OfRangeListBuild(&deviceCtx->Passthrough,
                                                combined,
                                                combinedCount,
                                                volumeSize);
                if (rs != OfStatusOk) {
                    deviceCtx->Passthrough.Count = 0;
                    TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE,
                                "passthrough+overlay ranges rejected (%d)", (int)rs);
                } else {
                    TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DEVICE,
                                "Passthrough includes overlay extents; ranges=%lu",
                                deviceCtx->Passthrough.Count);
                }
            }
        }
    }

'@

if (-not $c.Contains($anchor)) {
  # try LF-only
  $anchor = $anchor -replace "`r`n","`n"
  $c = $c -replace "`r`n","`n"
}
if (-not $c.Contains($anchor)) { throw 'BlockMapInitialize anchor not found in Device.c' }
$c = $c.Replace($anchor, $pre + $anchor)

$oldFb = @'
    if (driverCtx->OverlayBackend == (ULONG)OfOverlayFile) {
        //
        // File backend needs extents from the installer (Phase 3). Until then
        // fail closed to RAM so CoW still works in a VM.
        //
        TraceEvents(TRACE_LEVEL_WARNING, TRACE_DEVICE,
                    "File overlay requested but extents not loaded - using RAM backend");
    }

    status = OfOverlayCreateRam(Device, driverCtx->SlotCount, &deviceCtx->Overlay);
    if (!NT_SUCCESS(status)) {
        OfFilterTeardownCow(Device);
        return status;
    }
'@
$newFb = @'
    if (driverCtx->OverlayBackend == (ULONG)OfOverlayFile) {
        status = OfOverlayCreateFile(Device,
                                     driverCtx->SlotCount,
                                     driverCtx->OverlayExtents,
                                     driverCtx->OverlayExtentCount,
                                     &deviceCtx->Overlay);
    } else {
        status = OfOverlayCreateRam(Device, driverCtx->SlotCount, &deviceCtx->Overlay);
    }
    if (!NT_SUCCESS(status)) {
        OfFilterTeardownCow(Device);
        return status;
    }
'@
$oldFb2 = $oldFb -replace "`r`n","`n"
$newFb2 = $newFb -replace "`r`n","`n"
if ($c.Contains($oldFb)) { $c = $c.Replace($oldFb, $newFb) }
elseif ($c.Contains($oldFb2)) { $c = $c.Replace($oldFb2, $newFb2) }
else { throw 'file-backend RAM-fallback block not found' }

Set-Content $path $c -Encoding utf8
Write-Host 'Device.c patched OK'
