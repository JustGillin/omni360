/*
FILE : FolderBrowse.cpp
PROJECT : Omni360
DESCRIPTION : Lists drives and folders for browsing - see FolderBrowse.h.
*/

#include "FolderBrowse.h"
#include "Strings.h"
#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The ones main.cpp mounts at start (MountDrives): the hard drive, the
// three USB ports by Aurora's names, and the disc drive last.
static const char *const kDrives[] = { "Hdd1:", "HddX:", "Usb0:", "Usb1:", "Usb2:", "Dvd:" };

// HddX: the Original Xbox emulator's partition (XboxCompat.h mounts it) -
// worth reaching to swap emulator files, nowhere to keep a library.
static bool IsCompatPath(const char *path)
{
    return path != NULL && _strnicmp(path, "HddX:", 5) == 0;
}

bool IsDiscPath(const char *path)
{
    return path != NULL && _strnicmp(path, "Dvd:", 4) == 0;
}

const char *DriveKind(const char *drive)
{
    if (_stricmp(drive, "Hdd1:") == 0)
        return Tr(STR_HARD_DRIVE);
    if (_strnicmp(drive, "Usb", 3) == 0)
        return Tr(STR_USB_DRIVE);
    if (IsDiscPath(drive))
        return Tr(STR_DISC_DRIVE);
    if (IsCompatPath(drive))
        return Tr(STR_COMPAT_DRIVE);
    return NULL;
}

int ListDrives(FolderEntry *out, int max, bool all)
{
    int count = 0;
    for (int i = 0; i < (int)(sizeof(kDrives) / sizeof(kDrives[0])) && count < max; ++i)
    {
        if (!all && (IsDiscPath(kDrives[i]) || IsCompatPath(kDrives[i])))
            continue;
        // A port with nothing in it is mounted all the same, and reads as
        // missing here - which is what leaves it out.
        char root[16];
        _snprintf(root, sizeof(root), "%s\\", kDrives[i]);
        root[sizeof(root) - 1] = '\0';
        const DWORD attributes = GetFileAttributesA(root);
        if (attributes == (DWORD)-1 || !(attributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;

        FolderEntry &entry = out[count++];
        strcpy(entry.name, kDrives[i]);
        entry.folder = true;
        entry.size = 0;
    }
    return count;
}

static int CompareEntries(const void *a, const void *b)
{
    const FolderEntry *ea = (const FolderEntry *)a;
    const FolderEntry *eb = (const FolderEntry *)b;
    if (ea->folder != eb->folder)
        return ea->folder ? -1 : 1;
    return _stricmp(ea->name, eb->name);
}

int ListFolder(const char *path, bool files, FolderEntry *out, int max)
{
    char pattern[MAX_PATH + 8];
    _snprintf(pattern, sizeof(pattern), "%s\\*", path);
    pattern[sizeof(pattern) - 1] = '\0';

    WIN32_FIND_DATAA found;
    HANDLE h = FindFirstFileA(pattern, &found);
    if (h == INVALID_HANDLE_VALUE)
    {
        // An empty folder ends the same way on some drives: tell it from
        // one that can't be read by whether the folder is there at all. A
        // bare drive needs its root separator to be looked up.
        char probe[MAX_PATH + 8];
        const size_t len = strlen(path);
        _snprintf(probe, sizeof(probe), (len > 0 && path[len - 1] == ':') ? "%s\\" : "%s", path);
        probe[sizeof(probe) - 1] = '\0';
        const DWORD attributes = GetFileAttributesA(probe);
        return (attributes != (DWORD)-1 && (attributes & FILE_ATTRIBUTE_DIRECTORY)) ? 0 : -1;
    }

    int count = 0;
    do
    {
        const bool folder = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!folder && !files)
            continue;
        if (strcmp(found.cFileName, ".") == 0 || strcmp(found.cFileName, "..") == 0)
            continue;
        if (strlen(found.cFileName) >= FOLDER_ENTRY_NAME_SIZE)
            continue; // longer than FATX allows, so not one of ours to show

        FolderEntry &entry = out[count++];
        strcpy(entry.name, found.cFileName);
        entry.folder = folder;
        entry.size = folder ? 0 : (((unsigned long long)found.nFileSizeHigh << 32) | found.nFileSizeLow);
    } while (count < max && FindNextFileA(h, &found));
    FindClose(h);

    qsort(out, count, sizeof(FolderEntry), CompareEntries);
    return count;
}

bool FolderChild(const char *path, const char *name, char *out, size_t outSize)
{
    const int written = (path[0] == '\0') ? _snprintf(out, outSize, "%s", name)
                                          : _snprintf(out, outSize, "%s\\%s", path, name);
    if (written < 0 || (size_t)written >= outSize)
    {
        out[outSize - 1] = '\0';
        return false;
    }
    return true;
}

void FolderParent(const char *path, char *out, size_t outSize)
{
    _snprintf(out, outSize, "%s", path);
    out[outSize - 1] = '\0';
    char *slash = strrchr(out, '\\');
    if (slash != NULL)
        *slash = '\0'; // "Hdd1:\Games" -> "Hdd1:"
    else
        out[0] = '\0'; // a drive's parent is the list of drives
}
