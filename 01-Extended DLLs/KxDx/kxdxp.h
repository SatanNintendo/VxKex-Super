#pragma once

#include <KexComm.h>
#include <KexDll.h>
#include <KxDx.h>

EXTERN PKEX_PROCESS_DATA KexData;

typedef interface IMFMediaBuffer IMFMediaBuffer;
typedef interface IMFDXGIDeviceManager IMFDXGIDeviceManager;

//
// d3dkmt.c
// D3DKMTOpenAdapterFromLuid and D3DKMTEnumAdapters2 are exported directly
// via kxdx.def. Their prototypes are not needed here because the D3DKMT
// structure types are defined locally in d3dkmt.c.
//

//
// dcmpstub.c
//

HRESULT WINAPI CreateIDXGIFactoryMedia(
        OUT     PPVOID  FactoryMedia);

//
// direct2d.c
//

VOID WrapDirect2DDevice(
        IN OUT  ID2D1Device *Device);

VOID WrapDirect2DFactory(
        IN OUT  ID2D1Factory1 *Factory);

//
// dxgi.c
//

VOID WrapDXGIFactory(
        IN OUT  IDXGIFactory2 *Factory);

//
// rptnoint.c
//

VOID ReportNoInterfaceError(
        IN      REFIID  RefIID,
        IN      HRESULT Result,
        IN      PCWSTR  ErrorMessage);