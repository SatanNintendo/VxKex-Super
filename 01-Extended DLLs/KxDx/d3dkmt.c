///////////////////////////////////////////////////////////////////////////////
//
// Module Name:
//
//     d3dkmt.c
//
// Abstract:
//
//     Implementations of D3DKMT (Display Graphics Kernel Mode Thunk) functions
//     that are not available on Windows 7 but are required by Windows 10+
//     applications, particularly those using Vulkan.
//
//     Key functions:
//       - D3DKMTOpenAdapterFromLuid: Opens a graphics adapter by LUID.
//         On Win10+ this is a native API. On Win7 we implement it by finding
//         the DXGI adapter matching the requested LUID, obtaining an HDC for
//         its output, and calling D3DKMTOpenAdapterFromHdc (which IS available
//         on Win7).
//
//       - D3DKMTEnumAdapters2: Enumerates graphics adapters.
//         On Win10+ this is a native API. On Win7 we implement it using
//         DXGI adapter enumeration + D3DKMTOpenAdapterFromHdc.
//
//     Without these implementations, Vulkan applications crash when third-party
//     overlay layers (e.g. Mirillis Action!) call D3DKMTOpenAdapterFromLuid
//     and don't check the STATUS_NOT_SUPPORTED return value, leading to
//     NULL pointer dereference (reading address 0x88).
//
// Author:
//
//     VxKex Patch
//
///////////////////////////////////////////////////////////////////////////////

#include "buildcfg.h"
#include "kxdxp.h"

//
// D3DKMT type definitions.
// These types and structures are from d3dkmthk.h which is not available
// in the Windows 7.1 SDK used by VxKex.
//
// IMPORTANT: D3DKMT_HANDLE is UINT (always 32-bit), even on 64-bit systems.
// This matches the Windows kernel D3DKMT handle definition.
//

typedef UINT D3DKMT_HANDLE;
typedef UINT D3DDDI_VIDEO_PRESENT_SOURCE_ID;

//
// D3DKMT_OPENADAPTERFROMLUID
// This structure is used with D3DKMTOpenAdapterFromLuid (Windows 10+).
//

typedef struct _D3DKMT_OPENADAPTERFROMLUID {
        LUID    AdapterLuid;            // [in]  The LUID of the adapter to open
        D3DKMT_HANDLE hAdapter;         // [out] Handle to the opened adapter
} D3DKMT_OPENADAPTERFROMLUID;

//
// D3DKMT_OPENADAPTERFROMHDC
// This structure is used with D3DKMTOpenAdapterFromHdc (Windows 7+).
// On Windows 7, D3DKMTOpenAdapterFromHdc is exported from gdi32.dll.
//

typedef struct _D3DKMT_OPENADAPTERFROMHDC {
        HDC     hDc;                                                    // [in]
        D3DKMT_HANDLE hAdapter;                         // [out]
        LUID    AdapterLuid;                                    // [out]
        D3DDDI_VIDEO_PRESENT_SOURCE_ID VidPnSourceId;   // [out]
} D3DKMT_OPENADAPTERFROMHDC;

//
// D3DKMT_ADAPTERINFO
// Used with D3DKMTEnumAdapters2 (Windows 10+).
//

typedef struct _D3DKMT_ADAPTERINFO {
        D3DKMT_HANDLE hAdapter;
        LUID    AdapterLuid;
        UINT    NumOfSources;
} D3DKMT_ADAPTERINFO;

//
// D3DKMT_ENUMADAPTERS2
// Used with D3DKMTEnumAdapters2 (Windows 10+).
//

typedef struct _D3DKMT_ENUMADAPTERS2 {
        UINT            NumAdapters;            // [in/out]
        D3DKMT_ADAPTERINFO *pAdapters;  // [out]
} D3DKMT_ENUMADAPTERS2;

//
// Function pointer types for dynamically loaded functions.
// We use dynamic loading because:
//   1. NOGDI is defined in buildcfg.h, so GDI functions aren't declared
//   2. D3DKMTOpenAdapterFromHdc might not be in the import libraries
//   3. Avoiding circular dependencies with kxdx's own dxgi redirect
//

typedef NTSTATUS (WINAPI *PFN_D3DKMTOpenAdapterFromHdc)(
        IN OUT D3DKMT_OPENADAPTERFROMHDC *);

typedef HDC (WINAPI *PFN_GetDC)(
        IN HWND hWnd);

