#include <xtl.h>
#include <time.h>
#include <string>
#include <vector>
#include <stdio.h>
#include <sstream>
#include <fstream>
#include <stdarg.h>
#include <xkelib.h>
#include "Detours.h"
#include <winsockx.h>
#include <sys/stat.h>
#pragma comment(lib, "xnet.lib")

// internal hard disk
#define MOUNT_HDD "Hdd:"
#define DEVICE_HARDISK0_PART1 "\\Device\\Harddisk0\\Partition1"
// usb memory stick
#define MOUNT_USB "Usb:"
#define MOUNT_USB0 "Usb0:"
#define DEVICE_USB0 "\\Device\\Mass0"    
#define MOUNT_USB1 "Usb1:"
#define DEVICE_USB1 "\\Device\\Mass1"
#define MOUNT_USB2 "Usb2:"
#define DEVICE_USB2 "\\Device\\Mass2"
// internal slim trinity mu
#define MOUNT_INTMU "IntMu:"
#define DEVICE_INTMEM "\\Device\\BuiltInMuUsb\\Storage"
// CD / DVD 
#define MOUNT_DVD "Dvd:"
#define DEVICE_CDROM0 "\\Device\\Cdrom0"
// Nand Flash
#define MOUNT_FLASH "Flash:"
#define DEVICE_NAND_FLASH "\\Device\\Flash"
// DEVKIT folder on Hdd
#define MOUNT_DEVKIT "DEVKIT:"
#define DEVICE_DEVKIT "\\Device\\Harddisk0\\Partition1\\DEVKIT"
// Games folder on Hdd
#define MOUNT_HDDGAMES "HddGames:"
#define DEVICE_HDDGAMES "\\Device\\Harddisk0\\Partition1\\Games"
// Apps folder on Hdd
#define MOUNT_HDDAPPS "HddApps:"
#define DEVICE_HDDAPPS "\\Device\\Harddisk0\\Partition1\\Apps"
// Network share using connectx
#define DEVICE_NETSHARE "Netshare:"
#define DEVICE_SMB "\\Network\\Smb"
// internal corona 4g mu
#define MOUNT_MMCMU "MmcMu:"
#define DEVICE_MMCMU "\\Device\\BuiltInMuMmc\\Storage"
// big block NAND mu
#define MOUNT_FLASHMU "FlashMu:"
#define DEVICE_FLASHMU "\\Device\\BuiltInMuSfc"
// memory unit
#define MOUNT_MU "Mu:"
#define DEVICE_MEMORY_UNIT0 "\\Device\\Mu0"
#define MOUNT_MU1 "Mu1:"
#define DEVICE_MEMORY_UNIT1 "\\Device\\Mu1"    
// USB memory unit
#define MOUNT_USBMU0 "UsbMu0:"
#define DEVICE_USBMU0 "\\Device\\Mass0PartitionFile\\Storage"
#define MOUNT_USBMU1 "UsbMu1:"
#define DEVICE_USBMU1 "\\Device\\Mass1PartitionFile\\Storage"
#define MOUNT_USBMU2 "UsbMu2:"
#define DEVICE_USBMU2 "\\Device\\Mass2PartitionFile\\Storage"

// Boot-debugging switch, for when the console hangs before anything can be
// read. 0 returns from DllMain without touching anything (tests that the XEX
// itself loads). 1 also sets up the log and runs the kernel checks, but starts
// no worker. 99 runs normally; later stages are chosen with "stage=" in the
// INI instead, so they need no rebuild.
#ifndef BOOT_STAGE
#define BOOT_STAGE 99
#endif

// Runtime stages, from "stage=" in the INI (default: everything).
//   2 worker waits for the dashboard, logs, stops
//   3 + resolve XAM exports and the devkit/retail probe
//   4 + hook XamInputGetState
//   5 + hook XamInputGetCapabilitiesEx
//   6 + hook XamInputSetState
//   7 + hook XamInactivityDetectRecentActivity
//   8 + network connection to the PC (normal operation)
#define STAGE_WAIT_ONLY      2
#define STAGE_LOOKUPS        3
#define STAGE_HOOK_STATE     4
#define STAGE_HOOK_CAPS      5
#define STAGE_HOOK_SETSTATE  6
#define STAGE_HOOK_INACTIVE  7
#define STAGE_NETWORK        8

// Drive link created at load time for the folder the plugin XEX lives in, so
// the log and INI can be reached long before the dashboard mounts "Usb:".
#define PLUGIN_DIR_LINK "\\System??\\Omni360Bridge:"
#define PLUGIN_DIR_MOUNT "Omni360Bridge:"

PLDR_DATA_TABLE_ENTRY pDataTable = nullptr;
char pluginLoadPath[MAX_PATH] = "";
char pluginDir[MAX_PATH] = "";
char pluginPath[MAX_PATH] = "";
char pluginLogPath[MAX_PATH] = "";
char ip[64] = "192.0.2.1";
int port = 36062;
bool gotIp = false;
int bootStage = 99;
DWORD hookDelayMs = 15000;
char linkTarget[MAX_PATH] = "";
HRESULT linkResult = S_OK;
HRESULT linkFallbackResult = S_OK;

