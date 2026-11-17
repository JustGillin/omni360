#ifndef XBOX_COMPAT_H
#define XBOX_COMPAT_H

// ---------------------------------------------------------------------------
// The Original Xbox emulator: whether it's set up, and setting it up
// ---------------------------------------------------------------------------
//
// The 360 plays Original Xbox games through an emulator kept on a partition
// of its own, HddX - xbox.xex and the xefu*.xex files under HddX:\
// Compatibility. Drives Microsoft sold come with both; third-party and
// reformatted ones usually have neither, and then an Original Xbox game
// fails to start without saying why. ConsoleMods' guide:
// https://consolemods.org/wiki/Xbox_360:Original_Xbox_Games

enum XboxCompatState
{
    XBOX_COMPAT_READY,
    XBOX_COMPAT_NO_PARTITION, // HddX couldn't be mounted - the drive has no compatibility partition
    XBOX_COMPAT_NO_FILES      // HddX is there, but the emulator files aren't
};

// Mounts HddX: by the kernel's name for the partition. False if the drive
// has none. main.cpp calls it at startup, so a restored install can write.
bool MountCompatPartition();

enum XboxFormatResult
{
    XBOX_FORMAT_OK,             // formatted, and HddX: mounts
    XBOX_FORMAT_RESTART,        // formatted, but it won't mount until the console restarts
    XBOX_FORMAT_ALREADY_THERE,  // HddX mounts, or has a FATX header - nothing was written
    XBOX_FORMAT_NO_DEVICE,      // the kernel has no compatibility partition to open
    XBOX_FORMAT_WRONG_SIZE,     // it isn't the 256MB the layout is for - nothing was written
    XBOX_FORMAT_FAILED          // the write failed
};

// Writes an empty FATX volume to the compatibility partition, as HDD
// Compatibility Fixer does: through its own device, so nothing outside it
// can be touched, and only when it holds no FATX volume already - a working
// HddX is never formatted. Erases whatever was in the partition, which on a
// drive without a working HddX is nothing anyone can reach.
XboxFormatResult FormatCompatPartition();

// After a format that needs a restart: leaves Hdd1:\Omni360-xefu-pending.txt
// so the next startup queues the emulator files without being asked.
void MarkXefuPackPending();

// At startup, once the download queue is up: if the marker is there, HddX
// mounts and the emulator files are missing, queues them. The marker goes
// either way, so a drive that still won't mount doesn't retry every start.
// True when it queued them.
bool QueueXefuPackIfPending();

// Mounts HddX: and looks for the emulator. missing (may be NULL) gets the
// first file that isn't there, for the log and the alert.
XboxCompatState CheckXboxCompat(char *missing, int missingSize);

// Whether HddX has the Hacked Xefu Pack's emulators, not only Microsoft's:
// its newer xefu2019/xefu2021 builds, which no retail drive ever had. Those
// are what try a game off Microsoft's list - with only the retail files,
// xbox.xex refuses it and the game doesn't start.
bool XboxEmulatorIsHacked();

// Queues the Hacked Xefu Pack from the ConsoleMods wiki to unpack into
// HddX:\Compatibility, as a homebrew install - see DownloadQueue.h. Its
// emulators have the whitelist taken out, so any Original Xbox game is
// tried rather than only those Microsoft listed. Returns the queue's answer
// as an EnqueueResult.
int QueueXefuPack();

// ---------------------------------------------------------------------------
// Choosing the emulator for one game
// ---------------------------------------------------------------------------
//
// xbox.xex starts a game on Microsoft's list with the xefu file the list
// names, and any other on its fallback, which often isn't the one that
// works. Nothing tells it otherwise, so a game off the list gets its
// emulator the way ConsoleMods' Xefu Spoofer does it: the chosen file is
// copied over all eight xefu names, and its xefutitle over theirs, with the
// originals kept in HddX:\Compatibility\Omni360Backup. Every Original Xbox
// game runs on it until RestoreXboxEmulator puts them back, at the next start.

// The twelve emulators, in ConsoleMods' column order - XefuChoices.h's.
enum
{
    XEFU_1, XEFU_2, XEFU_3, XEFU_5, XEFU_1_1, XEFU_6, XEFU_7, XEFU_7B,
    XEFU_2019, XEFU_2021A, XEFU_2021B, XEFU_2021C,
    XEFU_COUNT
};

// The best emulator for a game off Microsoft's list, by ConsoleMods'
// compatibility list (tools/make_xefu_choices.py). False if it has none.
// emulator gets XEFU_*, config whether it's xefu7 with its config loader,
// and ratings (may be NULL) each emulator's: '0' untested, '1' doesn't work,
// '2' stops at the intro, '3' menus, '4' in-game, '5' playable.
bool XefuChoiceFor(unsigned long titleId, int *emulator, bool *config, char ratings[XEFU_COUNT + 1]);

// What players have reported going wrong with a game off Microsoft's list,
// from the same list, in English, its emulators named "Emulator 7" as the
// chooser names them. NULL if nothing is.
const char *XefuKnownIssues(unsigned long titleId);

// How far players got with a game off Microsoft's list on its best
// emulator, by the same list: 1 doesn't work, 2 stops at the intro, 3 at
// the menus, 4 in-game, 5 playable. 0 if nobody has tried it, or the list
// hasn't got it.
int XefuBestRating(unsigned long titleId);

// Puts emulator (XEFU_*) in place for every game, backing up the originals
// first. With config, xefu7's config loader, which reads ConsoleMods' file
// for the game from HddX:\Compatibility\Configs. False if a copy failed;
// what was copied stays until RestoreXboxEmulator.
bool SetXboxEmulator(int emulator, bool config);

// Puts the original emulator files back, if SetXboxEmulator changed them.
// At startup, and before a game that wants the usual ones. True if it had to.
bool RestoreXboxEmulator();

#endif
