/*
FILE : FileJobs.cpp
PROJECT : Omni360
DESCRIPTION : The file manager's copies, moves and deletes - see FileJobs.h.
*/

#include "FileJobs.h"
#include "DiscWorker.h"   // SpinDiscForCopy, for copies off the disc
#include "FolderBrowse.h" // IsDiscPath
#include "Strings.h"
#include "OutputConsole.h"
#include "parsing.h" // LogEscapePercent
#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FILE_JOBS     64           // waiting, running and finished together - a ticked folder's worth
#define FILE_COPY_CHUNK   (1024 * 1024)
#define FILE_PATH_SIZE    MAX_PATH

// Fixed slots, never shuffled: the worker holds a reference to the job it's
// running while the Queue page removes finished ones around it.
struct FileJob
{
    bool used;
    int id;
    FileJobKind kind;
    QueueJobState state;
    QueueOutcome outcome;
    char source[FILE_PATH_SIZE];
    char destFolder[FILE_PATH_SIZE];
    char name[128];                 // the last part of source, which is what it's called
    char phase[64];
    unsigned long long done, total; // bytes for a copy or move, items for a delete
    unsigned long long bytesPerSec;
    DWORD startedAt;
    char resultText[64];
    char resultDetail[160];
    bool cancel;
    bool handedOver;                // its popup has been taken
};

static CRITICAL_SECTION g_lock;
static bool g_lockReady = false;
static FileJob g_jobs[MAX_FILE_JOBS];
static int g_nextId = FILE_JOB_ID_BASE;
static HANDLE g_thread = NULL;
static HANDLE g_wake = NULL;
static volatile LONG g_stop = 0;
static char *g_buffer = NULL;

static void Lock() { EnterCriticalSection(&g_lock); }
static void Unlock() { LeaveCriticalSection(&g_lock); }

static FileJob *FindJob(int id)
{
    for (int i = 0; i < MAX_FILE_JOBS; ++i)
    {
        if (g_jobs[i].used && g_jobs[i].id == id)
            return &g_jobs[i];
    }
    return NULL;
}

// The used slots in the order the jobs were asked for.
static int JobsInOrder(int *order)
{
    int n = 0;
    for (int i = 0; i < MAX_FILE_JOBS; ++i)
    {
        if (!g_jobs[i].used)
            continue;
        int at = n++;
        while (at > 0 && g_jobs[order[at - 1]].id > g_jobs[i].id)
        {
            order[at] = order[at - 1];
            at--;
        }
        order[at] = i;
    }
    return n;
}

static void LogPath(const char *what, const char *path)
{
    char escaped[FILE_PATH_SIZE * 2];
    dprintf("[files] %s %s\n", what, LogEscapePercent(path, escaped, sizeof(escaped)) ? escaped : "(long path)");
}

// ---------------------------------------------------------------------------
// The filesystem
// ---------------------------------------------------------------------------

static DWORD Attributes(const char *path)
{
    char probe[FILE_PATH_SIZE + 2];
    const size_t length = strlen(path);
    _snprintf(probe, sizeof(probe), (length > 0 && path[length - 1] == ':') ? "%s\\" : "%s", path);
    probe[sizeof(probe) - 1] = '\0';
    return GetFileAttributesA(probe);
}

static bool Exists(const char *path) { return Attributes(path) != (DWORD)-1; }

