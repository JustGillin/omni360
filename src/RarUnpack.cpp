/*
FILE : RarUnpack.cpp
PROJECT : Omni360
DESCRIPTION : Unpacks one file from a RAR on the console with dmc_unrar. See
              RarUnpack.h.

dmc_unrar is compiled here, as C++ (the Xbox 360 SDK's C compiler has no
stdbool.h), with only the parts this needs: no stdio or Win32 file access of
its own - it reads through a RarUnpackInput.
*/

#define DMC_UNRAR_DISABLE_STDIO 1
#define DMC_UNRAR_DISABLE_WIN32 1
#define DMC_UNRAR_DISABLE_BE32TOH_BE64TOH 1

// Files of 2GB and more - a disc image is 7-8GB. dmc_unrar decides this from
// whether its own stdio or Win32 file access handles 64-bit offsets, and with
// both turned off it assumes not, refusing Deadpool's image as "Unsupported
// large file". Our RarUnpackInput reads at 64-bit offsets, and dmc_unrar's
// own sizes are 64-bit (it checks, when this is on).
#define DMC_HAS_LARGE_FILE_SUPPORT 1

// RAR4 never asks for more than 4MB. RAR5 can ask for gigabytes, and the
// default cap of 256MB is half the console's memory.
#define DMC_UNRAR_MAX_DICT_SIZE (64u * 1024u * 1024u)

// How much compressed data dmc_unrar asks for at a time; 4KB by default. Not
// much bigger: its RAR4 filter parser keeps one of these on the stack. The
// 1MB read buffer in RarUnpackMember is what keeps the drive's reads large.
#define DMC_UNRAR_BS_BUFFER_SIZE (16 * 1024)

#include "../dmc_unrar/dmc_unrar.c"

#include "RarUnpack.h"

#ifdef _XBOX
#include <xtl.h>
#else
#include <windows.h> // the PC test harness
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// A RarUnpackInput as dmc_unrar's io
// ---------------------------------------------------------------------------

struct InputStream
{
    RarUnpackInput *in;
    unsigned long long size;
    unsigned long long at;
};

static dmc_unrar_size_t StreamRead(void *opaque, void *buffer, dmc_unrar_size_t n)
{
    InputStream *s = (InputStream *)opaque;
    if (s->at >= s->size)
        return 0;
    if (n > s->size - s->at)
        n = s->size - s->at;

    // In reads the interface can take; dmc_unrar's are small anyway.
    unsigned char *out = (unsigned char *)buffer;
    dmc_unrar_size_t done = 0;
    while (done < n)
    {
        unsigned long take = (n - done > 0x40000000) ? 0x40000000 : (unsigned long)(n - done);
        unsigned long got = s->in->Read(s->at, out + done, take);
        s->at += got;
        done += got;
        if (got < take)
            break;
    }
    return done;
}

static bool StreamSeek(void *opaque, dmc_unrar_offset_t offset, int origin)
{
    InputStream *s = (InputStream *)opaque;
    dmc_unrar_offset_t to;

    if (origin == DMC_UNRAR_SEEK_SET)
        to = offset;
    else if (origin == DMC_UNRAR_SEEK_CUR)
        to = (dmc_unrar_offset_t)s->at + offset;
    else if (origin == DMC_UNRAR_SEEK_END)
        to = (dmc_unrar_offset_t)s->size + offset;
    else
        return false;

    if (to < 0 || (unsigned long long)to > s->size)
        return false;
    s->at = (unsigned long long)to;
    return true;
}

static dmc_unrar_offset_t StreamTell(void *opaque)
{
    return (dmc_unrar_offset_t)((InputStream *)opaque)->at;
}

// dmc_unrar calls this when the archive closes, or fails to open. The input is
// the caller's to close, so there's nothing to do - but it must exist.
static void StreamClose(void *opaque)
{
}

static dmc_unrar_io_handler g_streamFuncs = { NULL, StreamClose, StreamRead, StreamSeek, StreamTell };

// ---------------------------------------------------------------------------
// Unpacking
// ---------------------------------------------------------------------------

