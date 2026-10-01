#ifndef INFLATE_SOURCE_H
#define INFLATE_SOURCE_H

#include "GodConvert.h" // GodSource
#include <stdio.h>

// ---------------------------------------------------------------------------
// A disc image inside a downloaded zip, read at any offset
// ---------------------------------------------------------------------------
//
// The Redump games are zips holding one deflate-compressed disc image, and
// archive.org won't hand out the image at an offset - only from the start -
// so the zip is downloaded and decompressed on the console. Deflate can only
// be decompressed from the beginning, but the GoD converter reads the disc's
// file system out of order before it copies. So the image is indexed once:
//
//   InflateIndexer  decompresses the whole stream once, as the download
//                   lands, and every INFLATE_SPAN of output records a
//                   restart point - where in the compressed data a deflate
//                   block begins, its leftover bits, and the 32KB of output
//                   before it (deflate's window). It checks the zip's CRC-32
//                   while it's at it.
//
//   InflateSource   a GodSource over the image: a read goes back to the
//                   nearest restart point at or before it and decompresses
//                   forward, or simply carries on if it continues the last
//                   read - which is how the copy itself reads.
//
// This is zlib's zran technique (examples/zran.c), written for a zip that
// arrives in pieces. 16MB apart, an 8GB image has about 500 points, 16MB of
// memory.
//
// ZipPieces reads the downloaded zip by offset, from the 32MB pieces it was
// downloaded as - FATX can't hold a file over 4GB, and most of these zips are
// 7-9GB.

#define INFLATE_SPAN  (16ULL * 1024 * 1024)
#define INFLATE_WINDOW 32768

struct InflatePoint
{
    unsigned long long out; // offset in the image
    unsigned long long in;  // offset in the compressed data of the first whole byte
    int bits;               // bits of the byte before `in` that belong to the block
    unsigned char window[INFLATE_WINDOW];
};

class ZipPieces
{
public:
    // dir holds P00000.bin, P00001.bin, ... each pieceSize bytes but the last.
    ZipPieces(const char *dir, unsigned long long pieceSize);
    ~ZipPieces();

    void PiecePath(unsigned long index, char *out, size_t outSize) const;
    bool Read(unsigned long long offset, void *buffer, unsigned long len);

private:
    char dir[256];
    unsigned long long pieceSize;
    FILE *file;
    long openIndex;
};

class InflateIndexer
{
public:
    InflateIndexer(unsigned long long unpackedSize);
    ~InflateIndexer();

    bool Ok() const { return ok; }

    // The next len bytes of the compressed stream. False on a deflate error.
    bool Feed(const unsigned char *data, unsigned long len);

    bool Finished() const { return finished; }
    unsigned long long Out() const { return totalOut; }
    unsigned long Crc() const { return crc; }

    // The points, handed over - the indexer no longer owns them.
    InflatePoint *TakePoints(int *outCount);

private:
    bool AddPoint(int bits);

    void *zs; // z_stream, kept out of this header
    unsigned char *window;
    InflatePoint *points;
    int count, capacity;
    unsigned long long totalIn, totalOut, lastPointOut;
    unsigned long crc;
    bool ok, finished;
};

class InflateSource : public GodSource
{
public:
    // dataStart is where the member's compressed data starts in the zip.
    InflateSource(ZipPieces *pieces, unsigned long long dataStart, unsigned long long packedSize,
                  unsigned long long unpackedSize, const InflatePoint *points, int pointCount);
    ~InflateSource();

    bool ReadAt(unsigned long long offset, void *buffer, unsigned long len);
    unsigned long long Size() { return unpacked; }

    // How many times a read had to go back to a restart point - for the log.
    unsigned long Restarts() const { return restarts; }

private:
    bool RestartAt(const InflatePoint &point);
    bool Produce(unsigned char *out, unsigned long len);

    ZipPieces *pieces;
    unsigned long long dataStart, packed, unpacked;
    const InflatePoint *points;
    int pointCount;

    void *zs; // z_stream
    bool open;
    unsigned long long outPos; // image offset the stream is at
    unsigned long long inPos;  // zip offset of the next byte to feed it
    unsigned char *inBuf;
    unsigned char *scratch;
    unsigned long restarts;
};

#endif
