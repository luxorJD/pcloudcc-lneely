/*
  Copyright (c) 2013-2015 pCloud Ltd.  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met: Redistributions of source code must retain the above
  copyright notice, this list of conditions and the following
  disclaimer.  Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the following
  disclaimer in the documentation and/or other materials provided with
  the distribution.  Neither the name of pCloud Ltd nor the names of
  its contributors may be used to endorse or promote products derived
  from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL pCloud
  Ltd BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
  OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
  USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
  DAMAGE.
*/

#include "pcompat.h"
#include "plibs.h"
#include "psynclib.h"
#include "pdevice_monitor.h"

#define P_DEVICE_VERBOSE

#ifdef P_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT   0x0601

#include <windows.h>
#include <dbt.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <strsafe.h>
#include <shlwapi.h>
#include <winioctl.h>


#define CLS_NAME "DUMMY_CLASS"
#define HWND_MESSAGE     ((HWND)-3)
#define WM_USER_MEDIACHANGED WM_USER+88

#define MAX_LOADSTRING 100
static device_event_callback *callbacks;
static int clbsize = 10;
static int clbnum = 0;

typedef struct {
  DWORD dwItem1;    // dwItem1 contains the previous PIDL or name of the folder. 
  DWORD dwItem2;    // dwItem2 contains the new PIDL or name of the folder. 
} SHNOTIFYSTRUCT;

void 
padd_monitor_callback(device_event_callback callback) {
  if (callback) {
	if (clbnum == 0) {
	  callbacks = (device_event_callback *)psync_malloc(sizeof(device_event_callback)*clbsize);
	} else {
	  while (clbnum > clbsize) {
		device_event_callback *callbacks_old = callbacks;
		callbacks = (device_event_callback *)psync_malloc(sizeof(device_event_callback)*clbsize*2);
		memccpy(callbacks, callbacks_old, 0,sizeof(device_event_callback)*clbsize);
		clbsize = clbsize * 2;
		psync_free(callbacks_old);
	  }
	}
	callbacks[clbnum] = callback;
	clbnum++;
  }
}

static pdevice_info* 
new_dev_info(char *szPath, pdevice_types type,
			 device_event evt) {
  pdevice_info *infop;
  
  /*int pathsize = strlen(szPath);
  int infstrsize = sizeof(pdevice_info);
  int infsize = pathsize + infstrsize + 1;*/
  // pdevice_info *infop = (pdevice_info *)psync_malloc(infsize);
  //ZeroMemory(infop, infsize);
  infop = (pdevice_info *)psync_malloc(sizeof(pdevice_info));
  infop->filesystem_path = _strdup(szPath);
  //infop->filesystem_path = (char *)(infop) + infstrsize;
  //memcpy(infop->filesystem_path, szPath, pathsize);
  //infop->filesystem_path[pathsize] = '\0';
  infop->event = evt;
  infop->type = type;
  infop->isextended = 0;
  infop->size = sizeof(pdevice_info);
  infop->me = infop;
  return infop;
}

static void
put_into_storage(char **prop, char **dst,
				 char* src, uint32_t size) {
  *prop = *dst;
  memcpy(*dst, src, size);
  char * end = *dst;
  end[size] = '\0';
  (*dst) += (size+1);
}

