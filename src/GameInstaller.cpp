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
#include "ServiceCheck.h"  // ReportConnectionRate: whose fault a slow one is
#include "parsing.h"       // UrlEncodeFormValue
#include "OutputConsole.h"
#include "RarHeaders.h"     // a game kept in a RAR
#include "RarUnpack.h"      // ...and unpacked here, if compressed
#include "QueueState.h"     // the jobs, kept between runs
#include "zlib/zlib.h"      // crc32, for checking one

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <string>

#define PIECE_SIZE        (32ULL * 1024 * 1024)
#define CONNECTIONS       2
#define CONNECTION_STAGGER_MS 1500 // four opened at the same instant all stalled once
// Tries in a row that gain no ground before a piece is given up on. A try
// that brought bytes - cut off, or dropped as slow - costs nothing: it is
// progress, not a failure, and counting it meant a slow line ran out of
// tries part way into a 32 MB piece and failed the whole download (issue
// #3). PIECE_TRIES caps the total, so a piece that crawls still ends.
#define PIECE_ATTEMPTS    6
#define PIECE_TRIES       200
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
    bool pause; // stop like a cancel, but keep the job and its pieces
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

bool GameZipInstalledAs(const char *zipName, unsigned long *outTitleId, unsigned long *outMediaId)
{
    if (!g_running || zipName == NULL)
        return false;

    bool found = false;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_installedCount && !found; ++i)
    {
        if (strcmp(g_installed[i].zip, zipName) == 0)
        {
            found = true;
            if (outTitleId != NULL)
                *outTitleId = g_installed[i].titleId;
            if (outMediaId != NULL)
                *outMediaId = g_installed[i].mediaId;
        }
    }
    LeaveCriticalSection(&g_lock);
    return found;
}

bool IsGameZipInstalled(const char *zipName)
{
    return GameZipInstalledAs(zipName, NULL, NULL);
}

void ForgetInstalledZip(const char *zipName)
{
    if (!g_running || zipName == NULL)
        return;

    // Out of the list, then the file written afresh from what's left - it's
    // a few lines, and this keeps it from growing with every reinstall.
    static InstalledZip kept[MAX_INSTALLED_ZIPS];
    int keptCount = 0;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_installedCount; ++i)
    {
        if (strcmp(g_installed[i].zip, zipName) != 0)
            g_installed[keptCount++] = g_installed[i];
    }
    g_installedCount = keptCount;
    memcpy(kept, g_installed, keptCount * sizeof(InstalledZip));
    LeaveCriticalSection(&g_lock);

    FILE *f = fopen(INSTALLED_LIST, "wb");
    if (f == NULL)
    {
        dprintf("[game] couldn't rewrite %s\n", INSTALLED_LIST);
        return;
    }
    for (int i = 0; i < keptCount; ++i)
        fprintf(f, "%08lX %08lX %s\r\n", kept[i].titleId, kept[i].mediaId, kept[i].zip);
    fclose(f);
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

// A file's size, or 0.
static unsigned long long FileSize(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA attrs;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &attrs))
        return 0;
    return ((unsigned long long)attrs.nFileSizeHigh << 32) | attrs.nFileSizeLow;
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
// The installs still to do, kept between runs - see QueueState.h
// ---------------------------------------------------------------------------

#define INSTALL_STATE_FILE "game:\\QueuedInstalls.txt"
#define INSTALL_STATE_MAX  (8 * 1024) // MAX_GAME_JOBS of a few hundred bytes

// Under the lock. The pieces on the drive are only half of carrying on: this
// is the half that says which game they belong to. Not written while the app
// is closing - closing cancels every job, and the file has to keep them.
static void SaveInstallStateLocked()
{
    if (!g_running || g_shutdown)
        return;

    char text[INSTALL_STATE_MAX];
    size_t at = 0;
    text[0] = '\0';
    AppendQueueStateText(text, sizeof(text), &at,
                         "# Omni360 - the game installs still to do when this was written\nformat=1\n");

    int jobs = 0;
    for (int i = 0; i < MAX_GAME_JOBS; ++i)
    {
        const GameJob &job = g_jobs[i];
        if (!job.used || job.snap.state == QUEUE_FINISHED)
            continue;

        AppendQueueStateText(text, sizeof(text), &at, "job=game\nitem=%s\nfile=%s\nsize=%I64u\nname=%s\ntitleid=%08lX\n",
                             job.request.item, job.request.zipName, job.request.zipSize, job.request.name,
                             job.request.titleId);

        // job.pause counts as paused even before the download has noticed:
        // the app closing in between must not restart it.
        if (job.snap.state == QUEUE_PAUSED || (job.pause && !job.cancel))
            AppendQueueStateText(text, sizeof(text), &at, "paused=1\n");

        jobs++;
    }

    if (jobs > 0)
        WriteQueueStateFile(INSTALL_STATE_FILE, text);
    else
        DeleteQueueStateFile(INSTALL_STATE_FILE);
}

// ---------------------------------------------------------------------------
// What a download leaves behind
// ---------------------------------------------------------------------------
//
// The pieces stay on the drive when the app closes part way through, so the
// next run carries on from them instead of fetching 7-9 GB again. That only
// works if a later run can find them and be sure of them, which is what the
// two things here are for.
//
// The folder is named from the download itself - the item and the file in it
// - rather than from the job's id, which starts again at GAME_JOB_ID_BASE
// every run and so named a folder nothing could ever look up again.
//
// Beside the pieces is a note saying which file they came from and how it was
// shaped. A piece is only good for the file it was cut from, and archive.org
// does replace files under the same name, so the pieces are used only when
// the note matches what the server says today, byte for byte; otherwise the
// folder is thrown away and the download starts over. The note is also what
// marks a folder as this scheme's: one without it is from before this and is
// swept at startup.

#define STAGING_NOTE "staged.txt"

// FATX allows 42 characters, which the item and file name together go well
// past, so the folder is named by their checksum.
static unsigned long StagingKey(const char *item, const char *zipName)
{
    unsigned long key = crc32(0L, Z_NULL, 0);
    key = crc32(key, (const Bytef *)item, (uInt)strlen(item) + 1); // the NUL keeps the two apart
    return crc32(key, (const Bytef *)zipName, (uInt)strlen(zipName));
}

static void StagingDirFor(const char *root, const GameRequest &req, char *out, size_t outSize)
{
    _snprintf(out, outSize, "%s\\G%08lX", root, StagingKey(req.item, req.zipName));
    out[outSize - 1] = '\0';
}

static void StagingNotePath(const char *dir, char *out, size_t outSize)
{
    _snprintf(out, outSize, "%s\\" STAGING_NOTE, dir);
    out[outSize - 1] = '\0';
}

// Everything the pieces depend on: the file they were cut from and where the
// image sits inside it. Compared whole, so anything that moves invalidates
// them.
static void StagingNoteText(const GameRequest &req, unsigned long long dataStart, unsigned long long packed,
                            unsigned long crc, char *out, size_t outSize)
{
    _snprintf(out, outSize, "item=%s\nzip=%s\nsize=%I64u\nstart=%I64u\npacked=%I64u\ncrc=%08lX\n",
              req.item, req.zipName, req.zipSize, dataStart, packed, crc);
    out[outSize - 1] = '\0';
}

