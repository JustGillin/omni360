#include "HttpPlain.h"
#include "HttpResponse.h"
#include "downloadFile.h"
#include "dns.h"
#include "OutputConsole.h"

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#define XBOX_SO_BYPASS_SECURITY 0x5801
#define PLAIN_IO_TIMEOUT_MS     (30 * 1000) // small replies - a stall is a failure well before XboxTLS's 2 minutes
#define PLAIN_MAX_REDIRECTS     4

static int SocketRead(void *context, char *buf, int len)
{
    return recv(*(SOCKET *)context, buf, len, 0);
}

static bool SendAll(SOCKET s, const char *data, int len)
{
    while (len > 0)
    {
        int n = send(s, data, len, 0);
        if (n <= 0)
            return false;
        data += n;
        len -= n;
    }
    return true;
}

// "http://host[:port]/path" into its parts. The path keeps its leading '/'.
static bool SplitUrl(const char *url, char *host, size_t hostSize, unsigned short *port, std::string *path)
{
    if (_strnicmp(url, "http://", 7) != 0)
        return false;
    const char *h = url + 7;
    const char *slash = strchr(h, '/');
    const char *hostEnd = (slash != NULL) ? slash : h + strlen(h);
    const char *colon = (const char *)memchr(h, ':', hostEnd - h);

    size_t hostLen = (colon != NULL ? colon : hostEnd) - h;
    if (hostLen == 0 || hostLen >= hostSize)
        return false;
    memcpy(host, h, hostLen);
    host[hostLen] = '\0';

    *port = 80;
    if (colon != NULL)
    {
        int p = atoi(colon + 1);
        if (p <= 0 || p > 65535)
            return false;
        *port = (unsigned short)p;
    }
    *path = (slash != NULL) ? std::string(slash) : std::string("/");
    return true;
}

// A connection to url's host, and the request line and headers for it,
// without the blank line that ends them. INVALID_SOCKET on failure, said in
// the log.
static SOCKET ConnectFor(const char *url, std::string *request)
{
    char host[256];
    unsigned short port = 80;
    std::string path;
    if (!SplitUrl(url, host, sizeof(host), &port, &path))
    {
        dprintf("[http] not an http:// URL: %s\n", url);
        return INVALID_SOCKET;
    }

    char ip[64] = "";
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        if (ResolveDNS(host, ip, sizeof(ip)) || searchDnsCache(host, ip, sizeof(ip)) == 0)
            break;
        ip[0] = '\0';
        Sleep(500);
    }
    if (ip[0] == '\0')
    {
        dprintf("[http] could not resolve %s\n", host);
        return INVALID_SOCKET;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return INVALID_SOCKET;

    BOOL yes = TRUE;
    DWORD timeoutMs = PLAIN_IO_TIMEOUT_MS;
    setsockopt(s, SOL_SOCKET, XBOX_SO_BYPASS_SECURITY, (PCSTR)&yes, sizeof(yes));
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (PCSTR)&yes, sizeof(yes));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (PCSTR)&timeoutMs, sizeof(timeoutMs));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (PCSTR)&timeoutMs, sizeof(timeoutMs));

    sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = inet_addr(ip);
    if (connect(s, (sockaddr *)&sa, sizeof(sa)) == SOCKET_ERROR)
    {
        dprintf("[http] could not connect to %s (%s): WSA error %d\n", host, ip, WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }

    char hostHeader[280];
    if (port != 80)
        _snprintf(hostHeader, sizeof(hostHeader), "%s:%u", host, (unsigned)port);
    else
        _snprintf(hostHeader, sizeof(hostHeader), "%s", host);
    hostHeader[sizeof(hostHeader) - 1] = '\0';

    *request = "GET " + path + " HTTP/1.1\r\n"
               "Host: " + hostHeader + "\r\n"
               "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/147.0.0.0 Safari/537.36\r\n"
               "Accept: */*\r\n"
               "Accept-Encoding: identity, *;q=0\r\n"
               "Connection: close\r\n";
    return s;
}