// Keep file I/O out of the input/network hot paths. These calls are limited
// to startup and connection transitions. Each line is flushed and the file
// closed, so the last line survives a console freeze.
void BridgeLog(const char* format, ...)
{
	char message[512];
	va_list args;
	va_start(args, format);
	_vsnprintf(message, sizeof(message) - 1, format, args);
	va_end(args);
	message[sizeof(message) - 1] = '\0';

	DbgPrint("[Omni360InputBridge] %s\n", message);
	if (pluginLogPath[0] == '\0') return;

	char line[600];
	int length = _snprintf(line, sizeof(line) - 1, "[%lu] %s\r\n", GetTickCount(), message);
	if (length < 0) length = sizeof(line) - 1;
	line[sizeof(line) - 1] = '\0';

	HANDLE file = CreateFileA(pluginLogPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) return;
	SetFilePointer(file, 0, NULL, FILE_END);
	DWORD written = 0;
	WriteFile(file, line, (DWORD)length, &written, NULL);
	FlushFileBuffers(file);
	CloseHandle(file);
}

// Creates a drive name ("Name:") for a device path. Code running in a system
// thread looks drive names up in \System??, titles in \??, so make both.
// Returns the \System?? result, which is the one this plugin's own file
// calls use.
HRESULT MakeDriveLink(const char* name, const char* target)
{
	char systemName[64], titleName[64];
	_snprintf(systemName, sizeof(systemName) - 1, "\\System??\\%s", name);
	systemName[sizeof(systemName) - 1] = '\0';
	_snprintf(titleName, sizeof(titleName) - 1, "\\??\\%s", name);
	titleName[sizeof(titleName) - 1] = '\0';

	STRING link, device;
	RtlInitAnsiString(&device, target);

	RtlInitAnsiString(&link, titleName);
	ObDeleteSymbolicLink(&link);
	ObCreateSymbolicLink(&link, &device);

	RtlInitAnsiString(&link, systemName);
	ObDeleteSymbolicLink(&link);
	return ObCreateSymbolicLink(&link, &device);
}

// Works out where the plugin XEX was loaded from and links a drive name to that
// folder. Falls back to the internal hard drive if the path can't be used.
void SetUpPluginDirectory(HANDLE moduleHandle)
{
	pDataTable = (PLDR_DATA_TABLE_ENTRY)moduleHandle;
	if (pDataTable && pDataTable->FullDllName.Buffer) {
		int length = pDataTable->FullDllName.Length / sizeof(WCHAR);
		if (length >= MAX_PATH) length = MAX_PATH - 1;
		for (int i = 0; i < length; ++i)
			pluginLoadPath[i] = (char)pDataTable->FullDllName.Buffer[i];
		pluginLoadPath[length] = '\0';
	}

	char folder[MAX_PATH];
	strcpy(folder, pluginLoadPath);
	char* lastSlash = strrchr(folder, '\\');
	if (lastSlash) *lastSlash = '\0';
	else folder[0] = '\0';

	if (folder[0] != '\0' && folder[0] != '\\') {
		// Already a drive-style path ("Usb:\Plugins"), usable as it is.
		strcpy(pluginDir, folder);
	}
	else {
		const char* target = folder[0] ? folder : DEVICE_HARDISK0_PART1;
		strcpy(linkTarget, target);
		linkResult = MakeDriveLink(PLUGIN_DIR_MOUNT, target);
		if (FAILED(linkResult) && folder[0]) {
			strcpy(linkTarget, DEVICE_HARDISK0_PART1);
			linkFallbackResult = MakeDriveLink(PLUGIN_DIR_MOUNT, DEVICE_HARDISK0_PART1);
		}
		strcpy(pluginDir, PLUGIN_DIR_MOUNT);
	}

	_snprintf(pluginPath, sizeof(pluginPath) - 1, "%s\\Omni360InputBridge.ini", pluginDir);
	_snprintf(pluginLogPath, sizeof(pluginLogPath) - 1, "%s\\Omni360InputBridge.log", pluginDir);

	// Start a fresh log once it passes 256 KB.
	HANDLE file = CreateFileA(pluginLogPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file != INVALID_HANDLE_VALUE) {
		DWORD size = GetFileSize(file, NULL);
		CloseHandle(file);
		if (size != INVALID_FILE_SIZE && size > 256 * 1024)
			DeleteFileA(pluginLogPath);
	}
}

Detour XamInputGetStateDetour;
Detour XamInputGetCapabilitiesDetour;
Detour XamInactivityDetectRecentActivityDetour;
Detour XamInputSetStateDetour;

// Global Socket for the hook to access
SOCKET g_ServerSocket = INVALID_SOCKET;

// Cache to prevent sending duplicate packets (spam reduction)
XINPUT_VIBRATION g_LastVibration[4] = { {0,0}, {0,0}, {0,0}, {0,0} };

uint16_t swap_endianness_16(uint16_t val) {
	return (val >> 8) | (val << 8);
}

BOOL IsTrayOpen() {
	BYTE Input[0x10] = { 0 }, Output[0x10] = { 0 };
	Input[0] = 0xA;
	HalSendSMCMessage(Input, Output);
	return (Output[1] == 0x60);
}

enum ControllerStatus {
	INACTIVE,
	ACTIVE,
};

#pragma pack(push, 1)
struct Report {
	uint8_t reportId;
};

struct ButtonsReport : Report {
	uint8_t x;
	uint8_t y;
	uint8_t z;
	uint8_t rz;
	uint8_t triangle : 1;
	uint8_t circle : 1;
	uint8_t cross : 1;
	uint8_t square : 1;
	uint8_t hat_switch : 4;
	uint8_t r3 : 1;
	uint8_t l3 : 1;
	uint8_t options : 1;
	uint8_t share : 1;
	uint8_t r2 : 1;
	uint8_t l2 : 1;
	uint8_t r1 : 1;
	uint8_t l1 : 1;
	uint8_t : 6;
	uint8_t touchpad : 1;
	uint8_t ps : 1;
	uint8_t rx;
	uint8_t ry;
	uint8_t vendor_defined;
};
#pragma pack(pop)

