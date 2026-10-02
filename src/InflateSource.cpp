/*
FILE : InflateSource.cpp
PROJECT : Omni360
DESCRIPTION : A deflate-compressed disc image in a downloaded zip, readable at
              any offset through restart points. See InflateSource.h.
*/

#include "InflateSource.h"
#include "zlib/zlib.h"

#include <stdlib.h>
#include <string.h>

#define IN_CHUNK      (256 * 1024)
#define SCRATCH_SIZE  (64 * 1024)

#define ZS ((z_stream *)zs)

// ---------------------------------------------------------------------------
// ZipPieces
// ---------------------------------------------------------------------------

ZipPieces::ZipPieces(const char *pieceDir, unsigned long long size)
    : pieceSize(size), file(NULL), openIndex(-1)
{
    strncpy(dir, pieceDir, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';
}

ZipPieces::~ZipPieces()
{
    if (file != NULL)
        fclose(file);
}

void ZipPieces::PiecePath(unsigned long index, char *out, size_t outSize) const
{
    _snprintf(out, outSize, "%s\\P%05lu.bin", dir, index);
    out[outSize - 1] = '\0';
}

bool ZipPieces::Read(unsigned long long offset, void *buffer, unsigned long len)
{
    unsigned char *outp = (unsigned char *)buffer;

    while (len > 0)
    {
        long index = (long)(offset / pieceSize);
        unsigned long within = (unsigned long)(offset % pieceSize);

        if (index != openIndex)
        {
            if (file != NULL)
                fclose(file);
            char path[300];
            PiecePath((unsigned long)index, path, sizeof(path));
            file = fopen(path, "rb");
            openIndex = (file != NULL) ? index : -1;
            if (file == NULL)
                return false;
        }

        if (fseek(file, (long)within, SEEK_SET) != 0)
            return false;

        unsigned long take = (unsigned long)(pieceSize - within);
        if (take > len)
            take = len;

        if (fread(outp, 1, take, file) != take)
            return false;

        outp += take;
        offset += take;
        len -= take;
    }
    return true;
}

// ---------------------------------------------------------------------------
// InflateIndexer
// ---------------------------------------------------------------------------

InflateIndexer::InflateIndexer(unsigned long long unpackedSize)
    : zs(NULL), window(NULL), points(NULL), count(0), capacity(0),
      totalIn(0), totalOut(0), lastPointOut(0), crc(0), ok(false), finished(false)
{
    zs = calloc(1, sizeof(z_stream));
    window = (unsigned char *)malloc(INFLATE_WINDOW);
    capacity = (int)(unpackedSize / INFLATE_SPAN) + 4;
    points = (InflatePoint *)malloc(sizeof(InflatePoint) * capacity);
    if (zs == NULL || window == NULL || points == NULL)
        return;

    // Raw deflate, as a zip stores it.
    if (inflateInit2(ZS, -15) != Z_OK)
        return;

    ZS->next_out = window;
    ZS->avail_out = INFLATE_WINDOW;
    crc = crc32(0L, Z_NULL, 0);

    // The start of the stream is always a restart point - no leftover bits,
    // nothing to refer back to - so every offset has one at or before it,
    // even those before the first block boundary.
    memset(window, 0, INFLATE_WINDOW);
    ok = AddPoint(0);
}

InflateIndexer::~InflateIndexer()
{
    if (zs != NULL)
    {
        if (ok || finished)
            inflateEnd(ZS);
        free(zs);
    }
    free(window);
    free(points);
}

bool InflateIndexer::AddPoint(int bits)
{
    if (count >= capacity)
    {
        int grown = capacity * 2;
        InflatePoint *more = (InflatePoint *)realloc(points, sizeof(InflatePoint) * grown);
        if (more == NULL)
            return false;
        points = more;
        capacity = grown;
    }

    InflatePoint &p = points[count++];
    p.out = totalOut;
    p.in = totalIn;
    p.bits = bits;

    // The last 32KB of output, oldest first. The window is used as a ring:
    // what's left unwritten at its end is older than what's at its start.
    unsigned left = ZS->avail_out;
    if (left > 0)
        memcpy(p.window, window + INFLATE_WINDOW - left, left);
    if (left < INFLATE_WINDOW)
        memcpy(p.window + left, window, INFLATE_WINDOW - left);

    lastPointOut = totalOut;
    return true;
}

bool InflateIndexer::Feed(const unsigned char *data, unsigned long len)
{
    if (!ok)
        return false;
    if (finished)
        return true; // anything after the stream is the zip's directory

    ZS->next_in = (Bytef *)data;
    ZS->avail_in = len;

    // Carries on while there's input, and one call past running out if the
    // last one got anywhere - with Z_BLOCK, the end of the stream is only
    // reported on the call after the last block finishes, which may be the
    // call that used up the input.
    bool progressed = true;
    while (!finished && (ZS->avail_in > 0 || progressed))
    {
        if (ZS->avail_out == 0)
        {
            ZS->next_out = window;
            ZS->avail_out = INFLATE_WINDOW;
        }

        unsigned char *outStart = ZS->next_out;
        uInt inBefore = ZS->avail_in, outBefore = ZS->avail_out;

        // Z_BLOCK stops at each deflate block boundary, where a point can go.
        int rc = inflate(ZS, Z_BLOCK);

        uInt produced = outBefore - ZS->avail_out;
        totalIn += inBefore - ZS->avail_in;
        totalOut += produced;
        crc = crc32(crc, outStart, produced);
        progressed = (produced > 0 || inBefore != ZS->avail_in);

        if (rc == Z_STREAM_END)
        {
            finished = true;
            break;
        }
        if (rc != Z_OK && rc != Z_BUF_ERROR)
        {
            ok = false;
            return false;
        }

        // At the end of a block that isn't the last one, every SPAN of
        // output.
        bool atBlockEnd = (ZS->data_type & 128) && !(ZS->data_type & 64);
        if (atBlockEnd && totalOut - lastPointOut >= INFLATE_SPAN)
        {
            if (!AddPoint(ZS->data_type & 7))
            {
                ok = false;
                return false;
            }
        }
    }
    return true;
}

InflatePoint *InflateIndexer::TakePoints(int *outCount)
{
    InflatePoint *p = points;
    *outCount = count;
    points = NULL;
    count = 0;
    capacity = 0;
    return p;
}

// ---------------------------------------------------------------------------
// InflateSource
// ---------------------------------------------------------------------------

InflateSource::InflateSource(ZipPieces *zipPieces, unsigned long long start, unsigned long long packedSize,
                             unsigned long long unpackedSize, const InflatePoint *indexPoints, int indexCount)
    : pieces(zipPieces), dataStart(start), packed(packedSize), unpacked(unpackedSize),
      points(indexPoints), pointCount(indexCount), zs(NULL), open(false), outPos(0), inPos(0),
      inBuf(NULL), scratch(NULL), restarts(0)
{
    zs = calloc(1, sizeof(z_stream));
    inBuf = (unsigned char *)malloc(IN_CHUNK);
    scratch = (unsigned char *)malloc(SCRATCH_SIZE);
}

InflateSource::~InflateSource()
{
    if (open)
        inflateEnd(ZS);
    free(zs);
    free(inBuf);
    free(scratch);
}

bool InflateSource::RestartAt(const InflatePoint &point)
{
    if (zs == NULL || inBuf == NULL)
        return false;

    if (open)
        inflateEnd(ZS);
    memset(zs, 0, sizeof(z_stream));
    open = false;

    if (inflateInit2(ZS, -15) != Z_OK)
        return false;
    open = true;

    inPos = dataStart + point.in;
    ZS->avail_in = 0;

    // The block starts partway through the byte before `in`: hand inflate
    // the bits of it that belong to the block.
    if (point.bits > 0)
    {
        unsigned char byte = 0;
        if (!pieces->Read(inPos - 1, &byte, 1))
            return false;
        if (inflatePrime(ZS, point.bits, byte >> (8 - point.bits)) != Z_OK)
            return false;
    }

    // What the block may refer back to. Nothing, at the very start.
    if (point.out > 0 && inflateSetDictionary(ZS, point.window, INFLATE_WINDOW) != Z_OK)
        return false;

    outPos = point.out;
    restarts++;
    return true;
}

bool InflateSource::Produce(unsigned char *out, unsigned long len)
{
    while (len > 0)
    {
        if (ZS->avail_in == 0)
        {
            unsigned long long end = dataStart + packed;
            if (inPos >= end)
                return false; // the image is shorter than asked for
            unsigned long take = (end - inPos < IN_CHUNK) ? (unsigned long)(end - inPos) : IN_CHUNK;
            if (!pieces->Read(inPos, inBuf, take))
                return false;
            inPos += take;
            ZS->next_in = inBuf;
            ZS->avail_in = take;
        }

        ZS->next_out = out;
        ZS->avail_out = len;
        int rc = inflate(ZS, Z_NO_FLUSH);

        unsigned long produced = len - ZS->avail_out;
        outPos += produced;
        out += produced;
        len -= produced;

        if (rc == Z_STREAM_END && len > 0)
            return false;
        if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR)
            return false;
    }
    return true;
}

bool InflateSource::ReadAt(unsigned long long offset, void *buffer, unsigned long len)
{
    if (zs == NULL || inBuf == NULL || scratch == NULL || pointCount == 0)
        return false;
    if (offset + len > unpacked)
        return false;

    // Carrying on from the last read is free; anything behind it, or further
    // ahead than a restart point would be, goes back to the nearest point.
    if (!open || offset < outPos || offset - outPos > INFLATE_SPAN)
    {
        int best = 0;
        for (int i = 0; i < pointCount && points[i].out <= offset; ++i)
            best = i;
        if (!RestartAt(points[best]))
            return false;
    }

    while (outPos < offset)
    {
        unsigned long long gap = offset - outPos;
        unsigned long skip = (gap < SCRATCH_SIZE) ? (unsigned long)gap : SCRATCH_SIZE;
        if (!Produce(scratch, skip))
            return false;
    }

    return Produce((unsigned char *)buffer, len);
}