struct UnpackSink
{
    RarUnpackOutput *out;
    unsigned long long written;
    unsigned long long total;
    RarUnpackProgressFn progressFn;
    bool writeFailed;
    bool cancelled;
};

static bool WriteChunk(void *opaque, void **buffer, dmc_unrar_size_t *bufferSize,
                       dmc_unrar_size_t uncompressedSize, dmc_unrar_return *err)
{
    UnpackSink *o = (UnpackSink *)opaque;

    if (uncompressedSize > 0 && !o->out->Write(*buffer, (unsigned long)uncompressedSize))
    {
        o->writeFailed = true;
        *err = DMC_UNRAR_WRITE_FAIL;
        return false;
    }
    o->written += uncompressedSize;

    if (o->progressFn != NULL && !o->progressFn(o->written, o->total))
    {
        o->cancelled = true;
        *err = DMC_UNRAR_USER_CANCEL;
        return false;
    }
    return true;
}

// Same path, ignoring which separator each side uses.
static bool SamePath(const char *a, const char *b)
{
    for (;; ++a, ++b)
    {
        char ca = (*a == '\\') ? '/' : *a;
        char cb = (*b == '\\') ? '/' : *b;
        if (ca != cb)
            return false;
        if (ca == '\0')
            return true;
    }
}

RarUnpackResult RarUnpackFrom(RarUnpackInput *in, const char *memberName, RarUnpackOutput *out,
                              RarUnpackProgressFn progressFn, void printFunction(const char *_format, ...))
{
    InputStream stream;
    stream.in = in;
    stream.size = in->Size();
    stream.at = 0;

    dmc_unrar_archive archive;
    dmc_unrar_return r = dmc_unrar_archive_init(&archive);
    if (r != DMC_UNRAR_OK)
    {
        printFunction("ERROR: couldn't set up the unpacker: %s\n", dmc_unrar_strerror(r));
        return RAR_UNPACK_BAD_DATA;
    }
    archive.io.funcs = &g_streamFuncs;
    archive.io.opaque = &stream;
    archive.io.size = stream.size;

    r = dmc_unrar_archive_open(&archive);
    if (r != DMC_UNRAR_OK)
    {
        printFunction("ERROR: couldn't read the RAR's headers: %s\n", dmc_unrar_strerror(r));
        return RAR_UNPACK_BAD_DATA;
    }

    // dmc_unrar names files with '/'; the header walk with '\'.
    dmc_unrar_size_t index = 0;
    const dmc_unrar_size_t count = dmc_unrar_get_file_count(&archive);
    bool found = false;
    for (dmc_unrar_size_t i = 0; i < count && !found; ++i)
    {
        char name[512];
        if (dmc_unrar_file_is_directory(&archive, i))
            continue;
        if (dmc_unrar_get_filename(&archive, i, name, sizeof(name)) == 0)
            continue;
        if (SamePath(name, memberName))
        {
            index = i;
            found = true;
        }
    }

    if (!found)
    {
        dmc_unrar_archive_close(&archive);
        printFunction("ERROR: %s isn't in the downloaded part of the RAR (%u files there)\n", memberName, (unsigned)count);
        return RAR_UNPACK_NOT_FOUND;
    }

    r = dmc_unrar_file_is_supported(&archive, index);
    if (r != DMC_UNRAR_OK)
    {
        dmc_unrar_archive_close(&archive);
        printFunction("ERROR: can't unpack %s: %s\n", memberName, dmc_unrar_strerror(r));
        return RAR_UNPACK_UNSUPPORTED;
    }

    // Big writes suit FATX; the same 4MB the HTTPS downloads use.
    const size_t CHUNK = 4 * 1024 * 1024;
    void *chunk = malloc(CHUNK);
    if (chunk == NULL)
    {
        dmc_unrar_archive_close(&archive);
        printFunction("ERROR: out of memory for unpacking\n");
        return RAR_UNPACK_WRITE_FAILED;
    }

    UnpackSink sink;
    sink.out = out;
    sink.written = 0;
    sink.total = dmc_unrar_get_file_stat(&archive, index)->uncompressed_size;
    sink.progressFn = progressFn;
    sink.writeFailed = false;
    sink.cancelled = false;

    dmc_unrar_size_t produced = 0;
    r = dmc_unrar_extract_file_with_callback(&archive, index, chunk, CHUNK, &produced, true, &sink, WriteChunk);

    free(chunk);
    dmc_unrar_archive_close(&archive);

    RarUnpackResult result = RAR_UNPACK_OK;
    if (sink.cancelled)
        result = RAR_UNPACK_CANCELLED;
    else if (sink.writeFailed)
        result = RAR_UNPACK_WRITE_FAILED;
    else if (r != DMC_UNRAR_OK || sink.written != sink.total)
        result = RAR_UNPACK_BAD_DATA;

    if (result != RAR_UNPACK_OK && result != RAR_UNPACK_CANCELLED)
        printFunction("ERROR: unpacking %s stopped at %I64u of %I64u bytes: %s\n", memberName, sink.written,
                      sink.total, sink.writeFailed ? "couldn't write the file" : dmc_unrar_strerror(r));

    return result;
}

