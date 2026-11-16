#ifndef ZIP_EXTRACT_H
#define ZIP_EXTRACT_H

// ---------------------------------------------------------------------------
// Unpacking .zip archives - the ConsoleMods wiki's xefu packs
// ---------------------------------------------------------------------------
//
// Stored and deflated members only, which is what every zip made on a PC
// holds, and no zip64: the archives this is for are tens of megabytes. Each
// member is inflated 64KB at a time straight into its file, so a 19MB font
// never sits in memory whole, and checked against its CRC-32.

enum ZipResult
{
    ZIP_OK,
    ZIP_NOT_ZIP,        // no end-of-central-directory record - not a zip, or cut short
    ZIP_UNSUPPORTED,    // a method other than stored or deflate, encryption, or zip64
    ZIP_NO_MEMORY,
    ZIP_CORRUPT,        // a header out of place, bad deflate data, or a CRC that didn't match
    ZIP_WRITE_FAILED,   // a file or folder couldn't be made - a full drive
    ZIP_CANCELLED
};

const char *ZipResultText(ZipResult result);

// Called after each file with how far through the archive's unpacked bytes
// it is. Return false to stop.
typedef bool (*ZipProgressFn)(unsigned long long bytesDone, unsigned long long bytesTotal);

// Unpacks the archive under destDir, which is made if it isn't there,
// keeping its folders. With a stripFolder, only what's inside a folder of
// that name (case aside) is unpacked, and every folder down to and including
// it is left out of the path - so "Hacked_Xefu_Pack/Compatibility/xefu.xex"
// with stripFolder "Compatibility" lands as destDir\xefu.xex, whatever the
// pack's top folder happens to be called. stripFolder may be NULL.
//
// Names with ".." or a drive in them are refused rather than written
// outside destDir. A name FATX can't hold - over 42 characters - is left
// out rather than failing the lot; *skippedNames (may be NULL) is how many.
ZipResult ExtractZip(const char *archivePath, const char *destDir, const char *stripFolder, ZipProgressFn progress,
                     int *skippedNames = 0);

#endif
