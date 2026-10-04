#ifndef GOD_CONVERT_H
#define GOD_CONVERT_H

// Converts an Xbox 360 disc image into a Games on Demand (GOD) package - the
// format the dashboard and Aurora launch installed games from:
//
//   <contentRoot>\<TitleID>\00007000\<MediaID>          the header ("LIVE" package)
//   <contentRoot>\<TitleID>\00007000\<MediaID>.data\    Data0000, Data0001, ...
//
// An Original Xbox disc - default.xbe rather than default.xex - becomes the
// same kind of package, as the 360's backwards compatibility runs them, of
// content type 00005000 and named for its title ID, as iso2god-rs does:
//
//   <contentRoot>\<TitleID>\00005000\<TitleID>          and <TitleID>.data\
//
// Only the disc's game partition is copied, and only up to the last sector
// its files use - the rest of a disc is padding, which is why a GOD install
// is smaller than the ISO it came from.
//
// Each data file is up to 0xA1C4 blocks of 4KB. They are grouped 0xCC to a
// sub-hash block (the SHA-1 of every block after it), and each file opens
// with a master hash block listing its sub-hash blocks' SHA-1s. The master
// blocks are then chained from the last file back to the first - each one
// ends with the SHA-1 of the next file's - and the header carries the first
// file's, so the console can verify the whole package from the header down.
//
// The layout follows the original Iso2God and iso2god-rs
// (https://github.com/iliazeus/iso2god-rs, MIT). Where the two differ, this
// does what the original does: the last data file holds only the blocks the
// game uses, where iso2god-rs fills it out to the end of the ISO.
//
// Where the image comes from is the caller's business - a file on a PC, the
// console's disc drive, or anything else that can read at an offset - so the
// same code is tested on a PC against real ISOs.

// A disc image, read at arbitrary offsets. ReadAt reads exactly len bytes;
// reads past Size() are the caller's to avoid (GodConvert never asks for them).
class GodSource
{
public:
    virtual ~GodSource() {}
    virtual bool ReadAt(unsigned long long offset, void *buffer, unsigned long len) = 0;
    virtual unsigned long long Size() = 0;
};

#define GOD_CONTENT_GAMES_ON_DEMAND 0x00007000UL // a 360 disc's package
#define GOD_CONTENT_XBOX_ORIGINAL   0x00005000UL // an Original Xbox disc's

// The execution ID from default.xex - what the header says the package is -
// or for an Original Xbox disc, what default.xbe's certificate says.
struct GodTitleInfo
{
    unsigned long contentType;   // GOD_CONTENT_*
    unsigned long mediaId;       // an Original Xbox disc has none: its title ID, which names the package
    unsigned long version;
    unsigned long baseVersion;
    unsigned long titleId;
    unsigned char platform;
    unsigned char executableType;
    unsigned char discNumber;
    unsigned char discCount;
    char name[96];               // an Original Xbox disc's, from default.xbe's certificate, UTF-8; else empty
};

struct GodImageInfo
{
    GodTitleInfo title;

    const char *imageType;          // "XGD2", "XGD3", ... for logs
    unsigned long long rootOffset;  // where the game partition starts in the image
    unsigned long long usedSize;    // bytes of the partition the game's files reach

    unsigned long blockCount;       // 4KB blocks to copy
    unsigned long partCount;        // Data files
    unsigned long long outputSize;  // every byte the package will take, header included
};

enum GodResult
{
    GOD_OK,
    GOD_NOT_A_DISC_IMAGE,    // no Xbox game partition found
    GOD_BAD_FILESYSTEM,      // the partition's directory tables don't parse
    GOD_NO_DEFAULT_XEX,      // neither \default.xex nor an Original Xbox disc's \default.xbe
    GOD_BAD_XEX,             // default.xex has no readable execution ID, or default.xbe no certificate
    GOD_READ_FAILED,
    GOD_WRITE_FAILED,
    GOD_OUT_OF_MEMORY,
    GOD_CANCELLED
};

