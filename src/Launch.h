#ifndef LAUNCH_H
#define LAUNCH_H

#include "StfsParser.h" // InstalledGame

// ---------------------------------------------------------------------------
// Starting games and other apps
// ---------------------------------------------------------------------------
//
// Each of these ends Omni360 when it works, and never returns; when it
// returns, nothing was started and it says why in the log. What the console
// comes back to when the game ends is DashLaunch's choice, not Omni360's.

// A library game: a Games on Demand, arcade or Original Xbox package, by the
// Xam call the dashboard launches content packages with, or a folder game's
// default.xex or default.xbe.
void LaunchInstalledGame(const InstalledGame &game);

// An app from settings.txt's "app:" lines: an .xex or .xbe file, or a
// package (XeXMenu is one), started as a library game's would be.
void LaunchApp(const char *path);

// The game disc in the drive - default.xex, or an Original Xbox disc's
// default.xbe - as the dashboard starts one.
// asDisc boots it through the console's loader, as the dashboard does - which
// an Arcade compilation disc's menu needs (issue #11), but a disc the console
// itself won't boot (a backup: "Unrecognized Disc" from the stock Guide) never
// comes back from, leaving a black screen. Everything else starts its
// default.xex directly, as Omni360 did before.
void LaunchDisc(bool original, bool asDisc);

// The Xbox Dashboard itself, past DashLaunch's default - by its path rather
// than XLaunchNewImage's dashboard keyword, which DashLaunch sends to
// whatever it's set to start.
void LaunchXboxDashboard();

// Whether DashLaunch's launch.ini starts Omni360 itself - its Default= an XEX
// with Omni360's title ID, wherever that copy is - so ending the app would
// only open it again. False when there's no launch.ini, no Default, or the
// file can't be read.
bool DefaultDashboardIsOmni360();

// The Guide plugin (GuidePlugin/), carried inside Omni360's XEX, written to
// Hdd1:\Omni360 and loaded into the system through DashLaunch - as Aurora
// loads its own - so it outlives Omni360 until the console is turned off.
// It turns the Guide's Xbox Home into Omni360 Home. Settings' switch decides
// whether it's loaded at start. Says how it went in the log, and returns it.
enum GuidePluginResult
{
    GUIDE_PLUGIN_LOADED,      // loaded now
    GUIDE_PLUGIN_ALREADY,     // loaded earlier this boot
    GUIDE_PLUGIN_NOVA,        // Aurora's Nova has the Guide: ours stays out
    GUIDE_PLUGIN_NO_DASHLAUNCH,
    GUIDE_PLUGIN_FAILED
};
GuidePluginResult LoadGuidePlugin();

// Loads a system module - a plugin or a trainer - through DashLaunch's
// StartSysModule, given its device path (\Device\Harddisk0\...). False
// if DashLaunch isn't there or didn't answer within 5 s; else its NTSTATUS
// in *status. tag prefixes the log lines.
bool StartSystemModule(const char *devicePath, unsigned long *status, const char *tag);

#endif