static pdevice_extended_info * 
new_dev_ext_info(char *szPath, char * vendor, char *product, char* deviceid, 
				 pdevice_types type, device_event evt) {
  /*uint32_t pathsize = strlen(szPath);
	uint32_t vndsize = strlen(vendor);
	uint32_t prdsize = strlen(product);
	uint32_t devsize = strlen(deviceid);
	uint32_t infstrsize = sizeof(pdevice_extended_info);
	uint32_t infsize = pathsize + infstrsize + pathsize + vndsize + prdsize + 5;
	void * infovp = psync_malloc(infsize);
	pdevice_extended_info *infop = (pdevice_extended_info *)infovp;
	ZeroMemory(infop, infsize);
	char *storage_begin = (char *)(infovp)+infstrsize;
	put_into_storage(&infop->filesystem_path, &storage_begin, szPath, pathsize);
	put_into_storage(&infop->vendor, &storage_begin, vendor, vndsize);
	put_into_storage(&infop->product, &storage_begin, product, prdsize);
	put_into_storage(&infop->device_id, &storage_begin, deviceid, devsize);
	infop->type = type;
	infop->event = evt;
	infop->isextended = 1;
	infop->size = infsize;
	infop->me = infop;*/
  pdevice_extended_info *infop = 
	(pdevice_extended_info *)psync_malloc(sizeof(pdevice_extended_info));
  infop->filesystem_path = _strdup(szPath);
  infop->vendor = _strdup(vendor);
  infop->product = _strdup(product);
  infop->device_id = _strdup(deviceid);
  infop->type = type;
  infop->event = evt;
  infop->isextended = 1;
  infop->size = sizeof(pdevice_extended_info);
  infop->me = infop;
  return infop;
}

static void
notify_callbacks_free_run(void * param) {
  int i = 0; 
  while (i < clbnum) {
    device_event_callback c = callbacks[i];
    c(param);
    i++;
  }
  pdevice_info *p = (pdevice_info*)param;
  if (p->isextended) {
    pdevice_extended_info *e = (pdevice_extended_info*)param;
    psync_free(e->device_id);
    psync_free(e->filesystem_path);
    psync_free(e->product);
    psync_free(e->vendor);
    psync_free(e);
    return;
  }
  psync_free(p->filesystem_path);
  psync_free(p);
}

static void 
notify_callbacks_free(void * param) {
  psync_run_thread1("Device notifications", notify_callbacks_free_run, param);
}

static void
pdevice_types dev_decode_type(STORAGE_BUS_TYPE bustype, DWORD drivetype) {
  switch (bustype) {
  case BusTypeScsi: //fallthrough
  case BusTypeiScsi://fallthrough
  case BusTypeUsb:  //fallthrough
  case BusTypeSata: //returns
    if (drivetype == DRIVE_REMOVABLE) {
      return Dev_Types_UsbRemovableDisk;
	} else {
      return Dev_Types_UsbFixedDisk;
	}
  case BusTypeSd:  // returns
    return Dev_Types_AndroidDevice;
  case BusTypeMmc: // returns
    return Dev_Types_CameraDevice;
  }
}

static DWORD 
GetPhysicalDriveParams(char *strdrivepath IN, DWORD drivetype, 
					   char *fspath, void **deviceinfo OUT) {
  DWORD dwRet;
  HANDLE hDevice;
  STORAGE_PROPERTY_QUERY storagePropertyQuery;
  STORAGE_DESCRIPTOR_HEADER storageDescriptorHeader;
  DWORD dwBytesReturned;
  const DWORD dwOutBufferSize;
  BYTE* pOutBuffer;
  STORAGE_DEVICE_DESCRIPTOR* pDeviceDescriptor;

  dwRet = NO_ERROR;

  hDevice = CreateFileA(strdrivepath, 0,
						FILE_SHARE_READ | FILE_SHARE_WRITE,
						NULL, OPEN_EXISTING, 0, NULL);

  if (INVALID_HANDLE_VALUE == hDevice)
    return GetLastError();

  ZeroMemory(&storagePropertyQuery, sizeof(STORAGE_PROPERTY_QUERY));
  storagePropertyQuery.PropertyId = StorageDeviceProperty;
  storagePropertyQuery.QueryType = PropertyStandardQuery;

  storageDescriptorHeader = { 0 };
  dwBytesReturned = 0;
  if (!DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
					   &storagePropertyQuery, sizeof(STORAGE_PROPERTY_QUERY),
					   &storageDescriptorHeader, sizeof(STORAGE_DESCRIPTOR_HEADER),
					   &dwBytesReturned, NULL)) {
    dwRet = GetLastError();
    CloseHandle(hDevice);
    return dwRet;
  }

  // Alloc the output buffer
  dwOutBufferSize = storageDescriptorHeader.Size;
  pOutBuffer = (BYTE*)psync_malloc(dwOutBufferSize);
  ZeroMemory(pOutBuffer, dwOutBufferSize);

  // Get the storage device descriptor
  if (!DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
					   &storagePropertyQuery, sizeof(STORAGE_PROPERTY_QUERY),
					   pOutBuffer, dwOutBufferSize,
					   &dwBytesReturned, NULL)) {
    dwRet = GetLastError();
    free(pOutBuffer);
    CloseHandle(hDevice);
    return dwRet;
  }

  pDeviceDescriptor = (STORAGE_DEVICE_DESCRIPTOR*)pOutBuffer;
  *deviceinfo =
	new_dev_ext_info(fspath,
					 (char *)(pOutBuffer + pDeviceDescriptor->VendorIdOffset),
					 (char *)(pOutBuffer + pDeviceDescriptor->ProductIdOffset),
					 (char *)(pOutBuffer + pDeviceDescriptor->SerialNumberOffset),
					 dev_decode_type(pDeviceDescriptor->BusType, drivetype),
					 Dev_Event_arrival);

  free (pOutBuffer);
  CloseHandle(hDevice);
  return dwRet;
}