typedef int (WINAPI *PFN_ReleaseDC)(
        IN HWND hWnd,
        IN HDC hDC);

typedef HDC (WINAPI *PFN_CreateDCW)(
        IN LPCWSTR lpDriverName,
        IN LPCWSTR lpDeviceName,
        IN LPCWSTR lpOutput,
        IN CONST VOID *lpInitData);

typedef BOOL (WINAPI *PFN_DeleteDC)(
        IN HDC hdc);

typedef BOOL (WINAPI *PFN_EnumDisplayDevicesW)(
        IN LPCWSTR lpDevice,
        IN DWORD iDevNum,
        IN OUT VOID *lpDisplayDevice,
        IN DWORD dwFlags);

//
// KxDxGetD3DKMTOpenAdapterFromHdcProc
//
// Returns a function pointer to D3DKMTOpenAdapterFromHdc from gdi32.dll.
// On Windows 7, this function lives in gdi32.dll (not dxgi.dll).
// gdi32.dll is NOT redirected by VxKex, so we can safely load it.
//
// The function pointer is cached for performance.
//

STATIC PFN_D3DKMTOpenAdapterFromHdc KxDxGetD3DKMTOpenAdapterFromHdcProc(
        VOID)
{
        STATIC volatile PFN_D3DKMTOpenAdapterFromHdc pfnCached = NULL;
        PFN_D3DKMTOpenAdapterFromHdc pfnNew;
        HMODULE hGdi32;

        pfnNew = pfnCached;
        if (pfnNew) {
                return pfnNew;
        }

        hGdi32 = GetModuleHandleW(L"gdi32.dll");
        if (!hGdi32) {
                return NULL;
        }

        pfnNew = (PFN_D3DKMTOpenAdapterFromHdc)
                GetProcAddress(hGdi32, "D3DKMTOpenAdapterFromHdc");

        if (!pfnNew) {
                KexLogWarningEvent(
                        L"D3DKMTOpenAdapterFromHdc not found in gdi32.dll");
                return NULL;
        }

        //
        // Cache the function pointer using atomic compare-exchange.
        // If another thread already cached it, we just use that.
        //

        InterlockedCompareExchangePointer(
                (PPVOID) &pfnCached, pfnNew, NULL);

        return pfnCached;
}

//
// KxDxGetDC / KxDxReleaseDC
//
// Dynamic wrappers for GetDC/ReleaseDC from user32.dll.
// Needed because NOGDI is defined in buildcfg.h.
//

STATIC HDC KxDxGetDC(
        IN      HWND    hWnd)
{
        STATIC volatile PFN_GetDC pfnCached = NULL;
        PFN_GetDC pfnNew;
        HMODULE hUser32;

        pfnNew = pfnCached;
        if (!pfnNew) {
                hUser32 = GetModuleHandleW(L"user32.dll");
                if (hUser32) {
                        pfnNew = (PFN_GetDC) GetProcAddress(hUser32, "GetDC");
                }
                if (pfnNew) {
                        InterlockedCompareExchangePointer(
                                (PPVOID) &pfnCached, pfnNew, NULL);
                }
                pfnNew = pfnCached;
        }

        return pfnNew ? pfnNew(hWnd) : NULL;
}

STATIC int KxDxReleaseDC(
        IN      HWND    hWnd,
        IN      HDC     hDC)
{
        STATIC volatile PFN_ReleaseDC pfnCached = NULL;
        PFN_ReleaseDC pfnNew;
        HMODULE hUser32;

        pfnNew = pfnCached;
        if (!pfnNew) {
                hUser32 = GetModuleHandleW(L"user32.dll");
                if (hUser32) {
                        pfnNew = (PFN_ReleaseDC) GetProcAddress(hUser32, "ReleaseDC");
                }
                if (pfnNew) {
                        InterlockedCompareExchangePointer(
                                (PPVOID) &pfnCached, pfnNew, NULL);
                }
                pfnNew = pfnCached;
        }

        return pfnNew ? pfnNew(hWnd, hDC) : 0;
}

//
// KxDxCreateDCW / KxDxDeleteDC
//
// Dynamic wrappers for CreateDCW/DeleteDC from gdi32.dll.
// Used to create a DC for a specific display device name
// (e.g. L"\\\\.\\DISPLAY1") so we can open a D3DKMT adapter
// for a specific GPU.
//

