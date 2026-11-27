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
    LogFlush();
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
    LogFlush();
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
            LogFlush();
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
    LogFlush();
    launchTitle(XNA_LAUNCHER_PATH, XNA_LAUNCHER_FOLDER, NULL, 0);
    dprintf("[launch] the loader refused the XNA launcher\n");
}

static void StartTrainerFor(unsigned long titleId); // the trainer experiment, below

void LaunchInstalledGame(const InstalledGame &game)
{
    if (IsIndieGame(INDIE_GAMES_TITLE_ID, game.contentType) && !game.folder)
    {
        LaunchIndieGame(game);
        return;
    }
    StartTrainerFor(game.titleId);
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

void LaunchDisc(bool original, bool asDisc)
{
    // A compilation disc, booted as the dashboard boots one. XLaunchNewImage
    // below starts its default.xex, but as an executable run by path: the
    // Arcade compilation discs' menus came up that way and crashed starting
    // any of their games, where Aurora launching the same disc worked (issue
    // #11). Only for those: on a disc the console won't boot itself, this call
    // never returns and the screen stays black (Darksiders, 2026-11-26).
    if (!original && asDisc)
    {
        const XamLoaderLaunchTitleFn launchTitle = FindLoaderLaunchTitle();
        if (launchTitle != NULL)
        {
            dprintf("[launch] the disc, through XamLoaderLaunchTitleEx\n");
            // "game:\default.xex" here got "The game couldn't start" - looked
            // for in Omni360's own folder, it seems - so by the device.
            LogFlush();
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
    LogFlush();
    XLaunchNewImage(exe, 0);
    dprintf("[launch] the loader refused %s\n", exe);
}

void LaunchXboxDashboard()
{
    dprintf("[launch] the Xbox Dashboard\n");
    LogFlush();
    XLaunchNewImage("\\SystemRoot\\dash.xex", 0);
    dprintf("[launch] the loader refused the Xbox Dashboard\n");
}

// ---------------------------------------------------------------------------
// Where DashLaunch goes when Omni360 ends
// ---------------------------------------------------------------------------

#define OMNI360_TITLE_ID 0xFFFF0360UL // XboxTLS2.vcxproj's TitleID

// DashLaunch's names for drives - and the kernel's - as the drives main.cpp
// mounts. Its "Usb:" is the first port.
static const char *const kDashLaunchDrives[][2] = {
    { "Hdd:", "Hdd1:" },
    { "Hdd1:", "Hdd1:" },
    { "Usb:", "Usb0:" },
    { "Usb0:", "Usb0:" },
    { "Usb1:", "Usb1:" },
    { "Usb2:", "Usb2:" },
    { "\\Device\\Harddisk0\\Partition1", "Hdd1:" },
    { "\\Device\\Mass0", "Usb0:" },
    { "\\Device\\Mass1", "Usb1:" },
    { "\\Device\\Mass2", "Usb2:" },
};

static unsigned long ReadBigEndian32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | p[3];
}

// The title ID in an XEX's execution info (optional header 0x00040006); 0
// if it can't be read.
static unsigned long XexFileTitleId(const char *path)
{
    FILE *f = NULL;
    if (fopen_s(&f, path, "rb") != 0 || f == NULL)
        return 0;
    unsigned long titleId = 0;
    unsigned char head[24];
    if (fread(head, 1, sizeof(head), f) == sizeof(head) && memcmp(head, "XEX2", 4) == 0)
    {
        const unsigned long fieldCount = ReadBigEndian32(head + 20);
        for (unsigned long i = 0; i < fieldCount && i < 0x400; ++i)
        {
            unsigned char field[8];
            if (fread(field, 1, sizeof(field), f) != sizeof(field))
                break;
            if (ReadBigEndian32(field) != 0x00040006UL)
                continue;
            unsigned char info[16];
            if (fseek(f, (long)ReadBigEndian32(field + 4), SEEK_SET) == 0 &&
                fread(info, 1, sizeof(info), f) == sizeof(info))
                titleId = ReadBigEndian32(info + 12);
            break;
        }
    }
    fclose(f);
    return titleId;
}

// launch.ini's Default=, from the first launch.ini DashLaunch would find -
// the USB ports' roots, then the hard drive's. Empty if there's none.
static void ReadDashLaunchDefault(char *out, size_t outSize)
{
    out[0] = '\0';
    static const char *const kIniPaths[] = { "Usb0:\\launch.ini", "Usb1:\\launch.ini", "Usb2:\\launch.ini",
                                             "Hdd1:\\launch.ini" };
    for (int i = 0; i < sizeof(kIniPaths) / sizeof(kIniPaths[0]); ++i)
    {
        FILE *f = NULL;
        if (fopen_s(&f, kIniPaths[i], "r") != 0 || f == NULL)
            continue;
        char line[512];
        bool inPaths = false;
        while (fgets(line, sizeof(line), f) != NULL)
        {
            char *s = line;
            while (*s == ' ' || *s == '\t')
                ++s;
            char *end = s + strlen(s);
            while (end > s && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ' || end[-1] == '\t'))
                *--end = '\0';
            if (*s == '[')
            {
                inPaths = _strnicmp(s, "[Paths]", 7) == 0;
                continue;
            }
            if (!inPaths || _strnicmp(s, "Default", 7) != 0)
                continue;
            char *value = s + 7;
            while (*value == ' ' || *value == '\t')
                ++value;
            if (*value != '=')
                continue;
            ++value;
            while (*value == ' ' || *value == '\t')
                ++value;
            _snprintf(out, outSize, "%s", value);
            out[outSize - 1] = '\0';
            break;
        }
        fclose(f);
        dprintf("[launch] %s: Default = \"%s\"\n", kIniPaths[i], out);
        return; // DashLaunch uses the first one it finds
    }
}

bool DefaultDashboardIsOmni360()
{
    // Only DashLaunch reads launch.ini. Without it - a console on another
    // exploit - nothing redirects the end of a title, so there's no
    // default to find.
    HANDLE dashLaunch = NULL;
    if (XexGetModuleHandle("launch.xex", &dashLaunch) < 0 || dashLaunch == NULL)
    {
        dprintf("[launch] DashLaunch isn't loaded: Exit stays\n");
        return false;
    }

    char value[512];
    ReadDashLaunchDefault(value, sizeof(value));
    if (value[0] == '\0')
        return false;
    for (int i = 0; i < sizeof(kDashLaunchDrives) / sizeof(kDashLaunchDrives[0]); ++i)
    {
        const size_t len = strlen(kDashLaunchDrives[i][0]);
        if (_strnicmp(value, kDashLaunchDrives[i][0], len) != 0)
            continue;
        char path[600];
        _snprintf(path, sizeof(path), "%s%s", kDashLaunchDrives[i][1], value + len);
        path[sizeof(path) - 1] = '\0';
        const unsigned long titleId = XexFileTitleId(path);
        dprintf("[launch] the default, %s, is title %08lX\n", path, titleId);
        return titleId == OMNI360_TITLE_ID;
    }
    return false;
}

// ---------------------------------------------------------------------------
// The Guide plugin
// ---------------------------------------------------------------------------
//
// Loaded by DashLaunch, which every console running Omni360 has: launch.xex
// exports "start a system module" (ordinal 2 - DL_ORDINALS_STARTSYSMOD),
// returning the kernel loader's NTSTATUS. Two ways that didn't work first:
// - the kernel's XexLoadImage from this title's thread: access denied
//   (C0000022);
// - XexLoadImageFromMemory on a system thread of our own: the thread was
//   made but never ran, and outlived Omni360 pointing into its memory.
// DashLaunch's code stays loaded, so nothing is left pointing at ours.
//
// It wants a device path, and game: is only this title's name for its
// folder, so the plugin is copied to a place whose device is known first.

typedef DWORD (*DLaunchStartSysModuleFn)(char *modulePath);
#define DL_ORDINAL_START_SYS_MODULE 2

#define GUIDE_PLUGIN_SECTION "guide" // the plugin, inside Omni360's own XEX (XboxTLS2.vcxproj)
#define GUIDE_PLUGIN_NAME   "Omni360Guide.xex"
#define GUIDE_PLUGIN_COPY   "Hdd1:\\Omni360\\Omni360Guide.xex"
#define GUIDE_PLUGIN_DEVICE "\\Device\\Harddisk0\\Partition1\\Omni360\\Omni360Guide.xex"

struct GuidePluginLoad
{
    DLaunchStartSysModuleFn start;
    char path[128];
    volatile LONG status;
    volatile LONG done;
};

// On a thread of this title's: if DashLaunch never answers, the thread goes
// when Omni360 does, rather than being left behind in the system.
static DWORD WINAPI GuidePluginLoadEntry(LPVOID parameter)
{
    GuidePluginLoad *job = (GuidePluginLoad *)parameter;
    const DWORD status = job->start(job->path);
    InterlockedExchange(&job->status, (LONG)status);
    InterlockedExchange(&job->done, 1);
    return 0;
}

// The plugin, from inside Omni360's XEX, to GUIDE_PLUGIN_COPY - unless the
// one there is the same already, so it isn't rewritten every start.
static bool WriteGuidePlugin()
{
    PVOID data = NULL;
    ULONG size = 0;
    if (!XGetModuleSection(GetModuleHandle(NULL), GUIDE_PLUGIN_SECTION, &data, &size) || data == NULL || size == 0)
    {
        dprintf("[guide] this build has no plugin inside it\n");
        return false;
    }

    FILE *in = NULL;
    if (fopen_s(&in, GUIDE_PLUGIN_COPY, "rb") == 0 && in != NULL)
    {
        // The same bytes? Then it's this version, written before.
        bool same = true;
        unsigned char buffer[16 * 1024];
        ULONG at = 0;
        size_t got;
        while (same && (got = fread(buffer, 1, sizeof(buffer), in)) > 0)
        {
            same = at + got <= size && memcmp(buffer, (const unsigned char *)data + at, got) == 0;
            at += (ULONG)got;
        }
        fclose(in);
        if (same && at == size)
            return true;
    }

    CreateDirectoryA("Hdd1:\\Omni360", NULL);
    FILE *out = NULL;
    if (fopen_s(&out, GUIDE_PLUGIN_COPY, "wb") != 0 || out == NULL)
    {
        dprintf("[guide] couldn't write %s\n", GUIDE_PLUGIN_COPY);
        return false;
    }
    const bool ok = fwrite(data, 1, size, out) == size && fclose(out) == 0;
    if (!ok)
        dprintf("[guide] couldn't write the plugin to %s\n", GUIDE_PLUGIN_COPY);
    else
        dprintf("[guide] wrote the plugin to %s (%lu bytes)\n", GUIDE_PLUGIN_COPY, (unsigned long)size);
    return ok;
}

// DashLaunch's StartSysModule on devicePath, on a thread of this title's, for
// up to 5 s. False if DashLaunch isn't there or didn't answer; else its
// NTSTATUS in *status.
static bool StartSysModule(const char *devicePath, DWORD *status, const char *tag)
{
    HANDLE dashLaunch = NULL;
    PVOID address = NULL;
    if (XexGetModuleHandle("launch.xex", &dashLaunch) < 0 || dashLaunch == NULL ||
        XexGetProcedureAddress(dashLaunch, DL_ORDINAL_START_SYS_MODULE, &address) < 0 || address == NULL)
    {
        dprintf("[%s] DashLaunch isn't loaded, or has no export %d\n", tag, DL_ORDINAL_START_SYS_MODULE);
        return false;
    }

    static GuidePluginLoad job; // the thread may outlive a timed-out wait
    if (job.start != NULL && job.done == 0)
    {
        dprintf("[%s] an earlier load is still waiting on DashLaunch\n", tag);
        return false;
    }
    job.start = (DLaunchStartSysModuleFn)address;
    _snprintf(job.path, sizeof(job.path), "%s", devicePath);
    job.path[sizeof(job.path) - 1] = '\0';
    job.status = 0;
    job.done = 0;

    HANDLE thread = CreateThread(NULL, 64 * 1024, GuidePluginLoadEntry, &job, 0, NULL);
    if (thread == NULL)
    {
        dprintf("[%s] couldn't start a thread to load %s\n", tag, devicePath);
        return false;
    }
    const DWORD started = GetTickCount();
    while (job.done == 0 && GetTickCount() - started < 5000)
        Sleep(10);
    CloseHandle(thread);
    if (job.done == 0)
    {
        dprintf("[%s] DashLaunch didn't answer within 5 s\n", tag);
        return false;
    }
    *status = (DWORD)job.status;
    return true;
}

// ---------------------------------------------------------------------------
// Trainers - an experiment (2026-11-27)
// ---------------------------------------------------------------------------
//
// Aurora's trainers (XYZEngine, by XYZMods) are DLL XEXs that run as system
// modules at their own base, 0x91020000. Loading one does nothing but
// describe it; its exports, read off one loaded by Aurora:
//   2  Start(TRAINER_MAGIC, cheat bits, 0): bit n turns cheat n on; starts
//      three system threads. Once a second the first compares the running
//      title with the trainer's (title ID, media ID, base version, disc,
//      title update) - 4 seconds of another title and it gives up - then the
//      others apply the cheats.
//   3  Stop(keepLoaded), 4 unload, 6 its info (the cheats), 8 running?
// It registers with Aurora's Nova if that's loaded, and carries on without.
// So, as Aurora does: load it, Start it, and start the game straight after.
//
// For the experiment it's Hdd1:\Omni360\Trainers\<title ID>\<any>.xex, with
// an optional cheats.txt holding the bits in hex (all of them on without).

typedef DWORD (*TrainerStartFn)(DWORD magic, DWORD cheats, DWORD unused);
#define TRAINER_MAGIC        0x41554F41
#define TRAINER_ORDINAL_START 2
#define TRAINER_FOLDER        "Hdd1:\\Omni360\\Trainers"
#define TRAINER_COPY          "Hdd1:\\Omni360\\Omni360Trainer.xex"
#define TRAINER_DEVICE        "\\Device\\Harddisk0\\Partition1\\Omni360\\Omni360Trainer.xex"
#define TRAINER_MODULE        "Omni360Trainer.xex"

static bool CopyFileBytes(const char *from, const char *to)
{
    FILE *in = NULL, *out = NULL;
    if (fopen_s(&in, from, "rb") != 0 || in == NULL)
        return false;
    if (fopen_s(&out, to, "wb") != 0 || out == NULL)
    {
        fclose(in);
        return false;
    }
    unsigned char buffer[16 * 1024];
    size_t got;
    bool ok = true;
    while (ok && (got = fread(buffer, 1, sizeof(buffer), in)) > 0)
        ok = fwrite(buffer, 1, got, out) == got;
    fclose(in);
    return fclose(out) == 0 && ok;
}

static void StartTrainerFor(unsigned long titleId)
{
    char folder[96];
    _snprintf(folder, sizeof(folder), "%s\\%08lX", TRAINER_FOLDER, titleId);
    folder[sizeof(folder) - 1] = '\0';

    char pattern[128];
    _snprintf(pattern, sizeof(pattern), "%s\\*.xex", folder);
    pattern[sizeof(pattern) - 1] = '\0';
    WIN32_FIND_DATAA found;
    HANDLE find = FindFirstFileA(pattern, &found);
    if (find == INVALID_HANDLE_VALUE)
        return; // none for this game - the usual case
    FindClose(find);
    char trainer[300];
    _snprintf(trainer, sizeof(trainer), "%s\\%s", folder, found.cFileName);
    trainer[sizeof(trainer) - 1] = '\0';

    DWORD cheats = 0xFFFFFFFF;
    char cheatsPath[128];
    _snprintf(cheatsPath, sizeof(cheatsPath), "%s\\cheats.txt", folder);
    cheatsPath[sizeof(cheatsPath) - 1] = '\0';
    FILE *file = NULL;
    if (fopen_s(&file, cheatsPath, "r") == 0 && file != NULL)
    {
        unsigned long bits = 0;
        if (fscanf_s(file, "%lx", &bits) == 1)
            cheats = (DWORD)bits;
        fclose(file);
    }

    // Two can't both be loaded at the one base: one already running - ours
    // from before, or Aurora's - stays for now.
    HANDLE loaded = NULL;
    if ((XexGetModuleHandle((PSZ)TRAINER_MODULE, &loaded) >= 0 && loaded != NULL) ||
        (XexGetModuleHandle("dll.aurora.trainer", &loaded) >= 0 && loaded != NULL))
    {
        dprintf("[trainer] one is loaded already (%p) - not starting %s\n", loaded, trainer);
        return;
    }

    CreateDirectoryA("Hdd1:\\Omni360", NULL);
    if (!CopyFileBytes(trainer, TRAINER_COPY))
    {
        dprintf("[trainer] couldn't copy %s to %s\n", trainer, TRAINER_COPY);
        return;
    }
    DWORD status = 0;
    if (!StartSysModule(TRAINER_DEVICE, &status, "trainer"))
        return;
    loaded = NULL;
    PVOID start = NULL;
    if (XexGetModuleHandle((PSZ)TRAINER_MODULE, &loaded) < 0 || loaded == NULL ||
        XexGetProcedureAddress(loaded, TRAINER_ORDINAL_START, &start) < 0 || start == NULL)
    {
        dprintf("[trainer] %s: load status %08lX, module %p, no Start export\n", trainer, (unsigned long)status, loaded);
        return;
    }
    const DWORD started = ((TrainerStartFn)start)(TRAINER_MAGIC, cheats, 0);
    dprintf("[trainer] %s loaded at %p, Start(cheats %08lX) -> %lu\n", trainer, loaded, (unsigned long)cheats,
            (unsigned long)started);
}

GuidePluginResult LoadGuidePlugin()
{
    HANDLE loaded = NULL;
    if (XexGetModuleHandle((PSZ)GUIDE_PLUGIN_NAME, &loaded) >= 0 && loaded != NULL)
    {
        dprintf("[guide] the plugin is loaded already (%p)\n", loaded);
        return GUIDE_PLUGIN_ALREADY;
    }
    // Aurora's own Guide plugin already has the Guide's Home tab: leave it be.
    HANDLE nova = NULL;
    if (XexGetModuleHandle("Nova.xex", &nova) >= 0 && nova != NULL)
    {
        dprintf("[guide] Aurora's Nova is loaded - not adding ours\n");
        return GUIDE_PLUGIN_NOVA;
    }

    HANDLE dashLaunch = NULL;
    if (XexGetModuleHandle("launch.xex", &dashLaunch) < 0 || dashLaunch == NULL)
    {
        dprintf("[guide] DashLaunch isn't loaded: no plugin\n");
        return GUIDE_PLUGIN_NO_DASHLAUNCH;
    }
    if (!WriteGuidePlugin())
        return GUIDE_PLUGIN_FAILED;

    DWORD status = 0;
    if (!StartSysModule(GUIDE_PLUGIN_DEVICE, &status, "guide"))
        return GUIDE_PLUGIN_FAILED;
    loaded = NULL;
    XexGetModuleHandle((PSZ)GUIDE_PLUGIN_NAME, &loaded);
    dprintf("[guide] DashLaunch loading %s: status %08lX, now loaded: %s (%p)\n", GUIDE_PLUGIN_DEVICE,
            (unsigned long)status, loaded != NULL ? "yes" : "no", loaded);
    return loaded != NULL ? GUIDE_PLUGIN_LOADED : GUIDE_PLUGIN_FAILED;
}
