/*
FILE : XboxCompat.cpp
PROJECT : Omni360
DESCRIPTION : The Original Xbox emulator's partition and files. See XboxCompat.h.
*/

#include "XboxCompat.h"
#include "DownloadQueue.h" // EnqueueHomebrew
#include "OutputConsole.h" // dprintf
#include "driveMount.h"    // mount

#include <xtl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// The pack's own zip on the wiki. Its files sit under
// Hacked_Xefu_Pack\Compatibility, and only what's in Compatibility is kept.
#define XEFU_PACK_URL "https://consolemods.org/wiki/images/9/9d/Hacked_Xefu_Pack.zip"

// What the kernel calls the compatibility partition: SystemPartition, on a
// retail console with a working HddX (2026-11-15). Only that. Partition2 was
// tried after it once, as a guess, and is a different partition that mounts
// - with HddX's header wiped, HddX: landed on it, and the emulator files
// would have been unpacked there.
static const char *const kCompatDevices[] = {
    "\\Device\\Harddisk0\\SystemPartition",
};

// Every pack, retail or hacked, has these; xbox.xex picks among the rest.
static const char *const kEmulatorFiles[] = {
    "xbox.xex", "xefu.xex", "xefu1_1.xex", "xefu2.xex", "xefu3.xex",
    "xefu5.xex", "xefu6.xex", "xefu7.xex", "xefu7b.xex",
};

static bool Exists(const char *path)
{
    return GetFileAttributesA(path) != (DWORD)-1;
}

// Whether the drive's root can be listed. An empty one - a partition just
// formatted - has nothing to find, which still counts.
static bool RootReadable(const char *drive)
{
    char pattern[16];
    _snprintf(pattern, sizeof(pattern), "%s\\*", drive);
    pattern[sizeof(pattern) - 1] = '\0';
    WIN32_FIND_DATAA data;
    const HANDLE find = FindFirstFileA(pattern, &data);
    if (find != INVALID_HANDLE_VALUE)
    {
        FindClose(find);
        return true;
    }
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_NO_MORE_FILES;
}

// HddX: by whichever device answers. A symbolic link is made whether or not
// its device exists, so it counts once the drive's root can be read.
bool MountCompatPartition()
{
    static int found = -1;
    if (found >= 0)
    {
        mount("HddX:", (char *)kCompatDevices[found]);
        if (RootReadable("HddX:"))
            return true;
    }
    for (int i = 0; i < sizeof(kCompatDevices) / sizeof(kCompatDevices[0]); ++i)
    {
        mount("HddX:", (char *)kCompatDevices[i]);
        if (RootReadable("HddX:"))
        {
            if (found != i)
                dprintf("[xbox] HddX: is %s\n", kCompatDevices[i]);
            found = i;
            return true;
        }
    }
    dprintf("[xbox] no compatibility partition - none of its device names opened\n");
    return false;
}

XboxCompatState CheckXboxCompat(char *missing, int missingSize)
{
    if (missing != NULL && missingSize > 0)
        missing[0] = '\0';

    if (!MountCompatPartition())
        return XBOX_COMPAT_NO_PARTITION;

    for (int i = 0; i < sizeof(kEmulatorFiles) / sizeof(kEmulatorFiles[0]); ++i)
    {
        char path[64];
        _snprintf(path, sizeof(path), "HddX:\\Compatibility\\%s", kEmulatorFiles[i]);
        path[sizeof(path) - 1] = '\0';
        if (!Exists(path))
        {
            dprintf("[xbox] the emulator isn't set up: no %s\n", path);
            if (missing != NULL && missingSize > 0)
            {
                _snprintf(missing, missingSize, "%s", kEmulatorFiles[i]);
                missing[missingSize - 1] = '\0';
            }
            return XBOX_COMPAT_NO_FILES;
        }
    }
    return XBOX_COMPAT_READY;
}

extern "C" NTSTATUS NtReadFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                               PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, DWORD Length,
                               PLARGE_INTEGER ByteOffset);

// The compatibility partition's FATX layout, read off a retail drive's
// working HddX (2026-11-15):
//
//   0x0000  4KB   header: "XTAF", volume ID, 0x20 sectors a cluster, the
//                 root at cluster 1, two zero bytes, then 0xFF
//   0x1000  36KB  FAT16: (16384 clusters + 1) x 2 bytes, rounded up to 4KB.
//                 Entry 0 is FFF8, entry 1 - the root - FFFF
//   0xA000  16KB  the root folder, cluster 1: all 0xFF when it's empty
//
// A format is those 56KB and nothing else - the data clusters past them are
// free once the FAT says so.
#define HDDX_SIZE          0x10000000UL // 256MB
#define HDDX_CLUSTER       0x4000UL     // 32 sectors of 512 bytes
#define HDDX_FAT_OFFSET    0x1000UL
#define HDDX_FAT_SIZE      0x9000UL
#define HDDX_ROOT_OFFSET   (HDDX_FAT_OFFSET + HDDX_FAT_SIZE)
#define HDDX_FORMAT_BYTES  (HDDX_ROOT_OFFSET + HDDX_CLUSTER)

// The partition's own device - offsets from its start, and the kernel
// refusing anything past its end - rather than the whole disk at an offset
// worked out here, so a mistake can't reach Hdd1.
static HANDLE OpenCompatDevice(ACCESS_MASK access)
{
    STRING name;
    RtlInitAnsiString(&name, (PCHAR)kCompatDevices[0]);
    OBJECT_ATTRIBUTES attributes;
    attributes.RootDirectory = NULL;
    attributes.ObjectName = &name;
    attributes.Attributes = 0x40; // OBJ_CASE_INSENSITIVE
    IO_STATUS_BLOCK io;
    HANDLE device = NULL;
    const NTSTATUS status = NtOpenFile(&device, access | SYNCHRONIZE, &attributes, &io,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_SYNCHRONOUS_IO_NONALERT);
    if (status < 0)
    {
        dprintf("[xbox] couldn't open %s: %08lX\n", kCompatDevices[0], status);
        return NULL;
    }
    return device;
}