STATIC HDC KxDxCreateDCW(
        IN      LPCWSTR lpDeviceName)
{
        STATIC volatile PFN_CreateDCW pfnCached = NULL;
        PFN_CreateDCW pfnNew;
        HMODULE hGdi32;

        pfnNew = pfnCached;
        if (!pfnNew) {
                hGdi32 = GetModuleHandleW(L"gdi32.dll");
                if (hGdi32) {
                        pfnNew = (PFN_CreateDCW) GetProcAddress(hGdi32, "CreateDCW");
                }
                if (pfnNew) {
                        InterlockedCompareExchangePointer(
                                (PPVOID) &pfnCached, pfnNew, NULL);
                }
                pfnNew = pfnCached;
        }

        return pfnNew ? pfnNew(NULL, lpDeviceName, NULL, NULL) : NULL;
}

STATIC BOOL KxDxDeleteDC(
        IN      HDC     hdc)
{
        STATIC volatile PFN_DeleteDC pfnCached = NULL;
        PFN_DeleteDC pfnNew;
        HMODULE hGdi32;

        pfnNew = pfnCached;
        if (!pfnNew) {
                hGdi32 = GetModuleHandleW(L"gdi32.dll");
                if (hGdi32) {
                        pfnNew = (PFN_DeleteDC) GetProcAddress(hGdi32, "DeleteDC");
                }
                if (pfnNew) {
                        InterlockedCompareExchangePointer(
                                (PPVOID) &pfnCached, pfnNew, NULL);
                }
                pfnNew = pfnCached;
        }

        return pfnNew ? pfnNew(hdc) : FALSE;
}

//
// KxDxOpenAdapterFromHdc
//
// Helper function that calls D3DKMTOpenAdapterFromHdc via dynamic dispatch.
// Returns STATUS_NOT_SUPPORTED if the function is not available.
//

STATIC NTSTATUS KxDxOpenAdapterFromHdc(
        IN      HDC     hDc,
        OUT     D3DKMT_HANDLE *phAdapter,
        OUT     LUID    *pAdapterLuid OPTIONAL)
{
        PFN_D3DKMTOpenAdapterFromHdc pfnD3DKMTOpenAdapterFromHdc;
        D3DKMT_OPENADAPTERFROMHDC OpenAdapterFromHdc;
        NTSTATUS Status;

        ASSERT (hDc != NULL);
        ASSERT (phAdapter != NULL);

        pfnD3DKMTOpenAdapterFromHdc = KxDxGetD3DKMTOpenAdapterFromHdcProc();
        if (!pfnD3DKMTOpenAdapterFromHdc) {
                return STATUS_NOT_SUPPORTED;
        }

        RtlZeroMemory(&OpenAdapterFromHdc, sizeof(OpenAdapterFromHdc));
        OpenAdapterFromHdc.hDc = hDc;

        Status = pfnD3DKMTOpenAdapterFromHdc(&OpenAdapterFromHdc);

        if (NT_SUCCESS(Status)) {
                *phAdapter = OpenAdapterFromHdc.hAdapter;
                if (pAdapterLuid) {
                        *pAdapterLuid = OpenAdapterFromHdc.AdapterLuid;
                }
        }

        return Status;
}

//
// KxDxOpenD3DKMTAdapterForLuid
//
// Core implementation: finds the DXGI adapter matching the given LUID,
// creates a DC for its output, and opens a D3DKMT adapter handle.
//
// The approach:
//   1. Create a DXGI factory and enumerate adapters
//   2. Find the adapter whose LUID matches the requested one
//   3. Get the adapter's first output's device name
//   4. Create a DC for that device using CreateDCW
//   5. Call D3DKMTOpenAdapterFromHdc to get a kernel adapter handle
//
// If the matching adapter has no outputs (headless GPU), or if
// CreateDCW fails, we fall back to using the primary display DC.
//

