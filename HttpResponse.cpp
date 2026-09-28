/*
FILE : HttpResponse.cpp
PROJECT : Omni360
DESCRIPTION : reads one HTTP/1.1 response and stops at its end, so a
              connection can be reused. See HttpResponse.h for why.

Message length follows RFC 7230 section 3.3.3, in the order it gives:
  1. 1xx, 204 and 304 responses have no body.
  2. Transfer-Encoding: chunked is decoded to its terminating chunk,
     including any trailer lines after it.
  3. Otherwise Content-Length gives the body length exactly.
  4. Otherwise the body runs until the server closes the connection - which
     by definition leaves nothing to reuse.
*/

#include "HttpResponse.h"

#include <string.h>

#define HEADER_MAX   (16 * 1024)
#define LINE_MAX     256

// ---------------------------------------------------------------------------
// Buffered reading
// ---------------------------------------------------------------------------

// The read function hands back whatever arrived - one TLS record, part of
// one, or several responses' worth of bytes in principle - so everything
// reads through the connection's HttpStream buffer, and nothing assumes a
// response starts or ends on a read boundary.
typedef HttpStream Stream;

void HttpStreamInit(HttpStream *stream, HttpReadFn readFn, void *readContext)
{
    stream->read = readFn;
    stream->context = readContext;
    stream->len = 0;
    stream->pos = 0;
    stream->closed = false;
    stream->gotAny = false;
}

static bool StreamFill(Stream *s)
{
    if (s->pos < s->len)
        return true;
    if (s->closed)
        return false;

    int r = s->read(s->context, s->buf, HTTP_STREAM_BUFFER);
    if (r <= 0)
    {
        s->closed = true;
        return false;
    }

    s->len = r;
    s->pos = 0;
    s->gotAny = true;
    return true;
}

static bool StreamByte(Stream *s, char *c)
{
    if (!StreamFill(s))
        return false;
    *c = s->buf[s->pos++];
    return true;
}

// One line, without its CR LF (a bare LF is accepted too). False at end of
// stream, or if the line is longer than the buffer - a line that long in the
// places this is used means the framing is not what it claims to be.
static bool StreamLine(Stream *s, char *out, int cap, int *outLen)
{
    int n = 0;
    for (;;)
    {
        char c;
        if (!StreamByte(s, &c))
            return false;
        if (c == '\n')
            break;
        if (n >= cap - 1)
            return false;
        out[n++] = c;
    }

    if (n > 0 && out[n - 1] == '\r')
        n--;
    out[n] = '\0';
    if (outLen != NULL)
        *outLen = n;
    return true;
}

// ---------------------------------------------------------------------------
// Body storage
// ---------------------------------------------------------------------------

struct Sink
{
    char *body;
    unsigned long long cap;
    unsigned long long len;
    bool store;    // false: read and discard (non-2xx bodies)
    bool overflow;
};

static void SinkAppend(Sink *k, const char *data, unsigned long long n)
{
    if (!k->store || k->body == NULL)
        return;

    unsigned long long room = k->cap - k->len;
    if (n > room)
    {
        k->overflow = true;
        n = room;
    }

    memcpy(k->body + k->len, data, (size_t)n);
    k->len += n;
}

