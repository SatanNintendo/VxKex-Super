///////////////////////////////////////////////////////////////////////////////
//
// Module Name:
//
//     dwm.c
//
// Abstract:
//
//     Stubs for Desktop Window Manager (DWM) functions that are not
//     available on Windows 7 but are queried by some applications.
//
// Author:
//
//     VxKex Patch
//
///////////////////////////////////////////////////////////////////////////////

#include "buildcfg.h"
#include "kxmip.h"

//
// DwmIsNonClientRenderingEnabled
//
// Windows 8+ DWM function. Some applications query this through GetProcAddress.
// On Windows 7, DWM non-client rendering is always enabled, so we return
// S_OK with pfEnabled = TRUE.
//

HRESULT WINAPI DwmIsNonClientRenderingEnabled(
		IN	BOOL	*pfEnabled)
{
	if (!pfEnabled) {
		return E_INVALIDARG;
	}

	*pfEnabled = TRUE;
	return S_OK;
}
