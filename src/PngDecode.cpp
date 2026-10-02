/*
FILE : PngDecode.cpp
PROJECT : Omni360
DESCRIPTION : inflate (RFC 1950 / RFC 1951) and PNG decoding, for the cover
              thumbnails the XDK's own decoders refuse. See PngDecode.h for
              why this exists and what it covers.

The inflater is the classic canonical-Huffman design: a code is described only
by how many codes there are of each bit length, and decoded one bit at a time
by walking those counts. That is slower than a lookup table, and irrelevant
here - a 64x64 thumbnail inflates to about 16KB - while being short enough to
check line by line against the RFC, which is the property that matters in
code that parses untrusted input.
*/

#include "PngDecode.h"

#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Inflate
// ---------------------------------------------------------------------------

#define INF_MAX_BITS       15   // longest code deflate allows
#define INF_MAX_LITLEN    286   // literal/length codes a dynamic block may define
#define INF_MAX_DIST       30   // distance codes
#define INF_FIXED_LITLEN  288   // the fixed table also assigns 286 and 287, which are invalid to use

struct InflateState
{
    const unsigned char *in;
    unsigned long inSize;
    unsigned long inPos;

    unsigned long bitBuf;   // bits read from the input but not yet consumed, LSB first
    int bitCount;

    unsigned char *out;
    unsigned long outSize;
    unsigned long outPos;

    // Set when a read ran past the end of the input. InfBits then supplies
    // zeros rather than failing, so it doesn't need an error path of its own;
    // every loop checks this flag instead, and every WRITE is bounds-checked
    // against outSize independently, so zeros standing in for missing input
    // can produce a wrong image but never an out-of-bounds one.
    bool overrun;
};

// Takes the next `need` bits (0..16), least significant first, which is the
// order deflate packs everything except Huffman codes.
static unsigned long InfBits(InflateState *s, int need)
{
    unsigned long val = s->bitBuf;

    while (s->bitCount < need)
    {
        unsigned long byte = 0;
        if (s->inPos < s->inSize)
            byte = s->in[s->inPos++];
        else
            s->overrun = true;

        val |= byte << s->bitCount;
        s->bitCount += 8;
    }

    s->bitBuf = val >> need;
    s->bitCount -= need;
    return val & ((1UL << need) - 1);
}

// A canonical Huffman code. Given only the bit length of each symbol's code,
// deflate's rules fix the codes themselves, so this is all that's needed to
// decode: how many codes have each length, and the symbols in code order.
struct Huffman
{
    short count[INF_MAX_BITS + 1];
    short symbol[INF_FIXED_LITLEN];
};

// Builds a code from per-symbol lengths (each 0..15, 0 meaning unused).
// Returns 0 if the lengths describe a complete code, a positive number if
// they leave codes unassigned (incomplete), or a negative one if they assign
// more codes than the lengths allow (over-subscribed - never valid).
static int InfBuild(Huffman *h, const short *lengths, int n)
{
    int len;
    for (len = 0; len <= INF_MAX_BITS; ++len)
        h->count[len] = 0;

    for (int sym = 0; sym < n; ++sym)
        h->count[lengths[sym]]++;

    if (h->count[0] == n)
        return 0; // no codes at all; fine unless something tries to decode with it

    // Each extra bit of length doubles the codes available; subtract the ones
    // actually used at each length and see what's left over.
    int left = 1;
    for (len = 1; len <= INF_MAX_BITS; ++len)
    {
        left <<= 1;
        left -= h->count[len];
        if (left < 0)
            return left;
    }

    // Where each length's symbols start in symbol[], then fill it in symbol
    // order - which, within one length, is also code order.
    short offs[INF_MAX_BITS + 1];
    offs[1] = 0;
    for (len = 1; len < INF_MAX_BITS; ++len)
        offs[len + 1] = (short)(offs[len] + h->count[len]);

    for (int sym = 0; sym < n; ++sym)
    {
        if (lengths[sym] != 0)
            h->symbol[offs[lengths[sym]]++] = (short)sym;
    }

    return left;
}

