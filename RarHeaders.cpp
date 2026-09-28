/*
FILE : RarHeaders.cpp
PROJECT : Omni360
DESCRIPTION : RAR4 and RAR5 header parsing for the DLC pack walk. See
              RarHeaders.h.

Layouts are from RARLAB's own technical notes: "RAR 5.0 archive format" for
RAR5, and the RAR 4.x technote shipped with unrar for RAR4.
*/

#include "RarHeaders.h"

#include <string.h>

static unsigned long ReadLE16(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8);
}

static unsigned long ReadLE32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

// Copies a stored name, stopping at an embedded NUL (RAR4 puts one between an
// ASCII name and its encoded Unicode form) and turning '/' into '\', so both
// formats hand the rest of the app paths shaped the same way.
static void CopyName(char *out, const unsigned char *name, unsigned long nameLen)
{
    unsigned long n = 0;
    for (; n < nameLen && n < RAR_NAME_MAX - 1 && name[n] != 0; ++n)
        out[n] = (name[n] == '/') ? '\\' : (char)name[n];
    out[n] = '\0';
}

RarFormat RarDetectFormat(const unsigned char *data, unsigned long len, unsigned long *outSignatureLen)
{
    static const unsigned char SIG4[7] = { 0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00 };
    static const unsigned char SIG5[8] = { 0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00 };

    if (len >= 8 && memcmp(data, SIG5, 8) == 0)
    {
        *outSignatureLen = 8;
        return RAR_FORMAT_5;
    }
    if (len >= 7 && memcmp(data, SIG4, 7) == 0)
    {
        *outSignatureLen = 7;
        return RAR_FORMAT_4;
    }

    *outSignatureLen = 0;
    return RAR_FORMAT_UNKNOWN;
}

// ---------------------------------------------------------------------------
// RAR 4.x
// ---------------------------------------------------------------------------

#define RAR4_MAIN_HEAD   0x73
#define RAR4_FILE_HEAD   0x74
#define RAR4_NEWSUB_HEAD 0x7A  // comments, recovery records, other "service" data
#define RAR4_END_HEAD    0x7B

#define RAR4_LONG_BLOCK   0x8000 // a 32-bit ADD_SIZE of data follows the header
#define RAR4_LHD_LARGE    0x0100 // file header: 64-bit sizes, high halves before the name
#define RAR4_LHD_DIRECTORY 0x00E0
#define RAR4_MHD_PASSWORD 0x0080 // main header: the headers themselves are encrypted