typedef struct _XINPUT_VIBRATIONEX
{
	WORD                                wLeftMotorSpeed;
	WORD                                wRightMotorSpeed;
} XINPUT_VIBRATION_EX, *PXINPUT_VIBRATION_EX;

typedef struct _XINPUT_CAPABILITIESEX
{
	BYTE                                Type;
	BYTE                                SubType;
	WORD                                Flags;
	XINPUT_GAMEPAD                      Gamepad;
	XINPUT_VIBRATION                    Vibration;
	DWORD unk1;
	DWORD unk2;
	DWORD unk3;
} XINPUT_CAPABILITIES_EX, *PXINPUT_CAPABILITIES_EX;

typedef int(*xam_user_bind_device_callback_func_t)(unsigned int controllerId, unsigned int context, unsigned __int8 category, bool disconnect, unsigned __int8* userIndex);
typedef void(*mm_free_physical_memory_func_t)(DWORD type, DWORD address);

xam_user_bind_device_callback_func_t XamUserBindDeviceCallback = nullptr;
mm_free_physical_memory_func_t MmFreePhysicalMemory = nullptr;

DWORD* XampInputRoutedToSysapp = nullptr;

struct Controller {
	ButtonsReport currentState;
	uint8_t userIndex;
	uint32_t packetNumber;
	ControllerStatus ControllerStatus;

	Controller()
		: userIndex(0), packetNumber(0), ControllerStatus(INACTIVE)
	{
		memset(&currentState, 0, sizeof(currentState));
		currentState.x = 0x80;
		currentState.y = 0x80;
		currentState.z = 0x80;
		currentState.rz = 0x80;
		currentState.hat_switch = 8;
	}
} __declspec(align(4));

volatile int g_ClientThreadRunning = 1;

Controller connectedControllers[4];

void RemoveDevice(int controllerIndex) {
	if (controllerIndex < 0 || controllerIndex >= 4)
		return;
	if (connectedControllers[controllerIndex].ControllerStatus != ACTIVE)
		return;
	connectedControllers[controllerIndex].ControllerStatus = INACTIVE;
	if (XamUserBindDeviceCallback)
		XamUserBindDeviceCallback(0xa7553952 + controllerIndex, 0x0000000010000005 + controllerIndex, 0, true, 0);
}

void AddDevice(int controllerIndex) {
	if (controllerIndex < 0 || controllerIndex >= 4 || !XamUserBindDeviceCallback)
		return;
	Controller c = Controller();
	c.ControllerStatus = ACTIVE;
	c.packetNumber = 0;
	c.currentState.reportId = (uint8_t)controllerIndex;

	uint8_t userIndex = -1;
	XamUserBindDeviceCallback(0xa7553952 + controllerIndex, 0x0000000010000005 + controllerIndex, 0, false, &userIndex);
	c.userIndex = userIndex;

	connectedControllers[controllerIndex] = c;
}

int16_t ConvertToFullRange(uint8_t input, bool invert_y = false) {
	if (!invert_y)
		return static_cast<int16_t>((input - 128) * 256);
	else
		return static_cast<int16_t>((~(input)-128) * 256);
}

DWORD XamInputGetStateHook(DWORD user, DWORD flags, XINPUT_STATE* input_state) {
	DWORD status = XamInputGetStateDetour.GetOriginal<decltype(&XamInputGetStateHook)>()(user, flags, input_state);

	if ((user & 0xFF) == 0xFF)
		user = 0;

	if (!input_state)
		return status;

	static DWORD lastPressTime = 0;
	static const DWORD cooldownDuration = 1000;

	if (status == ERROR_DEVICE_NOT_CONNECTED) {
		memset(input_state, 0, sizeof(*input_state));
		ButtonsReport b;
		Controller* c = nullptr;
		for (int i = 0; i < (sizeof(connectedControllers) / sizeof(Controller)); i++) {
			if (connectedControllers[i].ControllerStatus == ACTIVE) {
				if (connectedControllers[i].userIndex == user) {
					c = &connectedControllers[i];
					b = connectedControllers[i].currentState;
					break;
				}
			}
		}

		if (!c)
			return status;

		// HUD is open.
		if (c->userIndex < 4 && XampInputRoutedToSysapp[c->userIndex]) {
			// 0x1 is used for titles, 0x0 is used by some offhosts and debug input.
			if ((flags == 0x1) || (flags == 0x0)) {
				return ERROR_SUCCESS;
			}
		}

		if (b.ps) {
			DWORD now = GetTickCount();
			if (now - lastPressTime >= cooldownDuration) {
				lastPressTime = now;
				XamInputSendXenonButtonPress(user);
			}
		}

		if (b.cross)
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_A;

		if (b.circle)
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_B;

		if (b.triangle)
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_Y;

		if (b.square)
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_X;

		if (b.options)
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_START;

		if (b.share)
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_BACK;

		if (b.r3)
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;

		if (b.l3)
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;

		if (b.r1)
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;

		if (b.l1)
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;

		if (b.hat_switch == 0 || b.hat_switch == 1 || b.hat_switch == 7)
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP;

		if (b.hat_switch == 1 || b.hat_switch == 2 || b.hat_switch == 3)
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;

		if (b.hat_switch == 3 || b.hat_switch == 4 || b.hat_switch == 5)
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;

		if (b.hat_switch == 5 || b.hat_switch == 6 || b.hat_switch == 7)
			input_state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;

		input_state->Gamepad.sThumbRX = ConvertToFullRange(b.z);
		input_state->Gamepad.sThumbRY = ConvertToFullRange(b.rz, true);

		input_state->Gamepad.sThumbLX = ConvertToFullRange(b.x);
		input_state->Gamepad.sThumbLY = ConvertToFullRange(b.y, true);

		input_state->Gamepad.bRightTrigger = b.ry;
		input_state->Gamepad.bLeftTrigger = b.rx;
		input_state->dwPacketNumber = ++c->packetNumber;

		return ERROR_SUCCESS;
	}
	return status;
}