static bool IsFolder(const char *path)
{
    const DWORD a = Attributes(path);
    return a != (DWORD)-1 && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool Join(const char *folder, const char *name, char *out)
{
    const int n = _snprintf(out, FILE_PATH_SIZE, "%s\\%s", folder, name);
    out[FILE_PATH_SIZE - 1] = '\0';
    return n > 0 && n < FILE_PATH_SIZE;
}

// What a tree holds: its bytes and how many files and folders.
static void Measure(const char *path, unsigned long long *bytes, unsigned long long *items)
{
    if (!IsFolder(path))
    {
        WIN32_FIND_DATAA found;
        HANDLE h = FindFirstFileA(path, &found);
        if (h != INVALID_HANDLE_VALUE)
        {
            *bytes += ((unsigned long long)found.nFileSizeHigh << 32) | found.nFileSizeLow;
            FindClose(h);
        }
        (*items)++;
        return;
    }
    (*items)++;
    char pattern[FILE_PATH_SIZE];
    if (!Join(path, "*", pattern))
        return;
    WIN32_FIND_DATAA found;
    HANDLE h = FindFirstFileA(pattern, &found);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do
    {
        if (strcmp(found.cFileName, ".") == 0 || strcmp(found.cFileName, "..") == 0)
            continue;
        char child[FILE_PATH_SIZE];
        if (!Join(path, found.cFileName, child))
            continue;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            Measure(child, bytes, items);
        else
        {
            *bytes += ((unsigned long long)found.nFileSizeHigh << 32) | found.nFileSizeLow;
            (*items)++;
        }
    } while (FindNextFileA(h, &found));
    FindClose(h);
}

// Progress, under the lock, at most a few times a second.
static void Report(FileJob &job, unsigned long long done)
{
    static DWORD lastAt = 0;
    const DWORD now = GetTickCount();
    if (done != job.total && now - lastAt < 250)
        return;
    lastAt = now;
    Lock();
    job.done = done;
    const DWORD elapsed = now - job.startedAt;
    if (elapsed > 500 && job.kind != FILE_JOB_DELETE)
        job.bytesPerSec = done * 1000ULL / elapsed;
    Unlock();
}

static bool Cancelled(const FileJob &job)
{
    Lock();
    const bool c = job.cancel || InterlockedCompareExchange(&g_stop, 0, 0) != 0;
    Unlock();
    return c;
}

enum StepResult
{
    STEP_OK,
    STEP_READ_FAILED,
    STEP_WRITE_FAILED,
    STEP_CANCELLED
};

// Removes a file or a whole folder. done counts the items as they go, for a
// delete's bar; NULL when it's tidying up after a failed copy.
static bool RemoveTree(FileJob *job, const char *path, unsigned long long *done)
{
    if (!IsFolder(path))
    {
        SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
        const bool ok = DeleteFileA(path) != 0;
        if (done != NULL)
        {
            (*done)++;
            Report(*job, *done);
        }
        return ok;
    }
    bool ok = true;
    char pattern[FILE_PATH_SIZE];
    if (Join(path, "*", pattern))
    {
        WIN32_FIND_DATAA found;
        HANDLE h = FindFirstFileA(pattern, &found);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (strcmp(found.cFileName, ".") == 0 || strcmp(found.cFileName, "..") == 0)
                    continue;
                char child[FILE_PATH_SIZE];
                if (Join(path, found.cFileName, child) && !RemoveTree(job, child, done))
                    ok = false;
                if (done != NULL && Cancelled(*job))
                {
                    FindClose(h);
                    return false;
                }
            } while (FindNextFileA(h, &found));
            FindClose(h);
        }
    }
    if (!RemoveDirectoryA(path))
        ok = false;
    if (done != NULL)
    {
        (*done)++;
        Report(*job, *done);
    }
    return ok;
}