static int ParseRar4(const unsigned char *data, unsigned long len, RarEntry *out, unsigned long long *outNext)
{
    // Every header starts HEAD_CRC(2) HEAD_TYPE(1) HEAD_FLAGS(2) HEAD_SIZE(2).
    if (len < 7)
        return RAR_HEADER_NEED_MORE;

    unsigned long type = data[2];
    unsigned long flags = ReadLE16(data + 3);
    unsigned long headSize = ReadLE16(data + 5);

    if (headSize < 7)
        return (headSize == 0) ? RAR_HEADER_END : RAR_HEADER_ERROR;

    if (type == RAR4_END_HEAD)
        return RAR_HEADER_END;

    if (type == RAR4_MAIN_HEAD && (flags & RAR4_MHD_PASSWORD))
        return RAR_HEADER_ENCRYPTED;

    // The data area after the header. For a file header, ADD_SIZE is its
    // PACK_SIZE field; any other header can carry one too when LONG_BLOCK is
    // set - comments and recovery records do - and skipping only HEAD_SIZE
    // for those lands the walk in the middle of their data.
    unsigned long long dataSize = 0;
    if (flags & RAR4_LONG_BLOCK)
    {
        if (len < 11)
            return RAR_HEADER_NEED_MORE;
        dataSize = ReadLE32(data + 7);
    }

    if (type == RAR4_FILE_HEAD)
    {
        // After the common 7 bytes:
        //   PACK_SIZE(4) UNP_SIZE(4) HOST_OS(1) FILE_CRC(4) FTIME(4)
        //   UNP_VER(1) METHOD(1) NAME_SIZE(2) ATTR(4)
        //   [HIGH_PACK_SIZE(4) HIGH_UNP_SIZE(4)]  - only with LHD_LARGE
        //   FILE_NAME
        const unsigned long fixed = 7 + 25;
        const unsigned long large = (flags & RAR4_LHD_LARGE) ? 8 : 0;

        if (len < fixed + large)
            return RAR_HEADER_NEED_MORE;

        unsigned long long packSize = ReadLE32(data + 7);
        unsigned long long unpSize = ReadLE32(data + 11);
        unsigned long nameSize = ReadLE16(data + 7 + 19);

        if (large)
        {
            packSize |= (unsigned long long)ReadLE32(data + fixed) << 32;
            unpSize |= (unsigned long long)ReadLE32(data + fixed + 4) << 32;
        }

        unsigned long nameStart = fixed + large;
        if (nameStart + nameSize > headSize)
            return RAR_HEADER_ERROR; // the name can't extend past its own header
        if (nameStart + nameSize > len)
            return RAR_HEADER_NEED_MORE;

        dataSize = packSize; // replaces the 32-bit ADD_SIZE read above

        // Zero-size entries are how RAR records the directories in a path;
        // the directory attribute says so outright as well.
        out->isFile = ((flags & RAR4_LHD_DIRECTORY) != RAR4_LHD_DIRECTORY) && unpSize > 0;
        CopyName(out->name, data + nameStart, nameSize);
        out->packSize = packSize;
        out->unpSize = unpSize;
    }

    *outNext = (unsigned long long)headSize + dataSize;
    return RAR_HEADER_OK;
}

// ---------------------------------------------------------------------------
// RAR 5.x
// ---------------------------------------------------------------------------

#define RAR5_HEAD_MAIN    1
#define RAR5_HEAD_FILE    2
#define RAR5_HEAD_SERVICE 3 // comments, recovery record, quick-open index...
#define RAR5_HEAD_CRYPT   4 // archive-wide header encryption
#define RAR5_HEAD_END     5

#define RAR5_HFL_EXTRA    0x0001 // header has an extra area
#define RAR5_HFL_DATA     0x0002 // header is followed by a data area

#define RAR5_FHFL_DIRECTORY 0x0001
#define RAR5_FHFL_UTIME     0x0002 // a 32-bit mtime is present
#define RAR5_FHFL_CRC32     0x0004 // a 32-bit data CRC is present

// RAR5's variable-length integer: 7 bits per byte, least significant first,
// high bit set on every byte but the last. Up to 10 bytes for 64 bits.
static bool ReadVint(const unsigned char *data, unsigned long len, unsigned long *pos,
                     unsigned long long *out)
{
    unsigned long long v = 0;
    for (int shift = 0; shift < 70; shift += 7)
    {
        if (*pos >= len)
            return false;
        unsigned char b = data[(*pos)++];
        if (shift == 63 && (b & 0x7E) != 0)
            return false; // would not fit in 64 bits
        v |= (unsigned long long)(b & 0x7F) << shift;
        if ((b & 0x80) == 0)
        {
            *out = v;
            return true;
        }
    }
    return false;
}