STATIC NTSTATUS KxDxOpenD3DKMTAdapterForLuid(
        IN      LUID    RequestedLuid,
        OUT     D3DKMT_HANDLE *phAdapter)
{
        HRESULT hr;
        IDXGIFactory1 *pFactory = NULL;
        IDXGIAdapter1 *pAdapter = NULL;
        IDXGIOutput *pOutput = NULL;
        DXGI_ADAPTER_DESC1 Desc;
        DXGI_OUTPUT_DESC OutputDesc;
        UINT AdapterIndex;
        BOOL Found = FALSE;
        HDC hdc = NULL;
        BOOL hdcFromCreateDC = FALSE;
        NTSTATUS Status;
        D3DKMT_HANDLE hAdapter;
        LUID ReturnedLuid;

        ASSERT (phAdapter != NULL);

        //
        // Create a DXGI factory for adapter enumeration.
        //

        hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (PPVOID) &pFactory);
        if (FAILED(hr)) {
                KexLogWarningEvent(
                        L"D3DKMTOpenAdapterFromLuid: Failed to create DXGI factory, hr=0x%08lx",
                        hr);
                return STATUS_NOT_SUPPORTED;
        }

        //
        // Find the DXGI adapter matching the requested LUID.
        //

        for (AdapterIndex = 0; ; AdapterIndex++) {
                hr = pFactory->lpVtbl->EnumAdapters1(
                        pFactory, AdapterIndex, &pAdapter);

                if (hr == DXGI_ERROR_NOT_FOUND) {
                        break;
                }

                if (FAILED(hr)) {
                        continue;
                }

                hr = pAdapter->lpVtbl->GetDesc1(pAdapter, &Desc);

                if (SUCCEEDED(hr) &&
                        Desc.AdapterLuid.LowPart == RequestedLuid.LowPart &&
                        Desc.AdapterLuid.HighPart == RequestedLuid.HighPart) {

                        Found = TRUE;

                        //
                        // Try to get the device name from the adapter's first output.
                        // The device name looks like "\\.\DISPLAY1" and can be used
                        // with CreateDCW to create a DC for that specific display.
                        //

                        if (SUCCEEDED(pAdapter->lpVtbl->EnumOutputs(pAdapter, 0, &pOutput))) {
                                if (SUCCEEDED(pOutput->lpVtbl->GetDesc(pOutput, &OutputDesc))) {
                                        hdc = KxDxCreateDCW(OutputDesc.DeviceName);
                                        if (hdc) {
                                                hdcFromCreateDC = TRUE;
                                        }
                                }
                                pOutput->lpVtbl->Release(pOutput);
                                pOutput = NULL;
                        }

                        pAdapter->lpVtbl->Release(pAdapter);
                        pAdapter = NULL;
                        break;
                }

                pAdapter->lpVtbl->Release(pAdapter);
                pAdapter = NULL;
        }

        pFactory->lpVtbl->Release(pFactory);
        pFactory = NULL;

        if (!Found) {
                KexLogDebugEvent(
                        L"D3DKMTOpenAdapterFromLuid: No DXGI adapter found matching "
                        L"LUID 0x%08lx%08lx",
                        RequestedLuid.HighPart, RequestedLuid.LowPart);
                return STATUS_NOT_SUPPORTED;
        }

        //
        // If we couldn't get a DC from the matching adapter's output,
        // fall back to the primary display DC.
        //

        if (!hdc) {
                KexLogDebugEvent(
                        L"D3DKMTOpenAdapterFromLuid: Could not create DC from adapter "
                        L"output, falling back to primary display DC");

                hdc = KxDxGetDC(NULL);
                if (!hdc) {
                        return STATUS_NOT_SUPPORTED;
                }
        }

        //
        // Open the D3DKMT adapter using D3DKMTOpenAdapterFromHdc.
        //

        Status = KxDxOpenAdapterFromHdc(hdc, &hAdapter, &ReturnedLuid);

        //
        // Clean up the DC.
        //

        if (hdcFromCreateDC) {
                KxDxDeleteDC(hdc);
        } else {
                KxDxReleaseDC(NULL, hdc);
        }

        if (!NT_SUCCESS(Status)) {
                KexLogWarningEvent(
                        L"D3DKMTOpenAdapterFromLuid: D3DKMTOpenAdapterFromHdc failed, "
                        L"Status=0x%08lx",
                        Status);
                return Status;
        }

        //
        // Check if the returned adapter LUID matches the requested one.
        // On single-GPU systems this will always match. On multi-GPU
        // systems with a fallback DC, it might not.
        //

        if (ReturnedLuid.LowPart != RequestedLuid.LowPart ||
                ReturnedLuid.HighPart != RequestedLuid.HighPart) {

                KexLogWarningEvent(
                        L"D3DKMTOpenAdapterFromLuid: Opened adapter LUID "
                        L"(0x%08lx%08lx) does not match requested LUID "
                        L"(0x%08lx%08lx). This may occur on multi-GPU systems "
                        L"with a fallback DC.",
                        ReturnedLuid.HighPart, ReturnedLuid.LowPart,
                        RequestedLuid.HighPart, RequestedLuid.LowPart);

                //
                // Return the handle anyway - it's better than crashing.
                // Most overlay layers just need a valid handle to query
                // basic adapter information.
                //
        }

        *phAdapter = hAdapter;
        return STATUS_SUCCESS;
}