static LRESULT 
message_handler(HWND *hwnd, UINT uint, WPARAM wparam, LPARAM lparam) {
  switch (uint) {
  case WM_NCCREATE:
	return 1;
	break;
  case WM_CREATE:
	return 0;
	break;
  case WM_DEVICECHANGE:
	return 0;
	break;
  case WM_USER_MEDIACHANGED:
	SHNOTIFYSTRUCT *shns;
	char szPath[MAX_PATH];
	pdevice_info *p;
	
	shns == (SHNOTIFYSTRUCT *)wparam;
	ZeroMemory(&szPath, MAX_PATH);

	switch (lparam) {
	case SHCNE_MEDIAINSERTED:        // media inserted event
	  SHGetPathFromIDListA((struct _ITEMIDLIST *)shns->dwItem1, szPath);
	  p = new_dev_info(szPath, Dev_Types_CDRomMedia, Dev_Event_arrival);
	  notify_callbacks_free(p);
	  break;
	case SHCNE_MEDIAREMOVED:        // media removed event
	  SHGetPathFromIDListA((struct _ITEMIDLIST *)shns->dwItem1, szPath);
	  p = new_dev_info(szPath, Dev_Types_CDRomMedia, Dev_Event_removed);
	  notify_callbacks_free(p);
	  break;
	case SHCNE_DRIVEADD:        // media removed event
	  DWORD	drivetype;
	  HANDLE hDevice;
	  PSTORAGE_DEVICE_DESCRIPTOR pDevDesc;
	  pdevice_extended_info *deviceinfo;
	  char szRootPath[];
	  char szDevicePath[];
	  char szVolumeAccessPath[];

	  deviceinfo = NULL;
	  SHGetPathFromIDListA((struct _ITEMIDLIST *)shns->dwItem1,
						   szPath);

	  // "X:\"    -> for GetDriveType
	  szRootPath = "X:\\";
	  szRootPath[0] = szPath[0];
	  
	  // "X:"     -> for QueryDosDevice
	  szDevicePath = "X:";
	  szDevicePath[0] = szPath[0];

	  // "\\.\X:" -> to open the volume
	  szVolumeAccessPath = "\\\\.\\X:";
	  szVolumeAccessPath[4] = szPath[0];
	  
	  drivetype = GetDriveTypeA(szRootPath);
	  switch (drivetype) {
	  case 0:					// The drive type cannot be determined.
		debug(D_NOTICE, "The drive type cannot be determined!");
		break;
	  case 1:					// The root directory does not exist.
		debug(D_NOTICE, "The root directory does not exist!");
		break;
	  case DRIVE_CDROM:		// The drive is a CD-ROM drive.
		debug(D_NOTICE, "The drive is a CD-ROM drive.");
	  case DRIVE_REMOVABLE:	// The drive can be removed from the drive.
	  case DRIVE_FIXED:		// The disk cannot be removed from the drive.
	  case DRIVE_REMOTE:		// The drive is a remote (network) drive.
		GetPhysicalDriveParams(szVolumeAccessPath, drivetype, 
							   szDevicePath, &deviceinfo);
		if (deviceinfo) {
		  notify_callbacks_free(deviceinfo);
		} else {
		  notify_callbacks_free(new_dev_info(szPath,
											 dev_decode_type(BusTypeUsb, drivetype),
											 Dev_Event_arrival));
		}
		break;
	  case DRIVE_RAMDISK:		// The drive is a RAM disk.
		break;
	  }
	  break;
	case SHCNE_DRIVEREMOVED:        // media removed event
	  SHGetPathFromIDListA((struct _ITEMIDLIST *)shns->dwItem1, szPath);
	  p = new_dev_info(szPath, Dev_Types_UsbFixedDisk, Dev_Event_removed);
	  notify_callbacks_free(p);
	  break;
	}
	break;
  }
  return 0;

}

