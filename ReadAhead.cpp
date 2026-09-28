/*
FILE : ReadAhead.cpp
PROJECT : Omni360
DESCRIPTION : Background read-ahead over a GodSource. See ReadAhead.h.

One producer (the thread) and one consumer (whoever calls ReadAt), sharing a
ring of chunks. The producer only ever fills the slot after the last used
one, and the consumer only ever reads and frees the slot at the head, so a
chunk is touched by one side at a time; the lock guards the ring's counters
and each chunk's state, never the copies themselves.
*/

#include "ReadAhead.h"

#ifdef _XBOX
#include <xtl.h>
#else
#include <windows.h>
#endif

#include <string.h>

enum ChunkState
{
    CHUNK_EMPTY,
    CHUNK_READING,
    CHUNK_READY,
    CHUNK_FAILED
};

struct ReadAheadSource::Chunk
{
    unsigned char *data;
    unsigned long long offset;
    unsigned long len;
    int state;
};

#define LOCK   ((CRITICAL_SECTION *)lock)

static DWORD WINAPI ProducerEntry(LPVOID param)
{
    ((ReadAheadSource *)param)->ProducerLoop();
    return 0;
}

ReadAheadSource::ReadAheadSource(GodSource *innerSource, unsigned long chunkBytes, int count)
    : inner(innerSource), chunkSize(chunkBytes), chunkCount(0), waitMs(0.0), chunks(NULL),
      lock(NULL), dataEvent(NULL), spaceEvent(NULL), thread(NULL),
      head(0), used(0), nextOffset(0), stopRequested(false), producerDone(true),
      running(false), consumePos(0)
{
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    msPerTick = 1000.0 / (double)freq.QuadPart;

    lock = new CRITICAL_SECTION;
    InitializeCriticalSection(LOCK);
    dataEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    spaceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    chunksAllocated = 0;
    chunks = new Chunk[count];
    for (int i = 0; i < count; ++i)
    {
        chunks[i].data = (unsigned char *)VirtualAlloc(NULL, chunkSize, MEM_COMMIT, PAGE_READWRITE);
        chunks[i].state = CHUNK_EMPTY;
        if (chunks[i].data == NULL)
            break;
        chunksAllocated = i + 1;
    }
    chunkCount = chunksAllocated;

    // Without memory or events it still works - every read goes straight
    // to the source, as it would without this wrapper.
    if (chunkCount < 2 || dataEvent == NULL || spaceEvent == NULL)
        chunkCount = 0;
}

ReadAheadSource::~ReadAheadSource()
{
    Stop();

    if (chunks != NULL)
    {
        for (int i = 0; i < chunksAllocated; ++i)
            VirtualFree(chunks[i].data, 0, MEM_RELEASE);
        delete[] chunks;
    }
    if (dataEvent != NULL) CloseHandle((HANDLE)dataEvent);
    if (spaceEvent != NULL) CloseHandle((HANDLE)spaceEvent);
    DeleteCriticalSection(LOCK);
    delete LOCK;
}

unsigned long long ReadAheadSource::Size()
{
    return inner->Size();
}

bool ReadAheadSource::Direct(unsigned long long offset, unsigned char *out, unsigned long len)
{
    return inner->ReadAt(offset, out, len);
}

bool ReadAheadSource::Start(unsigned long long from)
{
    if (chunkCount == 0 || from >= inner->Size())
        return false;

    head = 0;
    used = 0;
    nextOffset = from;
    stopRequested = false;
    producerDone = false;
    for (int i = 0; i < chunkCount; ++i)
        chunks[i].state = CHUNK_EMPTY;
    ResetEvent((HANDLE)dataEvent);
    ResetEvent((HANDLE)spaceEvent);

    HANDLE h = CreateThread(NULL, 0, ProducerEntry, this, CREATE_SUSPENDED, NULL);
    if (h == NULL)
        return false;

#ifdef _XBOX
    // Off the main thread's core. Hardware thread 2 is core 1's first,
    // where the reads' waiting doesn't compete with the hashing.
    XSetThreadProcessor(h, 2);
#endif

    thread = h;
    running = true;
    consumePos = from;
    ResumeThread(h);
    return true;
}

