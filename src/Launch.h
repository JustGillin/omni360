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
void LaunchDisc(bool original);

// The Xbox Dashboard itself, past DashLaunch's default - by its path rather
// than XLaunchNewImage's dashboard keyword, which DashLaunch sends to
// whatever it's set to start.
void LaunchXboxDashboard();

// Whether DashLaunch's launch.ini starts Omni360 itself - its Default= an XEX
// with Omni360's title ID, wherever that copy is - so ending the app would
// only open it again. False when there's no launch.ini, no Default, or the
// file can't be read.
bool DefaultDashboardIsOmni360();

#endif