DWORD XamInputGetCapabilitiesExHook(DWORD unk, DWORD user, DWORD flags, XINPUT_CAPABILITIES_EX* capabilities) {
	DWORD status = XamInputGetCapabilitiesDetour.GetOriginal<decltype(&XamInputGetCapabilitiesExHook)>()(unk, user, flags, capabilities);

	if ((user & 0xFF) == 0xFF)
		user = 0;

	if (!capabilities)
		return status;

	if (status == ERROR_DEVICE_NOT_CONNECTED) {
		Controller* c = nullptr;
		for (int i = 0; i < (sizeof(connectedControllers) / sizeof(Controller)); i++) {
			if (connectedControllers[i].ControllerStatus == ACTIVE) {
				if (connectedControllers[i].userIndex == user) {
					c = &connectedControllers[i];
					break;
				}
			}
		}

		if (!c)
			return status;

		capabilities->Type = XINPUT_DEVTYPE_GAMEPAD;
		capabilities->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
		capabilities->Flags = 0;

		XINPUT_STATE state;
		memset(&state, 0, sizeof(XINPUT_STATE));
		XamInputGetStateHook(user, 0, &state);
		capabilities->Gamepad = state.Gamepad;
		capabilities->Vibration.wLeftMotorSpeed = 0;
		capabilities->Vibration.wRightMotorSpeed = 0;
		return ERROR_SUCCESS;
	}
	return status;
}

void SendVibrationUpdate(int userIndex, unsigned short left, unsigned short right) {
	if (g_ServerSocket == INVALID_SOCKET) return;

	// OPTIMIZATION: Only send if the values changed
	if (g_LastVibration[userIndex].wLeftMotorSpeed == left &&
		g_LastVibration[userIndex].wRightMotorSpeed == right) {
		return;
	}

	// Update the cache
	g_LastVibration[userIndex].wLeftMotorSpeed = left;
	g_LastVibration[userIndex].wRightMotorSpeed = right;

	// 1. Format the message: "V:Index:Left:Right"
	char msgBuf[64];
	sprintf(msgBuf, "V:%d:%d:%d", userIndex, left, right);
	std::string msg = msgBuf;

	// 2. Build the WebSocket Frame (Masked)
	std::vector<uint8_t> frame;
	frame.push_back(0x81); // Byte 0: FIN + Text Opcode

	// Byte 1: Mask Bit (0x80) + Payload Length
	// (Assuming payload is short, < 126 bytes, which it is for this string)
	frame.push_back(0x80 | (uint8_t)msg.length());

	// Bytes 2-5: Generate Random Mask Key
	uint8_t mask[4];
	for (int i = 0; i < 4; i++) mask[i] = (uint8_t)(rand() % 0xFF);

	frame.push_back(mask[0]);
	frame.push_back(mask[1]);
	frame.push_back(mask[2]);
	frame.push_back(mask[3]);

	// Bytes 6+: Payload (XOR Encrypted with Mask)
	for (size_t i = 0; i < msg.length(); ++i) {
		frame.push_back(msg[i] ^ mask[i % 4]);
	}

	// 3. Send using NetDll
	NetDll_send(static_cast<XNCALLER_TYPE>(XNCALLER_SYSAPP),
		g_ServerSocket,
		(const char*)frame.data(),
		frame.size(),
		0);
}

DWORD XamInputSetStateHook(DWORD user, DWORD flags, PXINPUT_VIBRATION pVibration, BYTE bAmplitude, BYTE bFrequency, BYTE bOffset) {
    // Call the original first so the actual controller vibrates locally
    DWORD status = XamInputSetStateDetour.GetOriginal<decltype(&XamInputSetStateHook)>()(user, flags, pVibration, bAmplitude, bFrequency, bOffset);

	if ((user & 0xFF) == 0xFF)
		user = 0;

	if (status == ERROR_DEVICE_NOT_CONNECTED) {
		ButtonsReport b;
		Controller* c = nullptr;
		for (int i = 0; i < (sizeof(connectedControllers) / sizeof(Controller)); i++) {
			if (connectedControllers[i].ControllerStatus == ACTIVE) {
				if (connectedControllers[i].userIndex == user) {
					c = &connectedControllers[i];
					b = connectedControllers[i].currentState;
					break;
				}
			}
		}

		if (!c)
			return status;

		// Send to server
		if (pVibration != nullptr) {
			// userIndex might have flags (like 0xFF), mask them out if necessary, 
			// though usually SetState receives a clean index (0-3).
			int cleanIndex = user & 0xFF;

			if (cleanIndex < 4) {
				SendVibrationUpdate(cleanIndex, pVibration->wLeftMotorSpeed, pVibration->wRightMotorSpeed);
			}
		}
		else {
			// If pVibration is null, it usually implies stop (0,0)
			SendVibrationUpdate(user & 0xFF, 0, 0);
		}

		return ERROR_SUCCESS;
	}

    return status;
}

// fix for inactivity (screen dimming)
int XamInactivityDetectRecentActivityHook(DWORD r3) {
	// check if a controller is connected
	for (int i = 0; i < 4; i++) {
		if (connectedControllers[i].ControllerStatus == ACTIVE) {
			// return active
			return 1;
		}
	}
	return XamInactivityDetectRecentActivityDetour.GetOriginal<decltype(&XamInactivityDetectRecentActivityHook)>()(r3);
}

