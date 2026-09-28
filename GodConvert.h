#ifndef GOD_CONVERT_H
#define GOD_CONVERT_H

// Converts an Xbox 360 disc image into a Games on Demand (GOD) package - the
// format the dashboard and Aurora launch installed games from:
//
//   <contentRoot>\<TitleID>\00007000\<MediaID>          the header ("LIVE" package)
//   <contentRoot>\<TitleID>\00007000\<MediaID>.data\    Data0000, Data0001, ...
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

// The execution ID from default.xex - what the header says the package is.
struct GodTitleInfo
{
    unsigned long mediaId;
    unsigned long version;
    unsigned long baseVersion;
    unsigned long titleId;
    unsigned char platform;
    unsigned char executableType;
    unsigned char discNumber;
    unsigned char discCount;
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
    GOD_NO_DEFAULT_XEX,      // no \default.xex (an original Xbox disc has default.xbe - not handled yet)
    GOD_BAD_XEX,             // default.xex has no readable execution ID
    GOD_READ_FAILED,
    GOD_WRITE_FAILED,
    GOD_OUT_OF_MEMORY,
    GOD_CANCELLED
};

const char *GodResultText(GodResult result);

// Reads the image's filesystem and default.xex, and works out how big the
// package will be - enough to check the free space before starting.
GodResult GodInspect(GodSource *source, GodImageInfo *outInfo);

// Called after every sub-hash group (816KB) with bytes of the image copied so
// far. Returning false cancels the conversion.
typedef bool (*GodProgressFn)(unsigned long long bytesDone, unsigned long long bytesTotal, void *context);

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
// if non-NULL, gets the header's path.
GodResult GodConvert(GodSource *source, const GodImageInfo &info, const char *contentRoot,
                     const char *titleName, const unsigned char *iconPng, unsigned long iconPngSize,
                     GodProgressFn progress, void *progressContext,
                     char *outPackagePath, unsigned long outPackagePathSize);

#endif
