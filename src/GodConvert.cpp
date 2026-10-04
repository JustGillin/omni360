/*
FILE : GodConvert.cpp
PROJECT : Omni360
DESCRIPTION : Xbox 360 disc image to Games on Demand package. See
              GodConvert.h for the layout.

The disc side is XDVDFS (also called GDF): a volume descriptor at sector 0x20
of the game partition, then directory tables of little-endian entries. The
package side is an SVOD "LIVE" package. Offsets for both are from free60.org
and were checked against the original Iso2God and iso2god-rs.
*/

#include "GodConvert.h"

#ifdef _XBOX
#include <xtl.h>
#else
#include <windows.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inc\bearssl_hash.h"

#define SECTOR_SIZE       0x800ULL
#define BLOCK_SIZE        0x1000UL
#define BLOCKS_PER_GROUP  0xCCUL                                 // data blocks under one sub-hash block
#define GROUPS_PER_PART   0xCBUL                                 // sub-hash blocks under one master hash block
#define BLOCKS_PER_PART   (BLOCKS_PER_GROUP * GROUPS_PER_PART)   // 0xA1C4 data blocks in a full Data file
#define PART_FILE_BLOCKS  (1 + GROUPS_PER_PART * (1 + BLOCKS_PER_GROUP)) // 0xA290 blocks on disk, hashes included
#define HEADER_SIZE       0xB000UL
#define CONTENT_TYPE_GOD  GOD_CONTENT_GAMES_ON_DEMAND

// Limits on what the directory walk will accept. Real discs are nowhere near
// them; they are there so a corrupt table can't recurse forever or claim a
// multi-gigabyte directory.
#define MAX_DIR_DEPTH     32
#define MAX_DIR_ENTRIES   200000UL
#define MAX_TABLE_SIZE    (16UL * 1024 * 1024)

static unsigned long ReadLE16(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8);
}

static unsigned long ReadLE32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

static unsigned long ReadBE32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8) | (unsigned long)p[3];
}

static void WriteBE16(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

static void WriteBE24(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 16);
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)v;
}

static void WriteBE32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void WriteBE64(unsigned char *p, unsigned long long v)
{
    WriteBE32(p, (unsigned long)(v >> 32));
    WriteBE32(p + 4, (unsigned long)v);
}

static void Sha1(const void *data, unsigned long len, unsigned char out[20])
{
    br_sha1_context ctx;
    br_sha1_init(&ctx);
    br_sha1_update(&ctx, data, len);
    br_sha1_out(&ctx, out);
}

const char *GodResultText(GodResult result)
{
    switch (result)
    {
    case GOD_OK:               return "OK";
    case GOD_NOT_A_DISC_IMAGE: return "This isn't an Xbox 360 disc image";
    case GOD_BAD_FILESYSTEM:   return "The disc image's file table is damaged";
    case GOD_NO_DEFAULT_XEX:   return "No default.xex or default.xbe on the disc";
    case GOD_BAD_XEX:          return "The disc's default.xex or default.xbe has no title information";
    case GOD_READ_FAILED:      return "Couldn't read the disc image";
    case GOD_WRITE_FAILED:     return "Couldn't write the game to the drive";
    case GOD_OUT_OF_MEMORY:    return "Out of memory";
    case GOD_CANCELLED:        return "Cancelled";
    }
    return "Unknown error";
}

// ---------------------------------------------------------------------------
// Reading the disc
// ---------------------------------------------------------------------------

// Where the game partition sits, by disc format. Checked in this order - the
// order both reference tools use - because an XGD3 disc's descriptor offset
// is the least distinctive.
struct ImageType
{
    const char *name;
    unsigned long long rootOffset;
};

static const ImageType kImageTypes[] =
{
    { "XSF",  0x0ULL },        // just the game partition, already cut out of a disc
    { "XGD2", 0xFD90000ULL },
    { "XGD1", 0x18300000ULL }, // original Xbox
    { "XGD3", 0x2080000ULL },
};

static const char kVolumeMagic[] = "MICROSOFT*XBOX*MEDIA"; // 20 bytes on disc, no terminator

struct DirWalk
{
    GodSource *source;
    unsigned long long rootOffset;
    unsigned long long partitionSize;

    unsigned long long usedEnd; // furthest byte any table or file reaches
    unsigned long entries;

    bool foundXex;
    unsigned long xexSector;
    unsigned long xexSize;

    bool foundXbe;           // an Original Xbox disc's
    unsigned long xbeSector;
    unsigned long xbeSize;

    // Every table's and file's byte range, when UsedSectorsSource wants
    // them; NULL otherwise.
    unsigned long long *starts;
    unsigned long long *ends;
    unsigned long extentCount;
    unsigned long extentCapacity;
    bool outOfMemory;
};

