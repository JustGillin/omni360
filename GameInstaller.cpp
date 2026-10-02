/*
FILE : GameInstaller.cpp
PROJECT : Omni360
DESCRIPTION : Downloads a Redump game's zip from archive.org and installs it as
              Games on Demand, on a worker thread. See GameInstaller.h.
*/

#include "GameInstaller.h"
#include "InflateSource.h"
#include "GodConvert.h"
#include "ReadAhead.h"
#include "DiscWorker.h"    // WriteInstallMarker, ClearInstallMarker
#include "ArchiveOrgDLC.h" // DriveFreeSpace
#include "downloadFile.h"
#include "XboxTLS.h"       // XboxTLS_SetThreadIoTimeout
#include "StoreArt.h"      // FetchTitleIcon
#include "parsing.h"       // UrlEncodeFormValue
#include "OutputConsole.h"

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <string>

#define PIECE_SIZE        (32ULL * 1024 * 1024)
#define CONNECTIONS       2
#define CONNECTION_STAGGER_MS 1500 // four opened at the same instant all stalled once
#define PIECE_ATTEMPTS    6 // slow drops use them up too
#define STAGING_FOLDER    "Omni360Staging"
#define SPACE_MARGIN      (64ULL * 1024 * 1024)
#define AUTH_MAX          256

struct GameJob
{
    bool used; // the slot holds a job
    QueueJobSnapshot snap;
    GameRequest request;
    char auth[AUTH_MAX];
    bool cancel;
    bool handedOver;
};

static CRITICAL_SECTION g_lock;
static HANDLE g_wake = NULL;
static HANDLE g_thread = NULL;
static bool g_running = false;
static bool g_shutdown = false;
static char g_gamesPath[512] = "";

// Slots, not a list: a job stays where it is for its whole life, since the
// worker holds a pointer to the one it's running.
static GameJob g_jobs[MAX_GAME_JOBS];
static int g_nextId = GAME_JOB_ID_BASE;

// ---------------------------------------------------------------------------
// What's been installed, by zip - see IsGameZipInstalled
// ---------------------------------------------------------------------------
//
// A line a zip, "TITLEID MEDIAID zip name", added to as each install
// finishes. A title ID alone can't say which disc of a multi-disc game is
// installed; the media ID can, as each disc's package is named by it.

#define INSTALLED_LIST     "game:\\Store\\Installed.txt"
#define MAX_INSTALLED_ZIPS 256

struct InstalledZip
{
    unsigned long titleId;
    unsigned long mediaId;
    char zip[256];
};

static InstalledZip g_installed[MAX_INSTALLED_ZIPS];
static int g_installedCount = 0;

// Under the lock, once the installer is running. A later line for the same
// zip replaces an earlier one.
static void NoteInstalledZip(const char *zip, unsigned long titleId, unsigned long mediaId)
{
    int slot = 0;
    while (slot < g_installedCount && strcmp(g_installed[slot].zip, zip) != 0)
        slot++;
    if (slot == g_installedCount)
    {
        if (g_installedCount >= MAX_INSTALLED_ZIPS)
            return;
        g_installedCount++;
    }
    g_installed[slot].titleId = titleId;
    g_installed[slot].mediaId = mediaId;
    strncpy(g_installed[slot].zip, zip, sizeof(g_installed[slot].zip) - 1);
    g_installed[slot].zip[sizeof(g_installed[slot].zip) - 1] = '\0';
}

// The notes whose packages are still in the games folder.
static void LoadInstalledZips(const char *gamesPath)
{
    g_installedCount = 0;
    FILE *f = fopen(INSTALLED_LIST, "rb");
    if (f == NULL)
        return;

    char line[300];
    while (fgets(line, sizeof(line), f) != NULL)
    {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        unsigned long titleId = 0, mediaId = 0;
        int nameAt = 0;
        if (sscanf(line, "%lx %lx %n", &titleId, &mediaId, &nameAt) < 2 || nameAt <= 0 || line[nameAt] == '\0')
            continue;
        if (IsDiscInstalled(gamesPath, titleId, mediaId))
            NoteInstalledZip(line + nameAt, titleId, mediaId);
    }
    fclose(f);
}

// After an install: noted for this session, and in the file for the next.
static void RecordInstalledZip(const char *zip, unsigned long titleId, unsigned long mediaId)
{
    EnterCriticalSection(&g_lock);
    NoteInstalledZip(zip, titleId, mediaId);
    LeaveCriticalSection(&g_lock);

    CreateDirectoryA("game:\\Store", NULL); // fails harmlessly once it exists
    FILE *f = fopen(INSTALLED_LIST, "ab");
    if (f == NULL)
    {
        dprintf("[game] couldn't note the install in %s\n", INSTALLED_LIST);
        return;
    }
    fprintf(f, "%08lX %08lX %s\r\n", titleId, mediaId, zip);
    fclose(f);
}

bool IsGameZipInstalled(const char *zipName)
{
    if (!g_running || zipName == NULL)
        return false;

    bool found = false;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_installedCount && !found; ++i)
        found = (strcmp(g_installed[i].zip, zipName) == 0);
    LeaveCriticalSection(&g_lock);
    return found;
}

// The used slot whose job is in `state`, with the lowest id above `after`
// (or highest below it, with newestFirst) - the order the Queue lists them.
// -1 for none. Under the lock.
static int NextJob(QueueJobState state, int after, bool newestFirst)
{
    int pick = -1;
    for (int i = 0; i < MAX_GAME_JOBS; ++i)
    {
        if (!g_jobs[i].used || g_jobs[i].snap.state != state)
            continue;
        int id = g_jobs[i].snap.id;
        if (newestFirst ? (id >= after) : (id <= after))
            continue;
        if (pick < 0 || (newestFirst ? id > g_jobs[pick].snap.id : id < g_jobs[pick].snap.id))
            pick = i;
    }
    return pick;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static unsigned short LE16(const unsigned char *p) { return (unsigned short)(p[0] | (p[1] << 8)); }
static unsigned long LE32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}
static unsigned long long LE64(const unsigned char *p)
{
    return (unsigned long long)LE32(p) | ((unsigned long long)LE32(p + 4) << 32);
}

// The HTTP client's own chatter, minus the progress lines: two connections
// over a few hundred pieces would otherwise write thousands of them to the
// log. Failures still get through.
static void QuietPrint(const char *format, ...)
{
    char line[1024];
    va_list args;
    va_start(args, format);
    _vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    line[sizeof(line) - 1] = '\0';

    if (strstr(line, "ERROR") != NULL || strstr(line, "Failed") != NULL || strstr(line, "failed") != NULL)
        dprintf("%s", line);
}

