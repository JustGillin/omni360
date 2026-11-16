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

// Queues the Hacked Xefu Pack from the ConsoleMods wiki to unpack into
// HddX:\Compatibility, as a homebrew install - see DownloadQueue.h. Its
// emulators have the whitelist taken out, so any Original Xbox game is
// tried rather than only those Microsoft listed. Returns the queue's answer
// as an EnqueueResult.
int QueueXefuPack();

#endif
