/*
 * Library for WinUSB/libusb automated driver installation
 * Copyright (c) 2010 Pete Batard <pbatard@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */
#include <windows.h>
#include <setupapi.h>
#include <io.h>
#include <stdio.h>
#include <inttypes.h>
#include <objbase.h>
#include <shellapi.h>
#if defined(__CYGWIN__)
#include <unistd.h>
#endif

#include "installer_library.h"
#include "usbi.h"
#include "infs.h"
#include "resource.h"	// auto-generated during compilation

#define INF_NAME "libusb-device.inf"


/*
 * Global variables
 */
char* req_device_id;
bool dlls_available = false;
HANDLE pipe_handle = INVALID_HANDLE_VALUE;
// for 64 bit platforms detection
static BOOL (__stdcall *pIsWow64Process)(HANDLE, PBOOL) = NULL;

/*
 * For the retrieval of the device description on Windows 7
 */
#ifndef DEVPROPKEY_DEFINED
typedef struct {
    GUID  fmtid;
    ULONG pid;
} DEVPROPKEY;
#endif

const DEVPROPKEY DEVPKEY_Device_BusReportedDeviceDesc = {
	{ 0x540b947e, 0x8b40, 0x45bc, {0xa8, 0xa2, 0x6a, 0x0b, 0x89, 0x4c, 0xbd, 0xa2} }, 4 };

/*
 * Cfgmgr32.dll, SetupAPI.dll interface
 */
DLL_DECLARE(WINAPI, CONFIGRET, CM_Get_Parent, (PDEVINST, DEVINST, ULONG));
DLL_DECLARE(WINAPI, CONFIGRET, CM_Get_Child, (PDEVINST, DEVINST, ULONG));
DLL_DECLARE(WINAPI, CONFIGRET, CM_Get_Sibling, (PDEVINST, DEVINST, ULONG));
DLL_DECLARE(WINAPI, CONFIGRET, CM_Get_Device_IDA, (DEVINST, PCHAR, ULONG, ULONG));
DLL_DECLARE(WINAPI, BOOL, SetupDiGetDeviceProperty, (HDEVINFO, PSP_DEVINFO_DATA, const DEVPROPKEY*, ULONG*, PBYTE, DWORD, PDWORD, DWORD));