#define STAGING_NOTE_MAX 768

static bool WriteStagingNote(const char *dir, const char *text)
{
    char path[600];
    StagingNotePath(dir, path, sizeof(path));

    FILE *f = fopen(path, "wb");
    if (f == NULL)
        return false;
    const size_t n = strlen(text);
    const bool written = (fwrite(text, 1, n, f) == n);
    return (fclose(f) == 0) && written;
}

// The note as it is on disk, or "" if there is none.
static void ReadStagingNote(const char *dir, char *out, size_t outSize)
{
    out[0] = '\0';

    char path[600];
    StagingNotePath(dir, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return;
    const size_t n = fread(out, 1, outSize - 1, f);
    fclose(f);
    out[n] = '\0';
}

// Which pieces of the download are already whole. A piece counts only at
// exactly the length it covers: a shorter one was cut off mid-transfer, and
// is fetched again rather than trusted.
static unsigned long long ScanStagedPieces(const ZipPieces &pieces, unsigned long pieceCount,
                                           unsigned long long needed, bool *done, unsigned long *outCount)
{
    unsigned long long bytes = 0;
    unsigned long count = 0;

    for (unsigned long index = 0; index < pieceCount; ++index)
    {
        unsigned long long from = (unsigned long long)index * PIECE_SIZE;
        unsigned long long length = (from + PIECE_SIZE > needed) ? needed - from : PIECE_SIZE;

        char path[320];
        pieces.PiecePath(index, path, sizeof(path));
        if (FileSize(path) != length)
            continue;

        done[index] = true;
        bytes += length;
        count++;
    }

    *outCount = count;
    return bytes;
}

// Throws a staged download away. The note goes first: a piece still held open
// somewhere can refuse to be deleted, and a folder left half-removed must not
// be one a later run carries on from - without its note it is something
// nothing picks up and the next startup sweeps.
static void DiscardStaging(const char *dir)
{
    char path[600];
    StagingNotePath(dir, path, sizeof(path));
    DeleteFileA(path);
    RemoveFolder(dir);
}

// Removes staging folders no download can pick up again: the ones from before
// the pieces were kept, which are named after a job id this run will never
// issue, and any left without a readable note.
static void SweepStaging(const char *root)
{
    char pattern[600];
    _snprintf(pattern, sizeof(pattern), "%s\\*", root);
    pattern[sizeof(pattern) - 1] = '\0';

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;

    do
    {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;

        char dir[600], note[STAGING_NOTE_MAX];
        _snprintf(dir, sizeof(dir), "%s\\%s", root, fd.cFileName);
        dir[sizeof(dir) - 1] = '\0';

        ReadStagingNote(dir, note, sizeof(note));
        if (note[0] == '\0')
        {
            dprintf("[game] clearing out %s - nothing says what it was downloading\n", dir);
            RemoveFolder(dir);
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
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
// A game kept in a RAR
// ---------------------------------------------------------------------------
//
// Some games are on archive.org only as RARs (XBOX_360_1). The Store lists
// one only once make_store_titles.py has seen it's stored - no compression,
// "-m0" - so the disc image is in the RAR byte for byte, and is read straight
// out of the download, with no unpacking. A compressed one is unpacked on the
// console as it downloads (DownloadingRar, below). Encrypted or split ones are
// refused.

enum RarMemberResult
{
    RAR_MEMBER_OK,
    RAR_MEMBER_UNREADABLE,  // the headers couldn't be fetched or made sense of
    RAR_MEMBER_ENCRYPTED,
    RAR_MEMBER_SPLIT
};

// Whether a RAR's file is its disc image: an .iso, or anything a gigabyte or
// more. Some RARs hold a 30-byte "DVD1.dvd" layer-break note before it.
static bool IsDiscImage(const RarEntry &entry)
{
    const size_t n = strlen(entry.name);
    return (n > 4 && _stricmp(entry.name + n - 4, ".iso") == 0) || entry.unpSize >= 1000000000ULL;
}

// The archive's disc image: its size, CRC, and where its data starts.
static RarMemberResult ReadRarMember(const std::string &url, const char *auth, RarFormat format,
                                     unsigned long signatureLen, ZipMember *out, unsigned long long *outDataStart,
                                     bool *outHasCrc, bool *outCompressed)
{
    const unsigned long HEAD = 65536;
    unsigned char *head = (unsigned char *)malloc(HEAD + 1);
    if (head == NULL)
        return RAR_MEMBER_UNREADABLE;
    std::string u = url;
    unsigned long long len = 0;
    const int status = Get(u, auth, "Range: bytes=0-65535\r\n", (char *)head, HEAD, &len);
    if (status != 206 && status != 200)
    {
        dprintf("[game] RAR headers: HTTP %d\n", status);
        free(head);
        return RAR_MEMBER_UNREADABLE;
    }

    RarMemberResult result = RAR_MEMBER_UNREADABLE;
    unsigned long long at = signatureLen;
    for (int i = 0; i < 32 && at < len; ++i)
    {
        RarEntry entry;
        unsigned long long next = 0;
        const int r = RarParseHeader(format, head + at, (unsigned long)(len - at), &entry, &next);
        if (r == RAR_HEADER_ENCRYPTED)
        {
            result = RAR_MEMBER_ENCRYPTED;
            break;
        }
        if (r != RAR_HEADER_OK)
            break;
        if (entry.isFile && IsDiscImage(entry))
        {
            dprintf("[game] RAR%d member \"%s\": %I64u packed, %I64u unpacked, %s%s%s\n",
                    format == RAR_FORMAT_5 ? 5 : 4, entry.name, entry.packSize, entry.unpSize,
                    entry.stored ? "stored" : "compressed", entry.encrypted ? ", encrypted" : "",
                    entry.split ? ", split" : "");
            if (entry.encrypted)
                result = RAR_MEMBER_ENCRYPTED;
            else if (entry.split)
                result = RAR_MEMBER_SPLIT;
            else
            {
                *outCompressed = !entry.stored || entry.packSize != entry.unpSize;
                _snprintf(out->name, sizeof(out->name), "%s", entry.name);
                out->name[sizeof(out->name) - 1] = '\0';
                out->method = 0;
                out->crc = entry.crc;
                out->packed = entry.packSize;
                out->unpacked = entry.unpSize;
                out->localHeader = at;
                *outDataStart = at + entry.dataOffset;
                *outHasCrc = entry.hasCrc;
                result = RAR_MEMBER_OK;
            }
            break;
        }
        at += next;
    }
    free(head);
    return result;
}

// The image, straight out of the downloaded pieces from where it starts.
class StoredSource : public GodSource
{
public:
    StoredSource(ZipPieces *pieces_, unsigned long long start_, unsigned long long size_)
        : pieces(pieces_), start(start_), size(size_)
    {
    }
    bool ReadAt(unsigned long long offset, void *buffer, unsigned long len)
    {
        if (offset + len > size)
            return false;
        return pieces->Read(start + offset, buffer, len);
    }
    unsigned long long Size() { return size; }

private:
    ZipPieces *pieces;
    unsigned long long start;
    unsigned long long size;
};

// ---------------------------------------------------------------------------
// Downloading, two pieces at a time
// ---------------------------------------------------------------------------

static std::string HostOf(const std::string &url); // with the server ranking, below

// A connection that does badly over a window is dropped and its piece
// resumed - a server that trickles never trips the HTTP client's timeout,
// which waits for nothing at all to arrive.
//
// "Badly" is judged against the other connections, not against a number.
// Two fixed floors have now failed for the same reason: 512 KB/s was a
// trickle to a line that does 2-4 MB/s but more than a slower line ever
// had, and a floor set from the ranking probe fared no better, because the
// probe's first 4 MB arrives about four times faster than the download that
// follows it - 1.58 MB/s probed, 0.13-0.38 MB/s delivered, so every window
// dropped and no piece finished (issue #3).
//
// The connections share one line, so they are each other's measure: one
// far behind the others is stuck and worth re-opening, while all of them
// equally slow is simply the line, and dropping them helps nothing. Below
// SLOW_TRICKLE nothing is moving whatever the peers say.
#define SLOW_WINDOW_MS     20000
#define SLOW_TRICKLE       (48ULL * 1024)
#define SLOW_PEER_SHARE    4 // dropped under a quarter of the best peer
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
    unsigned long long movedBytes;  // everything received, finished pieces or not
    unsigned long long inFlight[CONNECTIONS];
    unsigned long long resumedAt[CONNECTIONS]; // what a resumed piece already had
    DWORD threadIds[CONNECTIONS];

    // Each connection's request in hand, for the slow check and the log.
    unsigned long piece[CONNECTIONS];
    DWORD windowStart[CONNECTIONS];             // when the current window began
    unsigned long long windowFrom[CONNECTIONS]; // the request's bytes then
    DWORD lastLog[CONNECTIONS];
    unsigned long long windowRate[CONNECTIONS]; // bytes a second over the last full window, 0 if none yet
    bool tooSlow[CONNECTIONS];                  // dropped by the slow check
    bool trickled[CONNECTIONS];                 // and it was the server, not the connection
    bool failed;
    int failStatus;
    const bool *cancel;
    const bool *pause;        // the other way a job stops - see Stopping
    HANDLE pieceReady;        // auto-reset: a piece finished, or the download stopped
};

static Download *g_download = NULL;

// The connections and the indexer ask this between every read, and nothing
// else: a pause that isn't here is one that changes the words on screen and
// leaves the download running.
static bool Stopping(const Download *d)
{
    return d->failed || *d->cancel || *d->pause || g_shutdown;
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
    double rate = 0.0, peerRate = 0.0;
    unsigned long long windowClosed = 0; // bytes a second over it, if one just did
    bool closed = false;
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
        const unsigned long long had = d->inFlight[slot];
        d->inFlight[slot] = d->resumedAt[slot] + done;
        if (d->inFlight[slot] > had)
            d->movedBytes += d->inFlight[slot] - had;
        piece = d->piece[slot];
        total = d->inFlight[slot];

        const DWORD elapsed = now - d->windowStart[slot];
        if (elapsed >= SLOW_WINDOW_MS)
        {
            const unsigned long long got = done - d->windowFrom[slot];
            const unsigned long long mine = got * 1000 / elapsed;
            rate = got / 1048576.0 / (elapsed / 1000.0);
            d->windowRate[slot] = mine;
            windowClosed = mine;
            closed = true;

            // The best any other connection managed over its last full
            // window. Nothing to compare against until one has finished.
            unsigned long long peak = 0;
            for (int i = 0; i < CONNECTIONS; ++i)
            {
                if (i != slot && d->windowRate[i] > peak)
                    peak = d->windowRate[i];
            }
            const bool trickle = mine < SLOW_TRICKLE;
            if (trickle || (peak > 0 && mine < peak / SLOW_PEER_SHARE))
            {
                d->tooSlow[slot] = true;
                d->trickled[slot] = trickle;
                keepGoing = false;
                slow = true;
                peerRate = peak / 1048576.0;
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

    // One connection's honest throughput over a full window, for the check
    // that works out whether a slow download is archive.org or this line.
    if (closed)
        ReportConnectionRate(windowClosed);

    if (slow && peerRate > 0.0)
        dprintf("[game] piece %lu on connection %d: %.2f MB/s from %s for %ds, against %.2f on the other "
                "connection - dropping it to resume\n",
                piece, slot + 1, rate, host.c_str(), SLOW_WINDOW_MS / 1000, peerRate);
    else if (slow)
        dprintf("[game] piece %lu on connection %d: %.2f MB/s from %s for %ds - nothing is coming, dropping it to "
                "resume\n",
                piece, slot + 1, rate, host.c_str(), SLOW_WINDOW_MS / 1000);
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
        // Pieces an earlier run already finished are on the drive - step over
        // them rather than fetching them again.
        while (d->nextPiece < d->pieceCount && d->done[d->nextPiece])
            d->nextPiece++;
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
        int fruitless = 0;           // tries in a row that brought nothing
        int tries = 0;
        DWORD started = GetTickCount();
        while (!got && !Stopping(d) && fruitless < PIECE_ATTEMPTS && tries < PIECE_TRIES)
        {
            if (tries++ > 0)
            {
                if (fruitless == 0)
                    dprintf("[game] piece %lu: %I64u of %I64u bytes in, resuming (try %d of %d)\n", index, have,
                            length, tries, PIECE_TRIES);
                else if (have > 0)
                    dprintf("[game] piece %lu: cut off after %I64u of %I64u bytes, resuming (%d of %d)\n",
                            index, have, length, fruitless + 1, PIECE_ATTEMPTS);
                else
                    dprintf("[game] piece %lu: HTTP %d, trying again (%d of %d)\n", index, status, fruitless + 1,
                            PIECE_ATTEMPTS);
                if (fruitless > 0)
                    Sleep(2000 * fruitless); // only back off when nothing is coming
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
            d->windowRate[slot] = 0; // a new connection has nothing to show the others yet
            d->tooSlow[slot] = false;
            d->trickled[slot] = false;
            LeaveCriticalSection(&g_lock);

            const char *target = (resumeFrom > 0) ? restPath : tempPath;
            unsigned long long size = 0;
            status = httpRequestHTTPS(url, HTTP_GET, NULL, headers, target, NULL, &size, true, NULL, 0,
                                      QuietPrint, length - resumeFrom, PieceProgress);

            // Dropped by the slow check: what came is good, as if cut off.
            EnterCriticalSection(&g_lock);
            const bool dropped = d->tooSlow[slot] && !Stopping(d);
            const bool trickled = d->trickled[slot];
            d->tooSlow[slot] = false;
            d->trickled[slot] = false;
            LeaveCriticalSection(&g_lock);
            if (dropped && status == HTTP_STATUS_CANCELLED)
            {
                status = 206;
                // Only a server giving nothing to anyone is the server's
                // fault. One connection behind its peers is that
                // connection's, and re-opening it is the whole remedy -
                // moving everything to another server would be a worse one.
                if (trickled)
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

            // A try that brought bytes is progress, however slow: the piece
            // keeps them and carries on. Only one that gained nothing - a
            // dead connection, a refused range - spends an attempt.
            if (have > resumeFrom)
                fruitless = 0;
            else
                ++fruitless;
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

    // A pause stopped the download the same way a cancel would, but the
    // install isn't over: it goes back to the queue to wait for the word.
    // Decided here, under the lock, so no frame of the UI ever sees it
    // finished - one that did would raise a popup for an install that is only
    // resting, and TakeFinishedGameJob would hand it over as done.
    if (outcome == QUEUE_OUTCOME_CANCELLED && job->pause && !job->cancel && !g_shutdown)
    {
        s.state = QUEUE_PAUSED;
        s.phase[0] = '\0';
        s.fraction = -1.0f;
        SaveInstallStateLocked();
        return;
    }

    s.state = QUEUE_FINISHED;
    s.outcome = outcome;
    s.notify = notify;
    _snprintf(s.resultText, sizeof(s.resultText), "%s", text);
    s.resultText[sizeof(s.resultText) - 1] = '\0';
    _snprintf(s.resultDetail, sizeof(s.resultDetail), "%s", detail != NULL ? detail : "");
    s.resultDetail[sizeof(s.resultDetail) - 1] = '\0';

    // One fewer install for the next run to pick up - unless this is the app
    // closing, which SaveInstallStateLocked sits out.
    SaveInstallStateLocked();
}

// A game's three steps, and each one's share of the whole job's percentage -
// the same as its segment's share of the Queue's bar (SnapshotQueue's
// kGameWidths), so the number always agrees with how much bar is filled.
// Downloading is the long wait, so it's most of both.
enum GameStep
{
    GAME_STEP_FINDING,
    GAME_STEP_DOWNLOADING,
    GAME_STEP_INSTALLING,
    GAME_STEP_COUNT
};
static const float kGameStepShares[GAME_STEP_COUNT] = { 0.05f, 0.90f, 0.05f };

static void SetPhase(GameJob *job, GameStep step, const char *phase, unsigned long long done,
                     unsigned long long total, unsigned long long bytesPerSec)
{
    EnterCriticalSection(&g_lock);
    QueueJobSnapshot &s = job->snap;

    // Asked to pause, and the connections haven't come back round to
    // noticing yet - one stalled can take STALL_TIMEOUT_MS. Say so, rather
    // than go on reporting a download the user has already stopped.
    _snprintf(s.phase, sizeof(s.phase), "%s", (job->pause && !job->cancel) ? "Pausing" : phase);
    s.phase[sizeof(s.phase) - 1] = '\0';
    s.bytesDone = done;
    s.bytesTotal = total;
    s.bytesPerSec = bytesPerSec;
    s.secondsLeft = (bytesPerSec > 0 && total > done) ? (total - done) / bytesPerSec : 0;
    s.step = step;
    s.stepCount = GAME_STEP_COUNT;
    s.stepFraction = (total > 0) ? (float)((double)done / (double)total) : -1.0f;
    s.fraction = JobFraction(kGameStepShares, step, s.stepFraction);
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
    SetPhase(p->job, GAME_STEP_INSTALLING, "Installing", done, total, ms > 3000 ? done * 1000 / ms : 0);

    EnterCriticalSection(&g_lock);
    bool keepGoing = !p->job->cancel && !p->job->pause && !g_shutdown;
    LeaveCriticalSection(&g_lock);
    return keepGoing;
}

static void FinishLocked(GameJob *job, QueueOutcome outcome, const char *text, const char *detail, bool notify)
{
    EnterCriticalSection(&g_lock);
    Finish(job, outcome, text, detail, notify);
    LeaveCriticalSection(&g_lock);
}

// True once the install in hand should stop, whether for good or for now: a
// pause stops the download exactly as a cancel does, and Finish is where the
// two part company.
static bool Cancelled(GameJob *job)
{
    EnterCriticalSection(&g_lock);
    bool c = job->cancel || job->pause || g_shutdown;
    LeaveCriticalSection(&g_lock);
    return c;
}

// Set aside by the user, rather than given up on. Like the app closing, it
// keeps what has been downloaded - see EndStaging.
static bool Paused(GameJob *job)
{
    EnterCriticalSection(&g_lock);
    const bool paused = job->pause && !job->cancel;
    LeaveCriticalSection(&g_lock);
    return paused;
}

// The app closing, rather than this job being stopped. The two arrive the
// same way - StopGameInstaller cancels every job - but they mean opposite
// things for what's been downloaded: closing keeps it, stopping throws it
// away. See EndStaging.
static bool ClosingDown()
{
    EnterCriticalSection(&g_lock);
    const bool closing = g_shutdown;
    LeaveCriticalSection(&g_lock);
    return closing;
}

// What becomes of the pieces when a job ends. Keeping them is what lets the
// next run carry on, so they're kept whenever the download might still be
// wanted: the app closed, archive.org gave up, the drive filled. They go when
// there's nothing to carry on to - the game installed - or when carrying on
// would be wrong: the user stopped it, or what came down turned out to be no
// good.
static void EndStaging(const char *dir, bool keep)
{
    if (!keep)
    {
        DiscardStaging(dir);
        return;
    }

    dprintf("[game] keeping what's downloaded in %s - the next run carries on from it\n", dir);
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
// The probe ranks the servers against each other, which it is good for.
// It is not a measure of the download to come: its first 4 MB arrives
// several times faster than the rest, so nothing downstream should take it
// for a speed the line can hold.
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

// ---------------------------------------------------------------------------
// A compressed RAR: unpacked as it downloads
// ---------------------------------------------------------------------------
//
// dmc_unrar (RarUnpack.h) reads the RAR straight out of the pieces, waiting
// for each to land, on this thread - in place of the zip indexer, so the two
// connections keep their cores. It writes the disc image to pieces of its own,
// which the GoD install then reads like a stored RAR's. Unpacking runs at
// about 3 MB/s on a console - slower than the download - so the bar follows
// the unpacking, and a piece is deleted once the unpacking is past it: the
// drive holds the image and whatever download is still waiting, never both
// whole.

class DownloadingRar : public RarUnpackInput
{
public:
    DownloadingRar(Download *d_, ZipPieces *pieces_, GameJob *job_)
        : d(d_), pieces(pieces_), job(job_), failed(false), consumed(0), dropped(0), lastReport(0),
          started(GetTickCount()), cache(NULL), cacheStart(0), cacheLen(0)
    {
        cache = (unsigned char *)malloc(CACHE_SIZE);
    }

    ~DownloadingRar() { free(cache); }

    bool Failed() const { return failed; }
    unsigned long long Size() { return d->total; }

    unsigned long Read(unsigned long long offset, void *buffer, unsigned long len)
    {
        if (failed || cache == NULL || offset >= d->total)
            return 0;
        if (len > d->total - offset)
            len = (unsigned long)(d->total - offset);

        unsigned char *out = (unsigned char *)buffer;
        unsigned long done = 0;
        while (done < len)
        {
            const unsigned long long at = offset + done;
            if (at < cacheStart || at >= cacheStart + cacheLen)
            {
                if (!Fill(at))
                {
                    failed = true;
                    return done;
                }
            }
            unsigned long take = (unsigned long)(cacheStart + cacheLen - at);
            if (take > len - done)
                take = len - done;
            memcpy(out + done, cache + (at - cacheStart), take);
            done += take;
        }

        if (offset + len > consumed)
            consumed = offset + len;
        Report(false);
        return done;
    }

    // Once more at the end, so the bar isn't left short.
    void Report(bool force)
    {
        const DWORD now = GetTickCount();
        if (!force && now - lastReport < 500)
            return;
        lastReport = now;

        EnterCriticalSection(&g_lock);
        bool downloaded = (d->doneBytes >= d->total);
        LeaveCriticalSection(&g_lock);

        const DWORD ms = now - started;
        SetPhase(job, GAME_STEP_DOWNLOADING, downloaded ? "Unpacking" : "Downloading and unpacking", consumed,
                 d->total, ms > 3000 ? consumed * 1000 / ms : 0);
    }

private:
    enum { CACHE_SIZE = 1024 * 1024 };

    // Loads the cache from `at`, waiting for its piece. dmc_unrar reads the
    // headers, then the data from start to end - so a read never needs a
    // piece behind the one before, and those can go.
    bool Fill(unsigned long long at)
    {
        const unsigned long index = (unsigned long)(at / PIECE_SIZE);
        if (!WaitFor(index))
            return false;

        // One piece at most - the next may not be here yet.
        unsigned long long end = (unsigned long long)(index + 1) * PIECE_SIZE;
        if (end > d->total)
            end = d->total;
        unsigned long len = (end - at > CACHE_SIZE) ? CACHE_SIZE : (unsigned long)(end - at);
        if (!pieces->Read(at, cache, len))
        {
            dprintf("[game] reading piece %lu to unpack failed\n", index);
            return false;
        }
        cacheStart = at;
        cacheLen = len;

        // Keep the one before, in case a header sits across the boundary.
        for (; dropped + 1 < index; ++dropped)
        {
            char path[300];
            pieces->PiecePath(dropped, path, sizeof(path));
            DeleteFileA(path);
        }
        return true;
    }

    bool WaitFor(unsigned long index)
    {
        for (;;)
        {
            EnterCriticalSection(&g_lock);
            bool ready = d->done[index];
            bool stop = Stopping(d);
            LeaveCriticalSection(&g_lock);

            if (ready)
                return true;
            if (stop)
                return false;
            Report(false);
            WaitForSingleObject(d->pieceReady, 500);
        }
    }

    Download *d;
    ZipPieces *pieces;
    GameJob *job;
    bool failed;
    unsigned long long consumed; // how far through the RAR the unpacking has read
    unsigned long dropped;       // pieces before this one are deleted
    DWORD lastReport;
    DWORD started;
    unsigned char *cache;
    unsigned long long cacheStart;
    unsigned long cacheLen;
};

// The unpacked image, written as pieces the size of the download's, for a
// StoredSource to read back - FATX can't hold a file of 4GB or more.
class ImagePieceWriter : public RarUnpackOutput
{
public:
    ImagePieceWriter(ZipPieces *image_) : image(image_), file(NULL), index(0), inPiece(0), buffer(NULL)
    {
        buffer = malloc(BUFFER_SIZE);
    }

    ~ImagePieceWriter()
    {
        Close();
        free(buffer);
    }

    bool Write(const void *data, unsigned long len)
    {
        const unsigned char *p = (const unsigned char *)data;
        while (len > 0)
        {
            if (file == NULL || inPiece == PIECE_SIZE)
            {
                if (file != NULL)
                {
                    bool ok = (fclose(file) == 0);
                    file = NULL;
                    index++;
                    if (!ok)
                        return false;
                }
                char path[300];
                image->PiecePath(index, path, sizeof(path));
                file = fopen(path, "wb");
                if (file == NULL)
                    return false;
                if (buffer != NULL)
                    setvbuf(file, (char *)buffer, _IOFBF, BUFFER_SIZE);
                inPiece = 0;
            }

            unsigned long take = (unsigned long)(PIECE_SIZE - inPiece);
            if (take > len)
                take = len;
            if (fwrite(p, 1, take, file) != take)
                return false;
            p += take;
            len -= take;
            inPiece += take;
        }
        return true;
    }

    bool Close()
    {
        if (file == NULL)
            return true;
        bool ok = (fclose(file) == 0);
        file = NULL;
        return ok;
    }

private:
    enum { BUFFER_SIZE = 4 * 1024 * 1024 };

    ZipPieces *image;
    FILE *file;
    unsigned long index;
    unsigned long long inPiece;
    void *buffer;
};

static void RunJob(GameJob *job, const char *gamesPath)
{
    const GameRequest &req = job->request;
    const char *auth = job->auth;

    SetPhase(job, GAME_STEP_FINDING, "Starting", 0, 0, 0);

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

    SetPhase(job, GAME_STEP_FINDING, "Finding the fastest server", 0, 0, 0);
    std::string servers[MAX_SERVERS];
    const int serverCount = RankServers(job, url, req.item, encodedZip, auth, req.zipSize, servers, MAX_SERVERS);
    url = servers[0];
    if (Cancelled(job))
    {
        FinishLocked(job, QUEUE_OUTCOME_CANCELLED, "Stopped", "Nothing was installed.", false);
        return;
    }

    // A zip's one member, compressed - or a RAR's, stored.
    ZipMember member;
    memset(&member, 0, sizeof(member));
    unsigned long long dataStart = 0;
    unsigned long signatureLen = 0;
    const RarFormat rarFormat = RarDetectFormat((const unsigned char *)small, (unsigned long)len, &signatureLen);
    const bool isRar = (rarFormat != RAR_FORMAT_UNKNOWN);
    bool hasCrc = true;
    bool compressedRar = false;
    if (isRar)
    {
        const RarMemberResult rar = ReadRarMember(url, auth, rarFormat, signatureLen, &member, &dataStart, &hasCrc,
                                                  &compressedRar);
        if (rar != RAR_MEMBER_OK)
        {
            FinishLocked(job, QUEUE_OUTCOME_FAILED, "Couldn't install",
                         rar == RAR_MEMBER_ENCRYPTED ? "This RAR has a password."
                         : rar == RAR_MEMBER_SPLIT     ? "This RAR is split into parts."
                                                       : "The RAR's headers couldn't be read.",
                         true);
            return;
        }
    }
    else
    {
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
        dataStart = member.localHeader + 30 + LE16(local + 26) + LE16(local + 28);
    }
    const unsigned long long needed = dataStart + member.packed;
    const bool stored = isRar && !compressedRar;

    dprintf("[game] %s: \"%s\", %I64u packed from %I64u, %I64u unpacked, crc %08lX\n", req.zipName, member.name,
            member.packed, dataStart, member.unpacked, member.crc);

    char root[300], dir[320], note[STAGING_NOTE_MAX], wasHere[STAGING_NOTE_MAX];
    StagingRoot(gamesPath, root, sizeof(root));
    CreateDirectoryA(root, NULL);
    StagingDirFor(root, req, dir, sizeof(dir));
    StagingNoteText(req, dataStart, member.packed, member.crc, note, sizeof(note));

    // What a run before this one left here, if it was downloading this same
    // file. A compressed RAR's pieces are deleted as the unpacking passes
    // them, and the unpacking itself can't be picked up part way through, so
    // those always start over.
    ReadStagingNote(dir, wasHere, sizeof(wasHere));
    const bool pickUp = !compressedRar && wasHere[0] != '\0' && strcmp(wasHere, note) == 0;
    if (!pickUp)
    {
        if (wasHere[0] != '\0')
            dprintf("[game] what's staged here isn't this download any more - starting over\n");
        DiscardStaging(dir);
    }

    if (!CreateDirectoryA(dir, NULL))
    {
        const DWORD attrs = GetFileAttributesA(dir);
        if (attrs == (DWORD)-1 || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
        {
            FinishLocked(job, QUEUE_OUTCOME_FAILED, "Couldn't install", "The staging folder couldn't be made.", true);
            return;
        }
    }
    if (!WriteStagingNote(dir, note))
        dprintf("[game] couldn't write the note in %s - this download won't be picked up again\n", dir);

    // --- Download, and index as the pieces land -----------------------------
    ZipPieces pieces(dir, PIECE_SIZE);

    const unsigned long pieceCount = (unsigned long)((needed + PIECE_SIZE - 1) / PIECE_SIZE);
    bool *done = (bool *)calloc(pieceCount, sizeof(bool));
    bool *wasStaged = (bool *)calloc(pieceCount, sizeof(bool)); // of those, the ones that were already here
    unsigned long long stagedBytes = 0;
    unsigned long stagedPieces = 0;
    if (done != NULL && wasStaged != NULL && pickUp)
    {
        stagedBytes = ScanStagedPieces(pieces, pieceCount, needed, done, &stagedPieces);
        memcpy(wasStaged, done, pieceCount * sizeof(bool));
        if (stagedPieces > 0)
            dprintf("[game] %lu of %lu pieces are already here (%I64u MB) - carrying on from them\n", stagedPieces,
                    pieceCount, stagedBytes / (1024 * 1024));
    }

    // Room for the zip and the game. The game is at most the image's size;
    // checked again, exactly, once the image says how big the game is. A
    // compressed RAR is unpacked to an image first, and its download is
    // deleted as the unpacking passes it: at worst the whole download and the
    // image, or the image and the game. Pieces already staged are space
    // already spent, so only the rest of the download is asked for.
    const unsigned long long spaceNeeded = compressedRar
        ? (needed > member.unpacked ? needed : member.unpacked) + member.unpacked
        : (needed - stagedBytes) + member.unpacked;
    unsigned long long freeSpace = 0;
    if (DriveFreeSpace(gamesPath, &freeSpace) && freeSpace < spaceNeeded + SPACE_MARGIN)
    {
        char needText[64] = "", freeText[64] = "", detail[160];
        FormatBytes(spaceNeeded, needText, sizeof(needText));
        FormatBytes(freeSpace, freeText, sizeof(freeText));
        _snprintf(detail, sizeof(detail), "It needs up to %s while it installs, and %s is free.", needText, freeText);
        detail[sizeof(detail) - 1] = '\0';
        free(done);
        free(wasStaged);
        if (stagedPieces == 0)
            DiscardStaging(dir); // nothing of worth in it - don't leave it sitting there
        FinishLocked(job, QUEUE_OUTCOME_NO_SPACE, "Not enough space", detail, true);
        return;
    }

    // A compressed RAR's image, unpacked.
    char imageDir[340];
    _snprintf(imageDir, sizeof(imageDir), "%s\\image", dir);
    imageDir[sizeof(imageDir) - 1] = '\0';
    ZipPieces imagePieces(imageDir, PIECE_SIZE);
    if (compressedRar && !CreateDirectoryA(imageDir, NULL))
    {
        free(done);
        free(wasStaged);
        DiscardStaging(dir);
        FinishLocked(job, QUEUE_OUTCOME_FAILED, "Couldn't install", "The staging folder couldn't be made.", true);
        return;
    }
    Download d;
    for (int i = 0; i < serverCount; ++i)
        d.servers[i] = servers[i];
    d.serverCount = serverCount;
    d.serverAt = 0;
    d.strikes = 0;
    d.auth = auth;
    d.pieces = &pieces;
    d.total = needed;
    d.pieceCount = pieceCount;
    d.nextPiece = 0;
    d.done = done;
    d.doneBytes = stagedBytes;
    d.movedBytes = 0;
    d.failed = false;
    d.failStatus = 0;
    d.cancel = &job->cancel;
    d.pause = &job->pause;
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
        d.windowRate[i] = 0;
        d.tooSlow[i] = false;
        d.trickled[i] = false;
    }

    // A zip's image is indexed for reading back out of its deflate stream;
    // a stored RAR's needs only its CRC, and a compressed one's is checked as
    // it's unpacked.
    InflateIndexer *indexer = isRar ? NULL : new InflateIndexer(member.unpacked);
    unsigned long crc = crc32(0L, Z_NULL, 0);
    unsigned char *chunk = (unsigned char *)malloc(1024 * 1024);

    if (d.done == NULL || wasStaged == NULL || d.pieceReady == NULL || (indexer != NULL && !indexer->Ok()) ||
        chunk == NULL)
    {
        free(d.done);
        free(wasStaged);
        if (d.pieceReady != NULL)
            CloseHandle(d.pieceReady);
        delete indexer;
        free(chunk);
        if (stagedPieces == 0)
            DiscardStaging(dir);
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
    RarUnpackResult unpacked = RAR_UNPACK_OK;

    if (compressedRar && indexOk)
    {
        DownloadingRar rar(&d, &pieces, job);
        ImagePieceWriter writer(&imagePieces);
        unpacked = RarUnpackFrom(&rar, member.name, &writer, NULL, dprintf);
        if (!writer.Close() && unpacked == RAR_UNPACK_OK)
            unpacked = RAR_UNPACK_WRITE_FAILED;
        rar.Report(true);
        indexOk = !rar.Failed();
        DWORD unpackSeconds = (GetTickCount() - downloadStart) / 1000;
        dprintf("[game] unpacking %s after %lu:%02lu (result %d)\n", unpacked == RAR_UNPACK_OK ? "finished" : "stopped",
                (unsigned long)(unpackSeconds / 60), (unsigned long)(unpackSeconds % 60), (int)unpacked);
    }

    for (unsigned long index = 0; !compressedRar && indexOk && index < d.pieceCount; ++index)
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

            // A download picked up from an earlier run reads its way back
            // through what's already here before the first new byte arrives -
            // the deflate stream can only be indexed from its start. That
            // reads the drive rather than the network, so it says so, and the
            // speed is of what this run has fetched, not of the drive.
            DWORD ms = GetTickCount() - downloadStart;
            unsigned long long fetched = bytes > stagedBytes ? bytes - stagedBytes : 0;
            SetPhase(job, GAME_STEP_DOWNLOADING,
                     wasStaged[index] ? "Checking what's already downloaded" : "Downloading", bytes, needed,
                     ms > 3000 ? fetched * 1000 / ms : 0);

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
            if (!pieces.Read(from, chunk, take) || (indexer != NULL && !indexer->Feed(chunk, take)))
            {
                dprintf("[game] indexing failed at %I64u\n", from);
                indexOk = false;
                break;
            }
            if (indexer == NULL)
                crc = crc32(crc, chunk, take);
            from += take;
        }
    }

    // Stop the connections if anything went wrong, and wait for them.
    if (!indexOk || unpacked != RAR_UNPACK_OK)
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
    free(wasStaged);
    free(chunk);

    DWORD downloadSeconds = (GetTickCount() - downloadStart) / 1000;
    dprintf("[game] downloaded %I64u MB in %lu:%02lu (%I64u MB of pieces finished)\n", d.movedBytes / (1024 * 1024),
            (unsigned long)(downloadSeconds / 60), (unsigned long)(downloadSeconds % 60), d.doneBytes / (1024 * 1024));

    const bool imageOk = indexOk && (compressedRar ? unpacked == RAR_UNPACK_OK
                                     : stored      ? (!hasCrc || crc == member.crc)
                                                   : (indexer->Finished() && indexer->Out() == member.unpacked &&
                                                      indexer->Crc() == member.crc));
    if (!imageOk)
    {
        bool cancelled = Cancelled(job);
        int failStatus = d.failStatus;
        if (indexOk && !cancelled && stored)
            dprintf("[game] the image didn't check out: crc %08lX (want %08lX)\n", crc, member.crc);
        else if (indexOk && !cancelled && !compressedRar)
            dprintf("[game] the image didn't check out: finished %d, %I64u of %I64u bytes, crc %08lX (want %08lX)\n",
                    indexer->Finished() ? 1 : 0, indexer->Out(), member.unpacked, indexer->Crc(), member.crc);
        delete indexer;

        // The app closing keeps what's downloaded, and so does a pause - the
        // whole point of one. The user stopping it doesn't. A download that
        // simply failed - archive.org stopping, the keys refused - keeps it
        // too, as it was good as far as it got; one that arrived whole and
        // then failed its checksum does not, as its bytes are the problem.
        EndStaging(dir, !compressedRar && (cancelled ? (ClosingDown() || Paused(job)) : !indexOk));

        if (cancelled)
            FinishLocked(job, QUEUE_OUTCOME_CANCELLED, "Stopped", "Nothing was installed.", false);
        else if (failStatus == 401 || failStatus == 403)
            FinishLocked(job, QUEUE_OUTCOME_KEYS_REJECTED, "Keys not accepted",
                         "archive.org refused the keys - check them in Settings.", true);
        else if (indexOk && unpacked == RAR_UNPACK_UNSUPPORTED)
            FinishLocked(job, QUEUE_OUTCOME_FAILED, "Couldn't install",
                         "This RAR is packed in a way the console can't unpack.", true);
        else if (indexOk && unpacked == RAR_UNPACK_WRITE_FAILED)
            FinishLocked(job, QUEUE_OUTCOME_FAILED, "Couldn't install",
                         "The unpacked game couldn't be written - is the drive full?", true);
        else if (indexOk)
            FinishLocked(job, QUEUE_OUTCOME_FAILED, "Download was damaged",
                         "The game's data didn't match its checksum. Try again.", true);
        else
            FinishLocked(job, QUEUE_OUTCOME_FAILED, "Download failed",
                         "archive.org stopped answering. Try again later.", true);
        return;
    }

    int pointCount = 0;
    InflatePoint *points = (indexer != NULL) ? indexer->TakePoints(&pointCount) : NULL;
    delete indexer;
    dprintf("[game] image checks out: %I64u bytes, crc %08lX%s, %d restart points\n", member.unpacked, member.crc,
            isRar && !hasCrc ? " (the RAR has none to check)" : "", pointCount);

    // --- Install --------------------------------------------------------------
    InflateSource inflated(&pieces, dataStart, member.packed, member.unpacked, points, pointCount);
    StoredSource storedImage(&pieces, dataStart, member.unpacked);
    StoredSource unpackedImage(&imagePieces, 0, member.unpacked);
    GodSource *image = compressedRar ? (GodSource *)&unpackedImage
                       : stored      ? (GodSource *)&storedImage
                                     : (GodSource *)&inflated;

    GodImageInfo info;
    GodResult result = GodInspect(image, &info);
    if (result != GOD_OK)
    {
        // Downloaded whole and checked, but it isn't a disc image this can
        // read - fetching it again would only arrive at the same place.
        free(points);
        EndStaging(dir, false);
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
        // The download is here and checks out - it's the game it makes that
        // won't fit. Kept, so freeing space and choosing it again installs
        // from what's already down rather than fetching it all over again.
        free(points);
        EndStaging(dir, !compressedRar);
        FinishLocked(job, QUEUE_OUTCOME_NO_SPACE, "Not enough space", detail, true);
        return;
    }

    ConvertProgress progress;
    progress.job = job;
    progress.started = GetTickCount();
    SetPhase(job, GAME_STEP_INSTALLING, "Installing", 0, info.usedSize, 0);

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
        ReadAheadSource ahead(image, 1024 * 1024, 8);
        result = GodConvert(&ahead, info, gamesPath, req.name, iconSize > 0 ? icon : NULL, iconSize,
                            ConvertProgressFn, &progress, packagePath, sizeof(packagePath), &timings);
    }
    ClearInstallMarker();

    DWORD seconds = (GetTickCount() - progress.started) / 1000;
    dprintf("[game] install %s after %lu:%02lu (%lu restarts): %s\n", GodResultText(result),
            (unsigned long)(seconds / 60), (unsigned long)(seconds % 60), inflated.Restarts(), packagePath);
    if (seconds > 0)
        dprintf("[game] %I64u MB at %.2f MB/s - waiting for data %.0fs, hashing %.0fs, writing %.0fs\n",
                info.usedSize / (1024 * 1024), (double)info.usedSize / (1024.0 * 1024.0) / (double)seconds,
                timings.readMs / 1000.0, timings.hashMs / 1000.0, timings.writeMs / 1000.0);

    // Installed, so there's nothing left to carry on to. A convert stopped by
    // the app closing, or by a drive that wouldn't take the writes, keeps the
    // download instead: it is whole and checked, and getting it again is the
    // long part.
    free(points);
    EndStaging(dir, !compressedRar && (result == GOD_WRITE_FAILED ||
                                       (result == GOD_CANCELLED && (ClosingDown() || Paused(job)))));

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

    // A session that ended mid-download leaves its pieces behind, and they
    // stay: choosing that game again carries on from them instead of fetching
    // 7-9 GB a second time. Only what no download could ever pick up again is
    // cleared out here - see SweepStaging.
    char root[300];
    StagingRoot(gamesPath, root, sizeof(root));
    SweepStaging(root);

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

// paused is for an install coming back from a previous run that was set aside
// then: everything else is queued ready to run.
static GameEnqueueResult EnqueueGame(const GameRequest &request, const char *authHeader, bool paused)
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
        job.snap.state = paused ? QUEUE_PAUSED : QUEUE_WAITING;
        job.pause = paused;
        job.snap.titleId = request.titleId;
        job.snap.fraction = -1.0f;
        _snprintf(job.snap.gameName, sizeof(job.snap.gameName), "%s", request.name);
        job.snap.gameName[sizeof(job.snap.gameName) - 1] = '\0';
        _snprintf(job.snap.title, sizeof(job.snap.title), "%s", request.zipName);
        job.snap.title[sizeof(job.snap.title) - 1] = '\0';
        SaveInstallStateLocked();
    }

    LeaveCriticalSection(&g_lock);
    if (result == GAME_QUEUED)
        SetEvent(g_wake);
    return result;
}

GameEnqueueResult EnqueueGameInstall(const GameRequest &request, const char *authHeader)
{
    return EnqueueGame(request, authHeader, false);
}

// Queues again whatever install was still to do when the app last closed.
// Its pieces are already on the drive - see the staging notes above - so this
// is what points at them again.
void RestoreGameInstalls(const char *authHeader)
{
    if (!g_running)
        return;

    char *text = ReadQueueStateFile(INSTALL_STATE_FILE);
    if (text == NULL)
        return;

    GameRequest request;
    memset(&request, 0, sizeof(request));
    bool inJob = false;
    bool paused = false;
    int queued = 0;

    char *at = text, *key = NULL, *value = NULL;
    while (NextQueueStateLine(&at, &key, &value))
    {
        // A "job=" line ends the one before it and starts the next.
        if (strcmp(key, "job") == 0)
        {
            if (inJob && request.zipName[0] != '\0' && EnqueueGame(request, authHeader, paused) == GAME_QUEUED)
                queued++;
            memset(&request, 0, sizeof(request));
            paused = false;
            inJob = (strcmp(value, "game") == 0);
            continue;
        }

        if (!inJob)
            continue;

        if (strcmp(key, "item") == 0)
            _snprintf(request.item, sizeof(request.item), "%s", value);
        else if (strcmp(key, "file") == 0)
            _snprintf(request.zipName, sizeof(request.zipName), "%s", value);
        else if (strcmp(key, "size") == 0)
            request.zipSize = _strtoui64(value, NULL, 10);
        else if (strcmp(key, "name") == 0)
            _snprintf(request.name, sizeof(request.name), "%s", value);
        else if (strcmp(key, "titleid") == 0)
            request.titleId = strtoul(value, NULL, 16);
        else if (strcmp(key, "paused") == 0)
            paused = (atoi(value) != 0);

        request.item[sizeof(request.item) - 1] = '\0';
        request.zipName[sizeof(request.zipName) - 1] = '\0';
        request.name[sizeof(request.name) - 1] = '\0';
    }

    if (inJob && request.zipName[0] != '\0' && EnqueueGame(request, authHeader, paused) == GAME_QUEUED)
        queued++; // the last block has no "job=" after it

    free(text);

    if (queued > 0)
        dprintf("[game] picked %d install%s back up from last time\n", queued, queued == 1 ? "" : "s");
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
    // After the ones still going, before the ones that are over.
    for (int i = NextJob(QUEUE_PAUSED, 0, false); i >= 0 && n < maxJobs; i = NextJob(QUEUE_PAUSED, g_jobs[i].snap.id, false))
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
        GameJob &job = g_jobs[i];
        if (!job.used || job.snap.id != id || job.snap.state == QUEUE_FINISHED)
            continue;

        job.cancel = true;

        // A paused job has no worker to notice - it finishes here. Its
        // download goes with it, as any cancelled one's does.
        if (job.snap.state == QUEUE_PAUSED)
        {
            char root[300], dir[320];
            StagingRoot(g_gamesPath, root, sizeof(root));
            StagingDirFor(root, job.request, dir, sizeof(dir));
            Finish(&job, QUEUE_OUTCOME_CANCELLED, "Stopped", "Nothing was installed.", false);
            LeaveCriticalSection(&g_lock);
            DiscardStaging(dir);
            SetEvent(g_wake);
            return;
        }
    }
    LeaveCriticalSection(&g_lock);
    SetEvent(g_wake);
}

void PauseGameJob(int id)
{
    if (!g_running)
        return;

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_GAME_JOBS; ++i)
    {
        GameJob &job = g_jobs[i];
        if (!job.used || job.snap.id != id || job.snap.state == QUEUE_FINISHED ||
            job.snap.state == QUEUE_PAUSED)
            continue;

        job.pause = true;

        if (job.snap.state == QUEUE_WAITING)
        {
            // Nothing is in flight, so it is paused as of now.
            job.snap.state = QUEUE_PAUSED;
            job.snap.fraction = -1.0f;
            SaveInstallStateLocked();
        }
        else
        {
            // The connections stop at their next progress report; until then
            // it is still downloading, and the row says so rather than
            // claiming a pause that hasn't happened yet. Finish sets the
            // state.
            _snprintf(job.snap.phase, sizeof(job.snap.phase), "%s", "Pausing");
            job.snap.phase[sizeof(job.snap.phase) - 1] = '\0';
            SaveInstallStateLocked(); // saved as paused in case the app closes meanwhile
        }
        break;
    }
    LeaveCriticalSection(&g_lock);
}

void ResumeGameJob(int id)
{
    if (!g_running)
        return;

    bool woken = false;

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_GAME_JOBS; ++i)
    {
        GameJob &job = g_jobs[i];
        if (!job.used || job.snap.id != id)
            continue;

        if (job.snap.state == QUEUE_PAUSED)
        {
            job.pause = false;
            job.snap.state = QUEUE_WAITING;
            SaveInstallStateLocked();
            woken = true;
        }
        else if (job.snap.state == QUEUE_ACTIVE && job.pause)
        {
            // Changed their mind before the download noticed - it never
            // stopped, so there is nothing to start again.
            job.pause = false;
            job.snap.phase[0] = '\0';
            SaveInstallStateLocked();
        }
        break;
    }
    LeaveCriticalSection(&g_lock);

    if (woken)
        SetEvent(g_wake);
}

void RemoveGameJob(int id)
{
    if (!g_running)
        return;

    char dir[320] = "";

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_GAME_JOBS; ++i)
    {
        // Only once main.cpp has seen it finish, so no popup goes missing.
        if (g_jobs[i].used && g_jobs[i].snap.id == id && g_jobs[i].snap.state == QUEUE_FINISHED &&
            g_jobs[i].handedOver)
        {
            g_jobs[i].used = false;

            // A job that failed or ran out of space keeps its download, for
            // choosing the game again to carry on from. Clearing the row is
            // the user saying they're done with it, so the pieces go with it
            // - unless the game has since been queued again, in which case
            // that job is downloading into them as we speak.
            const unsigned long key = StagingKey(g_jobs[i].request.item, g_jobs[i].request.zipName);
            bool inUse = false;
            for (int j = 0; j < MAX_GAME_JOBS && !inUse; ++j)
            {
                inUse = g_jobs[j].used && g_jobs[j].snap.state != QUEUE_FINISHED &&
                        StagingKey(g_jobs[j].request.item, g_jobs[j].request.zipName) == key;
            }

            if (!inUse)
            {
                char root[300];
                StagingRoot(g_gamesPath, root, sizeof(root));
                StagingDirFor(root, g_jobs[i].request, dir, sizeof(dir));
            }
        }
    }
    LeaveCriticalSection(&g_lock);

    if (dir[0] != '\0')
        DiscardStaging(dir);
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
