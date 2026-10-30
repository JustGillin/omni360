#ifndef SEVEN_ZIP_H
#define SEVEN_ZIP_H

// ---------------------------------------------------------------------------
// Unpacking .7z archives - the ConsoleMods homebrew repo's downloads
// ---------------------------------------------------------------------------
//
// With 7-Zip's own decoder (lzma\, see OMNI360.txt there). A solid block of
// up to SEVEN_ZIP_MAX_BLOCK is decoded whole, into memory; a bigger one - 266MB
// in RetroArch 360 V3's archive - a megabyte at a time into its files, as
// long as it's plain LZMA or LZMA2, as 7-Zip packs nearly everything. A big
// block with a filter in front (BCJ, Delta) is refused before anything is
// written - a 360 has 512MB for everything.
//
// The PC test (tools\test_7z) builds with a small limit, to put the streaming
// through its paces on small archives.

#ifndef SEVEN_ZIP_MAX_BLOCK
#define SEVEN_ZIP_MAX_BLOCK (192ULL * 1024 * 1024)
#endif

enum SevenZipResult
{
    SEVEN_ZIP_OK,
    SEVEN_ZIP_NOT_7Z,       // not a .7z, or a damaged one
    SEVEN_ZIP_UNSUPPORTED,  // a method or filter the decoder doesn't have
    SEVEN_ZIP_TOO_BIG,      // a solid block larger than SEVEN_ZIP_MAX_BLOCK that can't be streamed
    SEVEN_ZIP_NO_MEMORY,
    SEVEN_ZIP_CORRUPT,      // a checksum didn't match
    SEVEN_ZIP_WRITE_FAILED, // a file or folder couldn't be made - a full drive, or a name FATX won't take
    SEVEN_ZIP_CANCELLED
};

const char *SevenZipResultText(SevenZipResult result);

// Called after each file with how far through the archive's unpacked bytes
// it is. Return false to stop.
typedef bool (*SevenZipProgressFn)(unsigned long long bytesDone, unsigned long long bytesTotal);

// Unpacks every file in archivePath under destDir, which is made if it
// isn't there, keeping the archive's folders. When everything in the archive
// is in one folder named stripFolder (case aside), that folder is left out,
// so its contents land straight in destDir. stripFolder may be NULL. Names
// with ".." or a drive in them are refused rather than written outside
// destDir.
//
// A file whose name FATX can't hold - over 42 characters, or with a
// character like + or , in it - is left out rather than failing the lot;
// archives made on a PC have them, usually sample files. *skippedNames
// (may be NULL) is how many.
SevenZipResult ExtractSevenZip(const char *archivePath, const char *destDir, const char *stripFolder,
                               SevenZipProgressFn progress, int *skippedNames = 0);

#endif