// Decodes one symbol. Huffman codes are the one thing deflate packs most
// significant bit first, hence reading a bit at a time and shifting left.
// At each length, the codes of that length are the `count` consecutive values
// starting at `first`; if the bits so far fall in that range, that's the code.
static int InfDecode(InflateState *s, const Huffman *h)
{
    int code = 0;   // bits read so far
    int first = 0;  // first code of the current length
    int index = 0;  // where the current length's symbols start in symbol[]

    for (int len = 1; len <= INF_MAX_BITS; ++len)
    {
        code |= (int)InfBits(s, 1);

        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];

        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }

    return -1; // not a code - only possible with an incomplete table
}

// Base values and extra-bit counts for length codes 257..285 and distance
// codes 0..29, straight from RFC 1951 section 3.2.5.
static const short kLenBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short kLenExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short kDistBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577 };
static const short kDistExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

// The body of a compressed block: literals, and (length, distance) pairs
// meaning "copy length bytes from distance bytes back", until end-of-block.
static bool InfCodes(InflateState *s, const Huffman *lencode, const Huffman *distcode)
{
    for (;;)
    {
        if (s->overrun)
            return false;

        int sym = InfDecode(s, lencode);
        if (sym < 0)
            return false;

        if (sym < 256)
        {
            if (s->outPos >= s->outSize)
                return false;
            s->out[s->outPos++] = (unsigned char)sym;
        }
        else if (sym == 256)
        {
            return true; // end of block
        }
        else
        {
            sym -= 257;
            if (sym >= 29)
                return false; // 286 and 287 are reserved

            unsigned long len = (unsigned long)kLenBase[sym] + InfBits(s, kLenExtra[sym]);

            int dsym = InfDecode(s, distcode);
            if (dsym < 0 || dsym >= 30)
                return false; // 30 and 31 are reserved

            unsigned long dist = (unsigned long)kDistBase[dsym] + InfBits(s, kDistExtra[dsym]);

            if (dist > s->outPos)
                return false; // reaches back before the start - there's no preset dictionary
            if (len > s->outSize - s->outPos)
                return false;

            // Byte by byte, not memcpy/memmove: when dist < len the source
            // overlaps what's being written, and that's deliberate - it's how
            // deflate encodes a run, by copying bytes this same copy produced.
            while (len-- > 0)
            {
                s->out[s->outPos] = s->out[s->outPos - dist];
                s->outPos++;
            }
        }
    }
}

// Block type 0: uncompressed bytes, prefixed by a length and its complement.
static bool InfStored(InflateState *s)
{
    // Stored data starts on a byte boundary. InfBits never holds a whole
    // unread byte (it only fetches when short), so what's left is the tail of
    // the current byte, which is padding.
    s->bitBuf = 0;
    s->bitCount = 0;

    if (s->inSize - s->inPos < 4)
        return false;

    unsigned long len  = (unsigned long)s->in[s->inPos]     | ((unsigned long)s->in[s->inPos + 1] << 8);
    unsigned long nlen = (unsigned long)s->in[s->inPos + 2] | ((unsigned long)s->in[s->inPos + 3] << 8);
    s->inPos += 4;

    if (len != (~nlen & 0xFFFFUL))
        return false;
    if (len > s->inSize - s->inPos || len > s->outSize - s->outPos)
        return false;

    memcpy(s->out + s->outPos, s->in + s->inPos, len);
    s->inPos += len;
    s->outPos += len;
    return true;
}

// Block type 1: codes fixed by the RFC rather than sent in the stream.
static bool InfFixed(InflateState *s)
{
    Huffman lencode, distcode;
    short lengths[INF_FIXED_LITLEN];
    int sym;

    for (sym = 0;   sym < 144; ++sym) lengths[sym] = 8;
    for (;          sym < 256; ++sym) lengths[sym] = 9;
    for (;          sym < 280; ++sym) lengths[sym] = 7;
    for (;          sym < INF_FIXED_LITLEN; ++sym) lengths[sym] = 8;
    InfBuild(&lencode, lengths, INF_FIXED_LITLEN);

    for (sym = 0; sym < INF_MAX_DIST; ++sym)
        lengths[sym] = 5;
    InfBuild(&distcode, lengths, INF_MAX_DIST);

    return InfCodes(s, &lencode, &distcode);
}