//////////////////////////////////////////////////////////////////////////////
//
// D3DKMTOpenAdapterFromLuid
//
// Windows 10+ API. Opens a graphics adapter identified by its LUID.
//
// On Windows 7, we implement this by:
//   1. Enumerating DXGI adapters to find the one matching the LUID
//   2. Creating a DC for the adapter's display output
//   3. Calling D3DKMTOpenAdapterFromHdc (available on Win7 in gdi32.dll)
//
// This is critical for Vulkan applications with third-party overlay layers
// (such as Mirillis Action!) that call this function and don't check
// the return value, leading to NULL pointer dereference crashes.
//
//////////////////////////////////////////////////////////////////////////////

NTSTATUS WINAPI D3DKMTOpenAdapterFromLuid(
        IN OUT  D3DKMT_OPENADAPTERFROMLUID *Arg)
{
        NTSTATUS Status;

        if (!Arg) {
                return STATUS_INVALID_PARAMETER;
        }

        KexLogDebugEvent(
                L"D3DKMTOpenAdapterFromLuid called for LUID 0x%08lx%08lx",
                Arg->AdapterLuid.HighPart, Arg->AdapterLuid.LowPart);

        Status = KxDxOpenD3DKMTAdapterForLuid(Arg->AdapterLuid, &Arg->hAdapter);

        if (NT_SUCCESS(Status)) {
                KexLogInformationEvent(
                        L"D3DKMTOpenAdapterFromLuid: Successfully opened adapter "
                        L"handle 0x%x for LUID 0x%08lx%08lx",
                        Arg->hAdapter,
                        Arg->AdapterLuid.HighPart, Arg->AdapterLuid.LowPart);
        } else {
                Arg->hAdapter = 0;
                KexLogWarningEvent(
                        L"D3DKMTOpenAdapterFromLuid: Failed to open adapter for "
                        L"LUID 0x%08lx%08lx, Status=0x%08lx",
                        Arg->AdapterLuid.HighPart, Arg->AdapterLuid.LowPart,
                        Status);
        }

        return Status;
}

//////////////////////////////////////////////////////////////////////////////
//
// D3DKMTEnumAdapters2
//
// Windows 10+ API. Enumerates all graphics adapters in the system.
//
// On Windows 7, we implement this by:
//   1. Enumerating DXGI adapters
//   2. For each adapter with an output, opening a D3DKMT adapter handle
//   3. Filling in D3DKMT_ADAPTERINFO structures
//
// If pAdapters is NULL or NumAdapters is 0, we return the count of
// adapters and STATUS_SUCCESS. The caller should then allocate the
// array and call again.
//
//////////////////////////////////////////////////////////////////////////////