// ---------------------------------------------------------------------------
// DLC: the head in memory, then packedPath, read as one stream
// ---------------------------------------------------------------------------

// The size path has reached, from a handle of its own - the download is
// appending to it through another.
static bool PolledSize(const char *path, unsigned long long *outSize)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return false;
    bool ok = (_fseeki64(f, 0, SEEK_END) == 0);
    __int64 size = ok ? _ftelli64(f) : -1;
    fclose(f);
    if (size < 0)
        return false;
    *outSize = (unsigned long long)size;
    return true;
}

class SplicedInput : public RarUnpackInput
{
public:
    SplicedInput(const unsigned char *head_, unsigned long headLen_, const char *packedPath_, RarUnpackFeed *feed_)
        : head(head_), headLen(headLen_), packedPath(packedPath_), feed(feed_), packed(NULL), readBuffer(NULL),
          size(0), filePos(0), available(0)
    {
    }

    ~SplicedInput()
    {
        if (packed != NULL)
            fclose(packed);
        free(readBuffer);
    }

    bool Open()
    {
        packed = fopen(packedPath, "rb");
        if (packed == NULL)
            return false;

        // Read ahead in big pieces, as the writes are. Set before anything
        // else touches the file, as setvbuf requires.
        const size_t READ_BUFFER = 1024 * 1024;
        readBuffer = malloc(READ_BUFFER);
        if (readBuffer != NULL)
            setvbuf(packed, (char *)readBuffer, _IOFBF, READ_BUFFER);

        _fseeki64(packed, 0, SEEK_END);
        available = (unsigned long long)_ftelli64(packed);
        size = headLen + (feed != NULL ? feed->finalSize : available);
        _fseeki64(packed, 0, SEEK_SET);
        filePos = 0;
        return true;
    }

    unsigned long long Size() { return size; }

    unsigned long Read(unsigned long long offset, void *buffer, unsigned long len)
    {
        unsigned char *out = (unsigned char *)buffer;
        unsigned long done = 0;

        if (offset < headLen && len > 0)
        {
            unsigned long take = (unsigned long)(headLen - offset);
            if (take > len)
                take = len;
            memcpy(out, head + offset, take);
            offset += take;
            done += take;
        }

        if (done < len && offset < size)
        {
            const unsigned long long want = offset - headLen;
            unsigned long long take = len - done;
            if (offset + take > size)
                take = size - offset;

            // Still downloading: wait for the bytes, and read only what's
            // there. A short read once the download has given up ends the
            // unpacking.
            if (feed != NULL)
            {
                const unsigned long long have = WaitForBytes(want + take);
                if (have <= want)
                    return done;
                if (want + take > have)
                    take = have - want;
            }

            // Only when dmc_unrar has jumped. The CRT's seek drops the read
            // buffer even when it lands where the file already is.
            if (want != filePos)
            {
                if (_fseeki64(packed, (__int64)want, SEEK_SET) != 0)
                    return done;
                filePos = want;
            }
            size_t got = fread(out + done, 1, (size_t)take, packed);
            filePos += got;
            done += (unsigned long)got;
        }

        return done;
    }

private:
    // Waits until packedPath holds `need` bytes, or the download has stopped.
    // Returns how many it holds.
    unsigned long long WaitForBytes(unsigned long long need)
    {
        while (available < need)
        {
            // Read the flag before the size: a download that has just finished
            // has every byte on disk by the time it says so.
            const long state = InterlockedCompareExchange(&feed->state, 0, 0);

            unsigned long long polled = 0;
            if (PolledSize(packedPath, &polled) && polled > available)
            {
                available = polled;
                filePos = (unsigned long long)-1; // reseek: clears the end-of-file the last read hit
                continue;
            }

            if (state != RAR_FEED_DOWNLOADING)
                break; // no more is coming

            Sleep(100);
        }
        return available;
    }