// Block type 2: the stream sends its own codes, themselves Huffman-coded by a
// third, smaller code for the code lengths.
static bool InfDynamic(InflateState *s)
{
    // The order code-length-code lengths arrive in (RFC 1951 3.2.7), chosen so
    // the likely-unused ones come last and can be omitted.
    static const short kOrder[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

    int nlen  = (int)InfBits(s, 5) + 257;
    int ndist = (int)InfBits(s, 5) + 1;
    int ncode = (int)InfBits(s, 4) + 4;

    if (nlen > INF_MAX_LITLEN || ndist > INF_MAX_DIST)
        return false;

    short lengths[INF_MAX_LITLEN + INF_MAX_DIST];
    Huffman lencode, distcode;
    int index;

    for (index = 0; index < ncode; ++index)
        lengths[kOrder[index]] = (short)InfBits(s, 3);
    for (; index < 19; ++index)
        lengths[kOrder[index]] = 0;

    // The code-length code must be complete - there's no legitimate reason
    // for it not to be, unlike the two codes below.
    if (InfBuild(&lencode, lengths, 19) != 0)
        return false;

    // Literal/length and distance code lengths, as one run-length-coded
    // sequence: 0..15 are lengths, 16 repeats the previous length, 17 and 18
    // are runs of zeros.
    index = 0;
    while (index < nlen + ndist)
    {
        if (s->overrun)
            return false;

        int sym = InfDecode(s, &lencode);
        if (sym < 0)
            return false;

        if (sym < 16)
        {
            lengths[index++] = (short)sym;
            continue;
        }

        short len = 0;
        int repeat;
        if (sym == 16)
        {
            if (index == 0)
                return false; // nothing to repeat
            len = lengths[index - 1];
            repeat = 3 + (int)InfBits(s, 2);
        }
        else if (sym == 17)
        {
            repeat = 3 + (int)InfBits(s, 3);
        }
        else
        {
            repeat = 11 + (int)InfBits(s, 7);
        }

        if (index + repeat > nlen + ndist)
            return false;

        while (repeat-- > 0)
            lengths[index++] = len;
    }

    if (lengths[256] == 0)
        return false; // no end-of-block code, so the block could never end

    // An incomplete code is allowed in exactly one case: a single code of
    // length 1 (e.g. a block whose only distance is 1). Anything else
    // incomplete is corruption. Same rule zlib applies.
    int err = InfBuild(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen != lencode.count[0] + lencode.count[1]))
        return false;

    err = InfBuild(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist != distcode.count[0] + distcode.count[1]))
        return false;

    return InfCodes(s, &lencode, &distcode);
}

static unsigned long Adler32(const unsigned char *p, unsigned long n)
{
    unsigned long a = 1, b = 0;

    while (n > 0)
    {
        // 5552 is the most bytes that can be summed before b can overflow
        // 32 bits, so the modulo only needs doing once per chunk.
        unsigned long chunk = n < 5552 ? n : 5552;
        n -= chunk;

        while (chunk-- > 0)
        {
            a += *p++;
            b += a;
        }

        a %= 65521;
        b %= 65521;
    }

    return (b << 16) | a;
}

