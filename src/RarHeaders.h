#ifndef RAR_HEADERS_H
#define RAR_HEADERS_H

// Parses RAR archive headers one at a time, for the DLC pack walk in
// ArchiveOrgDLC.cpp - enough to list what an archive holds and where each
// entry's data sits, without decompressing anything. archive.org extracts the
// members server-side; this only has to know their names and sizes.
//
// Both on-disk formats are handled:
//
//   RAR 4.x  - signature "Rar!" 1A 07 00. Fixed-layout little-endian headers.
//   RAR 5.x  - signature "Rar!" 1A 07 01 00. A different design altogether:
//              variable-length integers, CRC32 on every header, UTF-8 names
//              with '/' separators.
//
// The walk used to know only RAR4, so a RAR5 pack failed at its very first
// header with "does not look like a RAR4 archive" - the failure Halo 3's pack
// produced. Whether that pack is in fact RAR5 is logged the first time the
// walk reaches it (see ListDlcMembers), since it's a private file that can't
// be inspected without the keys.
//
// Free of any Xbox header, so it's tested on a PC against real archives made
// by WinRAR's own rar.exe in both formats.

enum RarFormat
{
    RAR_FORMAT_UNKNOWN,
    RAR_FORMAT_4,
    RAR_FORMAT_5
};

// Identifies the format from the archive's first bytes and sets
// *outSignatureLen to how many of them the signature takes - the first header
// starts right after it.
RarFormat RarDetectFormat(const unsigned char *data, unsigned long len, unsigned long *outSignatureLen);

#define RAR_NAME_MAX 512

struct RarEntry
{
    bool isFile;                  // a real file with content, not a directory or a bookkeeping header
    char name[RAR_NAME_MAX];      // path inside the archive, always with '\' separators (RAR5 stores '/')
    unsigned long long packSize;  // bytes of archived data following the header
    unsigned long long unpSize;   // size once extracted

    // For reading a file's data straight out of the archive - a game's disc
    // image kept without compression ("store", -m0):
    unsigned long long dataOffset; // from the header's start to its data
    bool stored;                   // not compressed: the data is the file, byte for byte
    bool encrypted;                // the file's data is
    bool split;                    // it continues from, or into, another volume
    bool hasCrc;                   // crc is the file's CRC32 (RAR5 may leave it out)
    unsigned long crc;
};

// Return values for RarParseHeader.
#define RAR_HEADER_OK         0  // parsed; *outNext says where the next header is
#define RAR_HEADER_END        1  // the end-of-archive marker - nothing follows
#define RAR_HEADER_ERROR     -1  // not a valid header of this format
#define RAR_HEADER_NEED_MORE -2  // the header runs past the end of the data given
#define RAR_HEADER_ENCRYPTED -3  // the archive's headers are encrypted, so its contents can't be listed

// Parses the one header that starts at data[0]. On RAR_HEADER_OK, *outNext is
// the distance from data[0] to the next header - past this header AND the
// data area that follows it - and *out describes the entry.
int RarParseHeader(RarFormat format, const unsigned char *data, unsigned long len,
                   RarEntry *out, unsigned long long *outNext);

#endif