    const unsigned char *head;
    unsigned long long headLen;
    const char *packedPath;
    RarUnpackFeed *feed;
    FILE *packed;
    void *readBuffer;
    unsigned long long size;      // headLen + packedPath's size (its final size, when fed)
    unsigned long long filePos;   // where `packed` is, so it's only moved when it has to be
    unsigned long long available; // how much of packedPath is known to be on disk
};

class FileOutput : public RarUnpackOutput
{
public:
    FileOutput() : file(NULL), buffer(NULL) {}

    ~FileOutput()
    {
        Close();
        free(buffer);
    }

    bool Open(const char *path)
    {
        file = fopen(path, "wb");
        if (file == NULL)
            return false;
        // Big writes suit FATX.
        const size_t BUFFER = 4 * 1024 * 1024;
        buffer = malloc(BUFFER);
        if (buffer != NULL)
            setvbuf(file, (char *)buffer, _IOFBF, BUFFER);
        return true;
    }

    bool Write(const void *data, unsigned long len)
    {
        return fwrite(data, 1, len, file) == len;
    }

    bool Close()
    {
        if (file == NULL)
            return true;
        bool ok = (fclose(file) == 0);
        file = NULL;
        return ok;
    }

private:
    FILE *file;
    void *buffer;
};

RarUnpackResult RarUnpackMember(const unsigned char *head, unsigned long headLen, const char *packedPath,
                                const char *memberName, const char *outPath, RarUnpackProgressFn progressFn,
                                void printFunction(const char *_format, ...), RarUnpackFeed *feed)
{
    SplicedInput in(head, headLen, packedPath, feed);
    if (!in.Open())
    {
        printFunction("ERROR: couldn't open %s to unpack it\n", packedPath);
        return RAR_UNPACK_READ_FAILED;
    }

    FileOutput out;
    if (!out.Open(outPath))
    {
        printFunction("ERROR: couldn't create %s\n", outPath);
        return RAR_UNPACK_WRITE_FAILED;
    }

    RarUnpackResult result = RarUnpackFrom(&in, memberName, &out, progressFn, printFunction);
    if (!out.Close() && result == RAR_UNPACK_OK)
    {
        printFunction("ERROR: couldn't finish writing %s\n", outPath);
        result = RAR_UNPACK_WRITE_FAILED;
    }

    if (result != RAR_UNPACK_OK)
        remove(outPath);
    return result;
}

// ---------------------------------------------------------------------------
// Folder games: every file, into a folder
// ---------------------------------------------------------------------------

#define FATX_NAME_MAX 42

struct FolderSink
{
    FILE *file;
    RarUnpackAllInput *in;
    unsigned long long done;
    unsigned long long total;
    bool writeFailed;
    bool cancelled;
};

static bool WriteFolderChunk(void *opaque, void **buffer, dmc_unrar_size_t *bufferSize,
                             dmc_unrar_size_t uncompressedSize, dmc_unrar_return *err)
{
    FolderSink *s = (FolderSink *)opaque;
    if (uncompressedSize > 0 && fwrite(*buffer, 1, (size_t)uncompressedSize, s->file) != (size_t)uncompressedSize)
    {
        s->writeFailed = true;
        *err = DMC_UNRAR_WRITE_FAIL;
        return false;
    }
    s->done += uncompressedSize;
    if (!s->in->Unpacked(s->done, s->total))
    {
        s->cancelled = true;
        *err = DMC_UNRAR_USER_CANCEL;
        return false;
    }
    return true;
}

