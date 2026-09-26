#ifndef STFS_PARSER_H
#define STFS_PARSER_H

// Reads the metadata (Title ID, display name, embedded icon) directly out of
// an installed Xbox 360 content package (STFS format: "CON "/"LIVE"/"PIRS"),
// and enumerates installed titles from the console's own Content folder. This
// is entirely local/offline - no network, no dependency on Aurora - since the
// console already has this information on disk for every installed title.
//
// Header field offsets are from the Free60 wiki's STFS page. Metadata
// Version 1 and 2 both work - the fields this file actually reads (Content
// Type, Title ID, Display Name) sit before where the two layouts diverge, so
// only the max embedded-thumbnail size differs between them (handled
// internally). Confirmed against real Games-on-Demand packages this session,
// which - despite the "Version 2 = TV/series content" framing in some docs -
// use v2 metadata too. Display Name's on-disk encoding is ambiguous between
// sources (the wiki says UTF-8, established tooling historically treats it
// as UTF-16BE) - StfsReadTitleInfo detects which one it actually is per-file
// rather than assuming.

struct StfsTitleInfo
{
    unsigned long titleId;      // 8-hex-digit title ID, e.g. 0x415607FF - matches the Content\...\{TitleID} folder name
    unsigned long contentType;  // e.g. 0x00000002 = Marketplace Content (DLC), 0x00007000 = Game on Demand, etc.
    char displayName[256];      // UTF-8, decoded from whichever encoding the package actually used

    unsigned char *titleThumbnail;   // malloc'd PNG bytes (the box art icon) - NULL if the package has none. Caller must free().
    unsigned long titleThumbnailSize;

    // The package's OTHER embedded image. STFS carries two: the Title
    // Thumbnail above (artwork for the game as a whole) and this one, the
    // Thumbnail Image, which is artwork for this specific piece of content.
    // For an installed game package the two are usually similar or identical,
    // so this exists mainly as a second chance: GOD conversion tools very
    // often leave the Title Thumbnail zeroed or garbage while this one
    // survives, and it costs nothing to read - it already sits inside the
    // same header block StfsReadTitleInfo reads.
    unsigned char *contentThumbnail;
    unsigned long contentThumbnailSize;

    // Why a thumbnail was rejected, for the handful of titles that come out
    // without art. Populated on every parse whether or not it was needed;
    // EnumerateInstalledGames logs them only when both images came back NULL,
    // which is the only case anyone wants to read about.
    //
    // These exist because there are three separate reasons an image can be
    // refused below - declared size, bounds against how much of the header
    // actually got read, and the leading magic - and from the outside all
    // three look identical: a blank square. Guessing which one is in play
    // costs a full hardware build cycle per guess.
    // The first bytes of the raw Display Name field, before any decoding.
    // Logged only for names that decode to something non-ASCII, which is the
    // only case where the encoding is in question.
    //
    // This exists because a mojibake symptom cannot be diagnosed from the
    // decoded string alone - "WarfareA(R)" looks the same whether the package
    // stored one character and we mangled it, or the package stored the
    // mangling and we faithfully reproduced it. The raw bytes distinguish
    // those, and nothing else does.
    unsigned char diagRawName[24];
    int           diagRawNameLen;
    int           diagRawNameOffset; // where in the field the window starts, so the log is locatable
    bool          diagNameNonAscii;

    long          diagBytesRead;        // how much of the header this file actually yielded
    unsigned long diagTitleThumbSize;   // declared Title Thumbnail size, before clamping
    unsigned long diagContentThumbSize; // declared Thumbnail size, before clamping
    unsigned long diagTitleMagic;       // first 4 bytes at 0x571A, big-endian
    unsigned long diagContentMagic;     // first 4 bytes at 0x171A, big-endian
};

// Reads just the metadata (not the actual file contents) from one content
// package at packagePath. Returns false if packagePath isn't a recognized
// STFS package or uses the (unsupported here) Version 2 metadata layout.
bool StfsReadTitleInfo(const char *packagePath, StfsTitleInfo *outInfo);

// Frees outInfo->titleThumbnail if set.
void StfsFreeTitleInfo(StfsTitleInfo *info);

struct InstalledGame
{
    unsigned long titleId;
    unsigned long contentType;
    char displayName[256];
    char packagePath[512]; // the representative package this info was read from - StfsReadTitleInfo(packagePath, ...) again to get the icon on demand
};

// Content types worth listing as "games" in the picker - deliberately
// excludes Profile/Theme/GamerPicture/SavedGame/Cache/etc, which also live
// under the Content folder but aren't titles someone would pick DLC for.
bool IsGameContentType(unsigned long contentType);

// Walks contentBasePath (e.g. "Hdd1:\Content\0000000000000000") for
// TitleID\ContentType\* entries, opens one representative package per
// title, and fills outGames (caller-allocated, up to maxGames). Known gap:
// Games-on-Demand-style split packages (a folder of numbered parts instead
// of one file) aren't handled yet - such titles are silently skipped rather
// than mis-parsed; worth revisiting once tested against a real console
// library, since how common that layout is on a JTAG/RGH+Aurora setup wasn't
// verified this session.
int EnumerateInstalledGames(const char *contentBasePath, InstalledGame *outGames, int maxGames,
                            void printFunction(const char *_format, ...));

#endif
