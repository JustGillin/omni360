#ifndef DISC_SOURCE_H
#define DISC_SOURCE_H

#include "GodConvert.h"

// The disc in the console's drive, as a GodSource - so a game disc can be
// installed as Games on Demand without an ISO ever existing.
//
// The drive is opened raw (\Device\Cdrom0) and read in whole 2KB sectors,
// which is all the device accepts; reads at other offsets and lengths go
// through a sector-aligned bounce buffer. A read that fails is retried a few
// times before giving up, since a scratched disc often reads on a second try.
//
// Which part of the disc a raw read sees on a retail game disc is left to the
// converter to find out: GodInspect checks every known partition offset, so
// it works whether the drive presents the whole disc or only the game
// partition. Open() logs what it found either way, since that is the first
// thing to look at if a disc won't install.
class DiscSource : public GodSource
{
public:
    DiscSource();
    ~DiscSource();

    // False if there's no drive, no disc, or it can't be read. print, if
    // non-NULL, gets a line saying why, and the sizes the drive reports.
    bool Open(void (*print)(const char *format, ...));
    void Close();

    bool ReadAt(unsigned long long offset, void *buffer, unsigned long len);
    unsigned long long Size();

    // The first bytes of sector 0x20 at each partition offset GodInspect
    // checks, to the log - where the volume descriptor turned up, if anywhere.
    void LogProbe(void (*print)(const char *format, ...));

private:
    bool ReadSectors(unsigned long long start, unsigned long bytes);

    void *handle;
    unsigned long long size;
    unsigned char *bounce;
    void (*print)(const char *format, ...);
    int readErrorsLogged;
};

#endif