// Inflates a zlib stream (a deflate stream with a 2-byte header and an
// Adler-32 trailer, which is what PNG's IDAT data is) into exactly outSize
// bytes. Exactly: PNG fixes the decompressed size from the header, so a
// stream that produces more or less than that is broken either way.
static bool ZlibInflate(const unsigned char *in, unsigned long inSize,
                        unsigned char *out, unsigned long outSize,
                        const char **outError)
{
    if (inSize < 2 + 4)
    {
        *outError = "image data too short to be a zlib stream";
        return false;
    }

    unsigned int cmf = in[0];
    unsigned int flg = in[1];

    if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7)
    {
        *outError = "image data is not deflate-compressed";
        return false;
    }
    if (((cmf << 8) | flg) % 31 != 0)
    {
        *outError = "zlib header check failed";
        return false;
    }
    if (flg & 0x20)
    {
        *outError = "zlib stream needs a preset dictionary";
        return false;
    }

    InflateState s;
    s.in = in + 2;
    s.inSize = inSize - 2;
    s.inPos = 0;
    s.bitBuf = 0;
    s.bitCount = 0;
    s.out = out;
    s.outSize = outSize;
    s.outPos = 0;
    s.overrun = false;

    unsigned long last;
    do
    {
        last = InfBits(&s, 1);
        unsigned long type = InfBits(&s, 2);

        bool ok;
        if (type == 0)      ok = InfStored(&s);
        else if (type == 1) ok = InfFixed(&s);
        else if (type == 2) ok = InfDynamic(&s);
        else                ok = false; // type 3 is reserved

        if (s.overrun)
        {
            *outError = "image data is truncated";
            return false;
        }
        if (!ok)
        {
            *outError = "image data is corrupt (deflate)";
            return false;
        }
    }
    while (!last);

    if (s.outPos != outSize)
    {
        *outError = "image data is shorter than the header says";
        return false;
    }

    // The checksum follows on the next byte boundary.
    if (s.inSize - s.inPos < 4)
    {
        *outError = "zlib checksum missing";
        return false;
    }

    const unsigned char *t = s.in + s.inPos;
    unsigned long stored = ((unsigned long)t[0] << 24) | ((unsigned long)t[1] << 16) |
                           ((unsigned long)t[2] << 8)  |  (unsigned long)t[3];

    if (stored != Adler32(out, outSize))
    {
        *outError = "image data checksum mismatch";
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// PNG
// ---------------------------------------------------------------------------

static unsigned long ReadBE32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8)  |  (unsigned long)p[3];
}

static bool ChunkIs(const unsigned char *type, const char *name)
{
    return type[0] == (unsigned char)name[0] && type[1] == (unsigned char)name[1] &&
           type[2] == (unsigned char)name[2] && type[3] == (unsigned char)name[3];
}

// Samples per pixel for each colour type, or 0 for an invalid one.
static int ChannelsFor(int colourType)
{
    switch (colourType)
    {
    case 0: return 1; // greyscale
    case 2: return 3; // RGB
    case 3: return 1; // palette index
    case 4: return 2; // greyscale + alpha
    case 6: return 4; // RGBA
    default: return 0;
    }
}

// Which bit depths the spec allows for each colour type.
static bool DepthAllowed(int colourType, int depth)
{
    switch (colourType)
    {
    case 0:  return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case 3:  return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case 2:
    case 4:
    case 6:  return depth == 8 || depth == 16;
    default: return false;
    }
}

// The Adam7 passes: each takes every xstep'th pixel from xstart, on every
// ystep'th row from ystart. A non-interlaced image is the single pass
// (0, 0, 1, 1).
struct PngPass { int xstart, ystart, xstep, ystep; };

static const PngPass kAdam7[7] = {
    { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 },
    { 0, 2, 2, 4 }, { 1, 0, 2, 2 }, { 0, 1, 1, 2 } };

static const PngPass kNoInterlace[1] = { { 0, 0, 1, 1 } };

static unsigned long PassExtent(unsigned long full, int start, int step)
{
    if (full <= (unsigned long)start)
        return 0;
    return (full - (unsigned long)start + (unsigned long)step - 1) / (unsigned long)step;
}

static unsigned char Paeth(unsigned char a, unsigned char b, unsigned char c)
{
    int p = (int)a + (int)b - (int)c;
    int pa = abs(p - (int)a);
    int pb = abs(p - (int)b);
    int pc = abs(p - (int)c);

    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc)             return b;
    return c;
}

// Undoes one scanline's filter in place. cur and prev point just past the
// filter-type byte; prev is NULL for a pass's first row, which the spec
// treats as following a row of zeros. bpp is bytes per complete pixel,
// rounded up to 1 for sub-byte depths.
static bool Unfilter(int filter, unsigned char *cur, const unsigned char *prev,
                     unsigned long rowBytes, unsigned long bpp)
{
    unsigned long i;

    switch (filter)
    {
    case 0: // None
        return true;

    case 1: // Sub
        for (i = bpp; i < rowBytes; ++i)
            cur[i] = (unsigned char)(cur[i] + cur[i - bpp]);
        return true;

    case 2: // Up
        if (prev != NULL)
        {
            for (i = 0; i < rowBytes; ++i)
                cur[i] = (unsigned char)(cur[i] + prev[i]);
        }
        return true;

    case 3: // Average
        for (i = 0; i < rowBytes; ++i)
        {
            unsigned int left = i >= bpp ? cur[i - bpp] : 0;
            unsigned int up   = prev != NULL ? prev[i] : 0;
            cur[i] = (unsigned char)(cur[i] + ((left + up) >> 1));
        }
        return true;

    case 4: // Paeth
        for (i = 0; i < rowBytes; ++i)
        {
            unsigned char left   = i >= bpp ? cur[i - bpp] : 0;
            unsigned char up     = prev != NULL ? prev[i] : 0;
            unsigned char upLeft = (prev != NULL && i >= bpp) ? prev[i - bpp] : 0;
            cur[i] = (unsigned char)(cur[i] + Paeth(left, up, upLeft));
        }
        return true;

    default:
        return false;
    }
}