// A GET into a buffer, following redirects. *ioUrl ends up where the last
// redirect led.
static int Get(std::string &ioUrl, const char *auth, const char *range, char *buffer,
               unsigned long long capacity, unsigned long long *outLen)
{
    char headers[1024];
    _snprintf(headers, sizeof(headers), "%s%s", auth != NULL ? auth : "", range != NULL ? range : "");
    headers[sizeof(headers) - 1] = '\0';

    for (int hop = 0; hop < 4; ++hop)
    {
        unsigned long long len = capacity;
        int status = httpRequestHTTPS(ioUrl, HTTP_GET, NULL, headers, "", buffer, &len, false, NULL, 0, QuietPrint);
        if (status == 302 || status == 301)
        {
            char next[2048];
            strncpy(next, buffer, sizeof(next) - 1);
            next[sizeof(next) - 1] = '\0';
            ioUrl = next;
            continue;
        }
        *outLen = len;
        return status;
    }
    *outLen = 0;
    return -1;
}

static void DriveOf(const char *path, char *out, size_t outSize)
{
    const char *colon = strchr(path, ':');
    size_t n = (colon != NULL) ? (size_t)(colon - path) + 1 : 0;
    if (n == 0 || n >= outSize)
        n = 0;
    memcpy(out, path, n);
    out[n] = '\0';
}

// Every file in a folder, then the folder.
static void RemoveFolder(const char *dir)
{
    char pattern[600];
    _snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    pattern[sizeof(pattern) - 1] = '\0';

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
                continue;
            char path[600];
            _snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
            path[sizeof(path) - 1] = '\0';
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveFolder(path);
            else
                DeleteFileA(path);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

static void StagingRoot(const char *gamesPath, char *out, size_t outSize)
{
    char drive[16];
    DriveOf(gamesPath, drive, sizeof(drive));
    _snprintf(out, outSize, "%s\\" STAGING_FOLDER, drive);
    out[outSize - 1] = '\0';
}

// ---------------------------------------------------------------------------
// The zip's directory
// ---------------------------------------------------------------------------

struct ZipMember
{
    char name[512];
    unsigned short method;
    unsigned long crc;
    unsigned long long packed;
    unsigned long long unpacked;
    unsigned long long localHeader;
};

// The End of Central Directory record from the zip's last 64KB, its ZIP64
// locator and record when the sizes need 64 bits, and the first central
// directory entry with its ZIP64 extra field. Redump zips hold one member.
static bool ReadZipDirectory(const std::string &url, const char *auth, unsigned long long zipSize, ZipMember *out)
{
    const unsigned long TAIL = 65536;
    unsigned char *tail = (unsigned char *)malloc(TAIL + 4096);
    if (tail == NULL)
        return false;

    unsigned long long start = zipSize > TAIL ? zipSize - TAIL : 0;
    char range[96];
    _snprintf(range, sizeof(range), "Range: bytes=%I64u-%I64u\r\n", start, zipSize - 1);
    range[sizeof(range) - 1] = '\0';

    std::string u = url;
    unsigned long long len = 0;
    int status = Get(u, auth, range, (char *)tail, TAIL + 4095, &len);
    if ((status != 206 && status != 200) || len < 22)
    {
        dprintf("[game] zip directory: HTTP %d, %I64u bytes\n", status, len);
        free(tail);
        return false;
    }

    long eocd = -1;
    for (long i = (long)len - 22; i >= 0; --i)
    {
        if (LE32(tail + i) == 0x06054B50)
        {
            eocd = i;
            break;
        }
    }

    bool ok = false;
    if (eocd >= 0)
    {
        unsigned long long cdOffset = LE32(tail + eocd + 16);
        if (eocd >= 20 && LE32(tail + eocd - 20) == 0x07064B50)
        {
            unsigned long long z64 = LE64(tail + eocd - 20 + 8);
            if (z64 >= start && z64 - start + 56 <= len && LE32(tail + (z64 - start)) == 0x06064B50)
                cdOffset = LE64(tail + (z64 - start) + 48);
        }

        if (cdOffset >= start && cdOffset - start + 46 <= len && LE32(tail + (cdOffset - start)) == 0x02014B50)
        {
            const unsigned char *cd = tail + (cdOffset - start);
            out->method = LE16(cd + 10);
            out->crc = LE32(cd + 16);
            out->packed = LE32(cd + 20);
            out->unpacked = LE32(cd + 24);
            out->localHeader = LE32(cd + 42);
            unsigned short nameLen = LE16(cd + 28), extraLen = LE16(cd + 30);

            unsigned short copy = nameLen < sizeof(out->name) - 1 ? nameLen : (unsigned short)(sizeof(out->name) - 1);
            memcpy(out->name, cd + 46, copy);
            out->name[copy] = '\0';

            const unsigned char *extra = cd + 46 + nameLen, *extraEnd = extra + extraLen;
            while (extra + 4 <= extraEnd)
            {
                unsigned short id = LE16(extra), size = LE16(extra + 2);
                if (id == 0x0001)
                {
                    const unsigned char *f = extra + 4;
                    if (out->unpacked == 0xFFFFFFFFUL) { out->unpacked = LE64(f); f += 8; }
                    if (out->packed == 0xFFFFFFFFUL) { out->packed = LE64(f); f += 8; }
                    if (out->localHeader == 0xFFFFFFFFUL) { out->localHeader = LE64(f); f += 8; }
                }
                extra += 4 + size;
            }
            ok = true;
        }
    }

    if (!ok)
        dprintf("[game] the zip's directory couldn't be read from its last 64KB\n");
    free(tail);
    return ok;
}

// ---------------------------------------------------------------------------
// Downloading, two pieces at a time
// ---------------------------------------------------------------------------

static std::string HostOf(const std::string &url); // with the server ranking, below

// A connection that averages less than this over a window is dropped and its
// piece resumed - a server that trickles never trips the HTTP client's
// timeout, which waits for nothing at all to arrive.
#define SLOW_WINDOW_MS     20000
#define SLOW_BYTES_PER_SEC (512ULL * 1024)
#define RESUME_LOG_MS      10000 // how often a resumed piece reports itself

// How long a connection may receive nothing at all before it's dropped and
// resumed - the slow check can't see a stall, as nothing arrives to check.
// archive.org stops sending on both connections at once about once a
// minute; at the TLS layer's usual two minutes, five of those cost ten
// minutes of a Naruto download, and at 20 seconds thirty cost eleven of its
// 32. A range of a zip starts in a second or two, so this can be short.
#define STALL_TIMEOUT_MS   10000

// Slow drops and failed retries on one server before moving to the next.
// Cut-offs that resume don't count: every server cuts both connections every
// few minutes, and moving for those would only find slower ones.
#define STRIKES_TO_SWITCH  2
#define MAX_SERVERS        5

struct Download
{
    std::string servers[MAX_SERVERS]; // fastest first, redirects already followed
    int serverCount;
    int serverAt;                     // the one in use
    int strikes;                      // against it
    const char *auth;
    ZipPieces *pieces;
    unsigned long long total; // bytes of the zip that are needed
    unsigned long pieceCount;
    unsigned long nextPiece;
    bool *done;
    unsigned long long doneBytes;
    unsigned long long inFlight[CONNECTIONS];
    unsigned long long resumedAt[CONNECTIONS]; // what a resumed piece already had
    DWORD threadIds[CONNECTIONS];

    // Each connection's request in hand, for the slow check and the log.
    unsigned long piece[CONNECTIONS];
    DWORD windowStart[CONNECTIONS];             // when the current window began
    unsigned long long windowFrom[CONNECTIONS]; // the request's bytes then
    DWORD lastLog[CONNECTIONS];
    bool tooSlow[CONNECTIONS];                  // dropped by the slow check
    bool failed;
    int failStatus;
    const bool *cancel;
    HANDLE pieceReady;        // auto-reset: a piece finished, or the download stopped
};

static Download *g_download = NULL;

static bool Stopping(const Download *d)
{
    return d->failed || *d->cancel || g_shutdown;
}

// The HTTP client's progress, for whichever connection is calling: the bytes
// for the Queue page, and the slow check - false drops the request.
static bool PieceProgress(unsigned long long done, unsigned long long, unsigned long long, unsigned long long)
{
    Download *d = g_download;
    if (d == NULL)
        return false;

    const DWORD now = GetTickCount();
    bool report = false, slow = false;
    int slot = -1;
    unsigned long piece = 0;
    unsigned long long total = 0;
    double rate = 0.0;
    std::string host;

    EnterCriticalSection(&g_lock);
    DWORD me = GetCurrentThreadId();
    for (int i = 0; i < CONNECTIONS; ++i)
    {
        if (d->threadIds[i] == me)
            slot = i;
    }
    bool keepGoing = !Stopping(d);
    if (slot >= 0)
    {
        d->inFlight[slot] = d->resumedAt[slot] + done;
        piece = d->piece[slot];
        total = d->inFlight[slot];

        const DWORD elapsed = now - d->windowStart[slot];
        if (elapsed >= SLOW_WINDOW_MS)
        {
            const unsigned long long got = done - d->windowFrom[slot];
            rate = got / 1048576.0 / (elapsed / 1000.0);
            if (got * 1000 / elapsed < SLOW_BYTES_PER_SEC)
            {
                d->tooSlow[slot] = true;
                keepGoing = false;
                slow = true;
            }
            d->windowStart[slot] = now;
            d->windowFrom[slot] = done;
        }
        if (!slow && d->resumedAt[slot] > 0 && now - d->lastLog[slot] >= RESUME_LOG_MS)
        {
            // Over the window so far, or the one just finished.
            d->lastLog[slot] = now;
            report = true;
            const DWORD since = now - d->windowStart[slot];
            if (rate == 0.0 && since > 0)
                rate = (done - d->windowFrom[slot]) / 1048576.0 / (since / 1000.0);
        }
        if (report || slow)
            host = HostOf(d->servers[d->serverAt]);
    }
    LeaveCriticalSection(&g_lock);

    if (slow)
        dprintf("[game] piece %lu on connection %d: %.2f MB/s from %s for %ds - dropping it to resume\n", piece,
                slot + 1, rate, host.c_str(), SLOW_WINDOW_MS / 1000);
    else if (report)
        dprintf("[game] piece %lu on connection %d (resumed, %s): %.1f MB in, %.2f MB/s\n", piece, slot + 1,
                host.c_str(), total / 1048576.0, rate);
    return keepGoing;
}

// A slow drop or a failed retry against the server in use - server, the one
// the request went to. Enough of them, and both connections move to the next
// server in the ranking, back round to the first after the last. A strike
// against a server already left behind doesn't count.
static void StrikeServer(Download *d, int server, const char *why)
{
    std::string from, to;
    EnterCriticalSection(&g_lock);
    if (server == d->serverAt && ++d->strikes >= STRIKES_TO_SWITCH && d->serverCount > 1)
    {
        from = HostOf(d->servers[d->serverAt]);
        d->serverAt = (d->serverAt + 1) % d->serverCount;
        d->strikes = 0;
        to = HostOf(d->servers[d->serverAt]);
    }
    LeaveCriticalSection(&g_lock);

    if (!to.empty())
        dprintf("[game] %s on %s again - moving to %s\n", why, from.c_str(), to.c_str());
}

// A file's size, or 0.
static unsigned long long FileSize(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA attrs;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &attrs))
        return 0;
    return ((unsigned long long)attrs.nFileSizeHigh << 32) | attrs.nFileSizeLow;
}