// Moves exactly `count` bytes from the stream into the sink - or stops early,
// returning true with k->overflow set, the moment the body stops fitting.
//
// Stopping early rather than reading on and discarding: the case this guards
// against is a server that ignores a Range header and answers with the whole
// file, which for a DLC pack is gigabytes. Reading all of that just to keep
// the connection in step would take far longer than a fresh connection costs.
static bool StreamCopy(Stream *s, Sink *k, unsigned long long count)
{
    while (count > 0)
    {
        if (!StreamFill(s))
            return false;

        unsigned long long take = (unsigned long long)(s->len - s->pos);
        if (take > count)
            take = count;

        SinkAppend(k, s->buf + s->pos, take);
        s->pos += (int)take;
        count -= take;

        if (k->overflow)
            return true;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Header parsing
// ---------------------------------------------------------------------------

static char Lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// Case-insensitive: header names are, and so are the tokens compared here.
static bool NameIs(const char *line, int nameLen, const char *name)
{
    int i = 0;
    for (; i < nameLen && name[i] != '\0'; ++i)
    {
        if (Lower(line[i]) != name[i])
            return false;
    }
    return i == nameLen && name[i] == '\0';
}

// True if value holds token as one of its comma-separated items, e.g.
// "chunked" in "gzip, chunked".
static bool HasToken(const char *value, const char *token)
{
    size_t tokenLen = strlen(token);
    const char *p = value;

    while (*p != '\0')
    {
        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;

        const char *start = p;
        while (*p != '\0' && *p != ',')
            p++;

        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            end--;

        if ((size_t)(end - start) == tokenLen)
        {
            size_t i = 0;
            while (i < tokenLen && Lower(start[i]) == token[i])
                i++;
            if (i == tokenLen)
                return true;
        }
    }
    return false;
}

static bool ParseDecimal(const char *s, unsigned long long *out)
{
    unsigned long long v = 0;
    bool any = false;

    while (*s == ' ' || *s == '\t')
        s++;

    for (; *s >= '0' && *s <= '9'; ++s)
    {
        if (v > (0xFFFFFFFFFFFFFFFFULL - 9) / 10)
            return false;
        v = v * 10 + (unsigned long long)(*s - '0');
        any = true;
    }

    while (*s == ' ' || *s == '\t')
        s++;

    if (!any || *s != '\0')
        return false;

    *out = v;
    return true;
}

// A chunk-size line: hex digits, optionally followed by ";extensions".
static bool ParseChunkSizeLine(const char *s, unsigned long long *out)
{
    unsigned long long v = 0;
    int digits = 0;

    for (;; ++s)
    {
        int d;
        if (*s >= '0' && *s <= '9')      d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else break;

        if (++digits > 15)
            return false; // larger than anything real, and past where v could overflow
        v = (v << 4) | (unsigned long long)d;
    }

    while (*s == ' ' || *s == '\t')
        s++;

    if (digits == 0 || (*s != '\0' && *s != ';'))
        return false;

    *out = v;
    return true;
}

struct Headers
{
    int status;
    int minorVersion;
    bool haveLength;
    unsigned long long length;
    bool chunked;
    bool connectionClose;
};

// Reads the status line and header lines, up to and including the blank
// line that ends them.
static bool ReadHeaders(Stream *s, Headers *h, HttpResponseResult *out)
{
    // Location URLs make header lines the longest thing here; this leaves room
    // for one of HTTP_LOCATION_MAX plus its header name.
    char line[HTTP_LOCATION_MAX * 2];
    int len = 0;
    int total = 0;

    memset(h, 0, sizeof(*h));

    if (!StreamLine(s, line, sizeof(line), &len))
        return false;
    total += len;

    // "HTTP/1.1 206 Partial Content"
    if (len < 12 || memcmp(line, "HTTP/1.", 7) != 0 || line[8] != ' ' ||
        line[9] < '1' || line[9] > '5' || line[10] < '0' || line[10] > '9' ||
        line[11] < '0' || line[11] > '9')
        return false;

    h->minorVersion = line[7] - '0';
    h->status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');

    for (;;)
    {
        if (!StreamLine(s, line, sizeof(line), &len))
            return false;

        total += len;
        if (total > HEADER_MAX)
            return false;

        if (len == 0)
            return true; // blank line: end of headers

        const char *colon = (const char *)memchr(line, ':', (size_t)len);
        if (colon == NULL)
            continue; // not a header line; tolerated rather than fatal

        int nameLen = (int)(colon - line);
        const char *value = colon + 1;
        while (*value == ' ' || *value == '\t')
            value++;

        if (NameIs(line, nameLen, "content-length"))
        {
            unsigned long long v;
            if (!ParseDecimal(value, &v))
                return false;
            // Two different lengths leave no way to know where the body ends.
            if (h->haveLength && v != h->length)
                return false;
            h->haveLength = true;
            h->length = v;
        }
        else if (NameIs(line, nameLen, "transfer-encoding"))
        {
            if (HasToken(value, "chunked"))
                h->chunked = true;
        }
        else if (NameIs(line, nameLen, "connection"))
        {
            if (HasToken(value, "close"))
                h->connectionClose = true;
        }
        else if (NameIs(line, nameLen, "location"))
        {
            // A truncated URL is worse than none - it would send the next
            // request somewhere real but wrong - so an over-long one is
            // dropped instead.
            size_t n = strlen(value);
            while (n > 0 && (value[n - 1] == ' ' || value[n - 1] == '\t'))
                n--;
            if (n < sizeof(out->location))
            {
                memcpy(out->location, value, n);
                out->location[n] = '\0';
            }
            else
            {
                out->location[0] = '\0';
            }
        }
    }
}

// ---------------------------------------------------------------------------
// The response
// ---------------------------------------------------------------------------

bool ReadHttpResponse(HttpStream *stream, char *body, unsigned long long bodyCapacity,
                      HttpResponseResult *out)
{
    memset(out, 0, sizeof(*out));

    Stream &s = *stream;

    // Bytes already buffered from the last read belong to this response, so
    // they count as having received something.
    s.gotAny = (s.pos < s.len);

    Headers h;

    // A 1xx is an interim response with the real one straight after it -
    // skipped, apart from 101, which switches protocols and never is.
    for (;;)
    {
        bool ok = ReadHeaders(&s, &h, out);
        out->gotAnyBytes = s.gotAny;
        if (!ok)
            return false;

        out->status = h.status;
        if (h.status / 100 != 1 || h.status == 101)
            break;
        out->location[0] = '\0';
    }

    Sink k;
    k.body = body;
    k.cap = bodyCapacity;
    k.len = 0;
    k.store = (h.status / 100 == 2);
    k.overflow = false;

    bool framed = true; // the end of the body was found from the message itself

    if (h.status / 100 == 1 || h.status == 204 || h.status == 304)
    {
        // no body
    }
    else if (h.chunked)
    {
        // Chunked takes precedence over Content-Length, per the RFC.
        char line[LINE_MAX];
        for (;;)
        {
            unsigned long long size;
            if (!StreamLine(&s, line, sizeof(line), NULL) || !ParseChunkSizeLine(line, &size))
                return false;

            if (size == 0)
                break;

            if (!StreamCopy(&s, &k, size))
                return false;
            if (k.overflow)
                break; // abandoned mid-body - see StreamCopy

            // Each chunk's data is followed by CR LF, and nothing else.
            int len;
            if (!StreamLine(&s, line, sizeof(line), &len) || len != 0)
                return false;
        }

        // Trailer lines, then the blank line that really ends the message.
        // Leaving that blank line unread is the classic keep-alive bug: it
        // becomes the first line of the next response.
        for (; !k.overflow;)
        {
            int len;
            if (!StreamLine(&s, line, sizeof(line), &len))
                return false;
            if (len == 0)
                break;
        }
    }
    else if (h.haveLength)
    {
        if (!StreamCopy(&s, &k, h.length))
            return false;
    }
    else
    {
        // Delimited by the server closing the connection: read to the end.
        framed = false;
        while (!k.overflow)
        {
            if (!StreamFill(&s))
                break;
            SinkAppend(&k, s.buf + s.pos, (unsigned long long)(s.len - s.pos));
            s.pos = s.len;
        }
    }

    out->bodyLen = k.len;
    out->bodyOverflow = k.overflow;
    if (body != NULL)
        body[k.len] = '\0';

    // Reusable only if this was HTTP/1.1, the server didn't ask to close, the
    // body's end came from the message itself, and it was read to that end -
    // an overflowing body is abandoned partway, leaving the stream mid-body.
    // Anything already read past a clean end stays in the stream as the start
    // of the next response.
    out->keepAlive = (h.minorVersion >= 1) && !h.connectionClose && framed && !k.overflow;
    return true;
}
