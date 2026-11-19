/*
FILE : TitleUpdates.cpp
PROJECT : Omni360
DESCRIPTION : A game's installed title updates, and switching between them -
              see TitleUpdates.h.
*/

#include "TitleUpdates.h"
#include "OutputConsole.h"
#include "parsing.h" // LogEscapePercent
#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const kTuDrives[] = { "Hdd1", "Usb0", "Usb1", "Usb2" };
#define TU_DRIVE_COUNT ((int)(sizeof(kTuDrives) / sizeof(kTuDrives[0])))

#define TU_ASIDE_ROOT "Omni360\\Title updates"
#define TU_NUMBERS_FILE "Hdd1:\\" TU_ASIDE_ROOT "\\numbers.txt" // "<content ID> <number>" a line

static void MakeFolders(const char *path);

static void LogPath(const char *what, const char *path)
{
    char escaped[600];
    dprintf("[updates] %s %s\n", what, LogEscapePercent(path, escaped, sizeof(escaped)) ? escaped : "(long path)");
}

// The header fields wanted from a package, without its images: title ID at
// 0x360, content type at 0x344, version at 0x358, the UTF-16BE display name
// at 0x411. Read straight - the console is big-endian, as the header is.
static bool ReadUpdateHeader(const char *path, unsigned long *titleId, unsigned long *version, char *contentId,
                             char *name, size_t nameSize, unsigned long long *size)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    unsigned char header[0x500];
    DWORD got = 0;
    const bool read = ReadFile(h, header, sizeof(header), &got, NULL) && got == sizeof(header);
    DWORD high = 0;
    const DWORD low = GetFileSize(h, &high);
    CloseHandle(h);
    if (!read)
        return false;
    if (memcmp(header, "LIVE", 4) != 0 && memcmp(header, "CON ", 4) != 0 && memcmp(header, "PIRS", 4) != 0)
        return false;

    const unsigned long contentType = *(const unsigned long *)(header + 0x344);
    if (contentType != 0x000B0000)
        return false;
    *version = *(const unsigned long *)(header + 0x358);
    *titleId = *(const unsigned long *)(header + 0x360);
    *size = ((unsigned long long)high << 32) | low;
    for (int i = 0; i < 20; ++i)
        _snprintf(contentId + i * 2, 3, "%02X", header[0x32C + i]);
    contentId[40] = '\0';

    // UTF-16BE to UTF-8 - the names have a ™ now and then.
    size_t o = 0;
    for (int i = 0; i < 0x80 / 2 && o + 4 < nameSize; ++i)
    {
        const unsigned int c = (header[0x411 + i * 2] << 8) | header[0x411 + i * 2 + 1];
        if (c == 0)
            break;
        if (c < 0x80)
            name[o++] = (char)c;
        else if (c < 0x800)
        {
            name[o++] = (char)(0xC0 | (c >> 6));
            name[o++] = (char)(0x80 | (c & 0x3F));
        }
        else
        {
            name[o++] = (char)(0xE0 | (c >> 12));
            name[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            name[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    name[o] = '\0';
    // Some have doubled spaces ("Just Cause 2  Title Update #1").
    for (char *p = name; *p != '\0'; ++p)
    {
        while (p[0] == ' ' && p[1] == ' ')
            memmove(p, p + 1, strlen(p));
    }
    return true;
}

static void AddUpdate(InstalledTitleUpdate *out, int max, int *count, const char *path, const char *drive, bool on,
                      unsigned long wantTitle, bool checkTitle)
{
    if (*count >= max)
        return;
    InstalledTitleUpdate &u = out[*count];
    unsigned long titleId = 0;
    if (!ReadUpdateHeader(path, &titleId, &u.version, u.contentId, u.name, sizeof(u.name), &u.size))
        return;
    if (checkTitle && titleId != wantTitle)
        return;
    _snprintf(u.path, sizeof(u.path), "%s", path);
    u.path[sizeof(u.path) - 1] = '\0';
    _snprintf(u.drive, sizeof(u.drive), "%s", drive);
    u.drive[sizeof(u.drive) - 1] = '\0';
    u.on = on;
    u.number = 0;
    (*count)++;
}

// Every file in folder (matching pattern) as an update of the title's.
static void AddFolder(InstalledTitleUpdate *out, int max, int *count, const char *folder, const char *pattern,
                      const char *drive, bool on, unsigned long titleId, bool checkTitle)
{
    char search[300];
    _snprintf(search, sizeof(search), "%s\\%s", folder, pattern);
    search[sizeof(search) - 1] = '\0';
    WIN32_FIND_DATAA found;
    HANDLE h = FindFirstFileA(search, &found);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do
    {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        char path[300];
        _snprintf(path, sizeof(path), "%s\\%s", folder, found.cFileName);
        path[sizeof(path) - 1] = '\0';
        AddUpdate(out, max, count, path, drive, on, titleId, checkTitle);
    } while (FindNextFileA(h, &found));
    FindClose(h);
}

static int CompareUpdates(const void *a, const void *b)
{
    const InstalledTitleUpdate &x = *(const InstalledTitleUpdate *)a;
    const InstalledTitleUpdate &y = *(const InstalledTitleUpdate *)b;
    if (x.on != y.on)
        return x.on ? -1 : 1;
    if (x.version != y.version)
        return x.version > y.version ? -1 : 1;
    return _stricmp(x.path, y.path);
}

// The remembered numbers file, whole; empty if there's none.
static char g_numbers[16384];

static void LoadNumbers()
{
    g_numbers[0] = '\0';
    HANDLE h = CreateFileA(TU_NUMBERS_FILE, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD got = 0;
    if (!ReadFile(h, g_numbers, sizeof(g_numbers) - 1, &got, NULL))
        got = 0;
    g_numbers[got] = '\0';
    CloseHandle(h);
}

// The number remembered for contentId, from g_numbers; 0 if none.
static unsigned long NumberFor(const char *contentId)
{
    if (contentId[0] == '\0')
        return 0;
    for (const char *line = g_numbers; *line != '\0';)
    {
        if (_strnicmp(line, contentId, 40) == 0 && line[40] == ' ')
            return strtoul(line + 41, NULL, 10);
        const char *next = strchr(line, '\n');
        if (next == NULL)
            break;
        line = next + 1;
    }
    return 0;
}

void RememberTitleUpdateNumber(const char *contentId, unsigned long number)
{
    if (contentId == NULL || strlen(contentId) != 40 || number == 0)
        return;
    LoadNumbers();
    if (NumberFor(contentId) == number)
        return;
    MakeFolders("Hdd1:\\" TU_ASIDE_ROOT);
    HANDLE h = CreateFileA(TU_NUMBERS_FILE, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return;
    char line[64];
    const int n = _snprintf(line, sizeof(line), "%s %lu\r\n", contentId, number);
    DWORD wrote = 0;
    SetFilePointer(h, 0, NULL, FILE_END);
    if (n > 0)
        WriteFile(h, line, (DWORD)n, &wrote, NULL);
    CloseHandle(h);
}

int ListTitleUpdates(unsigned long titleId, InstalledTitleUpdate *out, int max)
{
    int count = 0;
    for (int d = 0; d < TU_DRIVE_COUNT; ++d)
    {
        const char *drive = kTuDrives[d];
        char folder[200];
        // On: Content's 000B0000 for this title, and Cache's TU_ files whose
        // header says it's this title's.
        _snprintf(folder, sizeof(folder), "%s:\\Content\\0000000000000000\\%08lX\\000B0000", drive, titleId);
        AddFolder(out, max, &count, folder, "*", drive, true, titleId, false);
        _snprintf(folder, sizeof(folder), "%s:\\Cache", drive);
        AddFolder(out, max, &count, folder, "TU_*", drive, true, titleId, true);
        // Set aside.
        _snprintf(folder, sizeof(folder), "%s:\\" TU_ASIDE_ROOT "\\%08lX\\Content", drive, titleId);
        AddFolder(out, max, &count, folder, "*", drive, false, titleId, false);
        _snprintf(folder, sizeof(folder), "%s:\\" TU_ASIDE_ROOT "\\%08lX\\Cache", drive, titleId);
        AddFolder(out, max, &count, folder, "*", drive, false, titleId, false);
    }
    LoadNumbers();
    for (int i = 0; i < count; ++i)
        out[i].number = NumberFor(out[i].contentId);
    qsort(out, count, sizeof(InstalledTitleUpdate), CompareUpdates);
    return count;
}

// Makes each folder along path, as CreateDirectory makes only the last.
static void MakeFolders(const char *path)
{
    char partial[300];
    _snprintf(partial, sizeof(partial), "%s", path);
    partial[sizeof(partial) - 1] = '\0';
    for (char *p = strchr(partial, '\\'); p != NULL; p = strchr(p + 1, '\\'))
    {
        *p = '\0';
        if (strchr(partial, '\\') != NULL) // not the bare drive
            CreateDirectoryA(partial, NULL);
        *p = '\\';
    }
    CreateDirectoryA(partial, NULL);
}

static const char *FileName(const char *path)
{
    const char *slash = strrchr(path, '\\');
    return slash != NULL ? slash + 1 : path;
}

// Whether an on update lives in Cache rather than Content.
static bool InCache(const char *path)
{
    return strstr(path, ":\\Cache\\") != NULL;
}

// Where a set-aside update goes back to, from the folder it was set aside in.
static bool OnPath(unsigned long titleId, const InstalledTitleUpdate &u, char *out, size_t outSize, char *folder,
                   size_t folderSize)
{
    const bool fromCache = strstr(u.path, "\\Cache\\") != NULL && strstr(u.path, TU_ASIDE_ROOT) != NULL;
    if (fromCache)
        _snprintf(folder, folderSize, "%s:\\Cache", u.drive);
    else
        _snprintf(folder, folderSize, "%s:\\Content\\0000000000000000\\%08lX\\000B0000", u.drive, titleId);
    folder[folderSize - 1] = '\0';
    const int n = _snprintf(out, outSize, "%s\\%s", folder, FileName(u.path));
    out[outSize - 1] = '\0';
    return n > 0 && (size_t)n < outSize;
}

static bool AsidePath(unsigned long titleId, const InstalledTitleUpdate &u, char *out, size_t outSize, char *folder,
                      size_t folderSize)
{
    _snprintf(folder, folderSize, "%s:\\" TU_ASIDE_ROOT "\\%08lX\\%s", u.drive, titleId,
              InCache(u.path) ? "Cache" : "Content");
    folder[folderSize - 1] = '\0';
    const int n = _snprintf(out, outSize, "%s\\%s", folder, FileName(u.path));
    out[outSize - 1] = '\0';
    return n > 0 && (size_t)n < outSize;
}

bool TurnOffTitleUpdate(unsigned long titleId, const InstalledTitleUpdate &update)
{
    if (!update.on)
        return true;
    char dest[300], folder[300];
    if (!AsidePath(titleId, update, dest, sizeof(dest), folder, sizeof(folder)))
        return false;
    MakeFolders(folder);
    LogPath("setting aside", update.path);
    if (!MoveFileA(update.path, dest))
    {
        dprintf("[updates] couldn't move it: %lu\n", GetLastError());
        return false;
    }
    return true;
}

bool TurnOnTitleUpdate(unsigned long titleId, const InstalledTitleUpdate &update)
{
    // Every other one that's on goes aside first, so only this one is.
    InstalledTitleUpdate all[MAX_TITLE_UPDATES];
    const int count = ListTitleUpdates(titleId, all, MAX_TITLE_UPDATES);
    for (int i = 0; i < count; ++i)
    {
        if (all[i].on && _stricmp(all[i].path, update.path) != 0 && !TurnOffTitleUpdate(titleId, all[i]))
            return false;
    }
    if (update.on)
        return true;
    char dest[300], folder[300];
    if (!OnPath(titleId, update, dest, sizeof(dest), folder, sizeof(folder)))
        return false;
    MakeFolders(folder);
    LogPath("turning on", update.path);
    if (!MoveFileA(update.path, dest))
    {
        dprintf("[updates] couldn't move it: %lu\n", GetLastError());
        return false;
    }
    return true;
}

bool DeleteTitleUpdate(const InstalledTitleUpdate &update)
{
    LogPath("deleting", update.path);
    SetFileAttributesA(update.path, FILE_ATTRIBUTE_NORMAL);
    return DeleteFileA(update.path) != 0;
}

// A folder game's default.xex: its XEX2 header's optional headers are
// (key, value) pairs from 0x18, 0x14 saying how many; the execution info
// (key 0x00040006) points at the media ID, then version, base version and
// title ID. Read straight, big-endian as the console is.
static bool ReadXexMediaId(const char *folder, unsigned long *mediaId)
{
    char path[300];
    _snprintf(path, sizeof(path), "%s\\default.xex", folder);
    path[sizeof(path) - 1] = '\0';
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    unsigned char header[0x1000];
    DWORD got = 0;
    const bool read = ReadFile(h, header, sizeof(header), &got, NULL) && got >= 0x18;
    bool found = false;
    if (read && memcmp(header, "XEX2", 4) == 0)
    {
        const unsigned long count = *(const unsigned long *)(header + 0x14);
        for (unsigned long i = 0; i < count && 0x18 + i * 8 + 8 <= got && !found; ++i)
        {
            const unsigned long key = *(const unsigned long *)(header + 0x18 + i * 8);
            const unsigned long value = *(const unsigned long *)(header + 0x18 + i * 8 + 4);
            if (key != 0x00040006)
                continue;
            // The info is usually within the first 4KB; read it where it is
            // if not.
            if (value + 4 <= got)
                *mediaId = *(const unsigned long *)(header + value);
            else
            {
                unsigned long id = 0;
                DWORD n = 0;
                if (SetFilePointer(h, (LONG)value, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER ||
                    !ReadFile(h, &id, 4, &n, NULL) || n != 4)
                    break;
                *mediaId = id;
            }
            found = true;
        }
    }
    CloseHandle(h);
    return found;
}

bool ReadGameMediaId(const char *packagePath, bool folder, unsigned long *mediaId)
{
    *mediaId = 0;
    if (folder)
        return ReadXexMediaId(packagePath, mediaId);
    HANDLE h = CreateFileA(packagePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    unsigned char header[0x360];
    DWORD got = 0;
    const bool read = ReadFile(h, header, sizeof(header), &got, NULL) && got == sizeof(header);
    CloseHandle(h);
    if (!read || (memcmp(header, "LIVE", 4) != 0 && memcmp(header, "CON ", 4) != 0 && memcmp(header, "PIRS", 4) != 0))
        return false;
    *mediaId = *(const unsigned long *)(header + 0x354);
    return *mediaId != 0;
}

void TitleUpdateFolderFor(unsigned long titleId, const char *fileName, const char *contentBase, char *out,
                          size_t outSize)
{
    if (_strnicmp(fileName, "TU_", 3) == 0)
        _snprintf(out, outSize, "Hdd1:\\Cache");
    else
        _snprintf(out, outSize, "%s\\%08lX\\000B0000", contentBase, titleId);
    out[outSize - 1] = '\0';
    MakeFolders(out);
}

bool TurnOnTitleUpdateAt(unsigned long titleId, const char *path)
{
    InstalledTitleUpdate all[MAX_TITLE_UPDATES];
    const int count = ListTitleUpdates(titleId, all, MAX_TITLE_UPDATES);
    for (int i = 0; i < count; ++i)
    {
        if (_stricmp(all[i].path, path) == 0)
            return TurnOnTitleUpdate(titleId, all[i]);
    }
    return false;
}