bool ReadConfig()
{
	if (gotIp) return true;

	BridgeLog("Reading config: %s", pluginPath);
	bool sawIp = false;
	FILE* file = fopen(pluginPath, "r");
	if (!file)
	{
		BridgeLog("Config not found");
		return false;
	}

	char buffer[256];
	while (fgets(buffer, sizeof(buffer), file))
	{
		buffer[strcspn(buffer, "\r\n")] = 0;
		char* line = buffer;
		while (*line == ' ' || *line == '\t') ++line;
		if (*line == 0 || *line == '#') continue;

		char lower[256];
		strncpy(lower, line, sizeof(lower) - 1);
		lower[sizeof(lower) - 1] = 0;
		for (char* p = lower; *p; ++p) *p = (char)tolower(*p);

		if (strncmp(lower, "ip=", 3) == 0)
		{
			const char* value = line + 3;
			while (*value == ' ' || *value == '\t') ++value;
			strncpy(ip, value, sizeof(ip) - 1);
			ip[sizeof(ip) - 1] = 0;
			sawIp = true;
		}
		else if (strncmp(lower, "port=", 5) == 0)
		{
			port = atoi(line + 5);
		}
		else if (strncmp(lower, "stage=", 6) == 0)
		{
			bootStage = atoi(line + 6);
		}
		else if (strncmp(lower, "delay=", 6) == 0)
		{
			int delay = atoi(line + 6);
			if (delay >= 0 && delay <= 600000) hookDelayMs = (DWORD)delay;
		}
	}
	fclose(file);
	BridgeLog("Config: stage=%d delay=%lums", bootStage, hookDelayMs);

	if (!sawIp || inet_addr(ip) == INADDR_NONE || port < 1 || port > 65535)
	{
		BridgeLog("Invalid IP or port in config");
		return false;
	}
	gotIp = true;
	BridgeLog("Config loaded: target %s:%d", ip, port);
	return true;
}

int HexDigit(char value)
{
	if (value >= '0' && value <= '9') return value - '0';
	if (value >= 'a' && value <= 'f') return value - 'a' + 10;
	if (value >= 'A' && value <= 'F') return value - 'A' + 10;
	return -1;
}

void ProcessInputText(const char* text, int length)
{
	if (!text || length < 2 || length >= 256) return;
	if ((text[0] != '0' && text[0] != '1') || text[1] < '0' || text[1] > '3') return;

	int controllerIndex = text[1] - '0';
	if (text[0] == '1')
	{
		RemoveDevice(controllerIndex);
		return;
	}

	// The active report is ten bytes rendered as twenty hexadecimal characters.
	if (length != 20) return;
	uint8_t raw[sizeof(ButtonsReport)];
	memset(raw, 0, sizeof(raw));
	for (int i = 0; i < 10; ++i)
	{
		int high = HexDigit(text[i * 2]);
		int low = HexDigit(text[i * 2 + 1]);
		if (high < 0 || low < 0) return;
		raw[i] = (uint8_t)((high << 4) | low);
	}

	if (connectedControllers[controllerIndex].ControllerStatus == INACTIVE)
		AddDevice(controllerIndex);
	if (connectedControllers[controllerIndex].ControllerStatus != ACTIVE)
		return;

	ButtonsReport buttonReport;
	memcpy(&buttonReport, raw, sizeof(buttonReport));
	connectedControllers[controllerIndex].currentState = buttonReport;
}

