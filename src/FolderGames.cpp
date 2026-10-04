/*
FILE : FolderGames.cpp
PROJECT : Omni360
DESCRIPTION : Games kept as extracted disc folders. See FolderGames.h.
*/

#include "FolderGames.h"
#include "GodConvert.h" // GodReadExecutableFile
#include "OutputConsole.h" // dprintf

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// (DWORD)-1 is GetFileAttributes' failure value; the XDK lacks
// INVALID_FILE_ATTRIBUTES.
static bool IsFile(const char *path)
{
    const DWORD attributes = GetFileAttributesA(path);
    return attributes != (DWORD)-1 && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// A Content folder's title or profile folder - eight or sixteen hex digits -
// holds packages, not game folders, so isn't looked in.
static bool IsHexFolderName(const char *name)
{
    const size_t len = strlen(name);
    if (len != 8 && len != 16)
        return false;
    for (size_t i = 0; i < len; ++i)
    {
        const char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

// "Disc1", "Disc 2", "disk_1", "DVD1", "CD2": a disc of a game whose folder
// is the one above.
static bool IsDiscFolderName(const char *name)
{
    static const char *const prefixes[] = { "disc", "disk", "dvd", "cd" };
    for (int p = 0; p < 4; ++p)
    {
        const size_t n = strlen(prefixes[p]);
        if (_strnicmp(name, prefixes[p], n) != 0)
            continue;
        const char *rest = name + n;
        while (*rest == ' ' || *rest == '_' || *rest == '-')
            rest++;
        if (*rest < '0' || *rest > '9')
            return false;
        while (*rest >= '0' && *rest <= '9')
            rest++;
        return *rest == '\0';
    }
    return false;
}

// The last part of a path: "Halo 3" of "Hdd1:\Games\Halo 3". Empty for a
// drive.
static const char *LastPart(const char *path)
{
    const char *slash = strrchr(path, '\\');
    const char *part = (slash != NULL) ? slash + 1 : path;
    return strchr(part, ':') != NULL ? "" : part;
}

struct FolderScan
{
    InstalledGame *out;
    int max;
    int count;
    int foldersSeen;
    int unreadable;
    void (*print)(const char *_format, ...);
};

// The game in dir, whose executable is exe: added unless it's another disc
// of one already found.
static void AddGame(FolderScan &scan, const char *dir, const char *exe)
{
    GodTitleInfo info;
    const GodResult result = GodReadExecutableFile(exe, &info);
    if (result != GOD_OK)
    {
        scan.print("  %s -> not readable as a game (%s)\n", exe, GodResultText(result));
        scan.unreadable++;
        return;
    }

    // A disc folder's game is the folder above it - when there is one.
    char folder[512];
    _snprintf(folder, sizeof(folder), "%s", dir);
    folder[sizeof(folder) - 1] = '\0';
    if (IsDiscFolderName(LastPart(folder)))
    {
        char *slash = strrchr(folder, '\\');
        if (slash != NULL && slash > folder && slash[-1] != ':')
            *slash = '\0';
    }

    for (int i = 0; i < scan.count; ++i)
    {
        if (scan.out[i].titleId == info.titleId)
            return; // its other disc, or a second copy - the first found stands
    }
    if (scan.count >= scan.max)
        return;

    InstalledGame &game = scan.out[scan.count++];
    memset(&game, 0, sizeof(game));
    game.titleId = info.titleId;
    game.contentType = info.contentType;
    game.folder = true;
    _snprintf(game.packagePath, sizeof(game.packagePath), "%s", folder);
    game.packagePath[sizeof(game.packagePath) - 1] = '\0';

    const char *name = info.name[0] != '\0' ? info.name : LastPart(folder);
    if (name[0] != '\0')
        _snprintf(game.displayName, sizeof(game.displayName), "%s", name);
    else
        _snprintf(game.displayName, sizeof(game.displayName), "Title %08lX", info.titleId);
    game.displayName[sizeof(game.displayName) - 1] = '\0';
}

static void Visit(FolderScan &scan, const char *dir, int depth)
{
    if (scan.count >= scan.max)
        return;
    scan.foldersSeen++;

    // A game: its executable at the top. Its own folders aren't looked in -
    // a game can have hundreds.
    char exe[600];
    _snprintf(exe, sizeof(exe), "%s\\default.xex", dir);
    exe[sizeof(exe) - 1] = '\0';
    if (IsFile(exe))
    {
        AddGame(scan, dir, exe);
        return;
    }
    _snprintf(exe, sizeof(exe), "%s\\default.xbe", dir);
    exe[sizeof(exe) - 1] = '\0';
    if (IsFile(exe))
    {
        AddGame(scan, dir, exe);
        return;
    }

    if (depth >= FOLDER_GAME_DEPTH)
        return;

    char pattern[600];
    _snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    pattern[sizeof(pattern) - 1] = '\0';
    WIN32_FIND_DATAA found;
    HANDLE h = FindFirstFileA(pattern, &found);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do
    {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        const char *name = found.cFileName;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;
        // A Content folder's title folders, a Games on Demand package's
        // .data folder, and the system's $SystemUpdate and the like.
        const size_t len = strlen(name);
        if (IsHexFolderName(name) || name[0] == '$' || (len > 5 && _stricmp(name + len - 5, ".data") == 0))
            continue;

        char sub[600];
        _snprintf(sub, sizeof(sub), "%s\\%s", dir, name);
        sub[sizeof(sub) - 1] = '\0';
        Visit(scan, sub, depth + 1);
    } while (scan.count < scan.max && FindNextFileA(h, &found));
    FindClose(h);
}

int EnumerateFolderGames(const char *root, InstalledGame *outGames, int maxGames,
                         void printFunction(const char *_format, ...))
{
    const DWORD startedAt = GetTickCount();
    FolderScan scan;
    memset(&scan, 0, sizeof(scan));
    scan.out = outGames;
    scan.max = maxGames;
    scan.print = printFunction;

    Visit(scan, root, 0);

    if (scan.count > 0 || scan.unreadable > 0)
        printFunction("Folder scan of %s: %d folder(s), %d game(s), %d unreadable, %lu ms\n", root,
                      scan.foldersSeen, scan.count, scan.unreadable, (unsigned long)(GetTickCount() - startedAt));
    return scan.count;
}

// Deletes path and everything in it. The files removed; *outOk false if
// anything couldn't be.
static int DeleteTree(const char *path, int depth, bool *outOk)
{
    if (depth > 32)
    {
        *outOk = false;
        return 0;
    }

    int removed = 0;
    char pattern[600];
    _snprintf(pattern, sizeof(pattern), "%s\\*", path);
    pattern[sizeof(pattern) - 1] = '\0';
    WIN32_FIND_DATAA found;
    HANDLE h = FindFirstFileA(pattern, &found);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (strcmp(found.cFileName, ".") == 0 || strcmp(found.cFileName, "..") == 0)
                continue;
            char child[600];
            _snprintf(child, sizeof(child), "%s\\%s", path, found.cFileName);
            child[sizeof(child) - 1] = '\0';
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                removed += DeleteTree(child, depth + 1, outOk);
                continue;
            }
            SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL); // a read-only file won't delete
            if (DeleteFileA(child))
                removed++;
            else
                *outOk = false;
        } while (FindNextFileA(h, &found));
        FindClose(h);
    }
    if (!RemoveDirectoryA(path))
        *outOk = false;
    return removed;
}

static void IgnoreLog(const char *, ...)
{
}

int RemoveFolderGame(const InstalledGame &game)
{
    // Never a drive, nor a folder that no longer holds this game - the scan,
    // run again on just that folder, has to find it there.
    const char *path = game.packagePath;
    if (!game.folder || LastPart(path)[0] == '\0')
        return 0;
    InstalledGame check;
    if (EnumerateFolderGames(path, &check, 1, IgnoreLog) != 1 || check.titleId != game.titleId ||
        _stricmp(check.packagePath, path) != 0)
    {
        dprintf("[library] not removing %s: it doesn't hold %08lX any more\n", path, game.titleId);
        return 0;
    }

    bool ok = true;
    const int removed = DeleteTree(path, 0, &ok);
    dprintf("[library] removed %s: %d file(s)%s\n", path, removed, ok ? "" : ", some couldn't be");
    return removed;
}
