/*
FILE : StfsParser.cpp
PROJECT : Omni360
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
// otherwise treat it as UTF-8/ASCII directly.
//
// Both paths now produce real UTF-8, which is what this buffer is declared to
// hold and what GameListUI's Utf8ToWide expects. The UTF-16BE branch used to
// clamp to Latin-1 and emit '?' for anything else; see the comment on that
// branch for what that cost. Characters outside the BMP still become '?',
// which is a limit of the WCHAR conversion downstream rather than this
// function, and does not arise for title names in practice.
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
        // Re-encode each UTF-16BE unit as real UTF-8, because that is what
        // this buffer is - GameListUI's Utf8ToWide decodes it later.
        //
        // This used to write "(hi == 0) ? (char)lo : '?'", which broke every
        // character above 0x7F in two different ways. A codepoint over 0xFF
        // became '?' immediately, so a trademark sign (U+2122) was destroyed
        // here, long before any font got a chance to draw it. And one between
        // 0x80 and 0xFF was written as a single raw byte, which is not valid
        // UTF-8 at all - a registered sign (U+00AE) came out as a lone 0xAE,
        // which Utf8ToWide then correctly rejected as a stray continuation
        // byte and replaced with '?' anyway.
        //
        // Both showed up on real titles in a real library: "Spider-Man(TM):
        // Dimensions" and "Call of Duty: Modern Warfare(R) 3".
        for (int i = 0; i + 1 < rawLen && o + 1 < outSize; i += 2)
        {
            unsigned long cp = ((unsigned long)raw[i] << 8) | raw[i + 1];

            if (cp == 0)
                break;

            // Surrogates only carry meaning in pairs, and a paired value lands
            // outside the BMP where Utf8ToWide substitutes '?' regardless. Not
            // worth decoding for a title name.
            if (cp >= 0xD800 && cp <= 0xDFFF)
            {
                out[o++] = '?';
                continue;
            }

            if (cp < 0x80)
            {
                out[o++] = (char)cp;
            }
            else if (cp < 0x800)
            {
                if (o + 2 >= outSize)
                    break; // truncate on a whole character, never mid-sequence
                out[o++] = (char)(0xC0 | (cp >> 6));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            }
            else
            {
                if (o + 3 >= outSize)
                    break;
                out[o++] = (char)(0xE0 | (cp >> 12));
                out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                out[o++] = (char)(0x80 | (cp & 0x3F));
            }
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

// Whether an embedded thumbnail starts with a magic number D3DX can actually
// decode.
//
// This gate is not cosmetic. GOD conversion tools have a long history of
// leaving the thumbnail field zeroed or garbage despite a nonzero declared
// size - thumbnails aren't needed for gameplay, so plenty of converters don't
// bother - and feeding those bytes straight to
// D3DXCreateTextureFromFileInMemoryEx crashed the GPU hard enough to need a
// cold boot. Confirmed the hard way earlier in this project.
//
// It accepted PNG only until now. D3DX decodes JPEG too, and a package whose
// art happens to be JPEG would have been refused for no reason - one candidate
// for why a few titles come out blank. Widening it keeps the protection that
// matters (a known-good magic, not merely a nonzero size) while removing a
// restriction nothing justified.
static bool ImageMagicLooksDecodable(const unsigned char *data)
{
    static const unsigned char PNG[8]  = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    static const unsigned char JPEG[3] = {0xFF, 0xD8, 0xFF};

    if (memcmp(data, PNG, sizeof(PNG)) == 0)
        return true;

    if (memcmp(data, JPEG, sizeof(JPEG)) == 0)
        return true;

    return false;
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

    // Each image has to clear three separate gates: a plausible declared
    // size, bounds against how much of the header this file actually yielded,
    // and a magic number D3DX can decode. See ImageMagicLooksDecodable for
    // why the last one is not optional.

    // Shortest magic the helper accepts; at or under this it cannot be an image.
    const unsigned long MIN_IMAGE_BYTES = 8;

    // Record what was actually found before any of it is judged, so a title
    // that ends up with no art can say which gate refused it.
    outInfo->diagBytesRead = actuallyRead;
    outInfo->diagTitleThumbSize = thumbSize;
    outInfo->diagContentThumbSize = contentThumbSize;
    if (actuallyRead >= 0x571A + 4)
        outInfo->diagTitleMagic = ReadBE32(header + 0x571A);
    if (actuallyRead >= 0x171A + 4)
        outInfo->diagContentMagic = ReadBE32(header + 0x171A);

    if (thumbSize > MIN_IMAGE_BYTES && (long)(0x571A + thumbSize) <= actuallyRead &&
        ImageMagicLooksDecodable(header + 0x571A))
    {
        outInfo->titleThumbnail = (unsigned char *)malloc(thumbSize);
        if (outInfo->titleThumbnail != NULL)
        {
            memcpy(outInfo->titleThumbnail, header + 0x571A, thumbSize);
            outInfo->titleThumbnailSize = thumbSize;
        }
    }

    // Same magic gate for the content thumbnail, for exactly the same reason -
    // a converter that zeroed one field very plausibly zeroed both.
    if (contentThumbSize > MIN_IMAGE_BYTES && (long)(0x171A + contentThumbSize) <= actuallyRead &&
        ImageMagicLooksDecodable(header + 0x171A))
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

                    // Only for titles that came out with no art at all. Three
                    // separate gates can refuse an image and from the outside
                    // all three look the same - a blank square - so this says
                    // which one it was rather than leaving it to guesswork at
                    // a full hardware build cycle per guess.
                    //
                    // Reading it: size 0 means the package genuinely declares
                    // no image. A nonzero size with 0x89504E47 (PNG) or
                    // FFD8FFxx (JPEG) magic that still got refused means the
                    // bounds check did it - the image runs past bytesRead, so
                    // this header file is shorter than the image it claims.
                    // Any other magic is a converter that left the field as
                    // garbage, which is exactly what the gate is there to
                    // catch.
                    if (IsGameContentType(info.contentType) &&
                        info.titleThumbnail == NULL && info.contentThumbnail == NULL)
                    {
                        printFunction("    no icon: bytesRead=%ld titleThumb(size=%lu magic=%08lX) contentThumb(size=%lu magic=%08lX)\n",
                                     info.diagBytesRead,
                                     info.diagTitleThumbSize, info.diagTitleMagic,
                                     info.diagContentThumbSize, info.diagContentMagic);
                    }

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