// Adds one file's bytes to the end of another.
static bool AppendFile(const char *path, const char *more)
{
    FILE *in = fopen(more, "rb");
    if (in == NULL)
        return false;
    FILE *out = fopen(path, "ab");
    if (out == NULL)
    {
        fclose(in);
        return false;
    }

    static const size_t CHUNK = 256 * 1024;
    char *buffer = (char *)malloc(CHUNK);
    bool ok = (buffer != NULL);
    while (ok)
    {
        size_t n = fread(buffer, 1, CHUNK, in);
        if (n == 0)
            break;
        ok = (fwrite(buffer, 1, n, out) == n);
    }
    ok = ok && !ferror(in);
    free(buffer);
    fclose(in);
    if (fclose(out) != 0)
        ok = false;
    return ok;
}

struct ConnectionArgs
{
    Download *d;
    int slot;
};

static DWORD WINAPI ConnectionEntry(LPVOID param)
{
    ConnectionArgs *args = (ConnectionArgs *)param;
    Download *d = args->d;
    const int slot = args->slot;

    XboxTLS_SetThreadIoTimeout(STALL_TIMEOUT_MS);

    for (;;)
    {
        EnterCriticalSection(&g_lock);
        if (Stopping(d) || d->nextPiece >= d->pieceCount)
        {
            LeaveCriticalSection(&g_lock);
            break;
        }
        unsigned long index = d->nextPiece++;
        LeaveCriticalSection(&g_lock);

        unsigned long long from = (unsigned long long)index * PIECE_SIZE;
        unsigned long long to = from + PIECE_SIZE;
        if (to > d->total)
            to = d->total;
        unsigned long long length = to - from;

        char finalPath[320], tempPath[330], restPath[330];
        d->pieces->PiecePath(index, finalPath, sizeof(finalPath));
        _snprintf(tempPath, sizeof(tempPath), "%s.tmp", finalPath);
        tempPath[sizeof(tempPath) - 1] = '\0';
        _snprintf(restPath, sizeof(restPath), "%s.rst", finalPath);
        restPath[sizeof(restPath) - 1] = '\0';

        // A cut-off piece resumes where it stopped: the rest goes to a second
        // file, appended to the first - the HTTP client only writes files
        // from the start. Both connections stall together every few minutes,
        // often well into a piece, and are cut off after STALL_TIMEOUT_MS.
        bool got = false;
        int status = 0;
        unsigned long long have = 0; // bytes of the piece already in tempPath
        DWORD started = GetTickCount();
        for (int attempt = 0; attempt < PIECE_ATTEMPTS && !got && !Stopping(d); ++attempt)
        {
            if (attempt > 0)
            {
                if (have > 0)
                    dprintf("[game] piece %lu: cut off after %I64u of %I64u bytes, resuming (%d of %d)\n",
                            index, have, length, attempt + 1, PIECE_ATTEMPTS);
                else
                    dprintf("[game] piece %lu: HTTP %d, trying again (%d of %d)\n", index, status, attempt + 1, PIECE_ATTEMPTS);
                Sleep(2000 * attempt);
                started = GetTickCount();
            }

            const unsigned long long resumeFrom = have;
            char headers[AUTH_MAX + 96];
            _snprintf(headers, sizeof(headers), "%sRange: bytes=%I64u-%I64u\r\n", d->auth, from + resumeFrom, to - 1);
            headers[sizeof(headers) - 1] = '\0';

            // The server in use now - it may have changed since the last try.
            std::string url;
            int server = 0;
            EnterCriticalSection(&g_lock);
            server = d->serverAt;
            url = d->servers[server];
            d->resumedAt[slot] = resumeFrom;
            d->inFlight[slot] = resumeFrom;
            d->piece[slot] = index;
            d->windowStart[slot] = GetTickCount();
            d->windowFrom[slot] = 0;
            d->lastLog[slot] = d->windowStart[slot];
            d->tooSlow[slot] = false;
            LeaveCriticalSection(&g_lock);

            const char *target = (resumeFrom > 0) ? restPath : tempPath;
            unsigned long long size = 0;
            status = httpRequestHTTPS(url, HTTP_GET, NULL, headers, target, NULL, &size, true, NULL, 0,
                                      QuietPrint, length - resumeFrom, PieceProgress);

            // Dropped by the slow check: what came is good, as if cut off.
            EnterCriticalSection(&g_lock);
            const bool dropped = d->tooSlow[slot] && !Stopping(d);
            d->tooSlow[slot] = false;
            LeaveCriticalSection(&g_lock);
            if (dropped && status == HTTP_STATUS_CANCELLED)
            {
                status = 206;
                StrikeServer(d, server, "Too slow");
            }

            // Whatever came of the rest, as long as it's the range asked for.
            bool spoilt = false;
            if (resumeFrom > 0)
            {
                if (status == 206 && !AppendFile(tempPath, restPath))
                    spoilt = true;
                DeleteFileA(restPath);
            }
            unsigned long long onDisk = FileSize(tempPath);

            if (status == 206 && onDisk == length && MoveFileExA(tempPath, finalPath, MOVEFILE_REPLACE_EXISTING))
            {
                got = true;
                DWORD ms = GetTickCount() - started;
                double mb = (length - resumeFrom) / 1048576.0;
                dprintf("[game] piece %lu on connection %d: %.1f MB in %.1fs, %.2f MB/s (%s)\n", index, slot + 1,
                        mb, ms / 1000.0, ms > 0 ? mb / (ms / 1000.0) : 0.0, HostOf(url).c_str());
            }
            else if (!spoilt && status == 206 && onDisk > 0 && onDisk < length)
            {
                have = onDisk;
            }
            else if (!spoilt && resumeFrom > 0 && status != 401 && status != 403 && onDisk == resumeFrom)
            {
                // The rest didn't come at all; what was there still is.
                if (!Stopping(d))
                    StrikeServer(d, server, "A retry failed");
            }
            else
            {
                have = 0;
                DeleteFileA(tempPath);
                if (status == 401 || status == 403)
                    break; // the keys - trying again won't help
                if (!Stopping(d))
                    StrikeServer(d, server, "A retry failed");
            }
        }
        if (!got)
            DeleteFileA(tempPath);

        EnterCriticalSection(&g_lock);
        d->resumedAt[slot] = 0;
        d->inFlight[slot] = 0;
        if (got)
        {
            d->done[index] = true;
            d->doneBytes += length;
        }
        else if (!Stopping(d))
        {
            d->failed = true;
            d->failStatus = status;
        }
        LeaveCriticalSection(&g_lock);
        SetEvent(d->pieceReady);
    }

    SetEvent(d->pieceReady);
    return 0;
}

