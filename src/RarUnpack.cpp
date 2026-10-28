/*
FILE : RarUnpack.cpp
PROJECT : Omni360
DESCRIPTION : Unpacks one file from a RAR on the console with dmc_unrar. See
              RarUnpack.h.

dmc_unrar is compiled here, as C++ (the Xbox 360 SDK's C compiler has no
stdbool.h), with only the parts this needs: no stdio or Win32 file access of
its own - it reads through the two-part stream below.
*/

#define DMC_UNRAR_DISABLE_STDIO 1
#define DMC_UNRAR_DISABLE_WIN32 1
#define DMC_UNRAR_DISABLE_BE32TOH_BE64TOH 1

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
// The head in memory, then packedPath, read as one stream
// ---------------------------------------------------------------------------

struct SplicedStream
{
    const unsigned char *head;
    unsigned long long headLen;
    FILE *packed;
    unsigned long long size; // headLen + packedPath's size (its final size, when fed)
    unsigned long long at;
    unsigned long long filePos; // where `packed` is, so it's only moved when it has to be
    void *readBuffer;           // packed's stdio buffer, freed with it

    // Unpacking while packedPath downloads (see RarUnpackFeed): how much of it
    // is known to be on disk so far.
    RarUnpackFeed *feed;
    const char *packedPath;
    unsigned long long available;
};

// The size packedPath has reached, from a handle of its own - the download is
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

// Waits until packedPath holds `need` bytes, or the download has stopped.
// Returns how many it holds.
static unsigned long long WaitForBytes(SplicedStream *s, unsigned long long need)
{
    while (s->available < need)
    {
        // Read the flag before the size: a download that has just finished
        // has every byte on disk by the time it says so.
        const long state = InterlockedCompareExchange(&s->feed->state, 0, 0);

        unsigned long long size = 0;
        if (PolledSize(s->packedPath, &size) && size > s->available)
        {
            s->available = size;
            s->filePos = (unsigned long long)-1; // reseek: clears the end-of-file the last read hit
            continue;
        }

        if (state != RAR_FEED_DOWNLOADING)
            break; // no more is coming

        Sleep(100);
    }
    return s->available;
}

static void CloseSpliced(SplicedStream *s)
{
    fclose(s->packed);
    free(s->readBuffer);
}

static dmc_unrar_size_t SplicedRead(void *opaque, void *buffer, dmc_unrar_size_t n)
{
    SplicedStream *s = (SplicedStream *)opaque;
    unsigned char *out = (unsigned char *)buffer;
    dmc_unrar_size_t done = 0;

    if (s->at < s->headLen && n > 0)
    {
        dmc_unrar_size_t take = (dmc_unrar_size_t)(s->headLen - s->at);
        if (take > n)
            take = n;
        memcpy(out, s->head + s->at, (size_t)take);
        s->at += take;
        done += take;
    }

    if (done < n && s->at < s->size)
    {
        // Only when dmc_unrar has jumped. The CRT's seek drops the read
        // buffer even when it lands where the file already is, so seeking
        // before every read sent each of dmc_unrar's small reads to the drive
        // on its own - the console unpacked Lego Rock Band's DLC at 2.5 MB/s.
        const unsigned long long want = s->at - s->headLen;
        unsigned long long take = n - done;
        if (s->at + take > s->size)
            take = s->size - s->at;

        // Still downloading: wait for the bytes, and read only what's there.
        // A short read once the download has given up ends the unpacking.
        if (s->feed != NULL)
        {
            const unsigned long long have = WaitForBytes(s, want + take);
            if (have <= want)
                return done;
            if (want + take > have)
                take = have - want;
        }

        if (want != s->filePos)
        {
            if (_fseeki64(s->packed, (__int64)want, SEEK_SET) != 0)
                return done;
            s->filePos = want;
        }
        size_t got = fread(out + done, 1, (size_t)take, s->packed);
        s->at += got;
        s->filePos += got;
        done += got;
    }

    return done;
}

