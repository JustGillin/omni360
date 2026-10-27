/*
FILE : Launch.cpp
PROJECT : Omni360
DESCRIPTION : Starting games and other apps. See Launch.h.
*/

#include "Launch.h"
#include "OutputConsole.h" // dprintf

#include <xtl.h>
#include <stdio.h>
#include <string.h>
#include "driveMount.h" // mount, for the disc's drive

// NTSTATUSes, which xtl.h doesn't define.
extern "C" LONG XexGetModuleHandle(PSZ moduleName, PHANDLE outHandle);
extern "C" LONG XexGetProcedureAddress(HANDLE module, DWORD ordinal, PVOID *outAddress);

// Ends this title and starts the executable a package holds - xexName at its
// root - as the dashboard does for Games on Demand and arcade games. Not in
// the XDK, and xapilib has no import for it, so it's found in xam.xex by its
// ordinal: 0x257, by Xenia's table of xam's exports - which has
// XamContentLaunchImage at 0x263, as xapilib does. Two arguments, as Xenia
// has it.
typedef DWORD (*XamContentLaunchImageFromFileInternalFn)(LPCSTR packagePath, LPCSTR xexName);
#define XAM_CONTENT_LAUNCH_IMAGE_FROM_FILE_INTERNAL 0x257

static XamContentLaunchImageFromFileInternalFn FindLaunchFromFile()
{
    HANDLE xam = NULL;
    PVOID address = NULL;
    if (XexGetModuleHandle("xam.xex", &xam) < 0 || xam == NULL ||
        XexGetProcedureAddress(xam, XAM_CONTENT_LAUNCH_IMAGE_FROM_FILE_INTERNAL, &address) < 0)
        return NULL;
    return (XamContentLaunchImageFromFileInternalFn)address;
}

// The drives main.cpp mounts, by the devices behind them. A path handed to
// the loader is read after this title has ended, when its drive names are
// gone with it, so it goes by the device.
static const char *const kDrives[][2] = {
    { "Hdd1:", "\\Device\\Harddisk0\\Partition1" },
    { "Usb0:", "\\Device\\Mass0" },
    { "Usb1:", "\\Device\\Mass1" },
    { "Usb2:", "\\Device\\Mass2" },
};

// path by its device, into out. False if its drive isn't one of those.
static bool DevicePath(const char *path, char *out, size_t outSize)
{
    for (int i = 0; i < sizeof(kDrives) / sizeof(kDrives[0]); ++i)
    {
        const size_t len = strlen(kDrives[i][0]);
        if (_strnicmp(path, kDrives[i][0], len) == 0)
        {
            _snprintf(out, outSize, "%s%s", kDrives[i][1], path + len);
            out[outSize - 1] = '\0';
            return true;
        }
    }
    dprintf("[launch] %s isn't on a drive the loader can be given\n", path);
    return false;
}

static bool IsFile(const char *path)
{
    const DWORD attributes = GetFileAttributesA(path);
    return attributes != (DWORD)-1 && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static bool EndsWith(const char *s, const char *suffix)
{
    const size_t n = strlen(s), m = strlen(suffix);
    return n >= m && _stricmp(s + n - m, suffix) == 0;
}

static void LaunchExecutable(const char *path)
{
    char device[600];
    if (!DevicePath(path, device, sizeof(device)))
        return;
    dprintf("[launch] %s\n", device);
    XLaunchNewImage(device, 0);
    dprintf("[launch] the loader refused %s\n", device);
}

static void LaunchPackage(const char *path, const char *xexName)
{
    char device[600];
    if (!DevicePath(path, device, sizeof(device)))
        return;
    const XamContentLaunchImageFromFileInternalFn launch = FindLaunchFromFile();
    if (launch == NULL)
    {
        dprintf("[launch] xam.xex has no export %X to launch packages with\n", XAM_CONTENT_LAUNCH_IMAGE_FROM_FILE_INTERNAL);
        return;
    }
    dprintf("[launch] %s, its %s\n", device, xexName);
    const DWORD result = launch(device, xexName);
    dprintf("[launch] the loader refused %s: %08lX\n", device, result);
}

void LaunchInstalledGame(const InstalledGame &game)
{
    if (game.folder)
    {
        // Whichever the scan found it by.
        char exe[600];
        _snprintf(exe, sizeof(exe), "%s\\default.xex", game.packagePath);
        exe[sizeof(exe) - 1] = '\0';
        if (!IsFile(exe))
        {
            _snprintf(exe, sizeof(exe), "%s\\default.xbe", game.packagePath);
            exe[sizeof(exe) - 1] = '\0';
        }
        LaunchExecutable(exe);
        return;
    }

    // An Original Xbox game's package holds the disc's default.xbe.
    LaunchPackage(game.packagePath, game.contentType == 0x00005000 ? "default.xbe" : "default.xex");
}

void LaunchApp(const char *path)
{
    if (EndsWith(path, ".xex") || EndsWith(path, ".xbe"))
        LaunchExecutable(path);
    else
        LaunchPackage(path, "default.xex");
}

void LaunchDisc(bool original)
{
    // By a drive named for it, as FreeStyle Dash starts discs. Neither the
    // device's own path ("\Device\Cdrom0\default.xex" - back to the
    // dashboard) nor one from the root as the XDK documents ("default.xex" -
    // looked for beside Omni360, so "The game couldn't start") works.
    mount("Dvd:", "\\Device\\Cdrom0");
    const char *exe = original ? "dvd:\\default.xbe" : "dvd:\\default.xex";
    dprintf("[launch] %s\n", exe);
    XLaunchNewImage(exe, 0);
    dprintf("[launch] the loader refused %s\n", exe);
}

void LaunchXboxDashboard()
{
    dprintf("[launch] the Xbox Dashboard\n");
    XLaunchNewImage("\\SystemRoot\\dash.xex", 0);
    dprintf("[launch] the loader refused the Xbox Dashboard\n");
}
