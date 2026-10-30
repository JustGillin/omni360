#ifndef HTTP_PLAIN_H
#define HTTP_PLAIN_H

#include "downloadFile.h" // DownloadProgressFn

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

// GETs url ("http://host[:port]/path") into the file at filePath, reporting
// progress as downloadFile.h's requests do - for the Homebrew test mirror, a
// folder served over the LAN. No redirects, and not a chunked reply: python's
// http.server and the like send a Content-Length. Returns the HTTP status,
// HTTP_STATUS_CANCELLED if progressFn stopped it, or 0 / -1 on failure, when
// nothing is left at filePath.
int HttpDownloadPlain(const char *url, const char *filePath, DownloadProgressFn progressFn);

#endif
