#ifndef READ_AHEAD_H
#define READ_AHEAD_H

#include "GodConvert.h"

// Wraps a slow GodSource - the disc drive - so it keeps reading while the
// converter hashes and writes what it already has.
//
// Without it the two take turns: the drive sits idle for every group while
// that group is hashed and written to the hard drive, and the converter sits
// idle while the next group is read. A background thread reading ahead into
// a ring of buffers lets both run at once, so the install runs at the speed
// of whichever is slower instead of the sum of the two.
//
// It starts on the first large read and follows it sequentially. Any read
// that doesn't carry on from the last one - the converter's small reads of
// the file table and default.xex - stops it and goes straight to the source,
// so correctness never depends on the access pattern; only speed does. The
// wrapped source is only ever used by one thread at a time.
//
// Free of any Xbox header, so it is tested on a PC, reading a real ISO
// through an artificially slowed source.
class ReadAheadSource : public GodSource
{
public:
    // chunkSize must be a multiple of the source's sector size (2KB).
    ReadAheadSource(GodSource *inner, unsigned long chunkSize, int chunkCount);
    ~ReadAheadSource();

    bool ReadAt(unsigned long long offset, void *buffer, unsigned long len);
    unsigned long long Size();

    // How long ReadAt callers spent waiting on the read-ahead thread, and on
    // direct reads, in milliseconds - for the log.
    double WaitMs() const { return waitMs; }

    // Implementation detail, public only so the thread entry can reach it.
    void ProducerLoop();

private:
    bool Start(unsigned long long from);
    void Stop();
    bool Direct(unsigned long long offset, unsigned char *out, unsigned long len);

    GodSource *inner;
    unsigned long chunkSize;
    int chunkCount;
    double waitMs;
    double msPerTick;

    struct Chunk;
    Chunk *chunks;
    int chunksAllocated;  // may be more than chunkCount, if read-ahead ended up disabled
    void *lock;         // CRITICAL_SECTION
    void *dataEvent;    // a chunk finished reading, or the thread stopped
    void *spaceEvent;   // a chunk was freed, or the thread is asked to stop
    void *thread;

    // Guarded by lock.
    int head, used;                  // oldest chunk not yet consumed; chunks filled or being filled
    unsigned long long nextOffset;   // where the thread reads next
    bool stopRequested;
    bool producerDone;

    // Consumer side only.
    bool running;
    unsigned long long consumePos;   // where the next sequential ReadAt starts
};

#endif