// The index'th sample in a row, at its native depth (so 0..65535 for 16-bit,
// 0..15 for 4-bit). Sub-byte samples are packed most significant bits first.
static unsigned long SampleAt(const unsigned char *row, unsigned long index, int depth)
{
    if (depth == 16)
        return ((unsigned long)row[index * 2] << 8) | (unsigned long)row[index * 2 + 1];
    if (depth == 8)
        return row[index];

    unsigned long bit = index * (unsigned long)depth;
    int shift = 8 - depth - (int)(bit & 7);
    return ((unsigned long)row[bit >> 3] >> shift) & ((1UL << depth) - 1);
}

// Scales a native-depth sample to 0..255. For the small depths, multiplying
// by 255 / max-value is exact (1 -> 255, 2 -> 85, 4 -> 17), so full-scale
// stays full-scale; 16-bit keeps its high byte.
static unsigned long To8(unsigned long v, int depth)
{
    switch (depth)
    {
    case 16: return v >> 8;
    case 4:  return v * 17;
    case 2:  return v * 85;
    case 1:  return v * 255;
    default: return v;
    }
}

bool PngDecodeToArgb(const unsigned char *data, unsigned long size,
                     unsigned long **outPixels,
                     unsigned long *outWidth, unsigned long *outHeight,
                     const char **outError)
{
    static const unsigned char kSignature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };

    const char *ignoredError;
    if (outError == NULL)
        outError = &ignoredError;

    *outPixels = NULL;
    *outWidth = 0;
    *outHeight = 0;
    *outError = NULL;

    if (data == NULL || size < 8 || memcmp(data, kSignature, 8) != 0)
    {
        *outError = "not a PNG";
        return false;
    }

    // --- Walk the chunks ---------------------------------------------------
    //
    // Only four matter: IHDR (dimensions and format), PLTE (the palette), tRNS
    // (transparency) and IDAT (the compressed image, possibly split across
    // several). Everything else - gamma, colour profile, text - is skipped.
    // CRCs aren't checked: the zlib stream carries its own checksum over the
    // only data that affects the output.

    unsigned long width = 0, height = 0;
    int depth = 0, colourType = -1, interlace = 0;
    bool sawHeader = false;

    unsigned long palette[256];   // as ARGB, alpha filled in from tRNS
    unsigned char paletteAlpha[256];
    int paletteCount = 0;
    int i;
    for (i = 0; i < 256; ++i)
        paletteAlpha[i] = 0xFF;

    bool hasColourKey = false;       // tRNS for greyscale / RGB: one colour that's transparent
    unsigned long keyR = 0, keyG = 0, keyB = 0;

    unsigned long idatTotal = 0;

    unsigned long pos = 8;
    while (pos < size)
    {
        if (size - pos < 12)
        {
            *outError = "chunk header runs past the end of the data";
            return false;
        }

        unsigned long len = ReadBE32(data + pos);
        const unsigned char *type = data + pos + 4;
        const unsigned char *body = data + pos + 8;

        if (len > size - pos - 12)
        {
            *outError = "chunk runs past the end of the data (truncated?)";
            return false;
        }

        if (!sawHeader && !ChunkIs(type, "IHDR"))
        {
            *outError = "first chunk is not IHDR";
            return false;
        }

        if (ChunkIs(type, "IHDR"))
        {
            if (sawHeader || len != 13)
            {
                *outError = "malformed IHDR";
                return false;
            }

            width      = ReadBE32(body);
            height     = ReadBE32(body + 4);
            depth      = body[8];
            colourType = body[9];
            interlace  = body[12];

            // body[10] (compression) and body[11] (filter method) have only
            // ever had one defined value each: 0.
            if (body[10] != 0 || body[11] != 0 || interlace > 1)
            {
                *outError = "unknown compression, filter or interlace method";
                return false;
            }
            if (!DepthAllowed(colourType, depth))
            {
                *outError = "invalid colour type / bit depth combination";
                return false;
            }
            if (width == 0 || height == 0 || width > PNG_MAX_DIMENSION || height > PNG_MAX_DIMENSION)
            {
                *outError = "image dimensions out of range";
                return false;
            }

            sawHeader = true;
        }
        else if (ChunkIs(type, "PLTE"))
        {
            if (len % 3 != 0 || len / 3 > 256 || len == 0)
            {
                *outError = "malformed palette";
                return false;
            }

            paletteCount = (int)(len / 3);
            for (i = 0; i < paletteCount; ++i)
            {
                palette[i] = ((unsigned long)body[i * 3] << 16) |
                             ((unsigned long)body[i * 3 + 1] << 8) |
                              (unsigned long)body[i * 3 + 2];
            }
        }
        else if (ChunkIs(type, "tRNS"))
        {
            // Its layout depends on the colour type, which IHDR has already
            // fixed by now. For RGBA and grey+alpha it's not allowed at all
            // (they carry real alpha) and is ignored.
            if (colourType == 3)
            {
                unsigned long n = len > 256 ? 256 : len;
                for (unsigned long k = 0; k < n; ++k)
                    paletteAlpha[k] = body[k];
            }
            else if (colourType == 0 && len >= 2)
            {
                keyR = keyG = keyB = ((unsigned long)body[0] << 8) | body[1];
                hasColourKey = true;
            }
            else if (colourType == 2 && len >= 6)
            {
                keyR = ((unsigned long)body[0] << 8) | body[1];
                keyG = ((unsigned long)body[2] << 8) | body[3];
                keyB = ((unsigned long)body[4] << 8) | body[5];
                hasColourKey = true;
            }
        }
        else if (ChunkIs(type, "IDAT"))
        {
            idatTotal += len; // can't overflow: every len is bounded by size
        }
        else if (ChunkIs(type, "IEND"))
        {
            break;
        }

        pos += 12 + len;
    }

    if (!sawHeader)
    {
        *outError = "no IHDR";
        return false;
    }
    if (idatTotal == 0)
    {
        *outError = "no image data";
        return false;
    }
    if (colourType == 3 && paletteCount == 0)
    {
        *outError = "palette image with no palette";
        return false;
    }

    for (i = 0; i < paletteCount; ++i)
        palette[i] |= (unsigned long)paletteAlpha[i] << 24;

    // --- Size the decompressed data from the header ------------------------
    //
    // Fixed before inflating a byte, so a stream that tries to produce more
    // is simply refused rather than trusted to say how big it is. Each row
    // of each pass is a filter-type byte plus the packed samples.

    const int channels = ChannelsFor(colourType);
    const unsigned long bitsPerPixel = (unsigned long)(channels * depth);
    const unsigned long filterBpp = (bitsPerPixel + 7) / 8; // bytes per pixel as the filters see it

    const PngPass *passes = interlace ? kAdam7 : kNoInterlace;
    const int passCount = interlace ? 7 : 1;

    // Worst case with the dimension cap is 512 rows of (1 + 512*8) bytes,
    // about 2MB - comfortably inside unsigned long.
    unsigned long rawSize = 0;
    int p;
    for (p = 0; p < passCount; ++p)
    {
        unsigned long pw = PassExtent(width, passes[p].xstart, passes[p].xstep);
        unsigned long ph = PassExtent(height, passes[p].ystart, passes[p].ystep);
        if (pw != 0 && ph != 0)
            rawSize += ph * (1 + (pw * bitsPerPixel + 7) / 8);
    }

    // --- Gather IDAT and inflate ------------------------------------------

    unsigned char *compressed = (unsigned char *)malloc(idatTotal);
    unsigned char *raw = (unsigned char *)malloc(rawSize);
    unsigned long *pixels = (unsigned long *)malloc(width * height * sizeof(unsigned long));

    if (compressed == NULL || raw == NULL || pixels == NULL)
    {
        free(compressed);
        free(raw);
        free(pixels);
        *outError = "out of memory";
        return false;
    }

    // Second walk, copying. The first walk already validated every chunk
    // boundary up to the point it stopped, so this one can't go astray.
    unsigned long copied = 0;
    pos = 8;
    while (pos + 12 <= size && copied < idatTotal)
    {
        unsigned long len = ReadBE32(data + pos);
        if (ChunkIs(data + pos + 4, "IDAT"))
        {
            memcpy(compressed + copied, data + pos + 8, len);
            copied += len;
        }
        pos += 12 + len;
    }

    bool ok = ZlibInflate(compressed, idatTotal, raw, rawSize, outError);
    free(compressed);

    if (!ok)
    {
        free(raw);
        free(pixels);
        return false;
    }

    // --- Unfilter each pass and expand to ARGB -----------------------------

    unsigned char *rowPtr = raw;

    for (p = 0; p < passCount; ++p)
    {
        const PngPass &pass = passes[p];
        unsigned long pw = PassExtent(width, pass.xstart, pass.xstep);
        unsigned long ph = PassExtent(height, pass.ystart, pass.ystep);
        if (pw == 0 || ph == 0)
            continue; // empty passes have no rows at all, not even filter bytes

        unsigned long rowBytes = (pw * bitsPerPixel + 7) / 8;
        const unsigned char *prev = NULL;

        for (unsigned long y = 0; y < ph; ++y)
        {
            int filter = rowPtr[0];
            unsigned char *cur = rowPtr + 1;

            if (!Unfilter(filter, cur, prev, rowBytes, filterBpp))
            {
                free(raw);
                free(pixels);
                *outError = "unknown scanline filter";
                return false;
            }

            unsigned long *dest = pixels + (pass.ystart + y * pass.ystep) * width;

            for (unsigned long x = 0; x < pw; ++x)
            {
                unsigned long a = 0xFF, r, g, b;

                switch (colourType)
                {
                case 0:
                {
                    unsigned long v = SampleAt(cur, x, depth);
                    if (hasColourKey && v == keyG)
                        a = 0;
                    r = g = b = To8(v, depth);
                    break;
                }
                case 2:
                {
                    unsigned long vr = SampleAt(cur, x * 3,     depth);
                    unsigned long vg = SampleAt(cur, x * 3 + 1, depth);
                    unsigned long vb = SampleAt(cur, x * 3 + 2, depth);
                    if (hasColourKey && vr == keyR && vg == keyG && vb == keyB)
                        a = 0;
                    r = To8(vr, depth);
                    g = To8(vg, depth);
                    b = To8(vb, depth);
                    break;
                }
                case 3:
                {
                    unsigned long idx = SampleAt(cur, x, depth);
                    // An index past the palette is invalid; draw it opaque
                    // black rather than refuse an otherwise good image.
                    unsigned long argb = idx < (unsigned long)paletteCount ? palette[idx] : 0xFF000000UL;
                    a = argb >> 24;
                    r = (argb >> 16) & 0xFF;
                    g = (argb >> 8) & 0xFF;
                    b = argb & 0xFF;
                    break;
                }
                case 4:
                    r = g = b = To8(SampleAt(cur, x * 2, depth), depth);
                    a = To8(SampleAt(cur, x * 2 + 1, depth), depth);
                    break;
                default: // 6
                    r = To8(SampleAt(cur, x * 4,     depth), depth);
                    g = To8(SampleAt(cur, x * 4 + 1, depth), depth);
                    b = To8(SampleAt(cur, x * 4 + 2, depth), depth);
                    a = To8(SampleAt(cur, x * 4 + 3, depth), depth);
                    break;
                }

                dest[pass.xstart + x * pass.xstep] = (a << 24) | (r << 16) | (g << 8) | b;
            }

            prev = cur;
            rowPtr += 1 + rowBytes;
        }
    }

    free(raw);

    *outPixels = pixels;
    *outWidth = width;
    *outHeight = height;
    return true;
}

void PngFree(unsigned long *pixels)
{
    free(pixels);
}