DWORD WINAPI StartWSConnection(LPVOID) {
    BridgeLog("Network worker started");
    if (!gotIp) {
        BridgeLog("No usable ip/port in the config; network stays off");
        return 0;
    }
    XNetStartupParams xnsp;
    memset(&xnsp, 0, sizeof(xnsp));
    xnsp.cfgSizeOfStruct = sizeof(XNetStartupParams);
    xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;

    int xnetResult = NetDll_XNetStartup(static_cast<XNCALLER_TYPE>(XNCALLER_SYSAPP), &xnsp);
    if (xnetResult != 0) {
        BridgeLog("XNetStartup failed: %d", xnetResult);
        return 0;
    }
    BridgeLog("XNetStartup succeeded");

    WSADATA wsaData;
    int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    
    if (wsaResult != 0) {
        BridgeLog("WSAStartup failed: %d", wsaResult);
        NetDll_XNetCleanup(static_cast<XNCALLER_TYPE>(XNCALLER_SYSAPP));
        return 0; // Cannot run without network stack
    }
    BridgeLog("WSAStartup succeeded");

    // Outer loop for Reconnection Logic
    while (g_ClientThreadRunning) {
        
        // Create Socket
        SOCKET sock = NetDll_socket(static_cast<XNCALLER_TYPE>(XNCALLER_SYSAPP), AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) {
            BridgeLog("Socket creation failed: %d", NetDll_WSAGetLastError());
            // Do NOT Cleanup WSA here, just sleep and retry the socket creation
            Sleep(3000);
            continue;
        }

        BOOL opt_true = TRUE;
        NetDll_setsockopt(static_cast<XNCALLER_TYPE>(XNCALLER_SYSAPP), sock, SOL_SOCKET, 0x5801, (PCSTR)&opt_true, sizeof(BOOL));

        // --- Connect to server ---
        SOCKADDR_IN target;
        target.sin_family = AF_INET;
        target.sin_port = htons(port);
        target.sin_addr.s_addr = inet_addr(ip);

        if (NetDll_connect(static_cast<XNCALLER_TYPE>(XNCALLER_SYSAPP), sock, (SOCKADDR*)&target, sizeof(target)) == SOCKET_ERROR) {
            BridgeLog("Connect to %s:%d failed, socket error %d; retrying", ip, port, NetDll_WSAGetLastError());
            NetDll_closesocket(static_cast<XNCALLER_TYPE>(XNCALLER_SYSAPP), sock);
            Sleep(3000);
            continue;
        }
        BridgeLog("TCP connected to PC bridge");

        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

        static char wsHandshake[512];
        sprintf(
            wsHandshake,
            "GET / HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n",
            ip, port
        );

        if (NetDll_send(static_cast<XNCALLER_TYPE>(XNCALLER_SYSAPP), sock, wsHandshake, strlen(wsHandshake), 0) == SOCKET_ERROR) {
            BridgeLog("WebSocket handshake send failed, socket error %d", NetDll_WSAGetLastError());
            NetDll_closesocket(static_cast<XNCALLER_TYPE>(XNCALLER_SYSAPP), sock);
            Sleep(3000);
            continue;
        }

        const int BUFSIZE = 4096;
        static char buf[BUFSIZE];
        int bytes = NetDll_recv(static_cast<XNCALLER_TYPE>(XNCALLER_SYSAPP), sock, buf, BUFSIZE - 1, 0);

        if (bytes <= 0) {
            BridgeLog("WebSocket handshake receive failed, socket error %d", NetDll_WSAGetLastError());
            NetDll_closesocket(static_cast<XNCALLER_TYPE>(XNCALLER_SYSAPP), sock);
            Sleep(3000);
            continue;
        }
        // ... Handshake processing ...
        BridgeLog("WebSocket connection ready");

		g_ServerSocket = sock;

        // --- WebSocket main loop ---
        bool connectionActive = true;
		static unsigned char pending[4096];
		static unsigned char incoming[1024];
		int pendingLength = 0;
		while (connectionActive && g_ClientThreadRunning) {
			bytes = NetDll_recv(static_cast<XNCALLER_TYPE>(XNCALLER_SYSAPP), sock, (char*)incoming, sizeof(incoming), 0);
			if (bytes <= 0 || pendingLength + bytes > sizeof(pending)) {
				connectionActive = false;
				break;
			}
			memcpy(pending + pendingLength, incoming, bytes);
			pendingLength += bytes;

			int consumed = 0;
			while (connectionActive && pendingLength - consumed >= 2) {
				unsigned char* data = pending + consumed;
				int available = pendingLength - consumed;
				int opcode = data[0] & 0x0F;
				bool masked = (data[1] & 0x80) != 0;
				unsigned long long payloadLength = data[1] & 0x7F;
				int offset = 2;

				if (payloadLength == 126) {
					if (available < 4) break;
					payloadLength = ((unsigned long long)data[2] << 8) | data[3];
					offset = 4;
				}
				else if (payloadLength == 127) {
					if (available < 10) break;
					payloadLength = 0;
					for (int i = 0; i < 8; ++i)
						payloadLength = (payloadLength << 8) | data[offset++];
				}

				if (payloadLength > 255) {
					connectionActive = false;
					break;
				}
				int maskOffset = masked ? 4 : 0;
				int frameLength = offset + maskOffset + (int)payloadLength;
				if (available < frameLength) break;

				if (opcode == 1) {
					char text[256];
					const unsigned char* mask = masked ? data + offset : nullptr;
					const unsigned char* payload = data + offset + maskOffset;
					for (int i = 0; i < (int)payloadLength; ++i)
						text[i] = (char)(payload[i] ^ (masked ? mask[i % 4] : 0));
					text[payloadLength] = 0;
					ProcessInputText(text, (int)payloadLength);
				}
				else if (opcode == 8) {
					connectionActive = false;
				}
				consumed += frameLength;
			}

			if (consumed > 0) {
				pendingLength -= consumed;
				memmove(pending, pending + consumed, pendingLength);
			}
		}

		g_ServerSocket = INVALID_SOCKET;
		for (int i = 0; i < 4; ++i)
			RemoveDevice(i);

		// Close the socket, but keep WSA loaded for the next attempt
		NetDll_closesocket(static_cast<XNCALLER_TYPE>(XNCALLER_SYSAPP), sock);
		BridgeLog("Connection closed; retrying in 3s");
		Sleep(3000);
	}
	NetDll_WSACleanup(static_cast<XNCALLER_TYPE>(XNCALLER_SYSAPP));
	NetDll_XNetCleanup(static_cast<XNCALLER_TYPE>(XNCALLER_SYSAPP));
	BridgeLog("Network worker stopped");
	return 0;
}

void* XamInputGetState = nullptr;
void* XamInputGetCapabilitiesEx = nullptr;
void* XamInputSetState = nullptr;
bool isDevkit = true;

bool initFunctionPointers() {
	isDevkit = *(uint32_t*)(0x8010D334) == 0x00000000;
	BridgeLog("Console mode probe: %s", isDevkit ? "devkit" : "retail");
	HANDLE kernelHandle = GetModuleHandleA("xboxkrnl.exe");

	if (!kernelHandle) {
		BridgeLog("Could not get kernel handle");
		return false;
	}

	HANDLE xamHandle = GetModuleHandleA("xam.xex");
	if (!xamHandle) {
		BridgeLog("Could not get XAM handle");
		return false;
	}

	XexGetProcedureAddress(kernelHandle, 189, &MmFreePhysicalMemory);

	XexGetProcedureAddress(xamHandle, 685, &XamInputGetCapabilitiesEx);
	XexGetProcedureAddress(xamHandle, 401, &XamInputGetState);
	XexGetProcedureAddress(xamHandle, 402, &XamInputSetState);
	if (!XamInputGetCapabilitiesEx || !XamInputGetState || !XamInputSetState) {
		BridgeLog("Required XAM exports are missing (capabilities=%p state=%p setstate=%p)", XamInputGetCapabilitiesEx, XamInputGetState, XamInputSetState);
		return false;
	}

	BridgeLog("XAM exports: state=%p caps=%p setstate=%p", XamInputGetState, XamInputGetCapabilitiesEx, XamInputSetState);

	if (isDevkit) {
		BridgeLog("Running in devkit mode");
		XamUserBindDeviceCallback = (xam_user_bind_device_callback_func_t)0x817A34B8; // 7C 8B 23 78 7C A4 2B 78 54 CA 06 3F
		XampInputRoutedToSysapp = (DWORD*)0x81D4F650;
	}
	else {
		BridgeLog("Running in retail mode");
		XamUserBindDeviceCallback = (xam_user_bind_device_callback_func_t)0x816D9060; // 7C 8B 23 78 7C A4 2B 78 54 CA 06 3F
		XampInputRoutedToSysapp = (DWORD*)0x81AAC2A0;
	}
	return true;
}

