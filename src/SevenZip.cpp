/*
FILE : SevenZip.cpp
PROJECT : Omni360
DESCRIPTION : unpacking .7z archives with 7-Zip's own decoder. See SevenZip.h.
*/

#include "SevenZip.h"
#include "OutputConsole.h" // dprintf

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lzma/7z.h"
#include "lzma/7zCrc.h"
#include "lzma/LzmaDec.h"
#include "lzma/Lzma2Dec.h"

// The decoder's memory, from the CRT heap. Its "temp" allocations are the
// same - it only keeps them apart so a caller could pool them.
static void *SzMalloc(ISzAllocPtr, size_t size)
{
    return size == 0 ? NULL : malloc(size);
}

static void SzFree(ISzAllocPtr, void *address)
{
    free(address);
}

static const ISzAlloc g_alloc = { SzMalloc, SzFree };

// The archive, as the decoder reads it: a file handle, for 64-bit seeks.
struct FileInStream
{
    ISeekInStream vt; // first, so the decoder's pointer to it is a pointer to this
    HANDLE file;
};

static SRes FileRead(ISeekInStreamPtr p, void *buf, size_t *size)
{
    const FileInStream *s = (const FileInStream *)p;
    if (*size == 0)
        return SZ_OK;
    DWORD got = 0;
    if (!ReadFile(s->file, buf, (DWORD)*size, &got, NULL))
    {
        *size = 0;
        return SZ_ERROR_READ;
    }
    *size = got;
    return SZ_OK;
}

static SRes FileSeek(ISeekInStreamPtr p, Int64 *pos, ESzSeek origin)
{
    const FileInStream *s = (const FileInStream *)p;
    const DWORD method = (origin == SZ_SEEK_SET) ? FILE_BEGIN : (origin == SZ_SEEK_CUR) ? FILE_CURRENT : FILE_END;
    LONG high = (LONG)(*pos >> 32);
    // (DWORD)-1 is SetFilePointer's failure value, when GetLastError says so;
    // the XDK lacks INVALID_SET_FILE_POINTER.
    SetLastError(NO_ERROR);
    const DWORD low = SetFilePointer(s->file, (LONG)(*pos & 0xFFFFFFFF), &high, method);
    if (low == (DWORD)-1 && GetLastError() != NO_ERROR)
        return SZ_ERROR_READ;
    *pos = ((Int64)high << 32) | low;
    return SZ_OK;
}

const char *SevenZipResultText(SevenZipResult result)
{
    switch (result)
    {
    case SEVEN_ZIP_OK:           return "unpacked";
    case SEVEN_ZIP_NOT_7Z:       return "not a 7z archive, or a damaged one";
    case SEVEN_ZIP_UNSUPPORTED:  return "packed in a way the console can't unpack";
    case SEVEN_ZIP_TOO_BIG:      return "too big to unpack on the console";
    case SEVEN_ZIP_NO_MEMORY:    return "not enough memory to unpack it";
    case SEVEN_ZIP_CORRUPT:      return "the archive is damaged";
    case SEVEN_ZIP_WRITE_FAILED: return "a file couldn't be written";
    case SEVEN_ZIP_CANCELLED:    return "cancelled";
    default:                     return "failed";
    }
}

static SevenZipResult FromSRes(SRes res)
{
    switch (res)
    {
    case SZ_OK:                return SEVEN_ZIP_OK;
    case SZ_ERROR_UNSUPPORTED: return SEVEN_ZIP_UNSUPPORTED;
    case SZ_ERROR_MEM:         return SEVEN_ZIP_NO_MEMORY;
    case SZ_ERROR_CRC:
    case SZ_ERROR_DATA:        return SEVEN_ZIP_CORRUPT;
    default:                   return SEVEN_ZIP_NOT_7Z;
    }
}