static bool ReadSector(HANDLE device, unsigned long offset, unsigned char *buffer)
{
    IO_STATUS_BLOCK io;
    LARGE_INTEGER at;
    at.QuadPart = offset;
    return NtReadFile(device, NULL, NULL, NULL, &io, buffer, 0x200, &at) >= 0 && io.Information == 0x200;
}

static void PutBE32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

XboxFormatResult FormatCompatPartition()
{
    // Never over a partition that works: whatever is on it stays.
    if (MountCompatPartition())
    {
        dprintf("[xbox] format: HddX already mounts - left alone\n");
        return XBOX_FORMAT_ALREADY_THERE;
    }

    HANDLE device = OpenCompatDevice(GENERIC_READ | GENERIC_WRITE);
    if (device == NULL)
        return XBOX_FORMAT_NO_DEVICE;

    XboxFormatResult result = XBOX_FORMAT_OK;
    unsigned char sector[0x200];
    unsigned char *image = NULL;

    // The size this layout is for, checked rather than assumed: the last
    // sector reads, and the one past it doesn't.
    if (!ReadSector(device, HDDX_SIZE - 0x200, sector) || ReadSector(device, HDDX_SIZE, sector))
    {
        dprintf("[xbox] format: %s isn't the 256MB this layout is for\n", kCompatDevices[0]);
        result = XBOX_FORMAT_WRONG_SIZE;
    }
    // And never over a FATX volume, even one that didn't mount.
    else if (!ReadSector(device, 0, sector) || memcmp(sector, "XTAF", 4) == 0)
    {
        dprintf("[xbox] format: the partition already has a FATX header - left alone\n");
        result = XBOX_FORMAT_ALREADY_THERE;
    }
    else if ((image = (unsigned char *)malloc(HDDX_FORMAT_BYTES)) == NULL)
        result = XBOX_FORMAT_FAILED;
    else
    {
        memset(image, 0xFF, HDDX_FORMAT_BYTES);

        // Header.
        memcpy(image, "XTAF", 4);
        PutBE32(image + 4, GetTickCount() ^ 0x4B584AE9); // a volume ID of its own
        PutBE32(image + 8, HDDX_CLUSTER / 0x200);
        PutBE32(image + 12, 1);
        image[16] = 0;
        image[17] = 0;

        // FAT: free throughout, but for the media entry and the root.
        unsigned char *fat = image + HDDX_FAT_OFFSET;
        memset(fat, 0, HDDX_FAT_SIZE);
        fat[0] = 0xFF;
        fat[1] = 0xF8;
        fat[2] = 0xFF;
        fat[3] = 0xFF;

        // The root folder stays all 0xFF: no entries.

        IO_STATUS_BLOCK io;
        LARGE_INTEGER at;
        at.QuadPart = 0;
        const NTSTATUS status = NtWriteFile(device, NULL, NULL, NULL, &io, image, HDDX_FORMAT_BYTES, &at);
        if (status < 0 || io.Information != HDDX_FORMAT_BYTES)
        {
            dprintf("[xbox] format: write failed: %08lX\n", status);
            result = XBOX_FORMAT_FAILED;
        }
        else
            dprintf("[xbox] format: wrote %lu bytes to %s\n", HDDX_FORMAT_BYTES, kCompatDevices[0]);
    }

    free(image);
    NtClose(device);

    // The kernel may still have it down as unreadable until a restart.
    if (result == XBOX_FORMAT_OK && !MountCompatPartition())
    {
        dprintf("[xbox] format: done, but HddX won't mount until a restart\n");
        result = XBOX_FORMAT_RESTART;
    }
    return result;
}

#define XEFU_PENDING_PATH "Hdd1:\\Omni360-xefu-pending.txt"

void MarkXefuPackPending()
{
    FILE *f = fopen(XEFU_PENDING_PATH, "w");
    if (f != NULL)
    {
        fputs("Omni360 queues the Original Xbox emulator files at its next start.\n", f);
        fclose(f);
    }
}

bool QueueXefuPackIfPending()
{
    if (!Exists(XEFU_PENDING_PATH))
        return false;
    remove(XEFU_PENDING_PATH);

    if (CheckXboxCompat(NULL, 0) != XBOX_COMPAT_NO_FILES)
    {
        dprintf("[xbox] emulator files were pending, but HddX %s\n",
                MountCompatPartition() ? "already has them" : "still doesn't mount");
        return false;
    }
    const int queued = QueueXefuPack();
    dprintf("[xbox] queued the pending emulator files: %d\n", queued);
    return queued == ENQUEUE_ADDED || queued == ENQUEUE_ALREADY_QUEUED;
}

int QueueXefuPack()
{
    if (!MountCompatPartition())
        return ENQUEUE_UNAVAILABLE;

    HomebrewJobSpec spec;
    memset(&spec, 0, sizeof(spec));
    _snprintf(spec.title, sizeof(spec.title) - 1, "Original Xbox emulator");
    _snprintf(spec.urls[0], sizeof(spec.urls[0]) - 1, "%s", XEFU_PACK_URL);
    spec.urlCount = 1;
    _snprintf(spec.kindFolder, sizeof(spec.kindFolder) - 1, "HddX:");
    _snprintf(spec.folder, sizeof(spec.folder) - 1, "Compatibility");
    return (int)EnqueueHomebrew(spec);
}
