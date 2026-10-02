#ifndef HTTP_PLAIN_H
#define HTTP_PLAIN_H

// Plain-HTTP GETs, for the Store's art and details from Xbox Live:
// download.xbox.com serves its images only over HTTP, and catalog.xboxlive.com
// over HTTPS needs a root the app doesn't carry. Everything else stays on
// HTTPS (downloadFile.h).
//
// Buffer mode only - these are images and small XML replies. Safe to call
// from any thread; each call is its own connection.

// GETs url ("http://host[:port]/path"), following up to four redirects; one
// to an https:// URL goes through httpRequestHTTPS. Returns the HTTP status,
// or 0 / a negative value on failure. On 200 the body is in buffer, NUL-
// terminated, and *outLen is its length - buffer needs room for capacity + 1
// bytes. A body larger than capacity is a failure (-1).
int HttpGetPlain(const char *url, const char *extraHeaderLines, char *buffer,
                 unsigned long long capacity, unsigned long long *outLen);

#endif
