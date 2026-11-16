/*
FILE : ZipExtract.cpp
PROJECT : Omni360
DESCRIPTION : Unpacking .zip archives. See ZipExtract.h.
*/

#include "ZipExtract.h"
#include "OutputConsole.h" // dprintf
#include "zlib/zlib.h"

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ZIP_CHUNK      (64 * 1024)
#define ZIP_TAIL_MAX   (65535 + 22) // the longest comment, and the record itself
#define FATX_NAME_MAX  42

const char *ZipResultText(ZipResult result)
{
    switch (result)
    {
    case ZIP_OK:           return "Unpacked";
    case ZIP_NOT_ZIP:      return "Not a zip, or a damaged one";
    case ZIP_UNSUPPORTED:  return "The zip uses a method Omni360 can't unpack";
    case ZIP_NO_MEMORY:    return "Not enough memory to unpack it";
    case ZIP_CORRUPT:      return "The zip is damaged";
    case ZIP_WRITE_FAILED: return "A file couldn't be written - is the drive full?";
    case ZIP_CANCELLED:    return "Cancelled";
    }
    return "Unknown error";
}

// Zip is little-endian and the 360 isn't, so every field is read by byte.
static unsigned long Le16(const unsigned char *p) { return (unsigned long)p[0] | ((unsigned long)p[1] << 8); }
static unsigned long Le32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static bool ReadAt(FILE *f, unsigned long offset, void *buffer, size_t size)
{
    return fseek(f, (long)offset, SEEK_SET) == 0 && fread(buffer, 1, size, f) == size;
}

// Every folder along path, made in turn; already there is fine.
static void MakeFolders(const char *path)
{
    char partial[600];
    _snprintf(partial, sizeof(partial), "%s", path);
    partial[sizeof(partial) - 1] = '\0';
    for (char *p = partial; *p != '\0'; ++p)
    {
        if (*p == '\\' && p > partial && p[-1] != ':')
        {
            *p = '\0';
            CreateDirectoryA(partial, NULL);
            *p = '\\';
        }
    }
    CreateDirectoryA(partial, NULL);
}

// The member's name as it lands under destDir, with backslashes, into out:
// false to leave it out - outside stripFolder, unsafe, or a folder entry.
// *tooLong is set for a part FATX can't hold.
static bool LandingName(const char *name, const char *stripFolder, char *out, size_t outSize, bool *tooLong)
{
    *tooLong = false;
    const char *rest = name;

    if (stripFolder != NULL && stripFolder[0] != '\0')
    {
        // Past the first path part named stripFolder.
        const size_t stripLen = strlen(stripFolder);
        const char *found = NULL;
        for (const char *part = name; *part != '\0';)
        {
            const char *end = strchr(part, '/');
            if (end == NULL)
                break; // a file's own name, not a folder
            if ((size_t)(end - part) == stripLen && _strnicmp(part, stripFolder, stripLen) == 0)
            {
                found = end + 1;
                break;
            }
            part = end + 1;
        }
        if (found == NULL)
            return false;
        rest = found;
    }

    const size_t len = strlen(rest);
    if (len == 0 || rest[len - 1] == '/' || len >= outSize)
        return false; // a folder of its own, made when a file needs it

    if (rest[0] == '/' || strchr(rest, ':') != NULL || strchr(rest, '\\') != NULL)
        return false;

    size_t partLen = 0;
    for (size_t i = 0; i <= len; ++i)
    {
        const char c = rest[i];
        if (c == '/' || c == '\0')
        {
            if (partLen == 2 && rest[i - 1] == '.' && rest[i - 2] == '.')
                return false;
            if (partLen > FATX_NAME_MAX)
                *tooLong = true;
            partLen = 0;
        }
        else
            ++partLen;
        out[i] = (c == '/') ? '\\' : c;
    }
    return !*tooLong;
}

struct ZipMember
{
    unsigned long method, crc, packed, size, localOffset;
};

