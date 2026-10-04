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

// One request on its own connection. *location gets a redirect's target.
static int GetOnce(const char *url, const char *extraHeaderLines, char *buffer, unsigned long long capacity,
                   unsigned long long *outLen, std::string *location)
{
    char host[256];
    unsigned short port = 80;
    std::string path;
    if (!SplitUrl(url, host, sizeof(host), &port, &path))
    {
        dprintf("[http] not an http:// URL: %s\n", url);
        return 0;
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
        return 0;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return 0;

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
        return 0;
    }

    char hostHeader[280];
    if (port != 80)
        _snprintf(hostHeader, sizeof(hostHeader), "%s:%u", host, (unsigned)port);
    else
        _snprintf(hostHeader, sizeof(hostHeader), "%s", host);
    hostHeader[sizeof(hostHeader) - 1] = '\0';

    std::string request = "GET " + path + " HTTP/1.1\r\n"
                          "Host: " + hostHeader + "\r\n"
                          "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/147.0.0.0 Safari/537.36\r\n"
                          "Accept: */*\r\n"
                          "Accept-Encoding: identity, *;q=0\r\n"
                          "Connection: close\r\n";
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
