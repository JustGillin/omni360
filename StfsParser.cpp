/*
FILE : StfsParser.cpp
PROJECT : xstore (DLC fork)
DESCRIPTION : reads Title ID / display name / box-art icon directly out of
              installed STFS content packages, and enumerates installed
              titles from the console's Content folder. Offsets are from the
              Free60 wiki's STFS page (Metadata Version 1 only - Version 2 is
              TV/series content and isn't relevant to games/DLC).

NOT compiled or tested on real hardware - see the fork's plan notes. The
Display Name encoding in particular is genuinely ambiguous between sources
(see DecodeDisplayName below) and is one of the first things worth verifying
against your own installed games once this builds.
*/

#include "StfsParser.h"

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long ReadBE32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

// Display Name is documented as UTF-8 by the Free60 wiki, but established
// STFS tooling (Modio, Velocity, Horizon) has historically treated it as
// UTF-16BE. Rather than bet on either, detect per-file: if alternating bytes
// are 0x00 (the ASCII-range signature of BE UTF-16), decode as UTF-16BE;
// otherwise treat it as UTF-8/ASCII directly. Non-Latin titles under the
// UTF-16BE path fall back to '?' per character - full UTF-8 re-encoding of
// non-Latin text isn't implemented, since it wasn't needed to validate the
// core approach.
static void DecodeDisplayName(const unsigned char *raw, int rawLen, char *out, int outSize)
{
    bool looksUtf16BE = false;

    for (int i = 0; i + 1 < rawLen; i += 2)
    {
        if (raw[i] == 0x00 && raw[i + 1] == 0x00)
            break; // terminator reached before finding any non-zero pair; ambiguous, fall through as UTF-8 (will just come out empty)
        if (raw[i] == 0x00)
        {
            looksUtf16BE = true;
            break;
        }
        if (raw[i] != 0x00)
        {
            looksUtf16BE = false;
            break;
        }
    }

    int o = 0;

    if (looksUtf16BE)
    {
        for (int i = 0; i + 1 < rawLen && o + 1 < outSize; i += 2)
        {
            unsigned char hi = raw[i];
            unsigned char lo = raw[i + 1];

            if (hi == 0 && lo == 0)
                break;

            out[o++] = (hi == 0) ? (char)lo : '?';
        }
    }
    else
    {
        for (int i = 0; i < rawLen && o + 1 < outSize; ++i)
        {
            if (raw[i] == '\0')
                break;
            out[o++] = (char)raw[i];
        }
    }

    out[o] = '\0';
}