// One request on its own connection. *location gets a redirect's target.
static int GetOnce(const char *url, const char *extraHeaderLines, char *buffer, unsigned long long capacity,
                   unsigned long long *outLen, std::string *location)
{
    std::string request;
    SOCKET s = ConnectFor(url, &request);
    if (s == INVALID_SOCKET)
        return 0;
    if (extraHeaderLines != NULL)
        request += extraHeaderLines;
    request += "\r\n";

    int status = 0;
    if (SendAll(s, request.c_str(), (int)request.length()))
    {
        // On the heap: HttpStream carries a 4KB buffer, and the location
        // another 2KB, which is a lot for a worker's stack.
        HttpStream *stream = (HttpStream *)malloc(sizeof(HttpStream));
        HttpResponseResult *response = (HttpResponseResult *)malloc(sizeof(HttpResponseResult));
        if (stream != NULL && response != NULL)
        {
            HttpStreamInit(stream, SocketRead, &s);
            if (ReadHttpResponse(stream, buffer, capacity, response))
            {
                status = response->status;
                if (status >= 200 && status < 300)
                {
                    if (response->bodyOverflow)
                    {
                        dprintf("[http] %s: larger than %I64u bytes\n", url, capacity);
                        status = -1;
                    }
                    else
                    {
                        *outLen = response->bodyLen;
                    }
                }
                else if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)
                {
                    *location = response->location;
                }
            }
            else
            {
                dprintf("[http] %s: the response couldn't be read (status %d)\n", url, response->status);
                status = 0;
            }
        }
        free(stream);
        free(response);
    }
    closesocket(s);
    return status;
}

int HttpGetPlain(const char *url, const char *extraHeaderLines, char *buffer,
                 unsigned long long capacity, unsigned long long *outLen)
{
    if (url == NULL || buffer == NULL || outLen == NULL)
        return 0;
    *outLen = 0;
    buffer[0] = '\0';

    // Reference-counted, like httpRequestHTTPS's.
    XNetStartupParams xnsp;
    memset(&xnsp, 0, sizeof(xnsp));
    xnsp.cfgSizeOfStruct = sizeof(xnsp);
    xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
    if (XNetStartup(&xnsp) != 0)
        return 0;
    WSADATA wsadata;
    if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0)
    {
        XNetCleanup();
        return 0;
    }

    std::string current = url;
    int status = 0;
    for (int hop = 0; hop <= PLAIN_MAX_REDIRECTS; ++hop)
    {
        if (_strnicmp(current.c_str(), "https://", 8) == 0)
        {
            unsigned long long len = capacity;
            status = httpRequestHTTPS(current, HTTP_GET, NULL, extraHeaderLines, "", buffer, &len, false, NULL, 0,
                                      dprintf);
            if (status == 200)
                *outLen = len;
            break; // httpRequestHTTPS hands a redirect back; callers of this don't expect one
        }

        std::string location;
        status = GetOnce(current.c_str(), extraHeaderLines, buffer, capacity, outLen, &location);
        if (location.empty())
            break;

        // A relative Location is against the same host.
        if (location[0] == '/')
        {
            size_t hostEnd = current.find('/', 7);
            location = current.substr(0, hostEnd) + location;
        }
        current = location;
        status = 0;
    }

    WSACleanup();
    XNetCleanup();
    return status;
}