// One member, from its local header's data to destPath.
static ZipResult UnpackMember(FILE *zip, const ZipMember &m, const char *destPath, unsigned char *in,
                              unsigned char *out)
{
    unsigned char local[30];
    if (!ReadAt(zip, m.localOffset, local, sizeof(local)) || Le32(local) != 0x04034b50)
        return ZIP_CORRUPT;
    const unsigned long dataStart = m.localOffset + 30 + Le16(local + 26) + Le16(local + 28);
    if (fseek(zip, (long)dataStart, SEEK_SET) != 0)
        return ZIP_CORRUPT;

    FILE *file = fopen(destPath, "wb");
    if (file == NULL)
        return ZIP_WRITE_FAILED;

    ZipResult result = ZIP_OK;
    unsigned long crc = crc32(0L, Z_NULL, 0);
    unsigned long remaining = m.packed, written = 0;

    if (m.method == 0)
    {
        while (remaining > 0 && result == ZIP_OK)
        {
            const size_t n = remaining < ZIP_CHUNK ? remaining : ZIP_CHUNK;
            if (fread(in, 1, n, zip) != n)
                result = ZIP_CORRUPT;
            else if (fwrite(in, 1, n, file) != n)
                result = ZIP_WRITE_FAILED;
            else
            {
                crc = crc32(crc, in, (uInt)n);
                remaining -= (unsigned long)n;
                written += (unsigned long)n;
            }
        }
    }
    else
    {
        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK)
        {
            fclose(file);
            return ZIP_NO_MEMORY;
        }
        int z = Z_OK;
        while (z != Z_STREAM_END && result == ZIP_OK)
        {
            if (zs.avail_in == 0)
            {
                if (remaining == 0)
                {
                    result = ZIP_CORRUPT; // ran out before the deflate stream ended
                    break;
                }
                const size_t n = remaining < ZIP_CHUNK ? remaining : ZIP_CHUNK;
                if (fread(in, 1, n, zip) != n)
                {
                    result = ZIP_CORRUPT;
                    break;
                }
                remaining -= (unsigned long)n;
                zs.next_in = in;
                zs.avail_in = (uInt)n;
            }
            zs.next_out = out;
            zs.avail_out = ZIP_CHUNK;
            z = inflate(&zs, Z_NO_FLUSH);
            if (z != Z_OK && z != Z_STREAM_END)
            {
                result = ZIP_CORRUPT;
                break;
            }
            const size_t produced = ZIP_CHUNK - zs.avail_out;
            if (produced > 0)
            {
                if (fwrite(out, 1, produced, file) != produced)
                    result = ZIP_WRITE_FAILED;
                crc = crc32(crc, out, (uInt)produced);
                written += (unsigned long)produced;
            }
        }
        inflateEnd(&zs);
    }

    if (fclose(file) != 0 && result == ZIP_OK)
        result = ZIP_WRITE_FAILED;
    if (result == ZIP_OK && (written != m.size || crc != m.crc))
        result = ZIP_CORRUPT;
    if (result != ZIP_OK)
        remove(destPath);
    return result;
}