// name ('/' separated) less `skip` leading characters, as a path under
// destDir with '\' - or false for one that's empty, climbs out with "..",
// names a drive, or has a part too long for FATX.
static bool FolderPathOf(const char *destDir, const char *name, size_t skip, char *out, size_t outSize)
{
    const char *rel = name + skip;
    if (*rel == '\0' || *rel == '/' || strchr(rel, ':') != NULL)
        return false;

    int n = _snprintf(out, outSize, "%s\\", destDir);
    if (n < 0 || (size_t)n >= outSize)
        return false;
    size_t at = (size_t)n;

    const char *part = rel;
    for (;;)
    {
        const char *end = strchr(part, '/');
        size_t len = (end != NULL) ? (size_t)(end - part) : strlen(part);
        if (len == 0 || len > FATX_NAME_MAX || (len == 1 && part[0] == '.') ||
            (len == 2 && part[0] == '.' && part[1] == '.'))
            return false;
        if (at + len + 2 > outSize)
            return false;
        memcpy(out + at, part, len);
        at += len;
        if (end == NULL || end[1] == '\0')
            break;
        out[at++] = '\\';
        part = end + 1;
    }
    out[at] = '\0';
    return true;
}

// Every folder along path past its first `from` characters - destDir's own
// already exist - and the last part too when lastToo, for a directory entry.
static void MakeFoldersAlong(const char *path, size_t from, bool lastToo)
{
    char dir[600];
    size_t len = strlen(path);
    if (len >= sizeof(dir))
        return;
    memcpy(dir, path, len + 1);
    for (size_t i = from; i < len; ++i)
    {
        if (dir[i] == '\\')
        {
            dir[i] = '\0';
            CreateDirectoryA(dir, NULL);
            dir[i] = '\\';
        }
    }
    if (lastToo)
        CreateDirectoryA(dir, NULL);
}

