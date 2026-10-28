#ifndef RAR_UNPACK_H
#define RAR_UNPACK_H

// Unpacks one file from a RAR on the console, with dmc_unrar (see
// dmc_unrar/README.md) - for DLC and disc images kept compressed in a RAR.
//
// archive.org will unpack a RAR member itself, but not past about 1 GiB for a
// client as slow as a console, and never from an offset (see
// EXTRACTED_SIZE_LIMIT in ArchiveOrgDLC.cpp). So the member's compressed
// bytes are downloaded as a plain range of the RAR, which can be resumed, and
// unpacked here.
//
// RarUnpackFrom does the unpacking, from any RarUnpackInput into any
// RarUnpackOutput. RarUnpackMember is that for DLC: from a file on the drive
// to a file on the drive.

// Called as the file is written; return false to cancel.
typedef bool (*RarUnpackProgressFn)(unsigned long long bytesWritten, unsigned long long bytesTotal);

enum RarUnpackResult
{
    RAR_UNPACK_OK,
    RAR_UNPACK_CANCELLED,
    RAR_UNPACK_NOT_FOUND,    // no file of that name in what was downloaded
    RAR_UNPACK_UNSUPPORTED,  // encrypted, split across volumes, or a method dmc_unrar can't do
    RAR_UNPACK_BAD_DATA,     // the data didn't decode, or its CRC didn't match - or ran out
    RAR_UNPACK_READ_FAILED,  // couldn't open or read the input
    RAR_UNPACK_WRITE_FAILED  // couldn't write the output - a full drive, usually
};

// The RAR as dmc_unrar reads it: from the start of the archive, or at least
// from a point it reads as one (see RarUnpackMember). dmc_unrar reads the
// headers first, then the file's data from start to end.
class RarUnpackInput
{
public:
    virtual ~RarUnpackInput() {}
    virtual unsigned long long Size() = 0;

    // Up to len bytes at offset. Fewer is the end of what can be had - the
    // unpacking then fails, unless it didn't need them.
    virtual unsigned long Read(unsigned long long offset, void *buffer, unsigned long len) = 0;
};

// Where the unpacked file goes, in order.
class RarUnpackOutput
{
public:
    virtual ~RarUnpackOutput() {}
    virtual bool Write(const void *data, unsigned long len) = 0;
};

// Unpacks memberName (its path inside the RAR, '\' or '/' separated) from in
// to out, checking its CRC.
RarUnpackResult RarUnpackFrom(RarUnpackInput *in, const char *memberName, RarUnpackOutput *out,
                              RarUnpackProgressFn progressFn, void printFunction(const char *_format, ...));

// ---------------------------------------------------------------------------
// DLC: from a file to a file
// ---------------------------------------------------------------------------
//
// The downloaded part doesn't have to be the whole RAR. dmc_unrar is shown a
// RAR made of two parts back to back:
//
//   head        the start of the real RAR - its signature and main header,
//               held in memory
//   packedPath  a file holding the rest: the member's own header and its
//               compressed data, and anything before it that the download
//               included
//
// For a member that doesn't depend on the ones before it, that is the head
// plus just its own header and data. A solid member needs every member before
// it decoded first, so its range starts right after the head.

// For unpacking while packedPath is still downloading. The unpacker reads the
// file as it grows, waiting whenever it catches up, until the downloading
// thread sets state to say there's no more coming.
#define RAR_FEED_DOWNLOADING 0
#define RAR_FEED_COMPLETE    1 // packedPath is all there
#define RAR_FEED_STOPPED     2 // the download gave up; the unpacking stops where the data does

struct RarUnpackFeed
{
    unsigned long long finalSize; // what packedPath will hold once downloaded
    volatile long state;          // RAR_FEED_*, set with InterlockedExchange
};

// Unpacks memberName to outPath, replacing it. On anything but RAR_UNPACK_OK,
// outPath is deleted. feed is NULL for a packedPath that's already complete.
RarUnpackResult RarUnpackMember(const unsigned char *head, unsigned long headLen, const char *packedPath,
                                const char *memberName, const char *outPath, RarUnpackProgressFn progressFn,
                                void printFunction(const char *_format, ...), RarUnpackFeed *feed = NULL);

#endif