// ---------------------------------------------------------------------------
// One job
// ---------------------------------------------------------------------------

// Under the lock.
static void Finish(GameJob *job, QueueOutcome outcome, const char *text, const char *detail, bool notify)
{
    QueueJobSnapshot &s = job->snap;
    s.state = QUEUE_FINISHED;
    s.outcome = outcome;
    s.notify = notify;
    _snprintf(s.resultText, sizeof(s.resultText), "%s", text);
    s.resultText[sizeof(s.resultText) - 1] = '\0';
    _snprintf(s.resultDetail, sizeof(s.resultDetail), "%s", detail != NULL ? detail : "");
    s.resultDetail[sizeof(s.resultDetail) - 1] = '\0';
}

static void SetPhase(GameJob *job, const char *phase, unsigned long long done, unsigned long long total,
                     unsigned long long bytesPerSec)
{
    EnterCriticalSection(&g_lock);
    QueueJobSnapshot &s = job->snap;
    _snprintf(s.phase, sizeof(s.phase), "%s", phase);
    s.phase[sizeof(s.phase) - 1] = '\0';
    s.bytesDone = done;
    s.bytesTotal = total;
    s.bytesPerSec = bytesPerSec;
    s.secondsLeft = (bytesPerSec > 0 && total > done) ? (total - done) / bytesPerSec : 0;
    s.fraction = (total > 0) ? (float)((double)done / (double)total) : -1.0f;
    LeaveCriticalSection(&g_lock);
}

struct ConvertProgress
{
    GameJob *job;
    DWORD started;
};

static bool ConvertProgressFn(unsigned long long done, unsigned long long total, void *context)
{
    ConvertProgress *p = (ConvertProgress *)context;
    DWORD ms = GetTickCount() - p->started;
    SetPhase(p->job, "Installing", done, total, ms > 3000 ? done * 1000 / ms : 0);

    EnterCriticalSection(&g_lock);
    bool keepGoing = !p->job->cancel && !g_shutdown;
    LeaveCriticalSection(&g_lock);
    return keepGoing;
}

static void FinishLocked(GameJob *job, QueueOutcome outcome, const char *text, const char *detail, bool notify)
{
    EnterCriticalSection(&g_lock);
    Finish(job, outcome, text, detail, notify);
    LeaveCriticalSection(&g_lock);
}

static bool Cancelled(GameJob *job)
{
    EnterCriticalSection(&g_lock);
    bool c = job->cancel || g_shutdown;
    LeaveCriticalSection(&g_lock);
    return c;
}