// convert a GUID to an hex GUID string
char* guid_to_string(const GUID guid)
{
	static char guid_string[MAX_GUID_STRING_LENGTH];

	sprintf(guid_string, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
		(unsigned int)guid.Data1, guid.Data2, guid.Data3,
		guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
		guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
	return guid_string;
}

// free a driver info struct
void free_di(struct driver_info *start)
{
	struct driver_info *tmp;
	while(start != NULL) {
		tmp = start;
		start = start->next;
		free(tmp);
	}
}

// Setup the Cfgmgr32 and SetupApi DLLs
static int init_dlls(void)
{
	DLL_LOAD(Cfgmgr32.dll, CM_Get_Parent, TRUE);
	DLL_LOAD(Cfgmgr32.dll, CM_Get_Child, TRUE);
	DLL_LOAD(Cfgmgr32.dll, CM_Get_Sibling, TRUE);
	DLL_LOAD(Cfgmgr32.dll, CM_Get_Device_IDA, TRUE);
	DLL_LOAD(Setupapi.dll, SetupDiGetDeviceProperty, FALSE);
	return 0;
}

// List all driverless USB devices
struct driver_info* list_driverless(void)
{
	unsigned i, j;
	DWORD size, reg_type;
	ULONG devprop_type;
	OSVERSIONINFO os_version;
	CONFIGRET r;
	HDEVINFO dev_info;
	SP_DEVINFO_DATA dev_info_data;
	char *prefix[3] = {"VID_", "PID_", "MI_"};
	char *token;
	char path[MAX_PATH_LENGTH];
	WCHAR desc[MAX_DESC_LENGTH];
	char driver[MAX_DESC_LENGTH];
	struct driver_info *ret = NULL, *cur = NULL, *drv_info;
	bool driverless;

	if (!dlls_available) {
		init_dlls();
	}

	// List all connected USB devices
	dev_info = SetupDiGetClassDevs(NULL, "USB", NULL, DIGCF_PRESENT|DIGCF_ALLCLASSES);
	if (dev_info == INVALID_HANDLE_VALUE) {
		return NULL;
	}

	// Find the ones that are driverless
	for (i = 0; ; i++)
	{
		driverless = false;

		dev_info_data.cbSize = sizeof(dev_info_data);
		if (!SetupDiEnumDeviceInfo(dev_info, i, &dev_info_data)) {
			break;
		}

		// SPDRP_DRIVER seems to do a better job at detecting driverless devices than
		// SPDRP_INSTALL_STATE
		if (SetupDiGetDeviceRegistryProperty(dev_info, &dev_info_data, SPDRP_DRIVER,
			&reg_type, (BYTE*)driver, MAX_KEY_LENGTH, &size)) {
			// Driverless devices should return an error
			// TODO: should we also return devices that have a driver?
			continue;
		}
//		usbi_dbg("driver: %s", driver);

		// Allocate a driver_info struct to store our data
		drv_info = calloc(1, sizeof(struct driver_info));
		if (drv_info == NULL) {
			free_di(ret);
			return NULL;
		}
		if (cur == NULL) {
			ret = drv_info;
		} else {
			cur->next = drv_info;
		}
		cur = drv_info;

		// Retrieve device ID. This is needed to re-enumerate our device and force
		// the final driver installation
		r = CM_Get_Device_IDA(dev_info_data.DevInst, path, MAX_PATH_LENGTH, 0);
		if (r != CR_SUCCESS) {
			usbi_err(NULL, "could not retrieve simple path for device %d: CR error %d",
				i, r);
			continue;
		} else {
			usbi_dbg("Driverless USB device (%d): %s", i, path);
		}
		drv_info->device_id = _strdup(path);

		// Retreive the device description as reported by the device itself
		memset(&os_version, 0, sizeof(OSVERSIONINFO));
		os_version.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
		if ( (GetVersionEx(&os_version) != 0)
		  && (os_version.dwBuildNumber < 7000) ) {
			// On Vista and earlier, we can use SPDRP_DEVICEDESC
			if (!SetupDiGetDeviceRegistryPropertyW(dev_info, &dev_info_data, SPDRP_DEVICEDESC,
				&reg_type, (BYTE*)desc, 2*MAX_DESC_LENGTH, &size)) {
				usbi_warn(NULL, "could not read device description for %d: %s",
					i, windows_error_str(0));
				desc[0] = 0;
			}
		} else {
			// On Windows 7, the information we want ("Bus reported device description") is
			// accessed through DEVPKEY_Device_BusReportedDeviceDesc
			if (SetupDiGetDeviceProperty == NULL) {
				usbi_warn(NULL, "failed to locate SetupDiGetDeviceProperty() is Setupapi.dll");
				desc[0] = 0;
			} else if (!SetupDiGetDeviceProperty(dev_info, &dev_info_data, &DEVPKEY_Device_BusReportedDeviceDesc,
				&devprop_type, (BYTE*)desc, 2*MAX_DESC_LENGTH, &size, 0)) {
				usbi_warn(NULL, "could not read device description for %d (Win7): %s",
					i, windows_error_str(0));
				desc[0] = 0;
			}
		}
		drv_info->desc = wchar_to_utf8(desc);
		usbi_dbg("Device description: %s", drv_info->desc);

		token = strtok (path, "\\#&");
		while(token != NULL) {
			for (j = 0; j < 3; j++) {
				if (safe_strncmp(token, prefix[j], strlen(prefix[j])) == 0) {
					switch(j) {
					case 0:
						safe_strcpy(drv_info->vid, sizeof(drv_info->vid), token);
						break;
					case 1:
						safe_strcpy(drv_info->pid, sizeof(drv_info->pid), token);
						break;
					case 2:
						safe_strcpy(drv_info->mi, sizeof(drv_info->mi), token);
						break;
					default:
						usbi_err(NULL, "unexpected case");
						break;
					}
				}
			}
			token = strtok (NULL, "\\#&");
		}
	}

	return ret;
}

// extract the embedded binary resources
int extract_binaries(char* path)
{
	char filename[MAX_PATH_LENGTH];
	FILE* fd;
	int i;

	for (i=0; i<nb_resources; i++) {
		safe_strcpy(filename, MAX_PATH_LENGTH, path);
		safe_strcat(filename, MAX_PATH_LENGTH, "\\");
		safe_strcat(filename, MAX_PATH_LENGTH, resource[i].subdir);

		if ( (_access(filename, 02) != 0) && (CreateDirectory(filename, 0) == 0) ) {
			usbi_err(NULL, "could not access directory: %s", filename);
			return -1;
		}
		safe_strcat(filename, MAX_PATH_LENGTH, "\\");
		safe_strcat(filename, MAX_PATH_LENGTH, resource[i].name);


		fd = fopen(filename, "wb");
		if (fd == NULL) {
			usbi_err(NULL, "failed to create file: %s", filename);
			return -1;
		}

		fwrite(resource[i].data, resource[i].size, 1, fd);
		fclose(fd);
	}

	usbi_dbg("successfully extracted files to %s", path);
	return 0;
}

// Create an inf and extract coinstallers in the directory pointed by path
// TODO: optional directory deletion
int create_inf(struct driver_info* drv_info, char* path, int type)
{
	char filename[MAX_PATH_LENGTH];
	FILE* fd;
	GUID guid;

	// TODO? create a reusable temp dir if path is NULL?
	if ((path == NULL) || (drv_info == NULL)) {
		return -1;
	}

	if ((type < USE_WINUSB) && (type > USE_LIBUSB)) {
		return -1;
	}

	// Try to create directory if it doesn't exist
	if ( (_access(path, 02) != 0) && (CreateDirectory(path, 0) == 0) ) {
		usbi_err(NULL, "could not access directory: %s", path);
		return -1;
	}

	extract_binaries(path);

	safe_strcpy(filename, MAX_PATH_LENGTH, path);
	safe_strcat(filename, MAX_PATH_LENGTH, "\\");
	safe_strcat(filename, MAX_PATH_LENGTH, INF_NAME);

	fd = fopen(filename, "w");
	if (fd == NULL) {
		usbi_err(NULL, "failed to create file: %s", filename);
		return -1;
	}

	fprintf(fd, "; libusb_device.inf\n");
	fprintf(fd, "; Copyright (c) 2010 libusb (GNU LGPL)\n");
	fprintf(fd, "[Strings]\n");
	fprintf(fd, "DeviceName = \"%s\"\n", drv_info->desc);
	fprintf(fd, "DeviceID = \"%s&%s", drv_info->vid, drv_info->pid);
	if (drv_info->mi[0] != 0) {
		fprintf(fd, "&%s\"\n", drv_info->mi);
	} else {
		fprintf(fd, "\"\n");
	}
	CoCreateGuid(&guid);
	fprintf(fd, "DeviceGUID = \"%s\"\n", guid_to_string(guid));
	fwrite(inf[type], strlen(inf[type]), 1, fd);
	fclose(fd);

	usbi_dbg("succesfully created %s", filename);
	return 0;
}

// Handle messages received from the elevated installer through the pipe
int process_message(char* buffer, DWORD size)
{
	DWORD junk;

	if (size <= 0)
		return -1;

	switch(buffer[0])
	{
	case IC_GET_DEVICE_ID:
		usbi_dbg("got request for device_id");
		WriteFile(pipe_handle, req_device_id, strlen(req_device_id), &junk, NULL);
		break;
	case IC_PRINT_MESSAGE:
		if (size < 2) {
			usbi_err(NULL, "print_message: no data");
			return -1;
		}
		usbi_dbg("[installer process] %s", buffer+1);
		break;
	default:
		usbi_err(NULL, "unrecognized installer message");
		return -1;
	}
	return 0;
}

// Run the elevated installer
int run_installer(char* path, char* device_id)
{
	SHELLEXECUTEINFO shExecInfo;
	char exename[MAX_PATH_LENGTH];
	HANDLE handle[2] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE};
	OVERLAPPED overlapped;
	int r;
	DWORD rd_count;
	BOOL is_x64 = false;
#define BUFSIZE 256
	char buffer[BUFSIZE];

	req_device_id = device_id;

	// Detect whether if we should run the 64 bit installer, without
	// relying on external libs
	if (sizeof(uintptr_t) < 8) {
		// This application is not 64 bit, but it might be 32 bit
		// running in WOW64
		pIsWow64Process = (BOOL (__stdcall *)(HANDLE, PBOOL))
			GetProcAddress(GetModuleHandle("KERNEL32"), "IsWow64Process");
		if (pIsWow64Process != NULL) {
			(*pIsWow64Process)(GetCurrentProcess(), &is_x64);
		}
	} else {
		// TODO: warn at compile time about redist of 64 bit app
		is_x64 = true;
	}

	// Use a pipe to communicate with our installer
	pipe_handle = CreateNamedPipe("\\\\.\\pipe\\libusb-installer", PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED,
		PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE, 1, 4096, 4096, 0, NULL);
	if (pipe_handle == INVALID_HANDLE_VALUE) {
		usbi_err(NULL, "could not create read pipe: %s", windows_error_str(0));
		r = -1; goto out;
	}

	// Set the overlapped for messaging
	memset(&overlapped, 0, sizeof(OVERLAPPED));
	handle[0] = CreateEvent(NULL, TRUE, FALSE, NULL);
	if(handle[0] == NULL) {
		r = -1; goto out;
	}
	overlapped.hEvent = handle[0];

	safe_strcpy(exename, MAX_PATH_LENGTH, path);
	// TODO: fallback to x86 if x64 unavailable
	if (is_x64) {
		safe_strcat(exename, MAX_PATH_LENGTH, "\\installer_x64.exe");
	} else {
		safe_strcat(exename, MAX_PATH_LENGTH, "\\installer_x86.exe");
	}

	shExecInfo.cbSize = sizeof(SHELLEXECUTEINFO);

	shExecInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
	shExecInfo.hwnd = NULL;
	shExecInfo.lpVerb = "runas";
	shExecInfo.lpFile = exename;
	// if INF_NAME ever has a space, it will be seen as multiple parameters
	shExecInfo.lpParameters = INF_NAME;
	shExecInfo.lpDirectory = path;
	// TODO: hide
//	shExecInfo.nShow = SW_NORMAL;
	shExecInfo.nShow = SW_HIDE;
	shExecInfo.hInstApp = NULL;

	if (!ShellExecuteEx(&shExecInfo)) {
		usbi_err(NULL, "ShellExecuteEx failed: %s", windows_error_str(0));
	}

	if (shExecInfo.hProcess == NULL) {
		usbi_dbg("user chose not to run the installer");
		r = -1; goto out;
	}
	handle[1] = shExecInfo.hProcess;

	while (1) {
		if (ReadFile(pipe_handle, buffer, 256, &rd_count, &overlapped)) {
			// Message was read synchronously
			process_message(buffer, rd_count);
		} else {
			switch(GetLastError()) {
			case ERROR_BROKEN_PIPE:
				// The pipe has been ended - wait for installer to finish
				WaitForSingleObject(handle[1], INFINITE);
				r = 0; goto out;
			case ERROR_PIPE_LISTENING:
				// Wait for installer to open the pipe
				Sleep(100);
				continue;
			case ERROR_IO_PENDING:
				switch(WaitForMultipleObjects(2, handle, FALSE, INFINITE)) {
				case WAIT_OBJECT_0: // Pipe event
					if (GetOverlappedResult(pipe_handle, &overlapped, &rd_count, FALSE)) {
						// Message was read asynchronously
						process_message(buffer, rd_count);
					} else {
						switch(GetLastError()) {
						case ERROR_BROKEN_PIPE:
							// The pipe has been ended - wait for installer to finish
							WaitForSingleObject(handle[1], INFINITE);
							r = 0; goto out;
						case ERROR_MORE_DATA:
							usbi_warn(NULL, "program assertion failed: message overflow");
							process_message(buffer, rd_count);
							break;
						default:
							usbi_err(NULL, "could not read from pipe (async): %s", windows_error_str(0));
							break;
						}
					}
					break;
				case WAIT_OBJECT_0+1:
					// installer process terminated
					r = 0; goto out;
				default:
					usbi_err(NULL, "could not read from pipe (wait): %s", windows_error_str(0));
					break;
				}
				break;
			default:
				usbi_err(NULL, "could not read from pipe (sync): %s", windows_error_str(0));
				break;
			}
		}
	}
out:
	safe_closehandle(handle[0]);
	safe_closehandle(handle[1]);
	safe_closehandle(pipe_handle);
	return r;
}

//TODO: add a call to free strings & list
