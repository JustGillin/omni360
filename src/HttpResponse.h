#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

// Reads exactly one HTTP/1.1 response off a byte stream and stops at its end,
// so the connection can carry another request afterwards.
//
// Why this exists alongside DumpResponse (downloadFile.cpp): DumpResponse
// finds the end of a response by reading until the server closes the
// connection, which is what "Connection: close" guarantees. That makes every
// request its own connection - a DNS lookup, a TCP connect and a full TLS
// handshake each time, and the handshake is the slow part on the 360's
// software crypto. The RAR header walk makes dozens of small requests in a
// row, so nearly all of its time went on handshakes. Reusing one connection
// needs the end of each response found from the response itself -
// Content-Length, or the chunked encoding's terminator - which is this.
//
// Deliberately free of any Xbox or TLS header: the stream is just a read
// function, so the framing rules are tested on a PC against responses split
// at arbitrary points, which is exactly where framing code goes wrong.

// Reads up to len bytes into buf. Returns the count read, or <= 0 once the
// stream is closed or failed - the same contract as recv and XboxTLS_Read.
typedef int (*HttpReadFn)(void *context, char *buf, int len);

#define HTTP_LOCATION_MAX 2048

struct HttpResponseResult
{
    int status;                 // e.g. 206; 0 if no valid status line arrived
    unsigned long long bodyLen; // bytes stored into the caller's body buffer
    bool bodyOverflow;          // the body didn't fit; bodyLen is the capacity, the rest was discarded
    bool keepAlive;             // the connection is positioned at a clean boundary and may be reused
    bool gotAnyBytes;           // false if the stream ended before a single byte - on a reused
                                // connection, the sign the server had already closed it
    char location[HTTP_LOCATION_MAX]; // the Location header, if any (redirects)
    char date[64];              // the Date header, if any: "Mon, 05 Oct 2026 15:20:00 GMT"
};

#define HTTP_STREAM_BUFFER 4096

// One connection's incoming bytes. It belongs to the CONNECTION, not to a
// single response, and that matters: the read function hands back whatever
// has arrived, which needn't stop where a response does, so bytes read past
// the end of one response are the start of the next and must be kept for it.
// Initialise once per connection with HttpStreamInit.
struct HttpStream
{
    HttpReadFn read;
    void *context;
    char buf[HTTP_STREAM_BUFFER];
    int len;
    int pos;
    bool closed;
    bool gotAny; // bytes were available for the response being read
};

void HttpStreamInit(HttpStream *stream, HttpReadFn readFn, void *readContext);

// Reads the next response from stream. The body is stored only for 2xx
// statuses; for any other status it is read and discarded, which keeps the
// connection usable.
//
// body must have room for bodyCapacity + 1 bytes: the body is always
// NUL-terminated after its last byte, as DumpResponse does for its callers.
//
// Returns false if the response couldn't be read to its end - the stream
// closed early, or the framing was malformed. The connection must be dropped
// then; out->gotAnyBytes says whether anything had arrived at all.
bool ReadHttpResponse(HttpStream *stream, char *body, unsigned long long bodyCapacity,
                      HttpResponseResult *out);

#endif