bool StfsReadTitleInfo(const char *packagePath, StfsTitleInfo *outInfo)
{
    memset(outInfo, 0, sizeof(StfsTitleInfo));

    FILE *f = fopen(packagePath, "rb");
    if (f == NULL)
        return false;

    unsigned char magic[4];
    if (fread(magic, 1, 4, f) != 4)
    {
        fclose(f);
        return false;
    }

    bool validMagic = (memcmp(magic, "CON ", 4) == 0) ||
                      (memcmp(magic, "LIVE", 4) == 0) ||
                      (memcmp(magic, "PIRS", 4) == 0);

    if (!validMagic)
    {
        fclose(f);
        return false;
    }

    // Everything we care about (Version 1 metadata + the title thumbnail)
    // lives within the first 0x971A bytes regardless of the package's real,
    // potentially multi-GB, total size.
    const long READ_SIZE = 0x971A;
    unsigned char *header = (unsigned char *)malloc(READ_SIZE);
    if (header == NULL)
    {
        fclose(f);
        return false;
    }

    fseek(f, 0, SEEK_SET);
    long actuallyRead = (long)fread(header, 1, READ_SIZE, f);
    fclose(f);

    // Only require enough bytes for what's actually needed to identify the
    // title (through Display Name, ending at 0x491) - NOT through the
    // thumbnail fields. GOD header files in particular can be much smaller
    // than a full DLC/game package's header and may carry no thumbnail data
    // at all; the earlier version of this check required room for the
    // Thumbnail Image Size field too, which silently rejected otherwise
    // perfectly valid, smaller headers as "not a real package."
    if (actuallyRead < 0x0491)
    {
        free(header);
        return false;
    }

    unsigned long metadataVersion = ReadBE32(header + 0x0348);

    // Version 1 vs 2 only changes the layout AFTER offset 0x03B1 (Series/
    // Season ID for TV content, and a smaller max thumbnail size) - Content
    // Type (0x0344), Title ID (0x0360), and Display Name (0x0411) are at
    // identical offsets in both, so both are readable the same way. An
    // earlier version of this function rejected all v2 metadata outright on
    // the wrong assumption that v2 only ever meant TV/series content; in
    // practice plenty of ordinary GOD game packages use v2 too (confirmed
    // against a real console's Games-on-Demand library this session), so
    // that was silently discarding valid titles.
    unsigned long maxThumbSize = (metadataVersion == 2) ? 0x3D00 : 0x4000;

    outInfo->contentType = ReadBE32(header + 0x0344);
    outInfo->titleId = ReadBE32(header + 0x0360);

    DecodeDisplayName(header + 0x0411, 0x80, outInfo->displayName, sizeof(outInfo->displayName));

    // Only look for a thumbnail if the file actually extends that far - a
    // small header (common for GOD packages) may not have this section at
    // all, and reading past actuallyRead would be uninitialized memory.
    unsigned long thumbSize = 0;
    if (actuallyRead >= 0x1716 + 4)
    {
        thumbSize = ReadBE32(header + 0x1716); // Title Thumbnail Image Size
        if (thumbSize > maxThumbSize)
            thumbSize = maxThumbSize; // clamp to this version's documented max in case of a bogus value
    }

    // The second embedded image. The two sit next to each other in the
    // metadata - sizes at 0x1712/0x1716, images at 0x171A/0x571A - and
    // READ_SIZE (0x971A) is exactly 0x571A + 0x4000, so this one is already
    // in the buffer: reading it costs no extra I/O at all.
    unsigned long contentThumbSize = 0;
    if (actuallyRead >= 0x1712 + 4)
    {
        contentThumbSize = ReadBE32(header + 0x1712); // Thumbnail Image Size
        if (contentThumbSize > maxThumbSize)
            contentThumbSize = maxThumbSize;
    }

    // GOD conversion tools have a long history of leaving the thumbnail
    // field zeroed/garbage despite a nonzero declared size (thumbnails
    // aren't needed for gameplay, so plenty of converters don't bother) -
    // confirmed necessary this session: feeding non-PNG bytes straight to
    // D3DXCreateTextureFromFileInMemoryEx crashed the GPU hard enough to need
    // a cold boot. Only trust it if it actually starts with the real PNG
    // signature.
    static const unsigned char PNG_SIGNATURE[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

    if (thumbSize > sizeof(PNG_SIGNATURE) && (long)(0x571A + thumbSize) <= actuallyRead &&
        memcmp(header + 0x571A, PNG_SIGNATURE, sizeof(PNG_SIGNATURE)) == 0)
    {
        outInfo->titleThumbnail = (unsigned char *)malloc(thumbSize);
        if (outInfo->titleThumbnail != NULL)
        {
            memcpy(outInfo->titleThumbnail, header + 0x571A, thumbSize);
            outInfo->titleThumbnailSize = thumbSize;
        }
    }

    // Same PNG-signature gate for the content thumbnail, for exactly the same
    // reason - a converter that zeroed one field very plausibly zeroed both.
    if (contentThumbSize > sizeof(PNG_SIGNATURE) && (long)(0x171A + contentThumbSize) <= actuallyRead &&
        memcmp(header + 0x171A, PNG_SIGNATURE, sizeof(PNG_SIGNATURE)) == 0)
    {
        outInfo->contentThumbnail = (unsigned char *)malloc(contentThumbSize);
        if (outInfo->contentThumbnail != NULL)
        {
            memcpy(outInfo->contentThumbnail, header + 0x171A, contentThumbSize);
            outInfo->contentThumbnailSize = contentThumbSize;
        }
    }

    free(header);
    return true;
}

void StfsFreeTitleInfo(StfsTitleInfo *info)
{
    if (info->titleThumbnail != NULL)
    {
        free(info->titleThumbnail);
        info->titleThumbnail = NULL;
    }

    if (info->contentThumbnail != NULL)
    {
        free(info->contentThumbnail);
        info->contentThumbnail = NULL;
    }
}

bool IsGameContentType(unsigned long contentType)
{
    switch (contentType)
    {
    case 0x00001000: // Xbox 360 Title
    case 0x00004000: // Installed Game
    case 0x00005000: // Xbox Original / Xbox Title
    case 0x00007000: // Game on Demand
    case 0x00008000: // Game Demo
    case 0x0000A000: // Game Title
    case 0x0000D000: // Arcade Title
    case 0x0000E000: // XNA
        return true;
    default:
        return false;
    }
}

int EnumerateInstalledGames(const char *contentBasePath, InstalledGame *outGames, int maxGames,
                            void printFunction(const char *_format, ...))
{
    int count = 0;
    int titleFoldersSeen = 0;
    int contentTypeFoldersSeen = 0;
    int packageFilesSeen = 0;

    char titleSearchPattern[512];
    _snprintf(titleSearchPattern, sizeof(titleSearchPattern), "%s\\*", contentBasePath);

    WIN32_FIND_DATAA titleFindData;
    HANDLE hTitleFind = FindFirstFileA(titleSearchPattern, &titleFindData);
    if (hTitleFind == INVALID_HANDLE_VALUE)
    {
        printFunction("ERROR: could not list %s\n", contentBasePath);
        return 0;
    }

    do
    {
        if (!(titleFindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        if (strcmp(titleFindData.cFileName, ".") == 0 || strcmp(titleFindData.cFileName, "..") == 0)
            continue;
        if (count >= maxGames)
            break;

        titleFoldersSeen++;

        char titleDir[512];
        _snprintf(titleDir, sizeof(titleDir), "%s\\%s", contentBasePath, titleFindData.cFileName);

        char contentTypeSearchPattern[512];
        _snprintf(contentTypeSearchPattern, sizeof(contentTypeSearchPattern), "%s\\*", titleDir);

        WIN32_FIND_DATAA ctFindData;
        HANDLE hCtFind = FindFirstFileA(contentTypeSearchPattern, &ctFindData);
        if (hCtFind == INVALID_HANDLE_VALUE)
            continue;

        bool foundForThisTitle = false;

        do
        {
            if (!(ctFindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                continue;
            if (strcmp(ctFindData.cFileName, ".") == 0 || strcmp(ctFindData.cFileName, "..") == 0)
                continue;

            contentTypeFoldersSeen++;

            char contentTypeDir[512];
            _snprintf(contentTypeDir, sizeof(contentTypeDir), "%s\\%s", titleDir, ctFindData.cFileName);

            char packageSearchPattern[512];
            _snprintf(packageSearchPattern, sizeof(packageSearchPattern), "%s\\*", contentTypeDir);

            WIN32_FIND_DATAA pkgFindData;
            HANDLE hPkgFind = FindFirstFileA(packageSearchPattern, &pkgFindData);
            if (hPkgFind == INVALID_HANDLE_VALUE)
                continue;

            do
            {
                if (pkgFindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    continue; // Games-on-Demand-style split package folder - known gap, see header comment

                packageFilesSeen++;

                char packagePath[512];
                _snprintf(packagePath, sizeof(packagePath), "%s\\%s", contentTypeDir, pkgFindData.cFileName);

                StfsTitleInfo info;
                if (StfsReadTitleInfo(packagePath, &info))
                {
                    printFunction("  %s -> parsed OK, contentType=%08lX, titleId=%08lX, name=\"%s\"%s\n",
                                 packagePath, info.contentType, info.titleId, info.displayName,
                                 IsGameContentType(info.contentType) ? "" : " (not a game content type, skipped)");

                    if (IsGameContentType(info.contentType) && count < maxGames)
                    {
                        InstalledGame *g = &outGames[count];
                        g->titleId = info.titleId;
                        g->contentType = info.contentType;
                        strncpy(g->displayName, info.displayName, sizeof(g->displayName) - 1);
                        g->displayName[sizeof(g->displayName) - 1] = '\0';
                        strncpy(g->packagePath, packagePath, sizeof(g->packagePath) - 1);
                        g->packagePath[sizeof(g->packagePath) - 1] = '\0';
                        count++;
                        foundForThisTitle = true;
                    }

                    StfsFreeTitleInfo(&info);
                }
                else
                {
                    // StfsReadTitleInfo's failure reasons (bad magic / too
                    // small / v2 metadata) all look identical from the
                    // outside - read the raw file directly here just for
                    // diagnosis, to pin down which one this actually is.
                    FILE *diagFile = fopen(packagePath, "rb");
                    if (diagFile == NULL)
                    {
                        printFunction("  %s -> FAILED TO OPEN (errno-level failure, not an STFS parsing issue)\n", packagePath);
                    }
                    else
                    {
                        fseek(diagFile, 0, SEEK_END);
                        long size = ftell(diagFile);
                        fseek(diagFile, 0, SEEK_SET);

                        unsigned char first16[16];
                        memset(first16, 0, sizeof(first16));
                        size_t got = fread(first16, 1, sizeof(first16), diagFile);
                        fclose(diagFile);

                        char hex[64];
                        char *h = hex;
                        for (size_t i = 0; i < got; ++i)
                        {
                            h += _snprintf(h, 4, "%02X ", first16[i]);
                        }
                        *h = '\0';

                        printFunction("  %s -> size=%ld bytes, first %u bytes: %s\n", packagePath, size, (unsigned)got, hex);
                    }
                }

            } while (!foundForThisTitle && count < maxGames && FindNextFileA(hPkgFind, &pkgFindData));

            FindClose(hPkgFind);

        } while (!foundForThisTitle && count < maxGames && FindNextFileA(hCtFind, &ctFindData));

        FindClose(hCtFind);

    } while (count < maxGames && FindNextFileA(hTitleFind, &titleFindData));

    FindClose(hTitleFind);

    printFunction("Scan summary: %d title folder(s), %d content-type folder(s), %d package file(s) examined, %d matched as games\n",
                 titleFoldersSeen, contentTypeFoldersSeen, packageFilesSeen, count);

    return count;
}