NTSTATUS WINAPI D3DKMTEnumAdapters2(
        IN OUT  D3DKMT_ENUMADAPTERS2 *Arg)
{
        HRESULT hr;
        IDXGIFactory1 *pFactory = NULL;
        IDXGIAdapter1 *pAdapter = NULL;
        IDXGIOutput *pOutput = NULL;
        DXGI_ADAPTER_DESC1 Desc;
        UINT AdapterCount;
        UINT AdapterIndex;
        HDC hdc;
        BOOL hdcFromCreateDC;
        D3DKMT_HANDLE hAdapter;
        NTSTATUS Status;

        if (!Arg) {
                return STATUS_INVALID_PARAMETER;
        }

        //
        // Create a DXGI factory for adapter enumeration.
        //

        hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (PPVOID) &pFactory);
        if (FAILED(hr)) {
                KexLogWarningEvent(
                        L"D3DKMTEnumAdapters2: Failed to create DXGI factory, hr=0x%08lx",
                        hr);
                return STATUS_NOT_SUPPORTED;
        }

        //
        // Count DXGI adapters.
        //

        AdapterCount = 0;
        for (AdapterIndex = 0; ; AdapterIndex++) {
                hr = pFactory->lpVtbl->EnumAdapters1(
                        pFactory, AdapterIndex, &pAdapter);

                if (hr == DXGI_ERROR_NOT_FOUND) {
                        break;
                }

                if (SUCCEEDED(hr)) {
                        AdapterCount++;
                        pAdapter->lpVtbl->Release(pAdapter);
                }
        }

        //
        // If the caller didn't provide an array, or the array is too small,
        // return the count and let them try again.
        //

        if (!Arg->pAdapters || Arg->NumAdapters == 0) {
                Arg->NumAdapters = AdapterCount;
                pFactory->lpVtbl->Release(pFactory);
                return STATUS_SUCCESS;
        }

        //
        // Fill in the adapter info array.
        //

        if (Arg->NumAdapters < AdapterCount) {
                AdapterCount = Arg->NumAdapters;
        }

        RtlZeroMemory(Arg->pAdapters, AdapterCount * sizeof(D3DKMT_ADAPTERINFO));

        for (AdapterIndex = 0; AdapterIndex < AdapterCount; AdapterIndex++) {
                hr = pFactory->lpVtbl->EnumAdapters1(
                        pFactory, AdapterIndex, &pAdapter);

                if (hr == DXGI_ERROR_NOT_FOUND) {
                        break;
                }

                if (FAILED(hr)) {
                        continue;
                }

                hr = pAdapter->lpVtbl->GetDesc1(pAdapter, &Desc);
                if (FAILED(hr)) {
                        pAdapter->lpVtbl->Release(pAdapter);
                        continue;
                }

                //
                // Do NOT use Desc.AdapterLuid here - the DXGI LUID and the
                // WDDM kernel LUID can differ on Windows 7. We set AdapterLuid
                // below from the value returned by D3DKMTOpenAdapterFromHdc.
                //

                //
                // Try to open a D3DKMT adapter handle.
                // We need an HDC for D3DKMTOpenAdapterFromHdc.
                // First try to get a DC from the adapter's output.
                //

                hdc = NULL;
                hdcFromCreateDC = FALSE;

                if (pAdapter->lpVtbl->EnumOutputs(pAdapter, 0, &pOutput) == S_OK) {
                        DXGI_OUTPUT_DESC OutputDesc;

                        if (SUCCEEDED(pOutput->lpVtbl->GetDesc(pOutput, &OutputDesc))) {
                                hdc = KxDxCreateDCW(OutputDesc.DeviceName);
                                if (hdc) {
                                        hdcFromCreateDC = TRUE;
                                }
                        }
                        pOutput->lpVtbl->Release(pOutput);
                }

                //
                // Fallback to primary display DC.
                //

                if (!hdc) {
                        hdc = KxDxGetDC(NULL);
                }

                if (hdc) {
                        LUID ReturnedLuid;

                        Status = KxDxOpenAdapterFromHdc(
                                hdc, &hAdapter, &ReturnedLuid);

                        if (hdcFromCreateDC) {
                                KxDxDeleteDC(hdc);
                        } else {
                                KxDxReleaseDC(NULL, hdc);
                        }

                        if (NT_SUCCESS(Status)) {
                                Arg->pAdapters[AdapterIndex].hAdapter = hAdapter;
                                Arg->pAdapters[AdapterIndex].AdapterLuid = ReturnedLuid;
                                Arg->pAdapters[AdapterIndex].NumOfSources = 1;
                                KexLogDebugEvent(
                                        L"D3DKMTEnumAdapters2: Adapter[%u] hAdapter=0x%lx "
                                        L"LUID=0x%08lx%08lx",
                                        AdapterIndex, hAdapter,
                                        ReturnedLuid.HighPart, ReturnedLuid.LowPart);
                        } else {
                                KexLogWarningEvent(
                                        L"D3DKMTEnumAdapters2: KxDxOpenAdapterFromHdc failed "
                                        L"for adapter[%u], Status=0x%08lx. "
                                        L"hdc=%p hdcFromCreateDC=%d",
                                        AdapterIndex, Status, hdc, (INT)hdcFromCreateDC);
                        }
                } else {
                        //
                        // Could not obtain any DC. Leave hAdapter as 0.
                        //
                }

                pAdapter->lpVtbl->Release(pAdapter);
        }

        Arg->NumAdapters = AdapterCount;
        pFactory->lpVtbl->Release(pFactory);

        return STATUS_SUCCESS;
}