static void 
device_change(void *param) {
  pdevice_info * pd = (pdevice_info*)param;
  debug(D_NOTICE, "type [%d] event [%d] size [%d] isex [%d] fspath [%s] \n", 
		pd->type, pd->event, pd->size, pd->isextended, pd->filesystem_path);

  if (pd->isextended) {
    pdevice_extended_info* pde = (pdevice_extended_info*)param;
    debug(D_NOTICE, "vendor [%s] product [%s] deviceid [%s] \n", 
		  pde->vendor, pde->product, pde->device_id);
  }

}

void 
pinit_device_monitor() {
  HWND hWnd = NULL;
  WNDCLASSEXA wx;
  MSG msg;
  ULONG m_ulSHChangeNotifyRegister;
  LPITEMIDLIST ppidl;

#ifdef P_DEVICE_VERBOSE
  padd_monitor_callback(device_change);
#endif

  ZeroMemory(&wx, sizeof(wx));

  wx.cbSize = sizeof(WNDCLASSEXA);
  wx.lpfnWndProc = (WNDPROC) (message_handler);
  wx.hInstance = (HINSTANCE) (GetModuleHandleA(0));
  wx.style = CS_HREDRAW | CS_VREDRAW;
  //wx.hInstance = GetModuleHandle(0);
  wx.hbrBackground = (HBRUSH)(COLOR_WINDOW);
  wx.lpszClassName = CLS_NAME;

  if (RegisterClassExA(&wx)) {
	hWnd = CreateWindowA(CLS_NAME, L"DevNotifWnd", WS_ICONIC,
						 0, 0, CW_USEDEFAULT, 0, HWND_MESSAGE,
						 NULL, GetModuleHandleA(0), NULL);//(void*)&guid);
  }

  if (hWnd == NULL) {
    debug(D_NOTICE, "Could not create message window! %d", GetLastError());
	return 1;
  }

  if (SHGetSpecialFolderLocation(hWnd, CSIDL_DESKTOP, &ppidl) == NOERROR) {
	SHChangeNotifyEntry shCNE;

	shCNE.pidl = ppidl;
	shCNE.fRecursive = TRUE;
	m_ulSHChangeNotifyRegister =
	  SHChangeNotifyRegister(hWnd, SHCNE_DISKEVENTS,
							 SHCNE_MEDIAINSERTED | SHCNE_MEDIAREMOVED
							 | SHCNE_DRIVEREMOVED | SHCNE_DRIVEADD,
							 WM_USER_MEDIACHANGED, 1, &shCNE); 

	if (m_ulSHChangeNotifyRegister == 0) {
	  debug(D_NOTICE, 
			"Shell Device Notify registration CD failed with error %d", 
			GetLastError());
	  return 2;
	}
  } else {
    debug(D_NOTICE, 
		  "Shell Device Notify registration CD failed with error %d ", 
		  GetLastError());
  }

  debug(D_NOTICE, "waiting for new devices..");

  while (GetMessage(&msg, NULL, 0, 0)) {
	TranslateMessage(&msg);
	DispatchMessage(&msg);
  }
  return 0;
}

#else

void 
padd_monitor_callback(device_event_callback callback) { /*noop*/ }

void 
pinit_device_monitor() { /*noop*/ }

#endif //P_OS_WINDOWS
