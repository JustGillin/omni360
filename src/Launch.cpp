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
#include <ctype.h>
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

// A loose executable by its drive's name - "Hdd1:\Emulators\RetroArch\
// default.xex" - as a disc only starts by "dvd:\default.xex". The drive names
// main.cpp mounts are the kernel's, not this title's, so they're still there
// for the loader once Omni360 has ended; mounted again first all the same.
// Returns only if the loader refused it.
static void LaunchByDriveName(const char *path)
{
    for (int i = 0; i < sizeof(kDrives) / sizeof(kDrives[0]); ++i)
    {
        const size_t len = strlen(kDrives[i][0]);
        if (_strnicmp(path, kDrives[i][0], len) == 0)
        {
            mount(kDrives[i][0], (char *)kDrives[i][1]);
            dprintf("[launch] %s\n", path);
            XLaunchNewImage(path, 0);
            dprintf("[launch] the loader refused %s\n", path);
            return;
        }
    }
}

// An indie game isn't a program of its own: it's content the console's XNA
// launcher runs - a system file, updated by the Indie Games title's update
// (584E07D2\000B0000\tu32000100_00000000), which is why the update's number
// names its folder. The launcher learns which game from its launch data, as
// Aurora was seen to pass it (read from the launcher's memory over XBDM,
// 2026-11-24): this magic, the package's file name in lower case, and the
// content device it's on - 1, the hard drive.
//
// It has to be booted as a dashboard boots a disc - XamLoaderLaunchTitleEx,
// its folder as the mount - for the update to be applied: XLaunchNewImage
// starts the launcher as it shipped, which reads the same launch data and
// goes straight back to the Xbox Dashboard.
#define XNA_LAUNCHER_FOLDER "\\Device\\Harddisk0\\SystemExtPartition\\32000100"
#define XNA_LAUNCHER_PATH   XNA_LAUNCHER_FOLDER "\\Xna_TitleLauncher.xex"
#define XNA_LAUNCH_MAGIC    0xCAFEBABEUL

struct XnaLaunchData
{
    DWORD magic;       // XNA_LAUNCH_MAGIC
    char fileName[44]; // the package's, lower case: its content ID in hex
    DWORD deviceId;    // the content device: 1 for the hard drive
};

// XamLoaderLaunchTitleEx - see LaunchDisc.
typedef VOID (*XamLoaderLaunchTitleFn)(LPCSTR launchPath, LPCSTR mountPath, LPCSTR commandLine, DWORD flags);
static XamLoaderLaunchTitleFn FindLoaderLaunchTitle();

static void LaunchIndieGame(const InstalledGame &game)
{
    const char *slash = strrchr(game.packagePath, '\\');
    const char *file = slash != NULL ? slash + 1 : game.packagePath;

    XnaLaunchData data;
    memset(&data, 0, sizeof(data));
    data.magic = XNA_LAUNCH_MAGIC;
    for (size_t i = 0; file[i] != '\0' && i < sizeof(data.fileName) - 1; ++i)
        data.fileName[i] = (char)tolower((unsigned char)file[i]);
    data.deviceId = 1;
    if (_strnicmp(game.packagePath, "Hdd1:", 5) != 0)
        dprintf("[launch] %s isn't on the hard drive - the XNA launcher may not find it\n", game.packagePath);

    const DWORD set = XSetLaunchData(&data, sizeof(data));
    const XamLoaderLaunchTitleFn launchTitle = FindLoaderLaunchTitle();
    if (set != ERROR_SUCCESS || launchTitle == NULL)
    {
        dprintf("[launch] can't start the XNA launcher: launch data %08lX, XamLoaderLaunchTitleEx %s\n", set,
                launchTitle != NULL ? "found" : "missing");
        return;
    }
    dprintf("[launch] indie game %s, through the XNA launcher\n", data.fileName);
    launchTitle(XNA_LAUNCHER_PATH, XNA_LAUNCHER_FOLDER, NULL, 0);
    dprintf("[launch] the loader refused the XNA launcher\n");
}

void LaunchInstalledGame(const InstalledGame &game)
{
    if (IsIndieGame(INDIE_GAMES_TITLE_ID, game.contentType) && !game.folder)
    {
        LaunchIndieGame(game);
        return;
    }
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
        // By its drive's name first, then its device's path - the order an
        // app goes in, and for the same reason: a game kept as a folder that
        // Omni360 didn't install went back to the stock dashboard when the
        // device's path was all it was given.
        LaunchByDriveName(exe);
        LaunchExecutable(exe);
        return;
    }

    // An Original Xbox game's package holds the disc's default.xbe.
    LaunchPackage(game.packagePath, game.contentType == 0x00005000 ? "default.xbe" : "default.xex");
}

void LaunchApp(const char *path)
{
    if (EndsWith(path, ".xex") || EndsWith(path, ".xbe"))
    {
        // An app like RetroArch is a launcher that starts its cores through
        // game:\ - which went back to the stock dashboard when it was started
        // by its device's path. By its drive's name first, then that.
        LaunchByDriveName(path);
        LaunchExecutable(path);
    }
    else
    {
        LaunchPackage(path, "default.xex");
    }
}

// Ends this title and boots another from a device mounted as its root - as
// a dashboard boots a disc. Not in the XDK; xam's export 0x1A5 by Xenia's
// table, with the arguments xkelib gives it. Its sibling 0x1A4,
// XamLoaderLaunchTitle(path, flags), took "\Device\Cdrom0\default.xex" and
// booted the stock dashboard instead.
#define XAM_LOADER_LAUNCH_TITLE 0x1A5

static XamLoaderLaunchTitleFn FindLoaderLaunchTitle()
{
    HANDLE xam = NULL;
    PVOID address = NULL;
    if (XexGetModuleHandle("xam.xex", &xam) < 0 || xam == NULL ||
        XexGetProcedureAddress(xam, XAM_LOADER_LAUNCH_TITLE, &address) < 0)
        return NULL;
    return (XamLoaderLaunchTitleFn)address;
}

void LaunchDisc(bool original)
{
    // A 360 disc, booted as the dashboard boots one. XLaunchNewImage below
    // starts its default.xex, but as an executable run by path: the Arcade
    // compilation discs' menus came up that way and crashed starting any of
    // their games, where Aurora launching the same disc worked (issue #11).
    if (!original)
    {
        const XamLoaderLaunchTitleFn launchTitle = FindLoaderLaunchTitle();
        if (launchTitle != NULL)
        {
            dprintf("[launch] the disc, through XamLoaderLaunchTitleEx\n");
            // "game:\default.xex" here got "The game couldn't start" - looked
            // for in Omni360's own folder, it seems - so by the device.
            launchTitle("\\Device\\Cdrom0\\default.xex", "\\Device\\Cdrom0", NULL, 0);
            dprintf("[launch] XamLoaderLaunchTitleEx returned - trying dvd:\\default.xex\n");
        }
        else
            dprintf("[launch] xam.xex has no export %X - trying dvd:\\default.xex\n", XAM_LOADER_LAUNCH_TITLE);
    }

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