static unsigned long Crc32(const unsigned char *p, unsigned long n)
{
    static unsigned long table[256];
    static bool built = false;

    if (!built)
    {
        for (unsigned long i = 0; i < 256; ++i)
        {
            unsigned long c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }

    unsigned long crc = 0xFFFFFFFFUL;
    while (n-- > 0)
        crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFUL;
}

static int ParseRar5(const unsigned char *data, unsigned long len, RarEntry *out, unsigned long long *outNext)
{
    // HEADER_CRC32(4) HEADER_SIZE(vint), then HEADER_SIZE bytes of header
    // starting at HEADER_TYPE. The CRC covers HEADER_SIZE's own bytes onward.
    if (len < 5)
        return RAR_HEADER_NEED_MORE;

    unsigned long pos = 4;
    unsigned long long headSize;
    if (!ReadVint(data, len, &pos, &headSize))
        return (len < 4 + 3) ? RAR_HEADER_NEED_MORE : RAR_HEADER_ERROR;

    // The format caps a header at 2MB; anything bigger is not a header.
    if (headSize == 0 || headSize > 2 * 1024 * 1024)
        return RAR_HEADER_ERROR;

    const unsigned long long headerEnd = (unsigned long long)pos + headSize;
    if (headerEnd > len)
        return RAR_HEADER_NEED_MORE;

    const unsigned long end = (unsigned long)headerEnd;

    // Checked on every header, which is what makes a RAR5 walk trustworthy:
    // a wrong offset lands on bytes whose CRC can't match, and is caught at
    // once rather than read as a nonsense header.
    if (Crc32(data + 4, end - 4) != ReadLE32(data))
        return RAR_HEADER_ERROR;

    unsigned long long type, flags, extraSize = 0, dataSize = 0;
    if (!ReadVint(data, end, &pos, &type) || !ReadVint(data, end, &pos, &flags))
        return RAR_HEADER_ERROR;
    if ((flags & RAR5_HFL_EXTRA) && !ReadVint(data, end, &pos, &extraSize))
        return RAR_HEADER_ERROR;
    if ((flags & RAR5_HFL_DATA) && !ReadVint(data, end, &pos, &dataSize))
        return RAR_HEADER_ERROR;

    if (type == RAR5_HEAD_END)
        return RAR_HEADER_END;
    if (type == RAR5_HEAD_CRYPT)
        return RAR_HEADER_ENCRYPTED;

    if (type == RAR5_HEAD_FILE)
    {
        // FILE_FLAGS UNPACKED_SIZE ATTRIBUTES [MTIME(4)] [DATA_CRC32(4)]
        // COMPRESSION_INFO HOST_OS NAME_LENGTH NAME   (then the extra area)
        unsigned long long fileFlags, unpSize, attributes, compression, hostOs, nameLen;

        if (!ReadVint(data, end, &pos, &fileFlags) ||
            !ReadVint(data, end, &pos, &unpSize) ||
            !ReadVint(data, end, &pos, &attributes))
            return RAR_HEADER_ERROR;

        if (fileFlags & RAR5_FHFL_UTIME)
            pos += 4;
        if (fileFlags & RAR5_FHFL_CRC32)
            pos += 4;

        if (pos > end ||
            !ReadVint(data, end, &pos, &compression) ||
            !ReadVint(data, end, &pos, &hostOs) ||
            !ReadVint(data, end, &pos, &nameLen))
            return RAR_HEADER_ERROR;

        if (nameLen > end - pos)
            return RAR_HEADER_ERROR;

        out->isFile = !(fileFlags & RAR5_FHFL_DIRECTORY) && unpSize > 0;
        CopyName(out->name, data + pos, (unsigned long)nameLen);
        out->packSize = dataSize;
        out->unpSize = unpSize;
    }

    // Main, service and anything unrecognised are simply stepped over - the
    // format's own sizes say how far, so there's no need to understand them.
    *outNext = headerEnd + dataSize;
    return RAR_HEADER_OK;
}

int RarParseHeader(RarFormat format, const unsigned char *data, unsigned long len,
                   RarEntry *out, unsigned long long *outNext)
{
    out->isFile = false;
    out->name[0] = '\0';
    out->packSize = 0;
    out->unpSize = 0;
    *outNext = 0;

    if (format == RAR_FORMAT_4)
        return ParseRar4(data, len, out, outNext);
    if (format == RAR_FORMAT_5)
        return ParseRar5(data, len, out, outNext);
    return RAR_HEADER_ERROR;
}