// The host part of an https:// URL.
static std::string HostOf(const std::string &url)
{
    size_t start = url.find("://");
    start = (start == std::string::npos) ? 0 : start + 3;
    size_t end = url.find('/', start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// The "result" of one of archive.org's metadata endpoints: a string, or the
// strings of an array. Small replies - the endpoints are per field.
static int MetadataStrings(const char *item, const char *field, std::string *out, int maxOut)
{
    std::string u = std::string("https://archive.org/metadata/") + item + "/" + field;
    char reply[4096];
    unsigned long long len = 0;
    if (Get(u, NULL, NULL, reply, sizeof(reply) - 1, &len) != 200)
        return 0;
    reply[len < sizeof(reply) ? (size_t)len : sizeof(reply) - 1] = '\0';

    const char *p = strstr(reply, "\"result\"");
    if (p == NULL)
        return 0;
    p += 8;

    int n = 0;
    while (n < maxOut)
    {
        const char *open = strchr(p, '"');
        if (open == NULL)
            break;
        const char *close = strchr(open + 1, '"');
        if (close == NULL)
            break;
        out[n++] = std::string(open + 1, close - open - 1);
        p = close + 1;
    }
    return n;
}

// archive.org's redirect picks a server, and not always a fast one - a run
// sent to a European copy got half what its US server gives. So: a few MB
// from each server that holds the item and from the one the redirect chose,
// fastest first, into out - the download starts on the first and moves down
// the list if a server goes bad. At least the redirect's. Returns how many.
static int RankServers(GameJob *job, const std::string &redirected, const char *item, const char *encodedZip,
                       const char *auth, unsigned long long zipSize, std::string *out, int maxOut)
{
    const unsigned long long PROBE = 4ULL * 1024 * 1024;
    out[0] = redirected;
    if (zipSize < PROBE * 2)
        return 1;

    std::string dir;
    std::string servers[4];
    int serverCount = 0;
    if (MetadataStrings(item, "dir", &dir, 1) == 1 && !dir.empty())
        serverCount = MetadataStrings(item, "workable_servers", servers, 4);

    std::string candidates[5];
    int count = 0;
    candidates[count++] = redirected;
    const std::string redirectedHost = HostOf(redirected);
    for (int i = 0; i < serverCount; ++i)
    {
        if (servers[i] != redirectedHost)
            candidates[count++] = std::string("https://") + servers[i] + dir + "/" + encodedZip;
    }
    if (count == 1)
    {
        dprintf("[game] no server list from archive.org; using %s\n", redirectedHost.c_str());
        return 1;
    }

    char *buffer = (char *)malloc((size_t)PROBE + 4096);
    if (buffer == NULL)
        return 1;

    // Past the zip's first piece, so a server's cache of the start doesn't
    // flatter it.
    char range[96];
    _snprintf(range, sizeof(range), "Range: bytes=%I64u-%I64u\r\n", PROBE, PROBE * 2 - 1);
    range[sizeof(range) - 1] = '\0';

    double rates[5];
    int ranked = 0;
    for (int i = 0; i < count && !Cancelled(job); ++i)
    {
        std::string u = candidates[i];
        unsigned long long len = 0;
        DWORD started = GetTickCount();
        int status = Get(u, auth, range, buffer, PROBE + 4095, &len);
        DWORD ms = GetTickCount() - started;

        if (status != 206 || len != PROBE)
        {
            dprintf("[game] server %s: HTTP %d, %I64u bytes - skipped\n", HostOf(candidates[i]).c_str(), status, len);
            continue;
        }
        double rate = PROBE / 1048576.0 / ((ms > 0 ? ms : 1) / 1000.0);
        dprintf("[game] server %s: %.2f MB/s\n", HostOf(u).c_str(), rate);

        // Into place by speed; u is where it redirected to, if anywhere.
        int at = ranked;
        while (at > 0 && rates[at - 1] < rate)
        {
            rates[at] = rates[at - 1];
            candidates[at] = candidates[at - 1];
            at--;
        }
        rates[at] = rate;
        candidates[at] = u;
        ranked++;
    }
    free(buffer);

    if (ranked == 0)
        return 1;
    int n = 0;
    for (; n < ranked && n < maxOut; ++n)
        out[n] = candidates[n];
    dprintf("[game] downloading from %s\n", HostOf(out[0]).c_str());
    return n;
}

static void RunJob(GameJob *job, const char *gamesPath)
{
    const GameRequest &req = job->request;
    const char *auth = job->auth;

    SetPhase(job, "Starting", 0, 0, 0);

    char encodedZip[700];
    if (!UrlEncodeFormValue(req.zipName, encodedZip, sizeof(encodedZip)))
    {
        FinishLocked(job, QUEUE_OUTCOME_FAILED, "Couldn't install", "The file name is too long.", true);
        return;
    }
    std::string url = std::string("https://archive.org/download/") + req.item + "/" + encodedZip;

    // The datanode, once, so every request after goes straight there.
    char small[4096];
    unsigned long long len = 0;
    int status = Get(url, auth, "Range: bytes=0-1023\r\n", small, sizeof(small) - 1, &len);
    if (status == 401 || status == 403)
    {
        FinishLocked(job, QUEUE_OUTCOME_KEYS_REJECTED, "Keys not accepted",
                     "archive.org refused the keys - check them in Settings.", true);
        return;
    }
    if (status != 206 || len < 30)
    {
        char detail[96];
        _snprintf(detail, sizeof(detail), "archive.org answered HTTP %d.", status);
        detail[sizeof(detail) - 1] = '\0';
        FinishLocked(job, QUEUE_OUTCOME_FAILED, "Couldn't reach archive.org", detail, true);
        return;
    }

    SetPhase(job, "Finding the fastest server", 0, 0, 0);
    std::string servers[MAX_SERVERS];
    const int serverCount = RankServers(job, url, req.item, encodedZip, auth, req.zipSize, servers, MAX_SERVERS);
    url = servers[0];
    if (Cancelled(job))
    {
        FinishLocked(job, QUEUE_OUTCOME_CANCELLED, "Stopped", "Nothing was installed.", false);
        return;
    }

    ZipMember member;
    memset(&member, 0, sizeof(member));
    if (!ReadZipDirectory(url, auth, req.zipSize, &member) || member.unpacked == 0)
    {
        FinishLocked(job, QUEUE_OUTCOME_FAILED, "Couldn't install", "The zip's directory couldn't be read.", true);
        return;
    }
    if (member.method != 8)
    {
        dprintf("[game] %s: compression method %u isn't deflate\n", req.zipName, (unsigned)member.method);
        FinishLocked(job, QUEUE_OUTCOME_FAILED, "Couldn't install", "The zip isn't compressed the usual way.", true);
        return;
    }

    // The member's local header - where its compressed data starts. It's at
    // the zip's start in practice, already in the bytes just fetched.
    unsigned char local[1024];
    if (member.localHeader == 0)
    {
        memcpy(local, small, 1024);
    }
    else
    {
        char range[96];
        _snprintf(range, sizeof(range), "Range: bytes=%I64u-%I64u\r\n", member.localHeader, member.localHeader + 1023);
        range[sizeof(range) - 1] = '\0';
        std::string u = url;
        Get(u, auth, range, (char *)local, sizeof(local), &len);
    }
    if (LE32(local) != 0x04034B50)
    {
        FinishLocked(job, QUEUE_OUTCOME_FAILED, "Couldn't install", "The zip's file header is missing.", true);
        return;
    }
    const unsigned long long dataStart = member.localHeader + 30 + LE16(local + 26) + LE16(local + 28);
    const unsigned long long needed = dataStart + member.packed;

    dprintf("[game] %s: \"%s\", %I64u packed from %I64u, %I64u unpacked, crc %08lX\n", req.zipName, member.name,
            member.packed, dataStart, member.unpacked, member.crc);

    // Room for the zip and the game. The game is at most the image's size;
    // checked again, exactly, once the image says how big the game is.
    unsigned long long freeSpace = 0;
    if (DriveFreeSpace(gamesPath, &freeSpace) && freeSpace < needed + member.unpacked + SPACE_MARGIN)
    {
        char needText[64] = "", freeText[64] = "", detail[160];
        FormatBytes(needed + member.unpacked, needText, sizeof(needText));
        FormatBytes(freeSpace, freeText, sizeof(freeText));
        _snprintf(detail, sizeof(detail), "It needs up to %s while it installs, and %s is free.", needText, freeText);
        detail[sizeof(detail) - 1] = '\0';
        FinishLocked(job, QUEUE_OUTCOME_NO_SPACE, "Not enough space", detail, true);
        return;
    }

    char root[300], dir[320];
    StagingRoot(gamesPath, root, sizeof(root));
    CreateDirectoryA(root, NULL);
    _snprintf(dir, sizeof(dir), "%s\\G%d", root, job->snap.id);
    dir[sizeof(dir) - 1] = '\0';
    RemoveFolder(dir);
    if (!CreateDirectoryA(dir, NULL))
    {
        FinishLocked(job, QUEUE_OUTCOME_FAILED, "Couldn't install", "The staging folder couldn't be made.", true);
        return;
    }

    // --- Download, and index as the pieces land -----------------------------
    ZipPieces pieces(dir, PIECE_SIZE);
    Download d;
    for (int i = 0; i < serverCount; ++i)
        d.servers[i] = servers[i];
    d.serverCount = serverCount;
    d.serverAt = 0;
    d.strikes = 0;
    d.auth = auth;
    d.pieces = &pieces;
    d.total = needed;
    d.pieceCount = (unsigned long)((needed + PIECE_SIZE - 1) / PIECE_SIZE);
    d.nextPiece = 0;
    d.done = (bool *)calloc(d.pieceCount, sizeof(bool));
    d.doneBytes = 0;
    d.failed = false;
    d.failStatus = 0;
    d.cancel = &job->cancel;
    d.pieceReady = CreateEvent(NULL, FALSE, FALSE, NULL);
    for (int i = 0; i < CONNECTIONS; ++i)
    {
        d.inFlight[i] = 0;
        d.resumedAt[i] = 0;
        d.threadIds[i] = 0;
        d.piece[i] = 0;
        d.windowStart[i] = 0;
        d.windowFrom[i] = 0;
        d.lastLog[i] = 0;
        d.tooSlow[i] = false;
    }

    InflateIndexer *indexer = new InflateIndexer(member.unpacked);
    unsigned char *chunk = (unsigned char *)malloc(1024 * 1024);

    if (d.done == NULL || d.pieceReady == NULL || !indexer->Ok() || chunk == NULL)
    {
        free(d.done);
        if (d.pieceReady != NULL)
            CloseHandle(d.pieceReady);
        delete indexer;
        free(chunk);
        RemoveFolder(dir);
        FinishLocked(job, QUEUE_OUTCOME_FAILED, "Couldn't install", "Out of memory.", true);
        return;
    }

    g_download = &d;

    ConnectionArgs args[CONNECTIONS];
    HANDLE threads[CONNECTIONS];
    int started = 0;
    for (int i = 0; i < CONNECTIONS; ++i)
    {
        args[i].d = &d;
        args[i].slot = i;
        threads[i] = CreateThread(NULL, 256 * 1024, ConnectionEntry, &args[i], CREATE_SUSPENDED, &d.threadIds[i]);
        if (threads[i] == NULL)
            break;
#ifdef _XBOX
        // TLS decryption is most of a connection's work, so each gets a core
        // of its own: hardware thread 1 (core 0, beside the UI; the disc
        // worker there mostly waits) and 4 (core 2, beside the cover worker).
        // Core 1 is this thread's, inflating the pieces as they arrive.
        XSetThreadProcessor(threads[i], i == 0 ? 1 : 4);
#endif
        ResumeThread(threads[i]);
        started++;
        if (i + 1 < CONNECTIONS)
            Sleep(CONNECTION_STAGGER_MS);
    }

    const DWORD downloadStart = GetTickCount();
    bool indexOk = (started > 0);

    for (unsigned long index = 0; indexOk && index < d.pieceCount; ++index)
    {
        // Wait for this piece, reporting progress meanwhile.
        for (;;)
        {
            EnterCriticalSection(&g_lock);
            bool ready = d.done[index];
            bool stop = Stopping(&d);
            unsigned long long bytes = d.doneBytes;
            for (int i = 0; i < CONNECTIONS; ++i)
                bytes += d.inFlight[i];
            LeaveCriticalSection(&g_lock);

            DWORD ms = GetTickCount() - downloadStart;
            SetPhase(job, "Downloading", bytes, needed, ms > 3000 ? bytes * 1000 / ms : 0);

            if (ready || stop)
            {
                indexOk = ready && !stop;
                break;
            }
            WaitForSingleObject(d.pieceReady, 500);
        }
        if (!indexOk)
            break;

        // Feed it to the indexer - only the member's compressed bytes.
        unsigned long long from = (unsigned long long)index * PIECE_SIZE;
        unsigned long long to = from + PIECE_SIZE;
        if (to > needed)
            to = needed;
        if (from < dataStart)
            from = dataStart;

        while (from < to)
        {
            unsigned long take = (to - from > 1024 * 1024) ? 1024 * 1024 : (unsigned long)(to - from);
            if (!pieces.Read(from, chunk, take) || !indexer->Feed(chunk, take))
            {
                dprintf("[game] indexing failed at %I64u\n", from);
                indexOk = false;
                break;
            }
            from += take;
        }
    }

    // Stop the connections if anything went wrong, and wait for them.
    if (!indexOk)
    {
        EnterCriticalSection(&g_lock);
        if (!d.failed)
            d.failed = true;
        LeaveCriticalSection(&g_lock);
    }
    if (started > 0)
        WaitForMultipleObjects(started, threads, TRUE, INFINITE);
    for (int i = 0; i < started; ++i)
        CloseHandle(threads[i]);
    g_download = NULL;
    CloseHandle(d.pieceReady);
    free(d.done);
    free(chunk);

    DWORD downloadSeconds = (GetTickCount() - downloadStart) / 1000;
    dprintf("[game] downloaded %I64u MB in %lu:%02lu\n", d.doneBytes / (1024 * 1024),
            (unsigned long)(downloadSeconds / 60), (unsigned long)(downloadSeconds % 60));

    if (!indexOk || !indexer->Finished() || indexer->Out() != member.unpacked || indexer->Crc() != member.crc)
    {
        bool cancelled = Cancelled(job);
        int failStatus = d.failStatus;
        if (indexOk && !cancelled)
            dprintf("[game] the image didn't check out: finished %d, %I64u of %I64u bytes, crc %08lX (want %08lX)\n",
                    indexer->Finished() ? 1 : 0, indexer->Out(), member.unpacked, indexer->Crc(), member.crc);
        delete indexer;
        RemoveFolder(dir);

        if (cancelled)
            FinishLocked(job, QUEUE_OUTCOME_CANCELLED, "Stopped", "Nothing was installed.", false);
        else if (failStatus == 401 || failStatus == 403)
            FinishLocked(job, QUEUE_OUTCOME_KEYS_REJECTED, "Keys not accepted",
                         "archive.org refused the keys - check them in Settings.", true);
        else if (indexOk)
            FinishLocked(job, QUEUE_OUTCOME_FAILED, "Download was damaged",
                         "The game's data didn't match its checksum. Try again.", true);
        else
            FinishLocked(job, QUEUE_OUTCOME_FAILED, "Download failed",
                         "archive.org stopped answering. Try again later.", true);
        return;
    }

    int pointCount = 0;
    InflatePoint *points = indexer->TakePoints(&pointCount);
    delete indexer;
    dprintf("[game] image checks out: %I64u bytes, crc %08lX, %d restart points\n", member.unpacked, member.crc, pointCount);

    // --- Install --------------------------------------------------------------
    InflateSource image(&pieces, dataStart, member.packed, member.unpacked, points, pointCount);

    GodImageInfo info;
    GodResult result = GodInspect(&image, &info);
    if (result != GOD_OK)
    {
        free(points);
        RemoveFolder(dir);
        FinishLocked(job, QUEUE_OUTCOME_FAILED, "Couldn't install", GodResultText(result), true);
        return;
    }

    dprintf("[game] %s image: title %08lX, media %08lX, %I64u bytes to install\n", info.imageType,
            info.title.titleId, info.title.mediaId, info.outputSize);
    EnterCriticalSection(&g_lock);
    job->snap.titleId = info.title.titleId;
    LeaveCriticalSection(&g_lock);

    if (DriveFreeSpace(gamesPath, &freeSpace) && freeSpace < info.outputSize + SPACE_MARGIN)
    {
        char needText[64] = "", freeText[64] = "", detail[160];
        FormatBytes(info.outputSize, needText, sizeof(needText));
        FormatBytes(freeSpace, freeText, sizeof(freeText));
        _snprintf(detail, sizeof(detail), "It needs %s, and %s is free.", needText, freeText);
        detail[sizeof(detail) - 1] = '\0';
        free(points);
        RemoveFolder(dir);
        FinishLocked(job, QUEUE_OUTCOME_NO_SPACE, "Not enough space", detail, true);
        return;
    }

    ConvertProgress progress;
    progress.job = job;
    progress.started = GetTickCount();
    SetPhase(job, "Installing", 0, info.usedSize, 0);

    // The game's icon for the package, so the dashboard and Aurora show it.
    // Without one the install goes ahead, with the dashboard's placeholder.
    static unsigned char icon[GOD_ICON_MAX];
    unsigned long iconSize = 0;
    if (!FetchTitleIcon(info.title.titleId, icon, sizeof(icon), &iconSize))
        iconSize = 0;

    WriteInstallMarker(gamesPath, info, req.name);

    char packagePath[600] = "";
    GodTimings timings;
    memset(&timings, 0, sizeof(timings));
    {
        // Decompressing runs on the read-ahead's thread while this one
        // hashes and writes. Scoped so it has finished before the image goes.
        ReadAheadSource ahead(&image, 1024 * 1024, 8);
        result = GodConvert(&ahead, info, gamesPath, req.name, iconSize > 0 ? icon : NULL, iconSize,
                            ConvertProgressFn, &progress, packagePath, sizeof(packagePath), &timings);
    }
    ClearInstallMarker();

    DWORD seconds = (GetTickCount() - progress.started) / 1000;
    dprintf("[game] install %s after %lu:%02lu (%lu restarts): %s\n", GodResultText(result),
            (unsigned long)(seconds / 60), (unsigned long)(seconds % 60), image.Restarts(), packagePath);
    if (seconds > 0)
        dprintf("[game] %I64u MB at %.2f MB/s - waiting for data %.0fs, hashing %.0fs, writing %.0fs\n",
                info.usedSize / (1024 * 1024), (double)info.usedSize / (1024.0 * 1024.0) / (double)seconds,
                timings.readMs / 1000.0, timings.hashMs / 1000.0, timings.writeMs / 1000.0);

    free(points);
    RemoveFolder(dir);

    if (result == GOD_OK)
    {
        // Noted before it's reported, so the Store never sees it finished
        // but not installed.
        RecordInstalledZip(req.zipName, info.title.titleId, info.title.mediaId);
        FinishLocked(job, QUEUE_OUTCOME_INSTALLED, "Installed",
                     "Play it from the dashboard or Aurora.", true);
    }
    else if (result == GOD_CANCELLED)
        FinishLocked(job, QUEUE_OUTCOME_CANCELLED, "Stopped", "Nothing was installed.", false);
    else if (result == GOD_WRITE_FAILED)
        FinishLocked(job, QUEUE_OUTCOME_FAILED, "Couldn't write the game",
                     "Check the drive the games folder is on, then try again.", true);
    else
        FinishLocked(job, QUEUE_OUTCOME_FAILED, "Couldn't install", GodResultText(result), true);
}

// ---------------------------------------------------------------------------
// The worker
// ---------------------------------------------------------------------------

static DWORD WINAPI InstallerEntry(LPVOID)
{
    for (;;)
    {
        GameJob *job = NULL;
        char gamesPath[512];

        EnterCriticalSection(&g_lock);
        if (g_shutdown)
        {
            LeaveCriticalSection(&g_lock);
            break;
        }
        for (int slot; (slot = NextJob(QUEUE_WAITING, 0, false)) >= 0;)
        {
            job = &g_jobs[slot];
            if (!job->cancel)
            {
                job->snap.state = QUEUE_ACTIVE;
                break;
            }
            Finish(job, QUEUE_OUTCOME_CANCELLED, "Stopped", "Nothing was installed.", false);
            job = NULL;
        }
        memcpy(gamesPath, g_gamesPath, sizeof(gamesPath));
        LeaveCriticalSection(&g_lock);

        if (job == NULL)
        {
            WaitForSingleObject(g_wake, INFINITE);
            continue;
        }

        dprintf("[game] installing \"%s\" from %s/%s\n", job->request.name, job->request.item, job->request.zipName);
        RunJob(job, gamesPath);
    }
    return 0;
}

bool StartGameInstaller(const char *gamesPath)
{
    if (g_running)
        return true;

    InitializeCriticalSection(&g_lock);
    SetGameInstallerGamesPath(gamesPath); // reads what's installed, too

    // A session that ended mid-download leaves its pieces behind.
    char root[300];
    StagingRoot(gamesPath, root, sizeof(root));
    RemoveFolder(root);

    g_wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (g_wake == NULL)
        return false;

    g_thread = CreateThread(NULL, 256 * 1024, InstallerEntry, NULL, CREATE_SUSPENDED, NULL);
    if (g_thread == NULL)
    {
        CloseHandle(g_wake);
        g_wake = NULL;
        return false;
    }
#ifdef _XBOX
    // Hashing and writing the game; the decompressing is on the read-ahead's
    // thread 2. Core 1's second thread, beside the search worker.
    XSetThreadProcessor(g_thread, 3);
#endif
    g_running = true;
    ResumeThread(g_thread);
    dprintf("[game] game installer started\n");
    return true;
}

bool StopGameInstaller(DWORD timeoutMs)
{
    if (!g_running)
        return true;

    EnterCriticalSection(&g_lock);
    g_shutdown = true;
    for (int i = 0; i < MAX_GAME_JOBS; ++i)
        g_jobs[i].cancel = true;
    LeaveCriticalSection(&g_lock);
    SetEvent(g_wake);

    bool stopped = (WaitForSingleObject(g_thread, timeoutMs) == WAIT_OBJECT_0);
    if (!stopped)
    {
        dprintf("[game] still running after %lums - leaving anyway\n", timeoutMs);
        return false;
    }
    CloseHandle(g_thread);
    g_thread = NULL;
    g_running = false;
    return true;
}

void SetGameInstallerGamesPath(const char *gamesPath)
{
    if (g_running)
        EnterCriticalSection(&g_lock);
    strncpy(g_gamesPath, gamesPath != NULL ? gamesPath : "", sizeof(g_gamesPath) - 1);
    g_gamesPath[sizeof(g_gamesPath) - 1] = '\0';
    LoadInstalledZips(g_gamesPath); // against the folder they'd be in now
    if (g_running)
        LeaveCriticalSection(&g_lock);
}

GameEnqueueResult EnqueueGameInstall(const GameRequest &request, const char *authHeader)
{
    if (!g_running)
        return GAME_INSTALLER_UNAVAILABLE;

    GameEnqueueResult result = GAME_QUEUED;
    EnterCriticalSection(&g_lock);

    int freeSlot = -1, oldest = -1;
    for (int i = 0; i < MAX_GAME_JOBS; ++i)
    {
        const GameJob &j = g_jobs[i];
        if (!j.used)
        {
            if (freeSlot < 0)
                freeSlot = i;
            continue;
        }
        if (j.snap.state != QUEUE_FINISHED && strcmp(j.request.item, request.item) == 0 &&
            strcmp(j.request.zipName, request.zipName) == 0)
            result = GAME_ALREADY_QUEUED;
        if (j.snap.state == QUEUE_FINISHED && j.handedOver && (oldest < 0 || j.snap.id < g_jobs[oldest].snap.id))
            oldest = i;
    }

    // Full of finished jobs, the oldest one already handed over makes room.
    int slot = (freeSlot >= 0) ? freeSlot : oldest;
    if (result == GAME_QUEUED && slot < 0)
        result = GAME_QUEUE_FULL;

    if (result == GAME_QUEUED)
    {
        GameJob &job = g_jobs[slot];
        memset(&job, 0, sizeof(job));
        job.used = true;
        job.request = request;
        strncpy(job.auth, authHeader != NULL ? authHeader : "", sizeof(job.auth) - 1);
        job.snap.id = g_nextId++;
        job.snap.kind = QUEUE_JOB_GAME_INSTALL;
        job.snap.state = QUEUE_WAITING;
        job.snap.titleId = request.titleId;
        job.snap.fraction = -1.0f;
        _snprintf(job.snap.gameName, sizeof(job.snap.gameName), "%s", request.name);
        job.snap.gameName[sizeof(job.snap.gameName) - 1] = '\0';
        _snprintf(job.snap.title, sizeof(job.snap.title), "%s", request.zipName);
        job.snap.title[sizeof(job.snap.title) - 1] = '\0';
    }

    LeaveCriticalSection(&g_lock);
    if (result == GAME_QUEUED)
        SetEvent(g_wake);
    return result;
}

int SnapshotGameJobs(QueueJobSnapshot *out, int maxJobs)
{
    if (!g_running)
        return 0;

    int n = 0;
    EnterCriticalSection(&g_lock);
    for (int i = NextJob(QUEUE_ACTIVE, 0, false); i >= 0 && n < maxJobs; i = NextJob(QUEUE_ACTIVE, g_jobs[i].snap.id, false))
        out[n++] = g_jobs[i].snap;
    for (int i = NextJob(QUEUE_WAITING, 0, false); i >= 0 && n < maxJobs; i = NextJob(QUEUE_WAITING, g_jobs[i].snap.id, false))
        out[n++] = g_jobs[i].snap;
    for (int i = NextJob(QUEUE_FINISHED, 0x7FFFFFFF, true); i >= 0 && n < maxJobs; i = NextJob(QUEUE_FINISHED, g_jobs[i].snap.id, true))
        out[n++] = g_jobs[i].snap;
    LeaveCriticalSection(&g_lock);
    return n;
}

int PendingGameJobCount()
{
    if (!g_running)
        return 0;

    int n = 0;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_GAME_JOBS; ++i)
    {
        if (g_jobs[i].used && g_jobs[i].snap.state != QUEUE_FINISHED)
            n++;
    }
    LeaveCriticalSection(&g_lock);
    return n;
}

void CancelGameJob(int id)
{
    if (!g_running)
        return;

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_GAME_JOBS; ++i)
    {
        if (g_jobs[i].used && g_jobs[i].snap.id == id && g_jobs[i].snap.state != QUEUE_FINISHED)
            g_jobs[i].cancel = true;
    }
    LeaveCriticalSection(&g_lock);
    SetEvent(g_wake);
}

void RemoveGameJob(int id)
{
    if (!g_running)
        return;

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_GAME_JOBS; ++i)
    {
        // Only once main.cpp has seen it finish, so no popup goes missing.
        if (g_jobs[i].used && g_jobs[i].snap.id == id && g_jobs[i].snap.state == QUEUE_FINISHED &&
            g_jobs[i].handedOver)
            g_jobs[i].used = false;
    }
    LeaveCriticalSection(&g_lock);
}

bool TakeFinishedGameJob(QueueJobSnapshot *out)
{
    if (!g_running)
        return false;

    bool found = false;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_GAME_JOBS; ++i)
    {
        if (g_jobs[i].used && g_jobs[i].snap.state == QUEUE_FINISHED && !g_jobs[i].handedOver)
        {
            g_jobs[i].handedOver = true;
            *out = g_jobs[i].snap;
            found = true;
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    return found;
}
