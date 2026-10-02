/*
FILE : DiscSource.cpp
PROJECT : Omni360
DESCRIPTION : The console's disc drive, read raw, for installing a disc as
              Games on Demand. See DiscSource.h.
*/

#include "DiscSource.h"

#include <xtl.h>
#include <stdio.h>
#include <string.h>

#include "driveMount.h" // STRING, OBJECT_ATTRIBUTES, IO_STATUS_BLOCK, NtOpenFile, NtClose

#define DISC_SECTOR      0x800UL
#define BOUNCE_SIZE      (1024UL * 1024)  // 512 sectors per read - one converter group is 816KB
#define READ_ATTEMPTS    4
#define FILE_FS_SIZE_INFORMATION_CLASS 3

// A full dual-layer XGD2 image, for when the drive won't say how big the disc
// is. Only bounds the partition search: the converter reads no further than
// the game's own file table says it uses.
#define FALLBACK_DISC_SIZE 7835492352ULL

typedef struct _FILE_FS_SIZE_INFORMATION
{
    LARGE_INTEGER TotalAllocationUnits;
    LARGE_INTEGER AvailableAllocationUnits;
    ULONG SectorsPerAllocationUnit;
    ULONG BytesPerSector;
} FILE_FS_SIZE_INFORMATION;

extern "C"
{
    NTSTATUS NtReadFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                        PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, DWORD Length, PLARGE_INTEGER ByteOffset);
    NTSTATUS NtQueryVolumeInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FsInformation,
                                          DWORD Length, DWORD FsInformationClass);
}

DiscSource::DiscSource()
    : handle(NULL), size(0), bounce(NULL), print(NULL), readErrorsLogged(0)
{
}

DiscSource::~DiscSource()
{
    Close();
}

void DiscSource::Close()
{
    if (handle != NULL)
    {
        NtClose((HANDLE)handle);
        handle = NULL;
    }
    if (bounce != NULL)
    {
        VirtualFree(bounce, 0, MEM_RELEASE);
        bounce = NULL;
    }
    size = 0;
}

bool DiscSource::Open(void (*printFn)(const char *format, ...))
{
    Close();
    print = printFn;
    readErrorsLogged = 0;

    // Page-aligned, which covers the device's sector alignment.
    bounce = (unsigned char *)VirtualAlloc(NULL, BOUNCE_SIZE, MEM_COMMIT, PAGE_READWRITE);
    if (bounce == NULL)
    {
        if (print) print("[disc] out of memory for the read buffer\n");
        return false;
    }

    STRING name;
    RtlInitAnsiString(&name, "\\Device\\Cdrom0");

    OBJECT_ATTRIBUTES attributes;
    attributes.RootDirectory = NULL;
    attributes.ObjectName = &name;
    attributes.Attributes = 0x40; // OBJ_CASE_INSENSITIVE

    IO_STATUS_BLOCK iosb;
    HANDLE h = NULL;
    NTSTATUS status = NtOpenFile(&h, GENERIC_READ | SYNCHRONIZE, &attributes, &iosb,
                                 FILE_SHARE_READ, FILE_SYNCHRONOUS_IO_NONALERT);
    if (status < 0)
    {
        if (print) print("[disc] couldn't open the drive: status 0x%08lX\n", (unsigned long)status);
        Close();
        return false;
    }
    handle = h;

    FILE_FS_SIZE_INFORMATION fs;
    memset(&fs, 0, sizeof(fs));
    status = NtQueryVolumeInformationFile(h, &iosb, &fs, sizeof(fs), FILE_FS_SIZE_INFORMATION_CLASS);
    if (status >= 0 && fs.TotalAllocationUnits.QuadPart > 0)
    {
        size = (unsigned long long)fs.TotalAllocationUnits.QuadPart * fs.SectorsPerAllocationUnit * fs.BytesPerSector;
        if (print) print("[disc] drive reports %I64u bytes (%I64u units x %lu sectors x %lu bytes)\n", size,
                         (unsigned long long)fs.TotalAllocationUnits.QuadPart,
                         (unsigned long)fs.SectorsPerAllocationUnit, (unsigned long)fs.BytesPerSector);
    }
    else
    {
        size = FALLBACK_DISC_SIZE;
        if (print) print("[disc] drive didn't report a size (status 0x%08lX); assuming %I64u\n",
                         (unsigned long)status, size);
    }

    // No disc, or one the drive can't read at all, fails right here rather
    // than as a puzzling "not a game disc" further on.
    if (!ReadSectors(0, DISC_SECTOR))
    {
        if (print) print("[disc] the first sector can't be read - no disc, or not a readable one\n");
        Close();
        return false;
    }

    return true;
}