const char *GodResultText(GodResult result);

// The title of a default.xex or default.xbe on the drive - an extracted
// game's - from its header, as GodInspect reads a disc's.
GodResult GodReadExecutableFile(const char *path, GodTitleInfo *out);

// Reads the image's filesystem and default.xex, and works out how big the
// package will be - enough to check the free space before starting.
GodResult GodInspect(GodSource *source, GodImageInfo *outInfo);

// Called after every sub-hash group (816KB) with bytes of the image copied so
// far. Returning false cancels the conversion.
typedef bool (*GodProgressFn)(unsigned long long bytesDone, unsigned long long bytesTotal, void *context);

// Where a conversion's time went, in milliseconds - for the log, to see
// whether the source, the hashing or the drive being written to is what
// limits the speed.
struct GodTimings
{
    double readMs;      // waiting for the source
    double hashMs;
    double writeMs;
    double progressMs;  // inside the progress callback (drawing, the cancel prompt)
};

// A disc image read through its filesystem: only the sectors its files and
// directory tables are in are read from the source, and every other byte
// comes back zero. An Original Xbox disc needs it - the 360's drive refuses
// to read its security ranges (status 0xC0000010), stretches no file is ever
// in, so copying the partition straight through stops at the first one.
// It skips the filler between files too, so it's faster.
//
// Build() walks the filesystem once; until it succeeds, reads pass straight
// through. Reading only forward is fine - ReadAheadSource sits on top.
class UsedSectorsSource : public GodSource
{
public:
    UsedSectorsSource(GodSource *inner, unsigned long long rootOffset);
    ~UsedSectorsSource();

    GodResult Build();
    bool ReadAt(unsigned long long offset, void *buffer, unsigned long len);
    unsigned long long Size() { return inner->Size(); }

    unsigned long long UsedBytes() const { return usedBytes; } // for the log
    unsigned long Extents() const { return count; }

private:
    GodSource *inner;
    unsigned long long rootOffset;
    unsigned long long *starts; // partition-relative byte ranges, sorted and merged
    unsigned long long *ends;
    unsigned long count;
    unsigned long long usedBytes;
};

// Largest icon the header has room for (metadata version 2 thumbnails).
#define GOD_ICON_MAX 0x3D00

// Writes the package under contentRoot (e.g. "Hdd1:\Content\0000000000000000").
// titleName is UTF-8 and goes in the header for the dashboard to show.
// iconPng, if non-NULL, is the title's icon as a PNG of at most GOD_ICON_MAX
// bytes (a larger one is left out) - what the dashboard, Aurora and our own
// library show for the game.
//
// The header is written last, so a package is only ever visible once it is
// whole. On any failure, what was written is removed again. outPackagePath,
// if non-NULL, gets the header's path, and outTimings where the time went.
GodResult GodConvert(GodSource *source, const GodImageInfo &info, const char *contentRoot,
                     const char *titleName, const unsigned char *iconPng, unsigned long iconPngSize,
                     GodProgressFn progress, void *progressContext,
                     char *outPackagePath, unsigned long outPackagePathSize,
                     GodTimings *outTimings = 0);

// For an install that ended too abruptly to clean up after itself - the
// console switched off, or the Guide button back to the dashboard. GodConvert
// removes its own partial output on every failure it sees; these are for the
// ones it can't.
//
// GodPackageSizeOnDisk is what the package's files take now, header included,
// or 0 if it has no header - compare it with GodImageInfo::outputSize to tell
// a finished install from a partial one. GodRemovePackage removes whatever of
// the package exists, header first; the title's other content (DLC, title
// updates) is left alone. A media ID equal to the title ID means an Original
// Xbox package, in 00005000 - a 360 disc's media ID never is its title ID.
unsigned long long GodPackageSizeOnDisk(const char *contentRoot, unsigned long titleId, unsigned long mediaId);
void GodRemovePackage(const char *contentRoot, unsigned long titleId, unsigned long mediaId);

#endif