//////////////////////////////////////////////////////////////////////////////
//
// D3DKMT stub functions
//
// The following D3DKMT functions are Windows 10+ APIs that are imported by
// the NVIDIA Vulkan driver (nvoglv64.dll) and other graphics drivers.
// These functions don't exist on Windows 7 and are not easily implementable
// without a WDDM 2.0+ driver model.
//
// However, the callers (Vulkan ICDs, overlay layers) expect these functions
// to exist and handle failure gracefully by checking return values. The
// problem occurs when the function resolution itself fails (returning
// STATUS_ENTRYPOINT_NOT_FOUND), which causes NULL pointer dereferences
// in overlay layers like Mirillis Action! that don't check for NULL
// before calling through function pointers.
//
// By providing these stub implementations that return STATUS_NOT_SUPPORTED,
// we ensure that:
//   1. The function pointers are NOT NULL (preventing crashes)
//   2. The callers receive a proper NTSTATUS error code they can handle
//   3. The Vulkan driver can gracefully fall back to alternative code paths
//
//////////////////////////////////////////////////////////////////////////////

NTSTATUS WINAPI D3DKMTSetVidPnSourceOwner1(
        IN      PVOID   Arg)
{
        KexLogDebugEvent(L"D3DKMTSetVidPnSourceOwner1 stub called");
        return STATUS_NOT_SUPPORTED;
}

NTSTATUS WINAPI D3DKMTSetContextInProcessSchedulingPriority(
        IN      PVOID   Arg)
{
        KexLogDebugEvent(L"D3DKMTSetContextInProcessSchedulingPriority stub called");
        return STATUS_NOT_SUPPORTED;
}

NTSTATUS WINAPI D3DKMTChangeSurfacePointer(
        IN      PVOID   Arg)
{
        KexLogDebugEvent(L"D3DKMTChangeSurfacePointer stub called");
        return STATUS_NOT_SUPPORTED;
}

NTSTATUS WINAPI D3DKMTCheckExclusiveOwnerShip(
        IN      PVOID   Arg)
{
        KexLogDebugEvent(L"D3DKMTCheckExclusiveOwnerShip stub called");
        return STATUS_NOT_SUPPORTED;
}

NTSTATUS WINAPI D3DKMTCreateKeyedMutex2(
        IN      PVOID   Arg)
{
        KexLogDebugEvent(L"D3DKMTCreateKeyedMutex2 stub called");
        return STATUS_NOT_SUPPORTED;
}

NTSTATUS WINAPI D3DKMTOpenKeyedMutex2(
        IN      PVOID   Arg)
{
        KexLogDebugEvent(L"D3DKMTOpenKeyedMutex2 stub called");
        return STATUS_NOT_SUPPORTED;
}

NTSTATUS WINAPI D3DKMTAcquireKeyedMutex2(
        IN      PVOID   Arg)
{
        KexLogDebugEvent(L"D3DKMTAcquireKeyedMutex2 stub called");
        return STATUS_NOT_SUPPORTED;
}

NTSTATUS WINAPI D3DKMTReleaseKeyedMutex2(
        IN      PVOID   Arg)
{
        KexLogDebugEvent(L"D3DKMTReleaseKeyedMutex2 stub called");
        return STATUS_NOT_SUPPORTED;
}

NTSTATUS WINAPI D3DKMTQueryResourceInfoFromNtHandle(
        IN      PVOID   Arg)
{
        KexLogDebugEvent(L"D3DKMTQueryResourceInfoFromNtHandle stub called");
        return STATUS_NOT_SUPPORTED;
}

NTSTATUS WINAPI D3DKMTShareObjects(
        IN      PVOID   Arg)
{
        KexLogDebugEvent(L"D3DKMTShareObjects stub called");
        return STATUS_NOT_SUPPORTED;
}

NTSTATUS WINAPI D3DKMTOpenNtHandleFromName(
        IN      PVOID   Arg)
{
        KexLogDebugEvent(L"D3DKMTOpenNtHandleFromName stub called");
        return STATUS_NOT_SUPPORTED;
}

NTSTATUS WINAPI D3DKMTOpenResourceFromNtHandle(
        IN      PVOID   Arg)
{
        KexLogDebugEvent(L"D3DKMTOpenResourceFromNtHandle stub called");
        return STATUS_NOT_SUPPORTED;
}

NTSTATUS WINAPI D3DKMTOpenSyncObjectFromNtHandle(
        IN      PVOID   Arg)
{
        KexLogDebugEvent(L"D3DKMTOpenSyncObjectFromNtHandle stub called");
        return STATUS_NOT_SUPPORTED;
}
