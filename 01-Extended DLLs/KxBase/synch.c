///////////////////////////////////////////////////////////////////////////////
//
// Module Name:
//
//     synch.c
//
// Abstract:
//
//     Contains functions related to thread synchronization.
//
// Author:
//
//     vxiiduu (11-Feb-2022)
//
// Environment:
//
//     Win32 mode.
//
// Revision History:
//
//     vxiiduu              11-Feb-2022  Initial creation.
//
///////////////////////////////////////////////////////////////////////////////

#include "buildcfg.h"
#include "kxbasep.h"

//
// This function is a wrapper around (Kex)RtlWaitOnAddress.
//
///////////////////////////////////////////////////////////////////////////////
//
// InitializeCriticalSectionEx
//
// Windows 8+ API. On Win7, winbase.h declares this with dllimport from
// kernel32, causing C4273 (inconsistent dll linkage) when we implement it
// here. We suppress the warning since KxBase IS the kernel32 replacement.
//
///////////////////////////////////////////////////////////////////////////////

#pragma warning(suppress: 4273)
KXBASEAPI BOOL WINAPI InitializeCriticalSectionEx(
                OUT     LPCRITICAL_SECTION      lpCriticalSection,
                IN      DWORD                           dwSpinCount,
                IN      DWORD                           Flags)
{
        if (!InitializeCriticalSectionAndSpinCount(lpCriticalSection, dwSpinCount)) {
                return FALSE;
        }

        return TRUE;
}

///////////////////////////////////////////////////////////////////////////////
//
// PrivIsDllSynchronizationHeld
//
// Undocumented Windows function used by some applications.
// Returns FALSE to indicate no DLL synchronization is held.
//
///////////////////////////////////////////////////////////////////////////////

#pragma warning(suppress: 4273)
KXBASEAPI BOOL WINAPI PrivIsDllSynchronizationHeld(
                VOID)
{
        return FALSE;
}

KXBASEAPI BOOL WINAPI WaitOnAddress(
        IN      VOLATILE VOID   *Address,
        IN      PVOID                   CompareAddress,
        IN      SIZE_T                  AddressSize,
        IN      DWORD                   Milliseconds OPTIONAL)
{
        NTSTATUS Status;
        PLARGE_INTEGER TimeOutPointer;
        LARGE_INTEGER TimeOut;

        TimeOutPointer = BaseFormatTimeOut(&TimeOut, Milliseconds);

        Status = KexRtlWaitOnAddress(
                Address,
                CompareAddress,
                AddressSize,
                TimeOutPointer);

        BaseSetLastNTError(Status);
        
        if (NT_SUCCESS(Status) && Status != STATUS_TIMEOUT) {
                return TRUE;
        } else {
                return FALSE;
        }
}