RarUnpackResult RarUnpackAll(RarUnpackAllInput *in, const char *destDir, void printFunction(const char *_format, ...))
{
    InputStream stream;
    stream.in = in;
    stream.size = in->Size();
    stream.at = 0;

    dmc_unrar_archive archive;
    dmc_unrar_return r = dmc_unrar_archive_init(&archive);
    if (r != DMC_UNRAR_OK)
    {
        printFunction("ERROR: couldn't set up the unpacker: %s\n", dmc_unrar_strerror(r));
        return RAR_UNPACK_BAD_DATA;
    }
    archive.io.funcs = &g_streamFuncs;
    archive.io.opaque = &stream;
    archive.io.size = stream.size;

    r = dmc_unrar_archive_open(&archive);
    if (r != DMC_UNRAR_OK)
    {
        printFunction("ERROR: couldn't read the RAR's headers: %s\n", dmc_unrar_strerror(r));
        return RAR_UNPACK_BAD_DATA;
    }

    const dmc_unrar_size_t count = dmc_unrar_get_file_count(&archive);
    static char name[1024]; // one install at a time

    // The top folder they all share, if they do, and the total to unpack.
    char top[256] = "";
    size_t topLen = 0;
    bool shared = true;
    unsigned long long total = 0;
    for (dmc_unrar_size_t i = 0; i < count; ++i)
    {
        if (dmc_unrar_get_filename(&archive, i, name, sizeof(name)) == 0)
        {
            dmc_unrar_archive_close(&archive);
            printFunction("ERROR: file %u in the RAR has no readable name\n", (unsigned)i);
            return RAR_UNPACK_BAD_DATA;
        }
        const bool isDir = dmc_unrar_file_is_directory(&archive, i) ? true : false;
        if (!isDir)
            total += dmc_unrar_get_file_stat(&archive, i)->uncompressed_size;

        const char *slash = strchr(name, '/');
        const size_t first = (slash != NULL) ? (size_t)(slash - name) : strlen(name);
        if (slash == NULL && !isDir)
            shared = false; // a file at the top
        else if (topLen == 0 && first > 0 && first < sizeof(top))
        {
            memcpy(top, name, first);
            top[first] = '\0';
            topLen = first;
        }
        else if (first != topLen || strncmp(name, top, topLen) != 0)
            shared = false;
    }
    const size_t skip = (shared && topLen > 0) ? topLen + 1 : 0;
    printFunction("[rar] %u entries, %I64u bytes to unpack%s%s\n", (unsigned)count, total,
                  skip ? ", from under " : "", skip ? top : "");
    in->HeadersRead(total);

    const size_t CHUNK = 4 * 1024 * 1024;
    void *chunk = malloc(CHUNK);
    void *fileBuffer = malloc(CHUNK);
    if (chunk == NULL || fileBuffer == NULL)
    {
        free(chunk);
        free(fileBuffer);
        dmc_unrar_archive_close(&archive);
        printFunction("ERROR: out of memory for unpacking\n");
        return RAR_UNPACK_WRITE_FAILED;
    }

    const size_t destLen = strlen(destDir) + 1;
    FolderSink sink;
    sink.file = NULL;
    sink.in = in;
    sink.done = 0;
    sink.total = total;
    sink.writeFailed = false;
    sink.cancelled = false;

    RarUnpackResult result = RAR_UNPACK_OK;
    for (dmc_unrar_size_t i = 0; i < count && result == RAR_UNPACK_OK; ++i)
    {
        dmc_unrar_get_filename(&archive, i, name, sizeof(name));
        const bool isDir = dmc_unrar_file_is_directory(&archive, i) ? true : false;
        if (isDir && strlen(name) <= skip)
            continue; // the top folder itself

        char path[600];
        if (!FolderPathOf(destDir, name, skip, path, sizeof(path)))
        {
            printFunction("ERROR: can't unpack \"%s\" - not a name the drive can hold\n", name);
            result = RAR_UNPACK_UNSUPPORTED;
            break;
        }
        MakeFoldersAlong(path, destLen, isDir);
        if (isDir)
            continue;

        r = dmc_unrar_file_is_supported(&archive, i);
        if (r != DMC_UNRAR_OK)
        {
            printFunction("ERROR: can't unpack %s: %s\n", name, dmc_unrar_strerror(r));
            result = RAR_UNPACK_UNSUPPORTED;
            break;
        }

        // Its solid block's start is the earliest this reads; the archive
        // before that is done with.
        dmc_unrar_file_block *block = dmc_unrar_get_file(&archive, i);
        if (block != NULL)
            in->Behind((block->solid_start != NULL ? block->solid_start : block)->start_pos);

        sink.file = fopen(path, "wb");
        if (sink.file == NULL)
        {
            printFunction("ERROR: couldn't create %s\n", path);
            result = RAR_UNPACK_WRITE_FAILED;
            break;
        }
        setvbuf(sink.file, (char *)fileBuffer, _IOFBF, CHUNK);

        const unsigned long long before = sink.done;
        dmc_unrar_size_t produced = 0;
        r = dmc_unrar_extract_file_with_callback(&archive, i, chunk, CHUNK, &produced, true, &sink, WriteFolderChunk);
        const bool closed = (fclose(sink.file) == 0);
        sink.file = NULL;

        const unsigned long long size = dmc_unrar_get_file_stat(&archive, i)->uncompressed_size;
        if (sink.cancelled)
            result = RAR_UNPACK_CANCELLED;
        else if (sink.writeFailed || !closed)
            result = RAR_UNPACK_WRITE_FAILED;
        else if (r != DMC_UNRAR_OK || sink.done - before != size)
            result = RAR_UNPACK_BAD_DATA;

        if (result != RAR_UNPACK_OK && result != RAR_UNPACK_CANCELLED)
            printFunction("ERROR: unpacking %s stopped at %I64u of %I64u bytes: %s\n", name, sink.done - before, size,
                          sink.writeFailed || !closed ? "couldn't write the file" : dmc_unrar_strerror(r));
    }

    free(chunk);
    free(fileBuffer);
    dmc_unrar_archive_close(&archive);
    return result;
}