static void AddExtent(DirWalk &w, unsigned long long start, unsigned long long length)
{
    if (w.starts == NULL || length == 0 || w.outOfMemory)
        return;
    if (w.extentCount == w.extentCapacity)
    {
        const unsigned long grow = w.extentCapacity * 2;
        unsigned long long *s2 = (unsigned long long *)realloc(w.starts, grow * sizeof(unsigned long long));
        if (s2 == NULL) { w.outOfMemory = true; return; }
        w.starts = s2;
        unsigned long long *e2 = (unsigned long long *)realloc(w.ends, grow * sizeof(unsigned long long));
        if (e2 == NULL) { w.outOfMemory = true; return; }
        w.ends = e2;
        w.extentCapacity = grow;
    }
    // Whole sectors: the drive reads no less.
    const unsigned long long first = start / SECTOR_SIZE * SECTOR_SIZE;
    const unsigned long long end = (start + length + SECTOR_SIZE - 1) / SECTOR_SIZE * SECTOR_SIZE;
    w.starts[w.extentCount] = first;
    w.ends[w.extentCount] = end;
    w.extentCount++;
}

static bool NameIs(const unsigned char *name, unsigned long len, const char *want)
{
    if (strlen(want) != len)
        return false;
    for (unsigned long i = 0; i < len; ++i)
    {
        char a = (char)name[i], b = want[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

// One directory table. Entries are packed into its sectors, 4-byte aligned,
// and never cross a sector boundary; the rest of a sector is 0xFF. The tables
// are really binary trees (the first two fields are child offsets), but every
// entry of a table is in its sectors, so reading them in order finds them all.
static GodResult WalkTable(DirWalk &w, unsigned long sector, unsigned long size, int depth, bool isRoot)
{
    if (size == 0)
        return GOD_OK;
    if (depth > MAX_DIR_DEPTH || size > MAX_TABLE_SIZE)
        return GOD_BAD_FILESYSTEM;

    unsigned long long tableStart = (unsigned long long)sector * SECTOR_SIZE;
    unsigned long sectors = (unsigned long)((size + SECTOR_SIZE - 1) / SECTOR_SIZE);
    unsigned long long tableBytes = (unsigned long long)sectors * SECTOR_SIZE;

    if (tableStart + tableBytes > w.partitionSize)
        return GOD_BAD_FILESYSTEM;

    if (tableStart + size > w.usedEnd)
        w.usedEnd = tableStart + size;
    AddExtent(w, tableStart, tableBytes);

    unsigned char *table = (unsigned char *)malloc((size_t)tableBytes);
    if (table == NULL)
        return GOD_OUT_OF_MEMORY;

    if (!w.source->ReadAt(w.rootOffset + tableStart, table, (unsigned long)tableBytes))
    {
        free(table);
        return GOD_READ_FAILED;
    }

    GodResult result = GOD_OK;

    for (unsigned long s = 0; s < sectors && result == GOD_OK; ++s)
    {
        const unsigned char *sec = table + s * SECTOR_SIZE;
        unsigned long pos = 0;

        while (pos + 14 <= SECTOR_SIZE)
        {
            const unsigned char *e = sec + pos;

            if (ReadLE16(e) == 0xFFFF || ReadLE16(e + 2) == 0xFFFF)
                break; // padding - the rest of this sector is empty

            unsigned long entSector = ReadLE32(e + 4);
            unsigned long entSize = ReadLE32(e + 8);
            unsigned char attributes = e[12];
            unsigned long nameLen = e[13];

            // No real entry has an empty name, and a name can't run past the
            // sector; either means the rest of it is zero fill.
            if (nameLen == 0 || pos + 14 + nameLen > SECTOR_SIZE)
                break;

            if (++w.entries > MAX_DIR_ENTRIES)
            {
                result = GOD_BAD_FILESYSTEM;
                break;
            }

            const bool isDirectory = (attributes & 0x10) != 0;

            if (entSize > 0)
            {
                unsigned long long end = (unsigned long long)entSector * SECTOR_SIZE + entSize;
                if (end > w.usedEnd)
                    w.usedEnd = end;
                if (!isDirectory) // a directory's table is added as it's walked
                    AddExtent(w, (unsigned long long)entSector * SECTOR_SIZE, entSize);
            }

            if (isRoot && !isDirectory && NameIs(e + 14, nameLen, "default.xex"))
            {
                w.foundXex = true;
                w.xexSector = entSector;
                w.xexSize = entSize;
            }
            if (isRoot && !isDirectory && NameIs(e + 14, nameLen, "default.xbe"))
            {
                w.foundXbe = true;
                w.xbeSector = entSector;
                w.xbeSize = entSize;
            }

            if (isDirectory)
            {
                result = WalkTable(w, entSector, entSize, depth + 1, false);
                if (result != GOD_OK)
                    break;
            }

            pos = (pos + 14 + nameLen + 3) & ~3UL;
        }
    }

    free(table);
    return result;
}

// The execution ID (optional header 0x00040006) out of default.xex's header.
// The header itself is plain even on a retail disc - only the image after it
// is encrypted - so no keys are needed.
static GodResult ReadExecutionInfo(GodSource *source, unsigned long long xexOffset, unsigned long xexSize,
                                   GodTitleInfo *out)
{
    unsigned char head[24];
    if (xexSize < sizeof(head))
        return GOD_BAD_XEX;
    if (!source->ReadAt(xexOffset, head, sizeof(head)))
        return GOD_READ_FAILED;
    if (memcmp(head, "XEX2", 4) != 0)
        return GOD_BAD_XEX;

    unsigned long fieldCount = ReadBE32(head + 20);
    if (fieldCount > 0x400 || 24 + fieldCount * 8 > xexSize)
        return GOD_BAD_XEX;

    unsigned char *fields = (unsigned char *)malloc(fieldCount * 8 + 1);
    if (fields == NULL)
        return GOD_OUT_OF_MEMORY;
    if (fieldCount > 0 && !source->ReadAt(xexOffset + 24, fields, fieldCount * 8))
    {
        free(fields);
        return GOD_READ_FAILED;
    }

    unsigned long infoOffset = 0;
    bool found = false;
    for (unsigned long i = 0; i < fieldCount; ++i)
    {
        if (ReadBE32(fields + i * 8) == 0x00040006UL)
        {
            infoOffset = ReadBE32(fields + i * 8 + 4);
            found = true;
            break;
        }
    }
    free(fields);

    unsigned char info[20];
    if (!found || infoOffset > xexSize || xexSize - infoOffset < sizeof(info))
        return GOD_BAD_XEX;
    if (!source->ReadAt(xexOffset + infoOffset, info, sizeof(info)))
        return GOD_READ_FAILED;

    out->contentType = CONTENT_TYPE_GOD;
    out->mediaId = ReadBE32(info + 0);
    out->version = ReadBE32(info + 4);
    out->baseVersion = ReadBE32(info + 8);
    out->titleId = ReadBE32(info + 12);
    out->platform = info[16];
    out->executableType = info[17];
    out->discNumber = info[18];
    out->discCount = info[19];
    return GOD_OK;
}

// An Original Xbox disc's title, from default.xbe's certificate: "XBEH", the
// image's base address at 0x104 and the certificate's at 0x118, both
// little-endian; the certificate's title ID at +0x08 and version at +0xAC.
// The disc has no media ID - its title ID names the package - and is one
// disc, as iso2god-rs writes it.
static GodResult ReadXbeInfo(GodSource *source, unsigned long long xbeOffset, unsigned long xbeSize,
                             GodTitleInfo *out)
{
    unsigned char head[0x120];
    if (xbeSize < sizeof(head))
        return GOD_BAD_XEX;
    if (!source->ReadAt(xbeOffset, head, sizeof(head)))
        return GOD_READ_FAILED;
    if (memcmp(head, "XBEH", 4) != 0)
        return GOD_BAD_XEX;

    const unsigned long base = ReadLE32(head + 0x104);
    const unsigned long certAddress = ReadLE32(head + 0x118);
    if (certAddress < base)
        return GOD_BAD_XEX;
    const unsigned long certOffset = certAddress - base;
    unsigned char cert[0xB0];
    if (certOffset > xbeSize || xbeSize - certOffset < sizeof(cert))
        return GOD_BAD_XEX;
    if (!source->ReadAt(xbeOffset + certOffset, cert, sizeof(cert)))
        return GOD_READ_FAILED;

    out->contentType = GOD_CONTENT_XBOX_ORIGINAL;
    out->titleId = ReadLE32(cert + 0x08);
    out->mediaId = out->titleId;
    out->version = ReadLE32(cert + 0xAC);
    out->baseVersion = 0;
    out->platform = 0;
    out->executableType = 0;
    out->discNumber = 1;
    out->discCount = 1;

    // The certificate's title name: 40 UTF-16LE characters at +0x0C, to
    // UTF-8 - the name to show for a disc no list knows.
    size_t n = 0;
    for (int i = 0; i < 40; ++i)
    {
        const unsigned long c = ReadLE16(cert + 0x0C + i * 2);
        if (c == 0)
            break;
        if (c < 0x80 && n + 1 < sizeof(out->name))
            out->name[n++] = (char)c;
        else if (c < 0x800 && n + 2 < sizeof(out->name))
        {
            out->name[n++] = (char)(0xC0 | (c >> 6));
            out->name[n++] = (char)(0x80 | (c & 0x3F));
        }
        else if (c >= 0x800 && n + 3 < sizeof(out->name))
        {
            out->name[n++] = (char)(0xE0 | (c >> 12));
            out->name[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out->name[n++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out->name[n] = '\0';
    while (n > 0 && out->name[n - 1] == ' ')
        out->name[--n] = '\0';
    return out->titleId != 0 ? GOD_OK : GOD_BAD_XEX;
}

// Blocks in the last Data file, and its size on disk with its hash blocks.
static unsigned long LastPartBlocks(const GodImageInfo &info)
{
    return info.blockCount - (info.partCount - 1) * BLOCKS_PER_PART;
}

static unsigned long long PartFileSize(unsigned long blocks)
{
    unsigned long groups = (blocks + BLOCKS_PER_GROUP - 1) / BLOCKS_PER_GROUP;
    return (unsigned long long)(1 + groups + blocks) * BLOCK_SIZE;
}

GodResult GodInspect(GodSource *source, GodImageInfo *outInfo)
{
    memset(outInfo, 0, sizeof(*outInfo));

    const unsigned long long imageSize = source->Size();

    const ImageType *type = NULL;
    unsigned char descriptor[28];

    for (size_t i = 0; i < sizeof(kImageTypes) / sizeof(kImageTypes[0]); ++i)
    {
        unsigned long long at = kImageTypes[i].rootOffset + 0x20 * SECTOR_SIZE;
        if (at + sizeof(descriptor) > imageSize)
            continue;
        if (!source->ReadAt(at, descriptor, sizeof(descriptor)))
            return GOD_READ_FAILED;
        if (memcmp(descriptor, kVolumeMagic, 20) == 0)
        {
            type = &kImageTypes[i];
            break;
        }
    }

    if (type == NULL)
        return GOD_NOT_A_DISC_IMAGE;

    DirWalk w;
    memset(&w, 0, sizeof(w));
    w.source = source;
    w.rootOffset = type->rootOffset;
    w.partitionSize = imageSize - type->rootOffset;
    w.usedEnd = 0x21 * SECTOR_SIZE; // through the volume descriptor itself

    GodResult result = WalkTable(w, ReadLE32(descriptor + 20), ReadLE32(descriptor + 24), 0, true);
    if (result != GOD_OK)
        return result;

    if (w.foundXex)
    {
        unsigned long long xexStart = (unsigned long long)w.xexSector * SECTOR_SIZE;
        if (xexStart + w.xexSize > w.partitionSize)
            return GOD_BAD_FILESYSTEM;
        result = ReadExecutionInfo(source, type->rootOffset + xexStart, w.xexSize, &outInfo->title);
    }
    else if (w.foundXbe)
    {
        // An Original Xbox disc.
        unsigned long long xbeStart = (unsigned long long)w.xbeSector * SECTOR_SIZE;
        if (xbeStart + w.xbeSize > w.partitionSize)
            return GOD_BAD_FILESYSTEM;
        result = ReadXbeInfo(source, type->rootOffset + xbeStart, w.xbeSize, &outInfo->title);
    }
    else
    {
        return GOD_NO_DEFAULT_XEX;
    }
    if (result != GOD_OK)
        return result;

    outInfo->imageType = type->name;
    outInfo->rootOffset = type->rootOffset;
    outInfo->usedSize = (w.usedEnd < w.partitionSize) ? w.usedEnd : w.partitionSize;

    outInfo->blockCount = (unsigned long)((outInfo->usedSize + BLOCK_SIZE - 1) / BLOCK_SIZE);
    outInfo->partCount = (outInfo->blockCount + BLOCKS_PER_PART - 1) / BLOCKS_PER_PART;
    if (outInfo->partCount == 0)
        outInfo->partCount = 1;

    outInfo->outputSize = HEADER_SIZE
                        + (unsigned long long)(outInfo->partCount - 1) * PART_FILE_BLOCKS * BLOCK_SIZE
                        + PartFileSize(LastPartBlocks(*outInfo));
    return GOD_OK;
}

// ---------------------------------------------------------------------------
// Reading only what the filesystem uses
// ---------------------------------------------------------------------------

UsedSectorsSource::UsedSectorsSource(GodSource *inner_, unsigned long long rootOffset_)
    : inner(inner_), rootOffset(rootOffset_), starts(NULL), ends(NULL), count(0), usedBytes(0)
{
}

UsedSectorsSource::~UsedSectorsSource()
{
    free(starts);
    free(ends);
}

GodResult UsedSectorsSource::Build()
{
    unsigned char descriptor[28];
    if (!inner->ReadAt(rootOffset + 0x20 * SECTOR_SIZE, descriptor, sizeof(descriptor)))
        return GOD_READ_FAILED;
    if (memcmp(descriptor, kVolumeMagic, 20) != 0)
        return GOD_NOT_A_DISC_IMAGE;

    DirWalk w;
    memset(&w, 0, sizeof(w));
    w.source = inner;
    w.rootOffset = rootOffset;
    w.partitionSize = inner->Size() - rootOffset;
    w.usedEnd = 0x21 * SECTOR_SIZE;
    w.extentCapacity = 1024;
    w.starts = (unsigned long long *)malloc(w.extentCapacity * sizeof(unsigned long long));
    w.ends = (unsigned long long *)malloc(w.extentCapacity * sizeof(unsigned long long));
    if (w.starts == NULL || w.ends == NULL)
    {
        free(w.starts);
        free(w.ends);
        return GOD_OUT_OF_MEMORY;
    }
    // The volume descriptor and what's before it.
    AddExtent(w, 0, 0x22 * SECTOR_SIZE);

    GodResult result = WalkTable(w, ReadLE32(descriptor + 20), ReadLE32(descriptor + 24), 0, true);
    if (result == GOD_OK && w.outOfMemory)
        result = GOD_OUT_OF_MEMORY;
    if (result != GOD_OK)
    {
        free(w.starts);
        free(w.ends);
        return result;
    }

    // Sorted by start (an insertion sort: a disc's files are nearly in
    // order already), then overlapping and touching ranges merged.
    for (unsigned long i = 1; i < w.extentCount; ++i)
    {
        const unsigned long long ks = w.starts[i], ke = w.ends[i];
        unsigned long j = i;
        while (j > 0 && w.starts[j - 1] > ks)
        {
            w.starts[j] = w.starts[j - 1];
            w.ends[j] = w.ends[j - 1];
            --j;
        }
        w.starts[j] = ks;
        w.ends[j] = ke;
    }
    unsigned long n = 0;
    for (unsigned long i = 0; i < w.extentCount; ++i)
    {
        if (n > 0 && w.starts[i] <= w.ends[n - 1])
        {
            if (w.ends[i] > w.ends[n - 1])
                w.ends[n - 1] = w.ends[i];
        }
        else
        {
            w.starts[n] = w.starts[i];
            w.ends[n] = w.ends[i];
            n++;
        }
    }

    free(starts);
    free(ends);
    starts = w.starts;
    ends = w.ends;
    count = n;
    usedBytes = 0;
    for (unsigned long i = 0; i < n; ++i)
        usedBytes += ends[i] - starts[i];
    return GOD_OK;
}

bool UsedSectorsSource::ReadAt(unsigned long long offset, void *buffer, unsigned long len)
{
    if (starts == NULL || offset < rootOffset)
        return inner->ReadAt(offset, buffer, len);

    unsigned char *out = (unsigned char *)buffer;
    memset(out, 0, len);
    const unsigned long long from = offset - rootOffset, to = from + len;

    // The first range that ends after from, by binary search.
    unsigned long lo = 0, hi = count;
    while (lo < hi)
    {
        const unsigned long mid = (lo + hi) / 2;
        if (ends[mid] <= from)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (unsigned long i = lo; i < count && starts[i] < to; ++i)
    {
        const unsigned long long a = (starts[i] > from) ? starts[i] : from;
        const unsigned long long b = (ends[i] < to) ? ends[i] : to;
        if (b > a && !inner->ReadAt(rootOffset + a, out + (a - from), (unsigned long)(b - a)))
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Writing the package
// ---------------------------------------------------------------------------

struct PackagePaths
{
    char titleDir[512];   // <root>\<TitleID>
    char typeDir[512];    // <root>\<TitleID>\00007000, or 00005000
    char header[512];     // ...\<MediaID>
    char dataDir[512];    // ...\<MediaID>.data
};

static bool BuildPaths(const char *contentRoot, const GodTitleInfo &title, PackagePaths *p)
{
    int n1 = _snprintf(p->titleDir, sizeof(p->titleDir), "%s\\%08lX", contentRoot, title.titleId);
    const unsigned long type = (title.contentType != 0) ? title.contentType : CONTENT_TYPE_GOD;
    int n2 = _snprintf(p->typeDir, sizeof(p->typeDir), "%s\\%08lX", p->titleDir, type);
    int n3 = _snprintf(p->header, sizeof(p->header), "%s\\%08lX", p->typeDir, title.mediaId);
    int n4 = _snprintf(p->dataDir, sizeof(p->dataDir), "%s.data", p->header);

    // _snprintf on this toolchain doesn't terminate on truncation.
    p->titleDir[sizeof(p->titleDir) - 1] = p->typeDir[sizeof(p->typeDir) - 1] = '\0';
    p->header[sizeof(p->header) - 1] = p->dataDir[sizeof(p->dataDir) - 1] = '\0';

    return n1 > 0 && n2 > 0 && n3 > 0 && n4 > 0 && (size_t)n4 < sizeof(p->dataDir) - 16;
}

static void PartPath(const PackagePaths &p, unsigned long part, char *out, size_t outSize)
{
    _snprintf(out, outSize, "%s\\Data%04lu", p.dataDir, part);
    out[outSize - 1] = '\0';
}

static bool EnsureDirectory(const char *path)
{
    if (CreateDirectoryA(path, NULL))
        return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

// Takes away a package at these paths - a previous install of the same disc,
// or what a failed conversion left. The header goes first, so the package
// stops being visible before its data does. The title and type folders are
// only removed if empty: they also hold the game's DLC and title updates.
static void RemovePackage(const PackagePaths &p)
{
    DeleteFileA(p.header);

    char part[600];
    for (unsigned long i = 0; i < 10000; ++i)
    {
        PartPath(p, i, part, sizeof(part));
        if (!DeleteFileA(part))
            break; // Data files are numbered without gaps
    }

    RemoveDirectoryA(p.dataDir);
    RemoveDirectoryA(p.typeDir);
    RemoveDirectoryA(p.titleDir);
}

static bool PathsFor(const char *contentRoot, unsigned long titleId, unsigned long mediaId, PackagePaths *p)
{
    GodTitleInfo title;
    memset(&title, 0, sizeof(title));
    title.titleId = titleId;
    title.mediaId = mediaId;
    // Named for its title ID: an Original Xbox package.
    title.contentType = (mediaId == titleId) ? GOD_CONTENT_XBOX_ORIGINAL : CONTENT_TYPE_GOD;
    return BuildPaths(contentRoot, title, p);
}

static unsigned long long FileSize(const char *path, bool *exists)
{
    *exists = false;
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return 0;
    *exists = true;
    unsigned long long size = 0;
    if (_fseeki64(f, 0, SEEK_END) == 0)
    {
        __int64 at = _ftelli64(f);
        if (at > 0)
            size = (unsigned long long)at;
    }
    fclose(f);
    return size;
}

unsigned long long GodPackageSizeOnDisk(const char *contentRoot, unsigned long titleId, unsigned long mediaId)
{
    PackagePaths paths;
    if (!PathsFor(contentRoot, titleId, mediaId, &paths))
        return 0;

    bool exists = false;
    unsigned long long total = FileSize(paths.header, &exists);
    if (!exists)
        return 0;

    char part[600];
    for (unsigned long i = 0; i < 10000; ++i)
    {
        PartPath(paths, i, part, sizeof(part));
        unsigned long long size = FileSize(part, &exists);
        if (!exists)
            break;
        total += size;
    }
    return total;
}

void GodRemovePackage(const char *contentRoot, unsigned long titleId, unsigned long mediaId)
{
    PackagePaths paths;
    if (PathsFor(contentRoot, titleId, mediaId, &paths))
        RemovePackage(paths);
}

// UTF-8 to UTF-16 big-endian, into a header field of fieldBytes, always
// terminated. Characters outside the BMP become '?' - the field is UCS-2.
static void WriteTitleField(unsigned char *field, size_t fieldBytes, const char *utf8)
{
    size_t maxChars = fieldBytes / 2 - 1;
    size_t n = 0;
    const unsigned char *s = (const unsigned char *)utf8;

    while (*s != 0 && n < maxChars)
    {
        unsigned long c = *s++;
        int extra = 0;

        if (c >= 0xF0)      { c &= 0x07; extra = 3; }
        else if (c >= 0xE0) { c &= 0x0F; extra = 2; }
        else if (c >= 0xC0) { c &= 0x1F; extra = 1; }
        else if (c >= 0x80) { c = '?'; }

        for (; extra > 0 && (*s & 0xC0) == 0x80; --extra)
            c = (c << 6) | (*s++ & 0x3F);
        if (extra > 0 || c > 0xFFFF)
            c = '?';

        WriteBE16(field + n * 2, c);
        ++n;
    }
    WriteBE16(field + n * 2, 0);
}

static void BuildHeader(unsigned char *h, const GodImageInfo &info, const char *titleName,
                        const unsigned char *iconPng, unsigned long iconPngSize,
                        const unsigned char mhtHash[20], unsigned long long partsTotalSize)
{
    memset(h, 0, HEADER_SIZE);

    memcpy(h + 0x000, "LIVE", 4);  // signature left blank - modded consoles don't check it
    memset(h + 0x22C, 0xFF, 8);    // first license entry: any

    WriteBE32(h + 0x340, 0xAD0E);  // header size
    WriteBE32(h + 0x344, info.title.contentType != 0 ? info.title.contentType : CONTENT_TYPE_GOD);
    WriteBE32(h + 0x348, 2);       // metadata version

    // Version and base version (0x358, 0x35C) stay 0, as in both reference
    // tools' output.
    // An Original Xbox disc has no media ID: 0, as iso2god-rs writes it.
    WriteBE32(h + 0x354, info.title.contentType == GOD_CONTENT_XBOX_ORIGINAL ? 0 : info.title.mediaId);
    WriteBE32(h + 0x360, info.title.titleId);
    h[0x364] = info.title.platform;
    h[0x365] = info.title.executableType;
    h[0x366] = info.title.discNumber;
    h[0x367] = info.title.discCount;

    // SVOD volume descriptor.
    h[0x379] = 0x24;               // descriptor size
    h[0x37A] = 0x05;               // block cache element count
    h[0x37B] = 0x05;               // worker thread processor
    h[0x37C] = 0x11;               // worker thread priority
    memcpy(h + 0x37D, mhtHash, 20);// first Data file's master hash block
    WriteBE24(h + 0x392, info.blockCount);
    WriteBE16(h + 0x395, 0);       // blocks not allocated

    WriteBE32(h + 0x39D, info.partCount);
    WriteBE64(h + 0x3A1, partsTotalSize);
    WriteBE32(h + 0x3A9, 1);       // descriptor type: SVOD

    WriteTitleField(h + 0x411, 0x80, titleName);  // display name (first locale)
    WriteTitleField(h + 0x1691, 0x80, titleName); // title name

    // The same icon as both the package thumbnail and the title thumbnail,
    // as Iso2God does - for a game they are one and the same. Without one,
    // both sizes stay 0 and the dashboard shows its placeholder.
    if (iconPng != NULL && iconPngSize > 0 && iconPngSize <= GOD_ICON_MAX)
    {
        WriteBE32(h + 0x1712, iconPngSize);
        WriteBE32(h + 0x1716, iconPngSize);
        memcpy(h + 0x171A, iconPng, iconPngSize);
        memcpy(h + 0x571A, iconPng, iconPngSize);
    }

    Sha1(h + 0x344, HEADER_SIZE - 0x344, h + 0x32C);
}

// Reads len bytes of the partition at offset, zero-filling anything past the
// end of the image. A game's last file can end mid-block, and the image may
// be cut short right after it.
static bool ReadPartition(GodSource *source, const GodImageInfo &info, unsigned long long offset,
                          unsigned char *out, unsigned long len)
{
    unsigned long long imageSize = source->Size();
    unsigned long long at = info.rootOffset + offset;

    unsigned long avail = 0;
    if (at < imageSize)
        avail = (imageSize - at < len) ? (unsigned long)(imageSize - at) : len;

    if (avail > 0 && !source->ReadAt(at, out, avail))
        return false;
    if (avail < len)
        memset(out + avail, 0, len - avail);
    return true;
}

GodResult GodConvert(GodSource *source, const GodImageInfo &info, const char *contentRoot,
                     const char *titleName, const unsigned char *iconPng, unsigned long iconPngSize,
                     GodProgressFn progress, void *progressContext,
                     char *outPackagePath, unsigned long outPackagePathSize,
                     GodTimings *outTimings)
{
    GodTimings timings;
    memset(&timings, 0, sizeof(timings));

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    const double msPerTick = 1000.0 / (double)freq.QuadPart;

    PackagePaths paths;
    if (!BuildPaths(contentRoot, info.title, &paths))
        return GOD_WRITE_FAILED;

    // A previous install of this disc is replaced, not merged: a leftover
    // Data file past the new last one would be harmless, a stale header not.
    RemovePackage(paths);

    if (!EnsureDirectory(paths.titleDir) || !EnsureDirectory(paths.typeDir) || !EnsureDirectory(paths.dataDir))
    {
        RemovePackage(paths);
        return GOD_WRITE_FAILED;
    }

    // Every master hash block is kept until the end: each one's last entry is
    // the hash of the next, so none is final until all the data is written.
    // Under 200KB for a full dual-layer disc.
    unsigned char *masters = (unsigned char *)calloc(info.partCount, BLOCK_SIZE);

    // One hash block followed by the data blocks it covers, laid out as they
    // go on disk, so each group is a single write.
    unsigned char *group = (unsigned char *)malloc((1 + BLOCKS_PER_GROUP) * BLOCK_SIZE);
    unsigned char *header = (unsigned char *)malloc(HEADER_SIZE);

    if (masters == NULL || group == NULL || header == NULL)
    {
        free(masters); free(group); free(header);
        RemovePackage(paths);
        return GOD_OUT_OF_MEMORY;
    }

    GodResult result = GOD_OK;
    unsigned long blocksDone = 0;
    unsigned long long partsTotalSize = 0;
    char partPath[600];

    for (unsigned long part = 0; part < info.partCount && result == GOD_OK; ++part)
    {
        PartPath(paths, part, partPath, sizeof(partPath));

        FILE *f = fopen(partPath, "wb");
        if (f == NULL)
        {
            result = GOD_WRITE_FAILED;
            break;
        }

        unsigned char *master = masters + part * BLOCK_SIZE;
        unsigned long blocksThisPart = (part + 1 < info.partCount) ? BLOCKS_PER_PART : LastPartBlocks(info);

        // Placeholder for the master hash block, filled in once the chain is known.
        memset(group, 0, BLOCK_SIZE);
        if (fwrite(group, 1, BLOCK_SIZE, f) != BLOCK_SIZE)
            result = GOD_WRITE_FAILED;

        for (unsigned long g = 0; result == GOD_OK && g * BLOCKS_PER_GROUP < blocksThisPart; ++g)
        {
            unsigned long n = blocksThisPart - g * BLOCKS_PER_GROUP;
            if (n > BLOCKS_PER_GROUP)
                n = BLOCKS_PER_GROUP;

            unsigned char *hashBlock = group;
            unsigned char *data = group + BLOCK_SIZE;

            QueryPerformanceCounter(&t0);
            bool readOk = ReadPartition(source, info, (unsigned long long)blocksDone * BLOCK_SIZE, data, n * BLOCK_SIZE);
            QueryPerformanceCounter(&t1);
            timings.readMs += (double)(t1.QuadPart - t0.QuadPart) * msPerTick;
            if (!readOk)
            {
                result = GOD_READ_FAILED;
                break;
            }

            memset(hashBlock, 0, BLOCK_SIZE);
            for (unsigned long b = 0; b < n; ++b)
                Sha1(data + b * BLOCK_SIZE, BLOCK_SIZE, hashBlock + b * 20);

            Sha1(hashBlock, BLOCK_SIZE, master + g * 20);
            QueryPerformanceCounter(&t0);
            timings.hashMs += (double)(t0.QuadPart - t1.QuadPart) * msPerTick;

            size_t bytes = (size_t)(1 + n) * BLOCK_SIZE;
            bool writeOk = (fwrite(group, 1, bytes, f) == bytes);
            QueryPerformanceCounter(&t1);
            timings.writeMs += (double)(t1.QuadPart - t0.QuadPart) * msPerTick;
            if (!writeOk)
            {
                result = GOD_WRITE_FAILED;
                break;
            }

            blocksDone += n;

            if (progress != NULL)
            {
                unsigned long long done = (unsigned long long)blocksDone * BLOCK_SIZE;
                if (done > info.usedSize)
                    done = info.usedSize;
                bool keepGoing = progress(done, info.usedSize, progressContext);
                QueryPerformanceCounter(&t0);
                timings.progressMs += (double)(t0.QuadPart - t1.QuadPart) * msPerTick;
                if (!keepGoing)
                    result = GOD_CANCELLED;
            }
        }

        if (fclose(f) != 0 && result == GOD_OK)
            result = GOD_WRITE_FAILED;

        partsTotalSize += PartFileSize(blocksThisPart);
    }

    // Chain the master blocks, last to first: each gets the hash of the one
    // after it appended. A full part's master holds 0xCB entries, so the
    // chain entry is the 0xCC'th - the block has room for exactly that.
    if (result == GOD_OK)
    {
        for (unsigned long part = info.partCount - 1; part > 0; --part)
        {
            unsigned long groupsInPrev = GROUPS_PER_PART; // every part but the last is full
            Sha1(masters + part * BLOCK_SIZE, BLOCK_SIZE,
                 masters + (part - 1) * BLOCK_SIZE + groupsInPrev * 20);
        }

        for (unsigned long part = 0; part < info.partCount && result == GOD_OK; ++part)
        {
            PartPath(paths, part, partPath, sizeof(partPath));
            FILE *f = fopen(partPath, "r+b");
            if (f == NULL)
            {
                result = GOD_WRITE_FAILED;
                break;
            }
            if (fwrite(masters + part * BLOCK_SIZE, 1, BLOCK_SIZE, f) != BLOCK_SIZE)
                result = GOD_WRITE_FAILED;
            if (fclose(f) != 0 && result == GOD_OK)
                result = GOD_WRITE_FAILED;
        }
    }

    // The header last: until it exists, nothing shows the package.
    if (result == GOD_OK)
    {
        unsigned char mhtHash[20];
        Sha1(masters, BLOCK_SIZE, mhtHash);
        BuildHeader(header, info, titleName, iconPng, iconPngSize, mhtHash, partsTotalSize);

        FILE *f = fopen(paths.header, "wb");
        if (f == NULL)
            result = GOD_WRITE_FAILED;
        else
        {
            if (fwrite(header, 1, HEADER_SIZE, f) != HEADER_SIZE)
                result = GOD_WRITE_FAILED;
            if (fclose(f) != 0 && result == GOD_OK)
                result = GOD_WRITE_FAILED;
        }
    }

    free(masters);
    free(group);
    free(header);

    if (outTimings != NULL)
        *outTimings = timings;

    if (result != GOD_OK)
    {
        RemovePackage(paths);
        return result;
    }

    if (outPackagePath != NULL && outPackagePathSize > 0)
    {
        _snprintf(outPackagePath, outPackagePathSize, "%s", paths.header);
        outPackagePath[outPackagePathSize - 1] = '\0';
    }
    return GOD_OK;
}