void ReadAheadSource::Stop()
{
    if (!running)
        return;

    EnterCriticalSection(LOCK);
    stopRequested = true;
    LeaveCriticalSection(LOCK);
    SetEvent((HANDLE)spaceEvent);

    // Waits out a read already in progress - the source mustn't be used by
    // two threads at once.
    WaitForSingleObject((HANDLE)thread, INFINITE);
    CloseHandle((HANDLE)thread);
    thread = NULL;
    running = false;
}

void ReadAheadSource::ProducerLoop()
{
    const unsigned long long size = inner->Size();

    for (;;)
    {
        EnterCriticalSection(LOCK);
        while (!stopRequested && used == chunkCount)
        {
            LeaveCriticalSection(LOCK);
            WaitForSingleObject((HANDLE)spaceEvent, INFINITE);
            EnterCriticalSection(LOCK);
        }

        if (stopRequested || nextOffset >= size)
        {
            producerDone = true;
            LeaveCriticalSection(LOCK);
            SetEvent((HANDLE)dataEvent);
            return;
        }

        Chunk &c = chunks[(head + used) % chunkCount];
        unsigned long long remaining = size - nextOffset;
        c.offset = nextOffset;
        c.len = (remaining < chunkSize) ? (unsigned long)remaining : chunkSize;
        c.state = CHUNK_READING;
        used++;
        nextOffset += c.len;
        LeaveCriticalSection(LOCK);

        bool ok = inner->ReadAt(c.offset, c.data, c.len);

        EnterCriticalSection(LOCK);
        c.state = ok ? CHUNK_READY : CHUNK_FAILED;
        if (!ok)
            producerDone = true; // the consumer retries this one directly, and reports it if it still fails
        LeaveCriticalSection(LOCK);
        SetEvent((HANDLE)dataEvent);

        if (!ok)
            return;
    }
}

bool ReadAheadSource::ReadAt(unsigned long long offset, void *buffer, unsigned long len)
{
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    unsigned char *out = (unsigned char *)buffer;
    bool ok = true;

    if (running && offset != consumePos)
        Stop(); // not a continuation - the converter is looking something up

    if (!running)
    {
        ok = Direct(offset, out, len);

        // A large read is the converter starting on the data itself: carry
        // on from where it ends.
        if (ok && len >= chunkSize / 2)
            Start(offset + len);
    }
    else
    {
        while (len > 0)
        {
            EnterCriticalSection(LOCK);
            while (used == 0 && !producerDone)
            {
                LeaveCriticalSection(LOCK);
                WaitForSingleObject((HANDLE)dataEvent, INFINITE);
                EnterCriticalSection(LOCK);
            }

            if (used == 0)
            {
                // The thread has stopped - the end of the source, or a failed read.
                LeaveCriticalSection(LOCK);
                Stop();
                ok = Direct(offset, out, len);
                break;
            }

            Chunk &c = chunks[head];
            while (c.state == CHUNK_READING)
            {
                LeaveCriticalSection(LOCK);
                WaitForSingleObject((HANDLE)dataEvent, INFINITE);
                EnterCriticalSection(LOCK);
            }
            int state = c.state;
            LeaveCriticalSection(LOCK);

            if (state != CHUNK_READY || offset < c.offset || offset >= c.offset + c.len)
            {
                // A failed read gets the source's own retries, directly.
                Stop();
                ok = Direct(offset, out, len);
                break;
            }

            unsigned long avail = (unsigned long)(c.offset + c.len - offset);
            unsigned long take = (avail < len) ? avail : len;
            memcpy(out, c.data + (offset - c.offset), take);
            out += take;
            offset += take;
            len -= take;

            if (offset == c.offset + c.len)
            {
                EnterCriticalSection(LOCK);
                c.state = CHUNK_EMPTY;
                head = (head + 1) % chunkCount;
                used--;
                LeaveCriticalSection(LOCK);
                SetEvent((HANDLE)spaceEvent);
            }
        }
        consumePos = offset;
    }

    QueryPerformanceCounter(&t1);
    waitMs += (double)(t1.QuadPart - t0.QuadPart) * msPerTick;
    return ok;
}
