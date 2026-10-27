/*
FILE : LibraryCache.cpp
PROJECT : Omni360
DESCRIPTION : The library as last read, kept for the next start. See LibraryCache.h.
*/

#include "LibraryCache.h"
#include "OutputConsole.h" // dprintf

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIBRARY_CACHE_FILE "game:\\Library.cache"
#define LIBRARY_CACHE_TEMP_FILE "game:\\Library.cache.tmp"

// Bumped whenever what's written changes, so an old file is ignored rather
// than misread.
#define LIBRARY_CACHE_MAGIC 0x4F4C4331UL // "OLC1"

// An image bigger than this isn't one a package holds - the file is damaged.
#define MAX_CACHED_IMAGE (64 * 1024)

static bool WriteBytes(FILE *f, const void *data, unsigned long size)
{
    return size == 0 || fwrite(data, 1, size, f) == size;
}

static bool WriteLong(FILE *f, unsigned long value)
{
    return WriteBytes(f, &value, sizeof(value));
}

static bool WriteString(FILE *f, const char *s)
{
    const unsigned long len = (unsigned long)strlen(s);
    return WriteLong(f, len) && WriteBytes(f, s, len);
}

static bool ReadBytes(FILE *f, void *data, unsigned long size)
{
    return size == 0 || fread(data, 1, size, f) == size;
}

static bool ReadLong(FILE *f, unsigned long *value)
{
    return ReadBytes(f, value, sizeof(*value));
}

// Into out (outSize bytes, terminated); false if it's longer than that.
static bool ReadString(FILE *f, char *out, size_t outSize)
{
    unsigned long len = 0;
    if (!ReadLong(f, &len) || len >= outSize || !ReadBytes(f, out, len))
        return false;
    out[len] = '\0';
    return true;
}

// An image as malloc'd bytes, NULL for none.
static bool ReadImage(FILE *f, unsigned char **image, unsigned long *size)
{
    *image = NULL;
    *size = 0;
    unsigned long len = 0;
    if (!ReadLong(f, &len) || len > MAX_CACHED_IMAGE)
        return false;
    if (len == 0)
        return true;
    *image = (unsigned char *)malloc(len);
    if (*image == NULL || !ReadBytes(f, *image, len))
    {
        free(*image);
        *image = NULL;
        return false;
    }
    *size = len;
    return true;
}

bool SaveLibraryCache(const char *folders, const InstalledGame *games, int count, const int *folderFound,
                      int folderCount)
{
    FILE *f = fopen(LIBRARY_CACHE_TEMP_FILE, "wb");
    if (f == NULL)
    {
        dprintf("[library cache] couldn't write %s\n", LIBRARY_CACHE_TEMP_FILE);
        return false;
    }

    bool ok = WriteLong(f, LIBRARY_CACHE_MAGIC) && WriteString(f, folders) &&
              WriteLong(f, (unsigned long)folderCount);
    for (int i = 0; ok && i < folderCount; ++i)
        ok = WriteLong(f, (unsigned long)folderFound[i]);

    ok = ok && WriteLong(f, (unsigned long)count);
    for (int i = 0; ok && i < count; ++i)
    {
        const InstalledGame &g = games[i];
        // 2 and 4 were the DLC and title update flags, no longer kept; older
        // files' are ignored.
        const unsigned char flags = g.folder ? 1 : 0;
        ok = WriteLong(f, g.titleId) && WriteLong(f, g.contentType) && WriteBytes(f, &flags, 1) &&
             WriteString(f, g.displayName) && WriteString(f, g.packagePath) &&
             WriteLong(f, g.titleThumbnailSize) && WriteBytes(f, g.titleThumbnail, g.titleThumbnailSize) &&
             WriteLong(f, g.contentThumbnailSize) && WriteBytes(f, g.contentThumbnail, g.contentThumbnailSize);
    }

    ok = (fclose(f) == 0) && ok;
    if (!ok)
    {
        dprintf("[library cache] writing it failed\n");
        remove(LIBRARY_CACHE_TEMP_FILE);
        return false;
    }

    remove(LIBRARY_CACHE_FILE); // rename won't replace an existing file
    if (rename(LIBRARY_CACHE_TEMP_FILE, LIBRARY_CACHE_FILE) != 0)
    {
        dprintf("[library cache] couldn't rename it into place\n");
        return false;
    }
    dprintf("[library cache] kept %d games\n", count);
    return true;
}

int LoadLibraryCache(const char *folders, InstalledGame *games, int maxGames, int *folderFound, int folderCount)
{
    FILE *f = fopen(LIBRARY_CACHE_FILE, "rb");
    if (f == NULL)
        return -1;

    int count = 0;
    bool ok = true;
    char keptFolders[2048];
    unsigned long magic = 0, keptFolderCount = 0, keptCount = 0;

    if (!ReadLong(f, &magic) || magic != LIBRARY_CACHE_MAGIC ||
        !ReadString(f, keptFolders, sizeof(keptFolders)) || strcmp(keptFolders, folders) != 0 ||
        !ReadLong(f, &keptFolderCount) || keptFolderCount != (unsigned long)folderCount)
    {
        dprintf("[library cache] not for these folders, or from another version - reading the drive\n");
        fclose(f);
        return -1;
    }

    for (int i = 0; ok && i < folderCount; ++i)
    {
        unsigned long found = 0;
        ok = ReadLong(f, &found);
        folderFound[i] = (int)found;
    }

    ok = ok && ReadLong(f, &keptCount) && keptCount <= (unsigned long)maxGames;
    for (unsigned long i = 0; ok && i < keptCount; ++i)
    {
        InstalledGame &g = games[count];
        memset(&g, 0, sizeof(g));
        unsigned char flags = 0;
        ok = ReadLong(f, &g.titleId) && ReadLong(f, &g.contentType) && ReadBytes(f, &flags, 1) &&
             ReadString(f, g.displayName, sizeof(g.displayName)) &&
             ReadString(f, g.packagePath, sizeof(g.packagePath)) &&
             ReadImage(f, &g.titleThumbnail, &g.titleThumbnailSize) &&
             ReadImage(f, &g.contentThumbnail, &g.contentThumbnailSize);
        if (!ok)
        {
            FreeInstalledGames(&g, 1);
            break;
        }
        g.folder = (flags & 1) != 0;
        count++;
    }
    fclose(f);

    if (!ok)
    {
        dprintf("[library cache] damaged - reading the drive\n");
        FreeInstalledGames(games, count);
        return -1;
    }
    return count;
}