// Copies a file or a folder's whole tree to dest, which mustn't exist yet.
static StepResult CopyTree(FileJob &job, const char *source, const char *dest, unsigned long long *done,
                           char *failedName)
{
    if (IsFolder(source))
    {
        if (!CreateDirectoryA(dest, NULL))
        {
            _snprintf(failedName, FILE_PATH_SIZE, "%s", dest);
            return STEP_WRITE_FAILED;
        }
        char pattern[FILE_PATH_SIZE];
        if (!Join(source, "*", pattern))
            return STEP_READ_FAILED;
        WIN32_FIND_DATAA found;
        HANDLE h = FindFirstFileA(pattern, &found);
        if (h == INVALID_HANDLE_VALUE)
            return STEP_OK; // an empty folder
        StepResult result = STEP_OK;
        do
        {
            if (strcmp(found.cFileName, ".") == 0 || strcmp(found.cFileName, "..") == 0)
                continue;
            char from[FILE_PATH_SIZE], to[FILE_PATH_SIZE];
            if (!Join(source, found.cFileName, from) || !Join(dest, found.cFileName, to))
            {
                _snprintf(failedName, FILE_PATH_SIZE, "%s", found.cFileName);
                result = STEP_WRITE_FAILED;
                break;
            }
            result = CopyTree(job, from, to, done, failedName);
        } while (result == STEP_OK && FindNextFileA(h, &found));
        FindClose(h);
        return result;
    }

    HANDLE in = CreateFileA(source, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (in == INVALID_HANDLE_VALUE)
    {
        _snprintf(failedName, FILE_PATH_SIZE, "%s", source);
        return STEP_READ_FAILED;
    }
    HANDLE out = CreateFileA(dest, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE)
    {
        CloseHandle(in);
        _snprintf(failedName, FILE_PATH_SIZE, "%s", dest);
        return STEP_WRITE_FAILED;
    }

    StepResult result = STEP_OK;
    for (;;)
    {
        if (Cancelled(job))
        {
            result = STEP_CANCELLED;
            break;
        }
        DWORD got = 0;
        if (!ReadFile(in, g_buffer, FILE_COPY_CHUNK, &got, NULL))
        {
            _snprintf(failedName, FILE_PATH_SIZE, "%s", source);
            result = STEP_READ_FAILED;
            break;
        }
        if (got == 0)
            break;
        DWORD written = 0;
        if (!WriteFile(out, g_buffer, got, &written, NULL) || written != got)
        {
            _snprintf(failedName, FILE_PATH_SIZE, "%s", dest);
            result = STEP_WRITE_FAILED;
            break;
        }
        *done += got;
        Report(job, *done);
    }
    CloseHandle(in);
    CloseHandle(out);
    return result;
}

// "Hdd1" of "Hdd1:\Games": the same drive, a move is a rename.
static bool SameDrive(const char *a, const char *b)
{
    const char *ca = strchr(a, ':');
    const char *cb = strchr(b, ':');
    return ca != NULL && cb != NULL && (ca - a) == (cb - b) && _strnicmp(a, b, ca - a) == 0;
}

static void Finish(FileJob &job, QueueOutcome outcome, StringId text, const char *detail)
{
    Lock();
    job.state = QUEUE_FINISHED;
    job.outcome = outcome;
    _snprintf(job.resultText, sizeof(job.resultText), "%s", Tr(text));
    _snprintf(job.resultDetail, sizeof(job.resultDetail), "%s", detail != NULL ? detail : "");
    job.resultText[sizeof(job.resultText) - 1] = job.resultDetail[sizeof(job.resultDetail) - 1] = '\0';
    Unlock();
}

static void SetPhase(FileJob &job, StringId phase, unsigned long long total)
{
    Lock();
    _snprintf(job.phase, sizeof(job.phase), "%s", Tr(phase));
    job.phase[sizeof(job.phase) - 1] = '\0';
    job.done = 0;
    job.total = total;
    job.bytesPerSec = 0;
    job.startedAt = GetTickCount();
    Unlock();
}

// The last part of a path, for a message.
static const char *LastPart(const char *path)
{
    const char *slash = strrchr(path, '\\');
    return slash != NULL ? slash + 1 : path;
}

static void RunDelete(FileJob &job)
{
    LogPath("deleting", job.source);
    unsigned long long bytes = 0, items = 0;
    SetPhase(job, STR_FILES_COUNTING, 0);
    Measure(job.source, &bytes, &items);
    SetPhase(job, STR_FILES_DELETING, items);
    unsigned long long done = 0;
    const bool ok = RemoveTree(&job, job.source, &done);
    if (Cancelled(job))
        Finish(job, QUEUE_OUTCOME_CANCELLED, STR_FILES_CANCELLED, Tr(STR_FILES_SOME_WERE_ALREADY_DELETED));
    else if (!ok)
        Finish(job, QUEUE_OUTCOME_FAILED, STR_FILES_COULDNT_DELETE, Tr(STR_FILES_SOME_COULDNT_BE_REMOVED));
    else
        Finish(job, QUEUE_OUTCOME_INSTALLED, STR_FILES_DELETED, NULL);
}

static void RunCopyOrMove(FileJob &job)
{
    const bool move = (job.kind == FILE_JOB_MOVE);
    const StringId failed = move ? STR_FILES_COULDNT_MOVE : STR_FILES_COULDNT_COPY;
    char dest[FILE_PATH_SIZE];
    if (!Join(job.destFolder, job.name, dest))
    {
        Finish(job, QUEUE_OUTCOME_FAILED, failed, Tr(STR_FILES_PATH_TOO_LONG));
        return;
    }
    LogPath(move ? "moving" : "copying", job.source);
    LogPath("  to", dest);

    if (Exists(dest))
    {
        Finish(job, QUEUE_OUTCOME_FAILED, failed, Tr(STR_FILES_ALREADY_THERE));
        return;
    }
    // A folder into itself, or one of its own folders, would never end.
    const size_t sourceLength = strlen(job.source);
    if (_strnicmp(dest, job.source, sourceLength) == 0 && (dest[sourceLength] == '\\' || dest[sourceLength] == '\0'))
    {
        Finish(job, QUEUE_OUTCOME_FAILED, failed, Tr(STR_FILES_INTO_ITSELF));
        return;
    }

    // On the same drive a move is a rename: instant, whatever the size.
    if (move && SameDrive(job.source, dest))
    {
        SetPhase(job, STR_FILES_MOVING, 0);
        if (MoveFileA(job.source, dest))
            Finish(job, QUEUE_OUTCOME_INSTALLED, STR_FILES_MOVED, NULL);
        else
            Finish(job, QUEUE_OUTCOME_FAILED, failed, NULL);
        return;
    }

    unsigned long long bytes = 0, items = 0;
    SetPhase(job, STR_FILES_COUNTING, 0);
    Measure(job.source, &bytes, &items);

    char root[8];
    const char *colon = strchr(job.destFolder, ':');
    if (colon != NULL && colon - job.destFolder < 5)
    {
        _snprintf(root, sizeof(root), "%.*s\\", (int)(colon - job.destFolder + 1), job.destFolder);
        root[sizeof(root) - 1] = '\0';
        ULARGE_INTEGER freeToCaller;
        if (GetDiskFreeSpaceExA(root, &freeToCaller, NULL, NULL) && freeToCaller.QuadPart < bytes)
        {
            Finish(job, QUEUE_OUTCOME_NO_SPACE, failed, Tr(STR_FILES_NOT_ENOUGH_SPACE));
            return;
        }
    }

    SetPhase(job, move ? STR_FILES_MOVING : STR_FILES_COPYING, bytes);
    // Off the disc, the drive spins up as it does for a disc install -
    // left alone it reads at about a quarter of the speed - and back down
    // once the copy is done, however it ended.
    const bool fromDisc = IsDiscPath(job.source);
    if (fromDisc)
        SpinDiscForCopy();
    unsigned long long done = 0;
    char failedName[FILE_PATH_SIZE] = "";
    const StepResult result = CopyTree(job, job.source, dest, &done, failedName);
    if (fromDisc)
        QuietDiscAfterCopy();
    if (result != STEP_OK)
    {
        // Whatever got there is removed: half a game is worse than none.
        RemoveTree(&job, dest, NULL);
        char detail[160];
        if (result == STEP_CANCELLED)
        {
            Finish(job, QUEUE_OUTCOME_CANCELLED, STR_FILES_CANCELLED, Tr(STR_FILES_NOTHING_WAS_LEFT));
            return;
        }
        TrFormat(detail, sizeof(detail), result == STEP_READ_FAILED ? STR_FILES_COULDNT_READ : STR_FILES_COULDNT_WRITE,
                 LastPart(failedName));
        detail[sizeof(detail) - 1] = '\0';
        Finish(job, QUEUE_OUTCOME_FAILED, failed, detail);
        return;
    }

    if (move)
    {
        SetPhase(job, STR_FILES_REMOVING_ORIGINAL, items);
        unsigned long long removed = 0;
        if (!RemoveTree(&job, job.source, &removed))
        {
            Finish(job, QUEUE_OUTCOME_PARTIAL, STR_FILES_MOVED, Tr(STR_FILES_ORIGINAL_NOT_ALL_REMOVED));
            return;
        }
    }
    Finish(job, QUEUE_OUTCOME_INSTALLED, move ? STR_FILES_MOVED : STR_FILES_COPIED, NULL);
}

static DWORD WINAPI WorkerThread(LPVOID)
{
    for (;;)
    {
        WaitForSingleObject(g_wake, INFINITE);
        for (;;)
        {
            if (InterlockedCompareExchange(&g_stop, 0, 0) != 0)
                return 0;
            FileJob *job = NULL;
            Lock();
            int order[MAX_FILE_JOBS];
            const int count = JobsInOrder(order);
            for (int i = 0; i < count && job == NULL; ++i)
            {
                if (g_jobs[order[i]].state == QUEUE_WAITING)
                    job = &g_jobs[order[i]];
            }
            if (job != NULL)
                job->state = QUEUE_ACTIVE;
            Unlock();
            if (job == NULL)
                break;
            if (job->kind == FILE_JOB_DELETE)
                RunDelete(*job);
            else
                RunCopyOrMove(*job);
        }
    }
}

// ---------------------------------------------------------------------------
// The queue
// ---------------------------------------------------------------------------

bool StartFileJobs()
{
    if (!g_lockReady)
    {
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
    }
    if (g_thread != NULL)
        return true;
    g_buffer = (char *)malloc(FILE_COPY_CHUNK);
    g_wake = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (g_buffer == NULL || g_wake == NULL)
        return false;
    InterlockedExchange(&g_stop, 0);
    g_thread = CreateThread(NULL, 64 * 1024, WorkerThread, NULL, 0, NULL);
    return g_thread != NULL;
}

void StopFileJobs(unsigned long timeoutMs)
{
    if (g_thread == NULL)
        return;
    InterlockedExchange(&g_stop, 1);
    SetEvent(g_wake);
    WaitForSingleObject(g_thread, timeoutMs);
    CloseHandle(g_thread);
    g_thread = NULL;
}

// A slot for one more: an empty one, or else the oldest finished job whose
// popup has gone. -1 when every slot is a job still to do.
static int FreeSlot()
{
    for (int i = 0; i < MAX_FILE_JOBS; ++i)
    {
        if (!g_jobs[i].used)
            return i;
    }
    int order[MAX_FILE_JOBS];
    const int count = JobsInOrder(order);
    for (int i = 0; i < count; ++i)
    {
        if (g_jobs[order[i]].state == QUEUE_FINISHED && g_jobs[order[i]].handedOver)
            return order[i];
    }
    return -1;
}

bool EnqueueFileJob(FileJobKind kind, const char *source, const char *destFolder)
{
    if (g_thread == NULL || source == NULL || strlen(source) >= FILE_PATH_SIZE ||
        (destFolder != NULL && strlen(destFolder) >= FILE_PATH_SIZE))
        return false;
    Lock();
    const int slot = FreeSlot();
    if (slot < 0)
    {
        Unlock();
        return false;
    }
    FileJob &job = g_jobs[slot];
    memset(&job, 0, sizeof(job));
    job.used = true;
    job.id = g_nextId++;
    if (g_nextId >= 1000000)
        g_nextId = FILE_JOB_ID_BASE;
    job.kind = kind;
    job.state = QUEUE_WAITING;
    strcpy(job.source, source);
    if (destFolder != NULL)
        strcpy(job.destFolder, destFolder);
    _snprintf(job.name, sizeof(job.name), "%s", LastPart(source));
    job.name[sizeof(job.name) - 1] = '\0';
    Unlock();
    SetEvent(g_wake);
    return true;
}

static void Snapshot(const FileJob &job, QueueJobSnapshot &out)
{
    memset(&out, 0, sizeof(out));
    out.id = job.id;
    out.kind = QUEUE_JOB_FILE;
    out.state = job.state;
    out.outcome = job.outcome;
    _snprintf(out.gameName, sizeof(out.gameName), "%s", job.name);
    if (job.kind == FILE_JOB_DELETE)
        TrFormat(out.title, sizeof(out.title), STR_FILES_DELETE_FROM, job.source);
    else
        TrFormat(out.title, sizeof(out.title), job.kind == FILE_JOB_MOVE ? STR_FILES_MOVE_TO : STR_FILES_COPY_TO,
                 job.destFolder);
    out.gameName[sizeof(out.gameName) - 1] = out.title[sizeof(out.title) - 1] = '\0';
    _snprintf(out.phase, sizeof(out.phase), "%s", job.phase);
    if (job.kind != FILE_JOB_DELETE)
    {
        out.bytesDone = job.done;
        out.bytesTotal = job.total;
        out.bytesPerSec = job.bytesPerSec;
        if (job.bytesPerSec > 0 && job.total > job.done)
            out.secondsLeft = (job.total - job.done) / job.bytesPerSec;
    }
    out.fraction = job.total > 0 ? (float)((double)job.done / (double)job.total) : -1.0f;
    out.stepFraction = out.fraction;
    _snprintf(out.resultText, sizeof(out.resultText), "%s", job.resultText);
    _snprintf(out.resultDetail, sizeof(out.resultDetail), "%s", job.resultDetail);
    out.notify = job.outcome != QUEUE_OUTCOME_CANCELLED;
}

int SnapshotFileJobs(QueueJobSnapshot *out, int maxJobs)
{
    if (!g_lockReady)
        return 0;
    Lock();
    int order[MAX_FILE_JOBS];
    const int count = JobsInOrder(order);
    int n = 0;
    for (int i = 0; i < count && n < maxJobs; ++i)
        Snapshot(g_jobs[order[i]], out[n++]);
    Unlock();
    return n;
}

int PendingFileJobCount()
{
    if (!g_lockReady)
        return 0;
    Lock();
    int n = 0;
    for (int i = 0; i < MAX_FILE_JOBS; ++i)
    {
        if (g_jobs[i].used && (g_jobs[i].state == QUEUE_WAITING || g_jobs[i].state == QUEUE_ACTIVE))
            n++;
    }
    Unlock();
    return n;
}

void CancelFileJob(int id)
{
    Lock();
    FileJob *job = FindJob(id);
    if (job != NULL)
    {
        if (job->state == QUEUE_WAITING)
        {
            job->state = QUEUE_FINISHED;
            job->outcome = QUEUE_OUTCOME_CANCELLED;
            _snprintf(job->resultText, sizeof(job->resultText), "%s", Tr(STR_FILES_CANCELLED));
            job->resultDetail[0] = '\0';
        }
        else
            job->cancel = true;
    }
    Unlock();
}

void RemoveFileJob(int id)
{
    Lock();
    FileJob *job = FindJob(id);
    if (job != NULL && job->state == QUEUE_FINISHED)
        job->used = false;
    Unlock();
}

bool TakeFinishedFileJob(QueueJobSnapshot *out)
{
    if (!g_lockReady)
        return false;
    bool found = false;
    Lock();
    for (int i = 0; i < MAX_FILE_JOBS && !found; ++i)
    {
        if (g_jobs[i].used && g_jobs[i].state == QUEUE_FINISHED && !g_jobs[i].handedOver)
        {
            g_jobs[i].handedOver = true;
            Snapshot(g_jobs[i], *out);
            found = true;
        }
    }
    Unlock();
    return found;
}