bool DiscSource::ReadSectors(unsigned long long start, unsigned long bytes)
{
    for (int attempt = 0; attempt < READ_ATTEMPTS; ++attempt)
    {
        if (attempt > 0)
            Sleep(250 * attempt);

        LARGE_INTEGER at;
        at.QuadPart = (LONGLONG)start;
        IO_STATUS_BLOCK iosb;
        memset(&iosb, 0, sizeof(iosb));

        NTSTATUS status = NtReadFile((HANDLE)handle, NULL, NULL, NULL, &iosb, bounce, bytes, &at);
        if (status >= 0 && iosb.Information == bytes)
            return true;

        if (print != NULL && readErrorsLogged < 20)
        {
            readErrorsLogged++;
            print("[disc] read of %lu bytes at 0x%I64X failed (try %d): status 0x%08lX, got %lu\n",
                  bytes, start, attempt + 1, (unsigned long)status, (unsigned long)iosb.Information);
        }
    }
    return false;
}

bool DiscSource::ReadAt(unsigned long long offset, void *buffer, unsigned long len)
{
    if (handle == NULL)
        return false;

    unsigned char *out = (unsigned char *)buffer;

    while (len > 0)
    {
        unsigned long long start = offset & ~(unsigned long long)(DISC_SECTOR - 1);
        unsigned long skip = (unsigned long)(offset - start);

        unsigned long want = skip + len;
        want = (want + DISC_SECTOR - 1) & ~(DISC_SECTOR - 1);
        if (want > BOUNCE_SIZE)
            want = BOUNCE_SIZE;

        if (!ReadSectors(start, want))
            return false;

        unsigned long take = want - skip;
        if (take > len)
            take = len;

        memcpy(out, bounce + skip, take);
        out += take;
        offset += take;
        len -= take;
    }
    return true;
}

unsigned long long DiscSource::Size()
{
    return size;
}

void DiscSource::LogProbe(void (*printFn)(const char *format, ...))
{
    if (printFn == NULL || handle == NULL)
        return;

    static const struct { const char *name; unsigned long long offset; } kOffsets[] =
    {
        { "start of disc", 0x0ULL },
        { "XGD2 partition", 0xFD90000ULL },
        { "XGD3 partition", 0x2080000ULL },
        { "XGD1 partition", 0x18300000ULL },
    };

    for (size_t i = 0; i < sizeof(kOffsets) / sizeof(kOffsets[0]); ++i)
    {
        unsigned long long at = kOffsets[i].offset + 0x20 * DISC_SECTOR;
        if (at + DISC_SECTOR > size)
        {
            printFn("[disc] probe %-15s 0x%09I64X: past the end\n", kOffsets[i].name, at);
            continue;
        }

        unsigned char b[20];
        if (!ReadAt(at, b, sizeof(b)))
        {
            printFn("[disc] probe %-15s 0x%09I64X: read failed\n", kOffsets[i].name, at);
            continue;
        }

        char text[21];
        for (int j = 0; j < 20; ++j)
            text[j] = (b[j] >= 0x20 && b[j] < 0x7F) ? (char)b[j] : '.';
        text[20] = '\0';

        printFn("[disc] probe %-15s 0x%09I64X: %02X %02X %02X %02X %02X %02X %02X %02X  \"%s\"\n",
                kOffsets[i].name, at, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], text);
    }
}
