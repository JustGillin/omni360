#ifndef DOWNLOAD_FILE_H
#define DOWNLOAD_FILE_H

#include <string>

enum HttpMethod
{
    HTTP_GET,
    HTTP_POST
};

/// @brief Called periodically from inside the download read loop so a caller can
/// draw a real progress bar instead of only getting the text lines printFunction
/// emits. Both download paths (chunked and non-chunked) report through this.
///
/// bytesTotal is 0 when the final size genuinely isn't known (no Content-Length
/// and no knownTotalSize hint), in which case secondsRemaining is 0 too and only
/// bytesDone/bytesPerSec are meaningful - a caller should show an indeterminate
/// state rather than a percentage.
///
/// This fires on its own ~100ms throttle, separate from (and much faster than)
/// the 2-3 second throttle on the text progress prints, so a bar animates
/// smoothly. Keep implementations cheap: this runs between socket reads, and
/// anything slow here directly costs download throughput.
///
/// Return true to carry on, false to cancel the download. A cancelled request
/// returns HTTP_STATUS_CANCELLED rather than a failure status, so callers that
/// retry failed transfers know not to. This is the only way to cancel: the
/// read loop no longer polls the controller itself, since downloads are
/// moving onto a worker thread and only the UI thread should read the pad.
typedef bool (*DownloadProgressFn)(unsigned long long bytesDone,
                                   unsigned long long bytesTotal,
                                   unsigned long long bytesPerSec,
                                   unsigned long long secondsRemaining);

/// What httpRequestHTTPS returns when its DownloadProgressFn asked to stop.
/// Negative, like its other failures, so "not 200/206" checks still treat it
/// as not-a-success - but distinct, so it isn't retried or reported as an error.
#define HTTP_STATUS_CANCELLED (-3)

/// @brief Formats a byte count as a human-readable string ("12.4 MB"). Already
/// used internally for the text progress lines; declared here so UI code can
/// format the same numbers the same way instead of reimplementing it.
/// output should be ~64 bytes; returns false on a NULL/zero-size buffer.
bool FormatBytes(unsigned long long bytes, char *output, size_t outputSize);

/// @brief downloads file over https using http 1.1 and tls 1.2
int downloadFileHTTPS(const std::string URL, const std::string fileName, char *dataBuffer, unsigned long long *outputBufferSize, bool downloadIntoFile, void printFunction(const char *_format, ...));

/// @brief general-purpose HTTPS request supporting GET/POST, a request body, extra
/// raw request header lines (each ending in "\r\n", e.g. "Cookie: a=b\r\n"), and
/// optional capture of any Set-Cookie values the response returns (accumulated into
/// cookieOutBuffer as "name=value; name2=value2"; pass NULL/0 to skip).
/// knownTotalSize is an optional hint for the final size of the response body,
/// used for progress/ETA reporting and completion validation when the server
/// doesn't send a Content-Length (e.g. a chunked response) - pass 0 if unknown.
/// progressFn, if non-NULL, is called periodically during the body read with
/// live byte counts - see DownloadProgressFn above.
int httpRequestHTTPS(const std::string URL, HttpMethod method, const char *requestBody,
                     const char *extraHeaderLines, const std::string fileName,
                     char *dataBuffer, unsigned long long *outputBufferSize, bool downloadIntoFile,
                     char *cookieOutBuffer, unsigned long long cookieOutBufferSize,
                     void printFunction(const char *_format, ...),
                     unsigned long long knownTotalSize = 0,
                     DownloadProgressFn progressFn = NULL);

/// @brief A connection kept open across a run of small GET requests.
///
/// httpRequestHTTPS opens a new connection for every request - DNS lookup,
/// TCP connect and a full TLS handshake - and asks the server to close it
/// afterwards. For one download that's fine. For the RAR header walk, which
/// makes dozens of tiny Range requests in a row, the handshakes were nearly
/// all of the time spent, since TLS setup is the slow part on the 360.
///
/// A session keeps one connection per host open and sends each request on
/// it, reconnecting only when the host changes or the server closes it. If a
/// reused connection turns out to have been closed while idle, the request is
/// retried once on a fresh one. Responses are read with ReadHttpResponse
/// (HttpResponse.h), which finds their end from the response itself rather
/// than by waiting for the connection to close.
///
/// Buffer-mode GETs only - large downloads to a file stay on httpRequestHTTPS.
struct HttpsSession;

/// Brings the network stack up for the session's lifetime. NULL on failure;
/// callers can fall back to httpRequestHTTPS then.
HttpsSession *HttpsSessionOpen(void printFunction(const char *_format, ...));

/// Closes the connection, if one is open, and logs how many requests went
/// over how many connections. Safe with NULL.
void HttpsSessionClose(HttpsSession *session);

/// Same contract as httpRequestHTTPS in buffer mode: returns the HTTP status
/// (0 or -1 on failure); on 200/206 the body is in dataBuffer and
/// *dataBufferSize is its length; on a 302 the redirect target is in
/// dataBuffer instead. dataBuffer needs room for *dataBufferSize + 1 bytes,
/// as the body is NUL-terminated.
int HttpsSessionGet(HttpsSession *session, const std::string &url, const char *extraHeaderLines,
                    char *dataBuffer, unsigned long long *dataBufferSize);

/// The Date header of the session's last response - the server's idea of
/// today, for when the console's clock can't be trusted. False, with out
/// empty, if there was none.
bool HttpsSessionServerDate(HttpsSession *session, char *out, size_t outSize);

#endif