// An entry's name as a path under the folder: UTF-16 to plain ASCII - FATX
// names are - with anything else as '_', and '/' as '\'. False for a name
// that would land outside it: a drive, a leading separator, or "..".
static bool EntryPath(const UInt16 *name, char *out, size_t outSize)
{
    size_t n = 0;
    for (; name[n] != 0; ++n)
    {
        if (n + 1 >= outSize)
            return false;
        UInt16 c = name[n];
        if (c == '/')
            c = '\\';
        out[n] = (c >= 0x20 && c < 0x7F) ? (char)c : '_';
    }
    out[n] = '\0';

    if (n == 0 || out[0] == '\\' || strchr(out, ':') != NULL)
        return false;
    for (const char *part = out; *part != '\0';)
    {
        const char *end = strchr(part, '\\');
        const size_t len = (end != NULL) ? (size_t)(end - part) : strlen(part);
        if (len == 2 && part[0] == '.' && part[1] == '.')
            return false;
        part += len + (end != NULL ? 1 : 0);
    }
    return true;
}

// Every folder down to path, after its drive.
static bool MakeFolders(const char *path)
{
    char partial[MAX_PATH + 1];
    const size_t len = strlen(path);
    if (len >= sizeof(partial))
        return false;
    memcpy(partial, path, len + 1);

    const char *colon = strchr(partial, ':');
    for (char *p = (colon != NULL) ? (char *)colon + 2 : partial; *p != '\0'; ++p)
    {
        if (*p != '\\')
            continue;
        *p = '\0';
        CreateDirectoryA(partial, NULL); // already there is fine
        *p = '\\';
    }
    if (!CreateDirectoryA(partial, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return false;
    return true;
}

static bool WriteWholeFile(const char *path, const Byte *data, size_t size)
{
    HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    bool ok = true;
    while (ok && size > 0)
    {
        const DWORD chunk = size > (1u << 20) ? (1u << 20) : (DWORD)size;
        DWORD wrote = 0;
        ok = WriteFile(file, data, chunk, &wrote, NULL) && wrote == chunk;
        data += chunk;
        size -= chunk;
    }
    CloseHandle(file);
    if (!ok)
        DeleteFileA(path);
    return ok;
}

// Whether every entry is inside one folder named folder - "RetroArch\..." for
// "RetroArch" - so it can be left out.
static bool AllInFolder(const CSzArEx &db, const char *folder)
{
    if (folder == NULL || folder[0] == '\0')
        return false;
    const size_t folderLen = strlen(folder);
    bool any = false;
    UInt16 name[MAX_PATH + 1];
    char path[MAX_PATH + 1];
    for (UInt32 i = 0; i < db.NumFiles; ++i)
    {
        if (SzArEx_GetFileNameUtf16(&db, i, NULL) > MAX_PATH)
            return false;
        SzArEx_GetFileNameUtf16(&db, i, name);
        if (!EntryPath(name, path, sizeof(path)) || _strnicmp(path, folder, folderLen) != 0)
            return false;
        if (path[folderLen] == '\0' && SzArEx_IsDir(&db, i))
            continue; // the folder itself
        if (path[folderLen] != '\\')
            return false;
        any = true;
    }
    return any;
}

// Whether FATX can hold every part of path: 42 characters at most, and none
// of the characters it refuses. Archives made on a PC have names it can't -
// RetroArch 360 V3's sample ROMs are named like "Pcengine Homebrew -
// Tongueman's Logic (FINAL 1.0).zip", 53 characters.
static bool FatxPath(const char *path)
{
    size_t partLen = 0;
    for (const char *p = path;; ++p)
    {
        if (*p == '\\' || *p == '\0')
        {
            if (partLen == 0 || partLen > 42)
                return false;
            if (*p == '\0')
                return true;
            partLen = 0;
            continue;
        }
        if (strchr("\"*+,/:;<=>?|", *p) != NULL)
            return false;
        partLen++;
    }
}

// Where entry i goes: destDir, then its name less the first stripLen
// characters. False for a name that would leave destDir, or one too long;
// *skip for the stripped folder itself, which has nowhere of its own to go,
// and for a name FATX can't hold, which is left out (*unwritable).
static bool TargetPath(const CSzArEx &db, UInt32 i, const char *destDir, size_t stripLen, char *out, size_t outSize,
                       bool *skip, bool *unwritable = NULL)
{
    UInt16 name[MAX_PATH + 1];
    char entry[MAX_PATH + 1];
    *skip = false;
    if (unwritable != NULL)
        *unwritable = false;
    if (SzArEx_GetFileNameUtf16(&db, i, NULL) > MAX_PATH)
        return false;
    SzArEx_GetFileNameUtf16(&db, i, name);
    if (!EntryPath(name, entry, sizeof(entry)))
    {
        dprintf("[7z] refusing an entry whose name leaves the folder\n");
        return false;
    }
    const size_t len = strlen(entry);
    const char *relative = entry + (len >= stripLen ? stripLen : len);
    if (relative[0] == '\0')
    {
        *skip = true;
        return true;
    }
    _snprintf(out, outSize, "%s\\%s", destDir, relative);
    out[outSize - 1] = '\0';
    if (!FatxPath(relative))
    {
        dprintf("[7z] left out - the drive can't hold its name: %s\n", relative);
        *skip = true;
        if (unwritable != NULL)
            *unwritable = true;
    }
    return true;
}

// Its folder, then the file, open for writing.
static HANDLE CreateWithFolders(char *path)
{
    char *slash = strrchr(path, '\\');
    if (slash != NULL)
    {
        *slash = '\0';
        const bool made = MakeFolders(path);
        *slash = '\\';
        if (!made)
            return INVALID_HANDLE_VALUE;
    }
    return CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

// ---------------------------------------------------------------------------
// Blocks too big to hold in memory
// ---------------------------------------------------------------------------
//
// SzArEx_Extract decodes a whole solid block into one buffer. A block of more
// than SEVEN_ZIP_MAX_BLOCK is decoded here instead, a megabyte at a time, each
// piece written into whichever of its files it belongs to - as long as it's
// a plain LZMA or LZMA2 stream, which is how 7-Zip packs nearly everything.
// One with a filter in front (BCJ for executables, Delta for audio) isn't
// done this way; those blocks are small in practice.

#define METHOD_LZMA2 0x21
#define METHOD_LZMA  0x30101

static bool ReadFolder(const CSzArEx &db, UInt32 f, CSzFolder *folder, const Byte **coderData)
{
    CSzData sd;
    *coderData = db.db.CodersData + db.db.FoCodersOffsets[f];
    sd.Data = *coderData;
    sd.Size = db.db.FoCodersOffsets[f + 1] - db.db.FoCodersOffsets[f];
    return SzGetNextFolderItem(folder, &sd) == SZ_OK;
}

static bool Streamable(const CSzArEx &db, UInt32 f)
{
    CSzFolder folder;
    const Byte *coderData;
    if (!ReadFolder(db, f, &folder, &coderData) || folder.NumCoders != 1 || folder.NumPackStreams != 1)
        return false;
    const UInt32 method = folder.Coders[0].MethodID;
    return method == METHOD_LZMA2 || method == METHOD_LZMA;
}

// The file being written as the stream reaches it.
struct StreamTarget
{
    const CSzArEx *db;
    UInt32 folder;
    UInt32 next;      // the next entry to look at for this folder's files
    bool open;        // a file is under way - in file, or discarded
    bool discard;     // its name won't go on the drive: read past, not written
    int skipped;      // files discarded
    HANDLE file;
    UInt32 index;     // the open file's entry
    UInt64 left;      // its bytes still to come
    UInt32 crc;
    char path[MAX_PATH + 1];
};

static SevenZipResult CloseTarget(StreamTarget &t)
{
    const bool closed = CloseHandle(t.file) != FALSE;
    t.file = INVALID_HANDLE_VALUE;
    if (!closed)
        return SEVEN_ZIP_WRITE_FAILED;
    if (SzBitWithVals_Check(&t.db->CRCs, t.index) && CRC_GET_DIGEST(t.crc) != t.db->CRCs.Vals[t.index])
    {
        dprintf("[7z] %s: checksum doesn't match\n", t.path);
        return SEVEN_ZIP_CORRUPT;
    }
    return SEVEN_ZIP_OK;
}

// Hands decoded bytes to the folder's files in order, opening each as it's
// reached and closing it once it's whole.
static SevenZipResult RouteBytes(StreamTarget &t, const Byte *data, size_t size, const char *destDir, size_t stripLen,
                                 SevenZipProgressFn progress, UInt64 total)
{
    const CSzArEx &db = *t.db;
    while (size > 0)
    {
        if (!t.open)
        {
            // The folder's next file with something in it. Folders and empty
            // files have no data, so no block; ExtractSevenZip makes those.
            while (t.next < db.NumFiles &&
                   (db.FileToFolder[t.next] != t.folder || db.UnpackPositions[t.next + 1] == db.UnpackPositions[t.next]))
                t.next++;
            if (t.next >= db.NumFiles)
                return SEVEN_ZIP_CORRUPT; // more data than files
            t.index = t.next++;
            bool skip, unwritable;
            if (!TargetPath(db, t.index, destDir, stripLen, t.path, sizeof(t.path), &skip, &unwritable) ||
                (skip && !unwritable))
                return SEVEN_ZIP_WRITE_FAILED;
            t.discard = unwritable;
            if (t.discard)
            {
                t.skipped++;
            }
            else
            {
                t.file = CreateWithFolders(t.path);
                if (t.file == INVALID_HANDLE_VALUE)
                {
                    dprintf("[7z] couldn't write %s\n", t.path);
                    return SEVEN_ZIP_WRITE_FAILED;
                }
            }
            t.open = true;
            t.left = db.UnpackPositions[t.index + 1] - db.UnpackPositions[t.index];
            t.crc = CRC_INIT_VAL;
        }

        const size_t chunk = (UInt64)size < t.left ? size : (size_t)t.left;
        if (!t.discard)
        {
            DWORD wrote = 0;
            if (!WriteFile(t.file, data, (DWORD)chunk, &wrote, NULL) || wrote != chunk)
            {
                CloseHandle(t.file);
                t.file = INVALID_HANDLE_VALUE;
                t.open = false;
                dprintf("[7z] couldn't write %s\n", t.path);
                return SEVEN_ZIP_WRITE_FAILED;
            }
            t.crc = CrcUpdate(t.crc, data, chunk);
        }
        t.left -= chunk;
        data += chunk;
        size -= chunk;

        if (t.left == 0)
        {
            t.open = false;
            if (!t.discard)
            {
                const SevenZipResult closed = CloseTarget(t);
                if (closed != SEVEN_ZIP_OK)
                    return closed;
            }
            if (progress != NULL && !progress(db.UnpackPositions[t.index + 1], total))
                return SEVEN_ZIP_CANCELLED;
        }
    }
    return SEVEN_ZIP_OK;
}

static SevenZipResult StreamFolder(const CSzArEx &db, UInt32 f, ILookInStreamPtr in, const char *destDir,
                                   size_t stripLen, SevenZipProgressFn progress, UInt64 total, int *skipped)
{
    CSzFolder folder;
    const Byte *coderData;
    if (!ReadFolder(db, f, &folder, &coderData))
        return SEVEN_ZIP_NOT_7Z;
    const CSzCoderInfo &coder = folder.Coders[0];
    const Byte *props = coderData + coder.PropsOffset;
    const bool lzma2 = (coder.MethodID == METHOD_LZMA2);

    const UInt32 packIndex = db.db.FoStartPackStreamIndex[f];
    UInt64 packLeft = db.db.PackPositions[packIndex + 1] - db.db.PackPositions[packIndex];
    UInt64 outLeft = SzAr_GetFolderUnpackSize(&db.db, f);
    if (LookInStream_SeekTo(in, db.dataPos + db.db.PackPositions[packIndex]) != SZ_OK)
        return SEVEN_ZIP_NOT_7Z;

    // The decoder keeps its dictionary - the block's own setting, 16MB for
    // 7-Zip's usual - and writes a piece at a time into out.
    CLzmaDec lzma;
    CLzma2Dec lzma2Dec;
    LzmaDec_CONSTRUCT(&lzma);
    Lzma2Dec_CONSTRUCT(&lzma2Dec);
    SRes res;
    if (lzma2)
    {
        res = coder.PropsSize == 1 ? Lzma2Dec_Allocate(&lzma2Dec, props[0], &g_alloc) : SZ_ERROR_UNSUPPORTED;
        if (res == SZ_OK)
            Lzma2Dec_Init(&lzma2Dec);
    }
    else
    {
        res = LzmaDec_Allocate(&lzma, props, coder.PropsSize, &g_alloc);
        if (res == SZ_OK)
            LzmaDec_Init(&lzma);
    }
    if (res != SZ_OK)
        return FromSRes(res);

    const size_t outSize = 1 << 20;
    Byte *out = (Byte *)malloc(outSize);
    StreamTarget target;
    target.db = &db;
    target.folder = f;
    target.next = db.FolderToFile[f];
    target.open = false;
    target.discard = false;
    target.skipped = 0;
    target.file = INVALID_HANDLE_VALUE;

    SevenZipResult result = (out != NULL) ? SEVEN_ZIP_OK : SEVEN_ZIP_NO_MEMORY;
    while (result == SEVEN_ZIP_OK && outLeft > 0)
    {
        const void *inBuf = NULL;
        size_t look = (1 << 18);
        if (look > packLeft)
            look = (size_t)packLeft;
        res = ILookInStream_Look(in, &inBuf, &look);
        if (res != SZ_OK)
        {
            result = FromSRes(res);
            break;
        }

        SizeT inDone = look;
        SizeT outDone = (outLeft < outSize) ? (SizeT)outLeft : outSize;
        ELzmaStatus status;
        res = lzma2 ? Lzma2Dec_DecodeToBuf(&lzma2Dec, out, &outDone, (const Byte *)inBuf, &inDone, LZMA_FINISH_ANY, &status)
                    : LzmaDec_DecodeToBuf(&lzma, out, &outDone, (const Byte *)inBuf, &inDone, LZMA_FINISH_ANY, &status);
        if (res != SZ_OK)
        {
            result = FromSRes(res);
            break;
        }
        if (inDone == 0 && outDone == 0)
        {
            result = SEVEN_ZIP_CORRUPT; // stuck: the stream ended before the block did
            break;
        }
        ILookInStream_Skip(in, inDone);
        packLeft -= inDone;
        outLeft -= outDone;
        result = RouteBytes(target, out, outDone, destDir, stripLen, progress, total);
    }

    if (target.open)
    {
        if (target.file != INVALID_HANDLE_VALUE)
            CloseHandle(target.file);
        if (result == SEVEN_ZIP_OK)
            result = SEVEN_ZIP_CORRUPT; // the block ended partway through a file
    }
    *skipped += target.skipped;
    free(out);
    if (lzma2)
        Lzma2Dec_Free(&lzma2Dec, &g_alloc);
    else
        LzmaDec_Free(&lzma, &g_alloc);
    return result;
}

SevenZipResult ExtractSevenZip(const char *archivePath, const char *destDir, const char *stripFolder,
                               SevenZipProgressFn progress, int *skippedNames)
{
    int skipped = 0;
    FileInStream archive;
    archive.vt.Read = FileRead;
    archive.vt.Seek = FileSeek;
    archive.file = CreateFileA(archivePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                               NULL);
    if (archive.file == INVALID_HANDLE_VALUE)
        return SEVEN_ZIP_NOT_7Z;

    const size_t kInputBufSize = 1 << 18;
    CLookToRead2 look;
    LookToRead2_CreateVTable(&look, False);
    look.buf = (Byte *)malloc(kInputBufSize);
    look.bufSize = kInputBufSize;
    look.realStream = &archive.vt;
    LookToRead2_INIT(&look);
    if (look.buf == NULL)
    {
        CloseHandle(archive.file);
        return SEVEN_ZIP_NO_MEMORY;
    }

    CrcGenerateTable();
    CSzArEx db;
    SzArEx_Init(&db);
    SevenZipResult result = FromSRes(SzArEx_Open(&db, &look.vt, &g_alloc, &g_alloc));

    // Each block too big to decode whole is streamed instead - refused here,
    // before anything is written, if it can't be.
    Byte *streamed = NULL; // by block: 1 if it's streamed
    if (result == SEVEN_ZIP_OK && db.db.NumFolders > 0)
    {
        streamed = (Byte *)calloc(db.db.NumFolders, 1);
        if (streamed == NULL)
            result = SEVEN_ZIP_NO_MEMORY;
    }
    for (UInt32 f = 0; result == SEVEN_ZIP_OK && f < db.db.NumFolders; ++f)
    {
        const UInt64 size = SzAr_GetFolderUnpackSize(&db.db, f);
        if (size <= SEVEN_ZIP_MAX_BLOCK)
            continue;
        if (Streamable(db, f))
        {
            streamed[f] = 1;
            dprintf("[7z] %s: block %u is %llu bytes - unpacking it a piece at a time\n", archivePath, f, size);
        }
        else
        {
            dprintf("[7z] %s: a block of %llu bytes, not plain LZMA, is more than the console can unpack\n",
                    archivePath, size);
            result = SEVEN_ZIP_TOO_BIG;
        }
    }

    const bool strip = (result == SEVEN_ZIP_OK) && AllInFolder(db, stripFolder);
    const size_t stripLen = strip ? strlen(stripFolder) + 1 : 0;
    const UInt64 total = (result == SEVEN_ZIP_OK) ? db.UnpackPositions[db.NumFiles] : 0;

    if (result == SEVEN_ZIP_OK && !MakeFolders(destDir))
        result = SEVEN_ZIP_WRITE_FAILED;

    UInt32 blockIndex = 0xFFFFFFFF; // the block in outBuffer - none yet
    Byte *outBuffer = NULL;
    size_t outBufferSize = 0;
    char path[MAX_PATH + 1];

    for (UInt32 i = 0; result == SEVEN_ZIP_OK && i < db.NumFiles; ++i)
    {
        // A streamed block's files are all written when its first is reached.
        const UInt32 folder = db.FileToFolder[i];
        const bool hasData = (folder != (UInt32)-1) && db.UnpackPositions[i + 1] > db.UnpackPositions[i];
        if (hasData && streamed[folder])
        {
            if (streamed[folder] == 1)
            {
                streamed[folder] = 2;
                result = StreamFolder(db, folder, &look.vt, destDir, stripLen, progress, total, &skipped);
            }
            continue;
        }

        bool skip, unwritable;
        if (!TargetPath(db, i, destDir, stripLen, path, sizeof(path), &skip, &unwritable))
        {
            result = SEVEN_ZIP_WRITE_FAILED;
            break;
        }
        if (skip)
        {
            // The stripped folder itself, or a name the drive can't hold.
            if (unwritable && !SzArEx_IsDir(&db, i))
                skipped++;
            continue;
        }

        if (SzArEx_IsDir(&db, i))
        {
            if (!MakeFolders(path))
                result = SEVEN_ZIP_WRITE_FAILED;
            continue;
        }

        size_t offset = 0, size = 0;
        result = FromSRes(SzArEx_Extract(&db, &look.vt, i, &blockIndex, &outBuffer, &outBufferSize, &offset, &size,
                                         &g_alloc, &g_alloc));
        if (result != SEVEN_ZIP_OK)
            break;

        // Its folder, then the file.
        char *slash = strrchr(path, '\\');
        if (slash != NULL)
        {
            *slash = '\0';
            const bool made = MakeFolders(path);
            *slash = '\\';
            if (!made)
            {
                result = SEVEN_ZIP_WRITE_FAILED;
                break;
            }
        }
        if (!WriteWholeFile(path, outBuffer + offset, size))
        {
            dprintf("[7z] couldn't write %s\n", path);
            result = SEVEN_ZIP_WRITE_FAILED;
            break;
        }

        if (progress != NULL && !progress(db.UnpackPositions[i + 1], total))
            result = SEVEN_ZIP_CANCELLED;
    }

    if (result != SEVEN_ZIP_OK)
        dprintf("[7z] %s: %s\n", archivePath, SevenZipResultText(result));
    else if (skipped > 0)
        dprintf("[7z] %s: unpacked, leaving out %d file(s) the drive can't name\n", archivePath, skipped);
    if (skippedNames != NULL)
        *skippedNames = skipped;

    free(streamed);
    free(outBuffer);
    SzArEx_Free(&db, &g_alloc);
    free(look.buf);
    CloseHandle(archive.file);
    return result;
}