static bool SplicedSeek(void *opaque, dmc_unrar_offset_t offset, int origin)
{
    SplicedStream *s = (SplicedStream *)opaque;
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

static dmc_unrar_offset_t SplicedTell(void *opaque)
{
    return (dmc_unrar_offset_t)((SplicedStream *)opaque)->at;
}

// dmc_unrar calls this when the archive closes, or fails to open. The file is
// RarUnpackMember's to close, so there's nothing to do - but it must exist.
static void SplicedClose(void *opaque)
{
}

static dmc_unrar_io_handler g_splicedFuncs = { NULL, SplicedClose, SplicedRead, SplicedSeek, SplicedTell };

// ---------------------------------------------------------------------------
// Writing the file
// ---------------------------------------------------------------------------

struct UnpackOutput
{
    FILE *file;
    unsigned long long written;
    unsigned long long total;
    RarUnpackProgressFn progressFn;
    bool writeFailed;
    bool cancelled;
};

static bool WriteChunk(void *opaque, void **buffer, dmc_unrar_size_t *bufferSize,
                       dmc_unrar_size_t uncompressedSize, dmc_unrar_return *err)
{
    UnpackOutput *o = (UnpackOutput *)opaque;

    if (uncompressedSize > 0 && fwrite(*buffer, 1, (size_t)uncompressedSize, o->file) != (size_t)uncompressedSize)
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

// ---------------------------------------------------------------------------

RarUnpackResult RarUnpackMember(const unsigned char *head, unsigned long headLen, const char *packedPath,
                                const char *memberName, const char *outPath, RarUnpackProgressFn progressFn,
                                void printFunction(const char *_format, ...), RarUnpackFeed *feed)
{
    SplicedStream stream;
    stream.head = head;
    stream.headLen = headLen;
    stream.at = 0;
    stream.feed = feed;
    stream.packedPath = packedPath;
    stream.packed = fopen(packedPath, "rb");
    if (stream.packed == NULL)
    {
        printFunction("ERROR: couldn't open %s to unpack it\n", packedPath);
        return RAR_UNPACK_READ_FAILED;
    }

    // Read ahead in big pieces, as the writes are - see SplicedRead. Set
    // before anything else touches the file, as setvbuf requires.
    const size_t READ_BUFFER = 1024 * 1024;
    stream.readBuffer = malloc(READ_BUFFER);
    if (stream.readBuffer != NULL)
        setvbuf(stream.packed, (char *)stream.readBuffer, _IOFBF, READ_BUFFER);

    _fseeki64(stream.packed, 0, SEEK_END);
    stream.available = (unsigned long long)_ftelli64(stream.packed);
    stream.size = headLen + (feed != NULL ? feed->finalSize : stream.available);
    _fseeki64(stream.packed, 0, SEEK_SET);
    stream.filePos = 0;

    dmc_unrar_archive archive;
    dmc_unrar_return r = dmc_unrar_archive_init(&archive);
    if (r != DMC_UNRAR_OK)
    {
        CloseSpliced(&stream);
        printFunction("ERROR: couldn't set up the unpacker: %s\n", dmc_unrar_strerror(r));
        return RAR_UNPACK_BAD_DATA;
    }
    archive.io.funcs = &g_splicedFuncs;
    archive.io.opaque = &stream;
    archive.io.size = stream.size;

    r = dmc_unrar_archive_open(&archive);
    if (r != DMC_UNRAR_OK)
    {
        CloseSpliced(&stream);
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
        CloseSpliced(&stream);
        printFunction("ERROR: %s isn't in the downloaded part of the RAR (%u files there)\n", memberName, (unsigned)count);
        return RAR_UNPACK_NOT_FOUND;
    }

    r = dmc_unrar_file_is_supported(&archive, index);
    if (r != DMC_UNRAR_OK)
    {
        dmc_unrar_archive_close(&archive);
        CloseSpliced(&stream);
        printFunction("ERROR: can't unpack %s: %s\n", memberName, dmc_unrar_strerror(r));
        return RAR_UNPACK_UNSUPPORTED;
    }

    UnpackOutput out;
    out.file = fopen(outPath, "wb");
    out.written = 0;
    out.total = dmc_unrar_get_file_stat(&archive, index)->uncompressed_size;
    out.progressFn = progressFn;
    out.writeFailed = false;
    out.cancelled = false;
    if (out.file == NULL)
    {
        dmc_unrar_archive_close(&archive);
        CloseSpliced(&stream);
        printFunction("ERROR: couldn't create %s\n", outPath);
        return RAR_UNPACK_WRITE_FAILED;
    }

    // Big writes suit FATX; the same 4MB the HTTPS downloads use.
    const size_t CHUNK = 4 * 1024 * 1024;
    void *chunk = malloc(CHUNK);
    void *fileBuffer = malloc(CHUNK);
    if (chunk == NULL || fileBuffer == NULL)
    {
        free(chunk);
        free(fileBuffer);
        fclose(out.file);
        remove(outPath);
        dmc_unrar_archive_close(&archive);
        CloseSpliced(&stream);
        printFunction("ERROR: out of memory for unpacking\n");
        return RAR_UNPACK_WRITE_FAILED;
    }
    setvbuf(out.file, (char *)fileBuffer, _IOFBF, CHUNK);

    dmc_unrar_size_t produced = 0;
    r = dmc_unrar_extract_file_with_callback(&archive, index, chunk, CHUNK, &produced, true, &out, WriteChunk);

    bool closedOk = (fclose(out.file) == 0);
    free(chunk);
    free(fileBuffer);
    dmc_unrar_archive_close(&archive);
    CloseSpliced(&stream);

    RarUnpackResult result = RAR_UNPACK_OK;
    if (out.cancelled)
        result = RAR_UNPACK_CANCELLED;
    else if (out.writeFailed || !closedOk)
        result = RAR_UNPACK_WRITE_FAILED;
    else if (r != DMC_UNRAR_OK)
        result = RAR_UNPACK_BAD_DATA;
    else if (out.written != out.total)
        result = RAR_UNPACK_BAD_DATA;

    if (result != RAR_UNPACK_OK)
    {
        if (result != RAR_UNPACK_CANCELLED)
            printFunction("ERROR: unpacking %s stopped at %I64u of %I64u bytes: %s\n", memberName, out.written,
                          out.total, out.writeFailed || !closedOk ? "couldn't write the file" : dmc_unrar_strerror(r));
        remove(outPath);
    }

    return result;
}