void NormalizePath(char* pluginPath)
{
	// 1. Define struct locally
	typedef struct {
		const char* mount;
		const char* device;
	} MountMapping;

	// 2. Define data locally as STATIC (initialized only once)
	// Put the longest paths (like DEVKIT or Games) ABOVE generic Hdd/Mass paths
	static const MountMapping driveMappings[] = {
		{ MOUNT_DEVKIT,    DEVICE_DEVKIT },
		{ MOUNT_HDDGAMES,  DEVICE_HDDGAMES },
		{ MOUNT_HDDAPPS,   DEVICE_HDDAPPS },
		{ MOUNT_USBMU0,    DEVICE_USBMU0 },
		{ MOUNT_USBMU1,    DEVICE_USBMU1 },
		{ MOUNT_USBMU2,    DEVICE_USBMU2 },
		{ MOUNT_HDD,       DEVICE_HARDISK0_PART1 },
		{ MOUNT_USB0,      DEVICE_USB0 },
		{ MOUNT_USB1,      DEVICE_USB1 },
		{ MOUNT_USB2,      DEVICE_USB2 },
		{ MOUNT_INTMU,     DEVICE_INTMEM },
		{ MOUNT_DVD,       DEVICE_CDROM0 },
		{ MOUNT_FLASH,     DEVICE_NAND_FLASH },
		{ MOUNT_MMCMU,     DEVICE_MMCMU },
		{ MOUNT_FLASHMU,   DEVICE_FLASHMU },
		{ MOUNT_MU,        DEVICE_MEMORY_UNIT0 },
		{ MOUNT_MU1,       DEVICE_MEMORY_UNIT1 },
		{ NULL, NULL }
	};

	int i;

	// 3. The Loop
	for (i = 0; driveMappings[i].mount != NULL; i++)
	{
		const char* devicePrefix = driveMappings[i].device;
		const char* mountPoint = driveMappings[i].mount;
		size_t devLen = strlen(devicePrefix);

		if (strncmp(pluginPath, devicePrefix, devLen) == 0)
		{
			// Ensure we matched a full directory name
			if (pluginPath[devLen] == '\\' || pluginPath[devLen] == '\0')
			{
				char temp[MAX_PATH];
				const char* remainingPath = pluginPath + devLen;

				// Skip the leading slash if present so we don't get "Hdd:\\folder"
				if (*remainingPath == '\\') {
					remainingPath++;
				}

				// Combine: "Hdd:" + "\" + "MyPath"
				sprintf(temp, "%s\\%s", mountPoint, remainingPath);

				strcpy(pluginPath, temp);
				return; // Stop processing once match is found
			}
		}
	}
}

// Repeats the load path and drive link results, and checks the log file can
// be written. If it can't, the log moves to the internal hard drive.
void ReportFileSetup()
{
	BridgeLog("Loaded from: %s", pluginLoadPath[0] ? pluginLoadPath : "(unknown)");
	BridgeLog("Drive link %s -> %s: %08X (fallback %08X)", PLUGIN_DIR_LINK, linkTarget[0] ? linkTarget : "(none)", linkResult, linkFallbackResult);
	BridgeLog("Log: %s  Config: %s", pluginLogPath, pluginPath);

	HANDLE file = CreateFileA(pluginLogPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file != INVALID_HANDLE_VALUE) {
		CloseHandle(file);
		BridgeLog("Log file opens for writing");
		return;
	}
	DWORD error = GetLastError();
	BridgeLog("Log file can't be opened: error %lu", error);

	HRESULT result = MakeDriveLink("Omni360BridgeHdd:", DEVICE_HARDISK0_PART1);
	strcpy(pluginLogPath, "Omni360BridgeHdd:\\Omni360InputBridge.log");
	BridgeLog("Logging to the hard drive instead (link %08X): %s", result, pluginLogPath);

	file = CreateFileA(pluginLogPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		BridgeLog("Hard drive log can't be opened either: error %lu", GetLastError());
		pluginLogPath[0] = 0;
		return;
	}
	CloseHandle(file);
	BridgeLog("Hard drive log opens for writing");
}

// Logs the first instructions at an address we are about to patch, and checks
// them against the expected pattern (mask bits of 0 are wildcards).
bool CheckCode(const char* name, void* address, const DWORD* expected, const DWORD* mask, int count)
{
	const DWORD* code = (const DWORD*)address;
	BridgeLog("%s @ %p: %08X %08X %08X %08X", name, address, code[0], code[1], code[2], code[3]);
	for (int i = 0; i < count; ++i) {
		if ((code[i] & mask[i]) != (expected[i] & mask[i])) {
			BridgeLog("%s: instruction %d does not match the expected pattern", name, i);
			return false;
		}
	}
	return true;
}

bool InstallHook(Detour& detour, const char* name, void* source, void* target)
{
	BridgeLog("Installing %s hook at %p", name, source);
	detour = Detour(source, target);
	bool installed = detour.Install();
	BridgeLog("%s hook %s", name, installed ? "installed" : "NOT installed");
	return installed;
}