// The status line's code and Content-Length (0 when there's none) of the
// header block in head, which ends at its blank line.
static int ParseHead(const char *head, unsigned long long *contentLength, bool *chunked)
{
    *contentLength = 0;
    *chunked = false;
    int status = 0;
    if (sscanf(head, "HTTP/%*d.%*d %d", &status) != 1)
        return 0;
    for (const char *line = strstr(head, "\r\n"); line != NULL; line = strstr(line, "\r\n"))
    {
        line += 2;
        if (_strnicmp(line, "Content-Length:", 15) == 0)
            *contentLength = _strtoui64(line + 15, NULL, 10);
        else if (_strnicmp(line, "Transfer-Encoding:", 18) == 0 && strstr(line, "chunked") != NULL &&
                 strstr(line, "chunked") < strstr(line, "\r\n"))
            *chunked = true;
    }
    return status;
}

int HttpDownloadPlain(const char *url, const char *filePath, DownloadProgressFn progressFn)
{
    if (url == NULL || filePath == NULL)
        return 0;

    XNetStartupParams xnsp;
    memset(&xnsp, 0, sizeof(xnsp));
    xnsp.cfgSizeOfStruct = sizeof(xnsp);
    xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
    if (XNetStartup(&xnsp) != 0)
        return 0;
    WSADATA wsadata;
    if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0)
    {
        XNetCleanup();
        return 0;
    }

    int status = 0;
    std::string request;
    SOCKET s = ConnectFor(url, &request);
    request += "\r\n";
    const int bufSize = 64 * 1024;
    char *buf = (char *)malloc(bufSize + 1);
    if (s != INVALID_SOCKET && buf != NULL && SendAll(s, request.c_str(), (int)request.length()))
    {
        // The headers, up to the blank line, and whatever of the body came
        // with them.
        int have = 0;
        char *bodyStart = NULL;
        while (have < bufSize)
        {
            const int n = recv(s, buf + have, bufSize - have, 0);
            if (n <= 0)
                break;
            have += n;
            buf[have] = '\0';
            if ((bodyStart = strstr(buf, "\r\n\r\n")) != NULL)
                break;
        }

        unsigned long long total = 0;
        bool chunked = false;
        if (bodyStart != NULL)
            status = ParseHead(buf, &total, &chunked);

        if (status == 200 && chunked)
        {
            dprintf("[http] %s: sent chunked, which a download to a file doesn't take\n", url);
            status = -1;
        }
        else if (status == 200)
        {
            FILE *out = fopen(filePath, "wb");
            if (out == NULL)
            {
                dprintf("[http] couldn't make %s\n", filePath);
                status = -1;
            }
            else
            {
                bodyStart += 4;
                unsigned long long done = (unsigned long long)(buf + have - bodyStart);
                bool ok = done == 0 || fwrite(bodyStart, 1, (size_t)done, out) == done;
                const DWORD started = GetTickCount();
                DWORD lastReport = 0;
                while (ok && (total == 0 || done < total))
                {
                    const int n = recv(s, buf, bufSize, 0);
                    if (n <= 0)
                        break;
                    ok = fwrite(buf, 1, n, out) == (size_t)n;
                    done += n;

                    const DWORD now = GetTickCount();
                    if (progressFn != NULL && now - lastReport >= 100)
                    {
                        lastReport = now;
                        const DWORD elapsed = now - started;
                        const unsigned long long perSec = elapsed > 0 ? done * 1000 / elapsed : 0;
                        const unsigned long long left = (total > done && perSec > 0) ? (total - done) / perSec : 0;
                        if (!progressFn(done, total, perSec, left))
                        {
                            status = HTTP_STATUS_CANCELLED;
                            ok = false;
                        }
                    }
                }
                ok = (fclose(out) == 0) && ok;
                if (status == 200 && (!ok || (total > 0 && done != total)))
                {
                    dprintf("[http] %s: got %I64u of %I64u bytes\n", url, done, total);
                    status = -1;
                }
                if (status != 200)
                    remove(filePath);
            }
        }
        else if (status != 0)
        {
            dprintf("[http] %s: status %d\n", url, status);
        }
    }

    free(buf);
    if (s != INVALID_SOCKET)
        closesocket(s);
    WSACleanup();
    XNetCleanup();
    return status;
}