ZipResult ExtractZip(const char *archivePath, const char *destDir, const char *stripFolder, ZipProgressFn progress,
                     int *skippedNames)
{
    if (skippedNames != NULL)
        *skippedNames = 0;

    FILE *zip = fopen(archivePath, "rb");
    if (zip == NULL)
        return ZIP_NOT_ZIP;
    fseek(zip, 0, SEEK_END);
    const long fileSize = ftell(zip);

    // The end-of-central-directory record, searched for from the end.
    const unsigned long tailSize = (unsigned long)(fileSize < ZIP_TAIL_MAX ? fileSize : ZIP_TAIL_MAX);
    unsigned char *tail = (unsigned char *)malloc(tailSize);
    unsigned char *in = (unsigned char *)malloc(ZIP_CHUNK);
    unsigned char *out = (unsigned char *)malloc(ZIP_CHUNK);
    unsigned char *dir = NULL;
    ZipResult result = ZIP_OK;

    if (tail == NULL || in == NULL || out == NULL)
        result = ZIP_NO_MEMORY;
    else if (tailSize < 22 || !ReadAt(zip, (unsigned long)fileSize - tailSize, tail, tailSize))
        result = ZIP_NOT_ZIP;

    unsigned long entries = 0, dirSize = 0, dirOffset = 0;
    if (result == ZIP_OK)
    {
        long at = (long)tailSize - 22;
        while (at >= 0 && Le32(tail + at) != 0x06054b50)
            --at;
        if (at < 0)
            result = ZIP_NOT_ZIP;
        else
        {
            entries = Le16(tail + at + 10);
            dirSize = Le32(tail + at + 12);
            dirOffset = Le32(tail + at + 16);
            if (entries == 0xFFFF || dirOffset == 0xFFFFFFFF)
                result = ZIP_UNSUPPORTED; // zip64
            else if ((unsigned long long)dirOffset + dirSize > (unsigned long long)fileSize)
                result = ZIP_CORRUPT;
        }
    }

    if (result == ZIP_OK)
    {
        dir = (unsigned char *)malloc(dirSize + 1);
        if (dir == NULL)
            result = ZIP_NO_MEMORY;
        else if (!ReadAt(zip, dirOffset, dir, dirSize))
            result = ZIP_CORRUPT;
    }

    // The unpacked total first, for the progress bar.
    unsigned long long total = 0;
    for (int pass = 0; pass < 2 && result == ZIP_OK; ++pass)
    {
        unsigned long long done = 0;
        unsigned long at = 0;
        for (unsigned long e = 0; e < entries && result == ZIP_OK; ++e)
        {
            if (at + 46 > dirSize || Le32(dir + at) != 0x02014b50)
            {
                result = ZIP_CORRUPT;
                break;
            }
            const unsigned char *h = dir + at;
            const unsigned long nameLen = Le16(h + 28);
            const unsigned long next = at + 46 + nameLen + Le16(h + 30) + Le16(h + 32);
            if (next > dirSize)
            {
                result = ZIP_CORRUPT;
                break;
            }

            ZipMember m;
            const unsigned long flags = Le16(h + 8);
            m.method = Le16(h + 10);
            m.crc = Le32(h + 16);
            m.packed = Le32(h + 20);
            m.size = Le32(h + 24);
            m.localOffset = Le32(h + 42);

            char name[600];
            const unsigned long copyLen = nameLen < sizeof(name) - 1 ? nameLen : sizeof(name) - 1;
            memcpy(name, h + 46, copyLen);
            name[copyLen] = '\0';
            at = next;

            char landing[600];
            bool tooLong = false;
            if (!LandingName(name, stripFolder, landing, sizeof(landing), &tooLong))
            {
                if (tooLong && pass == 1)
                {
                    dprintf("[zip] left out %s - a name too long for the drive\n", name);
                    if (skippedNames != NULL)
                        ++*skippedNames;
                }
                continue;
            }

            if (pass == 0)
            {
                total += m.size;
                continue;
            }

            if ((flags & 1) != 0 || (m.method != 0 && m.method != 8))
            {
                dprintf("[zip] %s: method %lu, flags %04lX\n", name, m.method, flags);
                result = ZIP_UNSUPPORTED;
                break;
            }

            char destPath[1200];
            _snprintf(destPath, sizeof(destPath), "%s\\%s", destDir, landing);
            destPath[sizeof(destPath) - 1] = '\0';
            char *slash = strrchr(destPath, '\\');
            *slash = '\0';
            MakeFolders(destPath);
            *slash = '\\';

            result = UnpackMember(zip, m, destPath, in, out);
            if (result != ZIP_OK)
            {
                dprintf("[zip] %s: %s\n", name, ZipResultText(result));
                break;
            }
            done += m.size;
            if (progress != NULL && !progress(done, total))
                result = ZIP_CANCELLED;
        }
    }

    free(dir);
    free(out);
    free(in);
    free(tail);
    fclose(zip);
    return result;
}