// Everything that touches XAM runs here, once the dashboard is up, so a bad
// hook can no longer stop the console from booting.
DWORD WINAPI BridgeWorker(LPVOID)
{
	BridgeLog("Worker started");

	DWORD started = GetTickCount();
	while (GetTickCount() - started < hookDelayMs)
		Sleep(500);
	BridgeLog("Waited %lums; current title %08X", GetTickCount() - started, XamGetCurrentTitleId());

	// DllMain's lines go out before XBDM's network is up, so repeat the
	// file setup here, where the XBDM watcher can see it.
	ReportFileSetup();
	ReadConfig();

	if (bootStage < STAGE_LOOKUPS) {
		BridgeLog("Stage %d: stopping before XAM lookups", bootStage);
		return 0;
	}
	if (!initFunctionPointers()) {
		BridgeLog("XAM initialization failed; stopping");
		return 0;
	}

	// The bind callback is called when a virtual pad is added. Check it now,
	// so a wrong address only disables pads instead of crashing later.
	static const DWORD bindExpected[] = { 0x7C8B2378, 0x7CA42B78, 0x54CA063F };
	static const DWORD bindMask[] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
	if (!CheckCode("XamUserBindDeviceCallback", (void*)XamUserBindDeviceCallback, bindExpected, bindMask, 3)) {
		BridgeLog("Virtual pads disabled: the bind callback address is wrong for this XAM");
		XamUserBindDeviceCallback = nullptr;
	}

	void* inactivity = (void*)(isDevkit ? 0x81750588 : 0x81695DE8);
	static const DWORD inactivityExpected[] = { 0x3D608100, 0x3D408100 }; // 3D 60 81 ?? 3D 40 81 ??
	static const DWORD inactivityMask[] = { 0xFFFFFF00, 0xFFFFFF00 };
	bool inactivityOk = CheckCode("XamInactivityDetectRecentActivity", inactivity, inactivityExpected, inactivityMask, 2);
	if (bootStage < STAGE_HOOK_STATE) {
		BridgeLog("Stage %d: stopping before hooks", bootStage);
		return 0;
	}

	InstallHook(XamInputGetStateDetour, "XamInputGetState", XamInputGetState, (void*)XamInputGetStateHook);
	if (bootStage < STAGE_HOOK_CAPS) { BridgeLog("Stage %d: stopping", bootStage); return 0; }

	InstallHook(XamInputGetCapabilitiesDetour, "XamInputGetCapabilitiesEx", XamInputGetCapabilitiesEx, (void*)XamInputGetCapabilitiesExHook);
	if (bootStage < STAGE_HOOK_SETSTATE) { BridgeLog("Stage %d: stopping", bootStage); return 0; }

	InstallHook(XamInputSetStateDetour, "XamInputSetState", XamInputSetState, (void*)XamInputSetStateHook);
	if (bootStage < STAGE_HOOK_INACTIVE) { BridgeLog("Stage %d: stopping", bootStage); return 0; }

	if (inactivityOk)
		InstallHook(XamInactivityDetectRecentActivityDetour, "XamInactivityDetectRecentActivity", inactivity, (void*)XamInactivityDetectRecentActivityHook);
	else
		BridgeLog("Skipping the inactivity hook: address is wrong for this XAM");
	if (bootStage < STAGE_NETWORK) { BridgeLog("Stage %d: stopping before network", bootStage); return 0; }

	BridgeLog("XAM input hooks done; starting network");
	return StartWSConnection(nullptr);
}

BOOL DllMain(HINSTANCE hModule, DWORD reason, void* pReserved)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
#if BOOT_STAGE <= 0
		return TRUE;
#endif
		SetUpPluginDirectory(hModule);
		BridgeLog("---------------- startup ----------------");
		BridgeLog("DllMain attach entered; module=%p", hModule);
		BridgeLog("Loaded from: %s", pluginLoadPath[0] ? pluginLoadPath : "(unknown)");
		BridgeLog("Log: %s  Config: %s", pluginLogPath, pluginPath);
		if (!XboxKrnlVersion) {
			BridgeLog("XboxKrnlVersion is null; aborting");
			return FALSE;
		}
		BridgeLog("Plugin attached; kernel build %lu", XboxKrnlVersion->Build);

		if ((XboxKrnlVersion->Build != 17559 && XboxKrnlVersion->Build != 17489) || IsTrayOpen()) {
			BridgeLog("Unsupported kernel build or disc tray open; aborting");
			return FALSE;
		}
		BridgeLog("Kernel and tray preflight passed");

#if BOOT_STAGE <= 1
		BridgeLog("BOOT_STAGE 1: not starting the worker");
		return TRUE;
#endif
		HANDLE worker = nullptr;
		DWORD threadResult = ExCreateThread(&worker, 0x10000, nullptr, nullptr, BridgeWorker, nullptr, 2);
		BridgeLog("Worker create returned %ld (handle=%p); DllMain done", threadResult, worker);
	}
	else if (reason == DLL_PROCESS_DETACH) {
		XamInputGetStateDetour.Remove();
		XamInputGetCapabilitiesDetour.Remove();
		XamInputSetStateDetour.Remove();
		XamInactivityDetectRecentActivityDetour.Remove();

		port = 3000;
		gotIp = false;
		g_ClientThreadRunning = 0;

		for (int i = 0; i < (sizeof(connectedControllers) / sizeof(Controller)); i++) {
			if (connectedControllers[i].ControllerStatus == ACTIVE) {
				RemoveDevice(i);
			}
		}
	}
	return TRUE;
}
