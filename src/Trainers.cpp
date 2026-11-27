/*
FILE : Trainers.cpp
PROJECT : Omni360
DESCRIPTION : Loading Aurora's trainers (XYZEngine) for a game. See Trainers.h.
*/

#include "Trainers.h"
#include "Launch.h"        // StartSystemModule
#include "OutputConsole.h" // dprintf
#include "cJSON.h"
#include "downloadFile.h"  // HttpsSession: the trainer pack, on GitHub

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <string>

extern "C" LONG XexGetModuleHandle(PSZ moduleName, PHANDLE outHandle);
extern "C" LONG XexGetProcedureAddress(HANDLE module, DWORD ordinal, PVOID *outAddress);

// How one works, read off ones loaded by Aurora (2026-11-27):
//
// A trainer (XYZEngine, by XYZMods) is a DLL XEX that runs as a system module
// at its own base, 0x91020000, so only one can be loaded at a time. Loading
// it runs its DllMain, which only describes it: a JSON block in its image
// ("framework" 1.0 - the game, the title updates it works with, its name,
// author and cheats). Exports:
//   2  Start(TRAINER_MAGIC, cheat bits, 0): bit n turns cheat n on; starts
//      three system threads. Once a second the first compares the running
//      title with the trainer's (title ID, media ID, base version, disc,
//      title update) - 4 seconds of another title and it gives up and
//      unloads itself - then the others apply the cheats.
//   3  Stop(keepLoaded), 4 unload, 6 its info as a binary block, 8 running?
// It registers with Aurora's Nova if that's loaded, and carries on without.
// So, as Aurora does: load it, Start it, and start the game straight after.
// Once its game has ended it notices within a few seconds and unloads itself.
//
// Omni360 never unloads one itself. Done from this title (the kernel's
// XexUnloadImage, its load count set from DashLaunch's 65535 to 1, as the
// trainer's own unload does) it went - but launching a game afterwards hung
// as Omni360 ended, every time. One loaded only to be read is started with
// no cheats instead: it finds Omni360 running, not its game, and unloads
// itself as it would after its game - harmlessly, as no cheat hooks anything.
//
// Some are buggy: Dead Space's has two cheats that hook again every 60 ms and
// crash the game within seconds. So nothing is turned on but what's chosen,
// or what the trainer itself has on by default.

typedef DWORD (*TrainerStartFn)(DWORD magic, DWORD cheats, DWORD unused);

#define TRAINER_MAGIC         0x41554F41
#define TRAINER_ORDINAL_START 2
#define TRAINER_FOLDER        "Hdd1:\\Omni360\\Trainers"
#define TRAINER_COPY          "Hdd1:\\Omni360\\Omni360Trainer.xex"
#define TRAINER_DEVICE        "\\Device\\Harddisk0\\Partition1\\Omni360\\Omni360Trainer.xex"
#define TRAINER_MODULE        "Omni360Trainer.xex"

// The kernel's record of a loaded module (LDR_DATA_TABLE_ENTRY), as far as
// it's needed here.
struct ModuleRecord
{
    LIST_ENTRY links[3];
    PVOID ntHeaders;     // 0x18
    PVOID imageBase;     // 0x1C
    DWORD sizeOfNtImage; // 0x20
};

static bool CopyFileBytes(const char *from, const char *to)
{
    FILE *in = NULL, *out = NULL;
    if (fopen_s(&in, from, "rb") != 0 || in == NULL)
        return false;
    if (fopen_s(&out, to, "wb") != 0 || out == NULL)
    {
        fclose(in);
        return false;
    }
    unsigned char buffer[16 * 1024];
    size_t got;
    bool ok = true;
    while (ok && (got = fread(buffer, 1, sizeof(buffer), in)) > 0)
        ok = fwrite(buffer, 1, got, out) == got;
    fclose(in);
    return fclose(out) == 0 && ok;
}

static void CopyText(char *out, size_t outSize, const char *text)
{
    _snprintf(out, outSize, "%s", text != NULL ? text : "");
    out[outSize - 1] = '\0';
}

// The trainer that's loaded - ours, or one Aurora started - or NULL.
static HANDLE LoadedTrainer()
{
    HANDLE loaded = NULL;
    if (XexGetModuleHandle((PSZ)TRAINER_MODULE, &loaded) >= 0 && loaded != NULL)
        return loaded;
    loaded = NULL;
    if (XexGetModuleHandle("dll.aurora.trainer", &loaded) >= 0 && loaded != NULL)
        return loaded;
    return NULL;
}

static bool WaitForTrainerToGo(DWORD timeoutMs)
{
    const DWORD started = GetTickCount();
    while (LoadedTrainer() != NULL)
    {
        if (GetTickCount() - started >= timeoutMs)
            return false;
        Sleep(100);
    }
    return true;
}

// Whether no trainer is loaded - after giving the last one time to go.
//
// One stops itself once its game has ended, or once it's been started with
// no cheats here: its watch sees a title not its own for a few seconds, and
// its own thread calls Stop(0), which unloads it. So it's only still here if
// that was moments ago - and it's waited for.
//
// It isn't asked to stop through its exports: Stop(1) (export 3) ends its
// threads, but then its watching thread unloads it - while the call, which
// sleeps a second before returning, may still be running in its code.
static bool TrainerGone()
{
    if (LoadedTrainer() == NULL)
        return true;
    dprintf("[trainer] the last one is still loaded - waiting for it to stop itself\n");
    const bool gone = WaitForTrainerToGo(12000);
    dprintf("[trainer] %s\n", gone ? "it has gone" : "it's still loaded after 12 s");
    return gone;
}

// The trainer at path, loaded (not started). NULL if it couldn't be.
static HANDLE LoadTrainer(const char *path)
{
    if (!TrainerGone())
    {
        dprintf("[trainer] the last one is still loaded - not loading %s\n", path);
        return NULL;
    }
    CreateDirectoryA("Hdd1:\\Omni360", NULL);
    if (!CopyFileBytes(path, TRAINER_COPY))
    {
        dprintf("[trainer] couldn't copy %s to %s\n", path, TRAINER_COPY);
        return NULL;
    }
    DWORD status = 0;
    if (!StartSystemModule(TRAINER_DEVICE, &status, "trainer"))
        return NULL;
    HANDLE loaded = NULL;
    if (XexGetModuleHandle((PSZ)TRAINER_MODULE, &loaded) < 0 || loaded == NULL)
    {
        dprintf("[trainer] %s didn't load: status %08lX\n", path, (unsigned long)status);
        return NULL;
    }
    return loaded;
}

// Starts a loaded trainer: with the cheats to play with, or with none so it
// goes again (see above).
static bool StartTrainer(HANDLE module, DWORD cheats)
{
    PVOID start = NULL;
    if (XexGetProcedureAddress(module, TRAINER_ORDINAL_START, &start) < 0 || start == NULL)
    {
        dprintf("[trainer] it has no Start export\n");
        return false;
    }
    const DWORD started = ((TrainerStartFn)start)(TRAINER_MAGIC, cheats, 0);
    dprintf("[trainer] Start(cheats %08lX) -> %lu\n", (unsigned long)cheats, (unsigned long)started);
    return started != 0;
}

static long JsonLong(const cJSON *object, const char *name, long fallback)
{
    const cJSON *item = cJSON_GetObjectItem(object, name);
    return (item != NULL && cJSON_IsNumber(item)) ? (long)item->valuedouble : fallback;
}

// The trainer's JSON, found in its image: DllMain writes it there.
static const char *FindTrainerJson(HANDLE module)
{
    const ModuleRecord *record = (const ModuleRecord *)module;
    const char *base = (const char *)record->imageBase;
    const DWORD size = record->sizeOfNtImage;
    if ((DWORD)base < 0x90000000 || (DWORD)base >= 0xA0000000 || size < 0x1000 || size > 0x200000)
    {
        dprintf("[trainer] unexpected image: %p, %lu bytes\n", base, (unsigned long)size);
        return NULL;
    }

    static const char key[] = "\"framework\"";
    for (DWORD at = 0; at + sizeof(key) < size; ++at)
    {
        if (base[at] != '"' || memcmp(base + at, key, sizeof(key) - 1) != 0)
            continue;
        // Back to the brace that opens it all: { "header": { "framework" ...
        DWORD start = at;
        int braces = 0;
        while (start > 0 && at - start < 64)
        {
            --start;
            if (base[start] == '{' && ++braces == 2)
                return base + start;
        }
    }
    return NULL;
}

static bool ReadJson(const char *json, TrainerInfo *out)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL)
    {
        dprintf("[trainer] its JSON didn't parse\n");
        return false;
    }
    const cJSON *exec = cJSON_GetObjectItem(root, "execinfo");
    const cJSON *meta = cJSON_GetObjectItem(root, "metadata");
    if (exec == NULL || meta == NULL)
    {
        cJSON_Delete(root);
        return false;
    }
    out->titleId = (unsigned long)JsonLong(exec, "titleid", 0);
    out->mediaId = JsonLong(exec, "mediaid", -1);
    out->baseVersion = JsonLong(exec, "basever", -1);
    out->disc = JsonLong(exec, "discnum", -1);
    const cJSON *tus = cJSON_GetObjectItem(exec, "tuver");
    out->tuCount = 0;
    for (const cJSON *tu = tus != NULL ? tus->child : NULL; tu != NULL && out->tuCount < MAX_TRAINER_TUS; tu = tu->next)
        out->tus[out->tuCount++] = (long)tu->valuedouble;

    CopyText(out->name, sizeof(out->name), cJSON_GetStringValue(cJSON_GetObjectItem(meta, "displayname")));
    CopyText(out->author, sizeof(out->author), cJSON_GetStringValue(cJSON_GetObjectItem(meta, "author")));
    CopyText(out->description, sizeof(out->description), cJSON_GetStringValue(cJSON_GetObjectItem(meta, "description")));
    CopyText(out->version, sizeof(out->version), cJSON_GetStringValue(cJSON_GetObjectItem(meta, "version")));
    const cJSON *cheats = cJSON_GetObjectItem(meta, "cheats");
    out->cheatCount = 0;
    for (const cJSON *cheat = cheats != NULL ? cheats->child : NULL; cheat != NULL && out->cheatCount < MAX_TRAINER_CHEATS;
         cheat = cheat->next)
    {
        CopyText(out->cheats[out->cheatCount], sizeof(out->cheats[0]),
                 cJSON_GetStringValue(cJSON_GetObjectItem(cheat, "text")));
        out->defaults[out->cheatCount] = cJSON_IsTrue(cJSON_GetObjectItem(cheat, "default"));
        out->cheatCount++;
    }
    cJSON_Delete(root);
    return true;
}

// What a trainer said about itself is kept beside it, so it's loaded only
// the first time: info.json, the trainer's own JSON, with the file it came
// from and that file's size added to tell when it's been replaced.
static void InfoPath(const char *trainerPath, char *out, size_t outSize)
{
    CopyText(out, outSize, trainerPath);
    char *slash = strrchr(out, '\\');
    if (slash != NULL)
        CopyText(slash + 1, outSize - (size_t)(slash + 1 - out), "info.json");
}

static unsigned long FileSizeOf(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data))
        return 0;
    return data.nFileSizeLow;
}

static const char *FileNameOf(const char *path)
{
    const char *slash = strrchr(path, '\\');
    return slash != NULL ? slash + 1 : path;
}

static bool ReadKeptInfo(const char *path, TrainerInfo *out)
{
    char infoPath[320];
    InfoPath(path, infoPath, sizeof(infoPath));
    FILE *file = NULL;
    if (fopen_s(&file, infoPath, "rb") != 0 || file == NULL)
        return false;
    static char text[16 * 1024];
    const size_t got = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[got] = '\0';

    cJSON *root = cJSON_Parse(text);
    if (root == NULL)
        return false;
    const char *from = cJSON_GetStringValue(cJSON_GetObjectItem(root, "omni360file"));
    const long size = JsonLong(root, "omni360size", -1);
    const bool current = from != NULL && _stricmp(from, FileNameOf(path)) == 0 && size == (long)FileSizeOf(path);
    cJSON_Delete(root);
    return current && ReadJson(text, out);
}

static void KeepInfo(const char *path, const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL)
        return;
    cJSON_AddStringToObject(root, "omni360file", FileNameOf(path));
    cJSON_AddNumberToObject(root, "omni360size", (double)FileSizeOf(path));
    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (text == NULL)
        return;
    char infoPath[320];
    InfoPath(path, infoPath, sizeof(infoPath));
    FILE *file = NULL;
    if (fopen_s(&file, infoPath, "wb") == 0 && file != NULL)
    {
        fwrite(text, 1, strlen(text), file);
        fclose(file);
    }
    cJSON_free(text);
}

static void LogInfo(const TrainerInfo &info, const char *how)
{
    dprintf("[trainer] %s (%s): \"%s\" by %s, v%s, title %08lX, %d TUs (first %ld), %d cheats\n", info.path, how,
            info.name, info.author, info.version, info.titleId, info.tuCount, info.tuCount > 0 ? info.tus[0] : -1L,
            info.cheatCount);
    for (int i = 0; i < info.cheatCount; ++i)
        dprintf("[trainer]   %d) %s%s\n", i, info.cheats[i], info.defaults[i] ? " (on)" : "");
}

bool FindTrainer(unsigned long titleId, char *path, size_t pathSize)
{
    char pattern[128];
    _snprintf(pattern, sizeof(pattern), "%s\\%08lX\\*.xex", TRAINER_FOLDER, titleId);
    pattern[sizeof(pattern) - 1] = '\0';
    WIN32_FIND_DATAA found;
    HANDLE find = FindFirstFileA(pattern, &found);
    if (find == INVALID_HANDLE_VALUE)
        return false;
    FindClose(find);
    _snprintf(path, pathSize, "%s\\%08lX\\%s", TRAINER_FOLDER, titleId, found.cFileName);
    path[pathSize - 1] = '\0';
    return true;
}

bool ReadTrainerInfo(const char *path, TrainerInfo *out)
{
    memset(out, 0, sizeof(*out));
    if (ReadKeptInfo(path, out))
    {
        CopyText(out->path, sizeof(out->path), path);
        LogInfo(*out, "kept");
        return true;
    }

    memset(out, 0, sizeof(*out));
    HANDLE module = LoadTrainer(path);
    if (module == NULL)
        return false;
    const char *json = FindTrainerJson(module);
    bool ok = false;
    if (json != NULL)
    {
        ok = ReadJson(json, out);
        if (ok)
            KeepInfo(path, json);
    }
    else
    {
        dprintf("[trainer] no JSON in its image\n");
    }
    StartTrainer(module, 0); // so it goes again
    CopyText(out->path, sizeof(out->path), path);
    if (ok)
        LogInfo(*out, "loaded");
    return ok;
}

// The cheats chosen for a game: Hdd1:\Omni360\Trainers\<title ID>\cheats.txt,
// the bits in hex.
static void CheatsPath(unsigned long titleId, char *out, size_t outSize)
{
    _snprintf(out, outSize, "%s\\%08lX\\cheats.txt", TRAINER_FOLDER, titleId);
    out[outSize - 1] = '\0';
}

bool LoadTrainerCheats(unsigned long titleId, unsigned long *bits)
{
    char path[128];
    CheatsPath(titleId, path, sizeof(path));
    FILE *file = NULL;
    if (fopen_s(&file, path, "r") != 0 || file == NULL)
        return false;
    unsigned long value = 0;
    const bool ok = fscanf_s(file, "%lx", &value) == 1;
    fclose(file);
    *bits = value;
    return ok;
}

bool SaveTrainerCheats(unsigned long titleId, unsigned long bits)
{
    char path[128];
    CheatsPath(titleId, path, sizeof(path));
    FILE *file = NULL;
    if (fopen_s(&file, path, "w") != 0 || file == NULL)
        return false;
    const bool ok = fprintf(file, "%lx\n", bits) > 0;
    return fclose(file) == 0 && ok;
}

// Title IDs shared by two games, and a piece of the name of the one the
// pack's trainers aren't for.
struct SharedTitle
{
    unsigned long titleId;
    const char *notFor;
};
static const SharedTitle kSharedTitles[] = {
    { 0x545407F2, "Episodes from Liberty City" }, // the pack's is "GTA 4"
};

bool TrainersFitGame(unsigned long titleId, const char *gameName)
{
    for (size_t i = 0; i < sizeof(kSharedTitles) / sizeof(kSharedTitles[0]); ++i)
    {
        if (kSharedTitles[i].titleId != titleId || gameName == NULL)
            continue;
        // Case-blind: package names come in every case.
        const size_t want = strlen(kSharedTitles[i].notFor);
        for (const char *at = gameName; *at != '\0'; ++at)
            if (_strnicmp(at, kSharedTitles[i].notFor, want) == 0)
                return false;
    }
    return true;
}

bool TrainerSetToStart(unsigned long titleId, const char *gameName)
{
    char trainer[300];
    if (!TrainersFitGame(titleId, gameName) || !FindTrainer(titleId, trainer, sizeof(trainer)))
        return false;
    unsigned long bits = 0;
    if (LoadTrainerCheats(titleId, &bits))
        return bits != 0;
    // Nothing chosen: its defaults, from what it said about itself when it
    // was read - one never read has none known, and the picker reads it.
    TrainerInfo info;
    if (!ReadKeptInfo(trainer, &info))
        return false;
    for (int i = 0; i < info.cheatCount; ++i)
        if (info.defaults[i])
            return true;
    return false;
}

static bool g_skipNextTrainer = false;

void SkipTrainerNextLaunch()
{
    g_skipNextTrainer = true;
}

void StartTrainerFor(unsigned long titleId, const char *gameName)
{
    const bool skip = g_skipNextTrainer;
    g_skipNextTrainer = false; // one launch only, whichever game it is
    char trainer[300];
    if (!FindTrainer(titleId, trainer, sizeof(trainer)))
        return; // none for this game - the usual case
    if (skip)
    {
        dprintf("[trainer] playing %08lX without its trainer this time\n", titleId);
        return;
    }
    if (!TrainersFitGame(titleId, gameName))
    {
        dprintf("[trainer] %08lX's trainer isn't for %s - not starting it\n", titleId, gameName);
        return;
    }

    unsigned long cheats = 0;
    if (!LoadTrainerCheats(titleId, &cheats))
    {
        // Nothing chosen yet: the trainer's own defaults.
        TrainerInfo info;
        if (ReadTrainerInfo(trainer, &info))
            for (int i = 0; i < info.cheatCount; ++i)
                if (info.defaults[i])
                    cheats |= 1UL << i;
    }
    if (cheats == 0)
    {
        dprintf("[trainer] no cheats chosen for %08lX - not starting its trainer\n", titleId);
        return;
    }

    HANDLE loaded = LoadTrainer(trainer);
    if (loaded == NULL)
        return;
    dprintf("[trainer] %s loaded at %p\n", trainer, loaded);
    StartTrainer(loaded, cheats);
}

// ---------------------------------------------------------------------------
// The community trainer pack, on GitHub
// ---------------------------------------------------------------------------
//
// Its trainers are at Aurora/user/Trainers/<title ID>/<file>.xex. One request
// for the repository's whole file list (git trees, ~340 KB of JSON) gives
// every one of them; a trainer comes from the contents API as raw bytes. Both
// are api.github.com, which the update check already talks to - nothing has
// to be trusted beyond it. 60 requests an hour are allowed without an
// account: a list a day and the odd trainer is far inside that.

#define PACK_REPO      "StonedModder/Aurora-JTAG-RGH-Trainer-Pack-Repository"
#define PACK_LIST_URL  "https://api.github.com/repos/" PACK_REPO "/git/trees/main?recursive=1"
#define PACK_FILE_URL  "https://api.github.com/repos/" PACK_REPO "/contents/"
#define PACK_PREFIX    "Aurora/user/Trainers/"
#define PACK_LIST_FILE "Hdd1:\\Omni360\\TrainerPack.txt"
#define PACK_LIST_MAX  (1024 * 1024)
#define PACK_FILE_MAX  (1024 * 1024)
#define PACK_FRESH_FOR (24ULL * 60 * 60 * 10000000) // a day, in FILETIME units

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

static unsigned long long FileTimeValue(const FILETIME &t)
{
    return ((unsigned long long)t.dwHighDateTime << 32) | t.dwLowDateTime;
}

// Whether the kept list is less than a day old.
static bool PackListFresh()
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(PACK_LIST_FILE, GetFileExInfoStandard, &data))
        return false;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const unsigned long long at = FileTimeValue(data.ftLastWriteTime), today = FileTimeValue(now);
    return today >= at && today - at < PACK_FRESH_FOR;
}

// "Aurora/user/Trainers/415608EC/Trainer(RETROBYTE).xex": its title ID and
// file name, for a trainer directly in a title's folder. False for anything
// else - other files, and the copies some folders keep a level down.
static bool ParsePackPath(const char *path, unsigned long *titleId, const char **file)
{
    if (_strnicmp(path, PACK_PREFIX, sizeof(PACK_PREFIX) - 1) != 0)
        return false;
    const char *id = path + sizeof(PACK_PREFIX) - 1;
    const char *slash = strchr(id, '/');
    if (slash == NULL || slash - id != 8 || strchr(slash + 1, '/') != NULL)
        return false;
    const size_t len = strlen(slash + 1);
    if (len < 5 || _stricmp(slash + 1 + len - 4, ".xex") != 0)
        return false;
    char hex[9];
    memcpy(hex, id, 8);
    hex[8] = '\0';
    char *end = NULL;
    *titleId = strtoul(hex, &end, 16);
    *file = slash + 1;
    return end != NULL && *end == '\0';
}

static DWORD WINAPI PackRefreshEntry(LPVOID)
{
    char *reply = (char *)malloc(PACK_LIST_MAX + 1);
    HttpsSession *session = HttpsSessionOpen(QuietPrint);
    if (reply == NULL || session == NULL)
    {
        free(reply);
        HttpsSessionClose(session);
        return 0;
    }
    unsigned long long len = PACK_LIST_MAX;
    const int status = HttpsSessionGet(session, PACK_LIST_URL, "Accept: application/vnd.github+json\r\n", reply, &len);
    HttpsSessionClose(session);
    if (status != 200 || len > PACK_LIST_MAX)
    {
        dprintf("[trainer] the pack's list didn't come: status %d\n", status);
        free(reply);
        return 0;
    }
    reply[len] = '\0';

    cJSON *root = cJSON_ParseWithLength(reply, (size_t)len);
    free(reply);
    const cJSON *tree = root != NULL ? cJSON_GetObjectItem(root, "tree") : NULL;
    if (tree == NULL)
    {
        dprintf("[trainer] the pack's list didn't parse\n");
        cJSON_Delete(root);
        return 0;
    }

    // Written beside, then put in place, so a reader never sees half of it.
    CreateDirectoryA("Hdd1:\\Omni360", NULL);
    FILE *out = NULL;
    if (fopen_s(&out, PACK_LIST_FILE ".new", "w") != 0 || out == NULL)
    {
        cJSON_Delete(root);
        return 0;
    }
    int count = 0;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, tree)
    {
        const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(entry, "path"));
        unsigned long titleId = 0;
        const char *file = NULL;
        if (path == NULL || !ParsePackPath(path, &titleId, &file))
            continue;
        fprintf(out, "%08lX\t%ld\t%s\n", titleId, JsonLong(entry, "size", 0), path);
        count++;
    }
    cJSON_Delete(root);
    const bool written = fclose(out) == 0;
    if (written && count > 0)
    {
        DeleteFileA(PACK_LIST_FILE);
        MoveFileA(PACK_LIST_FILE ".new", PACK_LIST_FILE);
        dprintf("[trainer] the pack lists %d trainers\n", count);
    }
    return 0;
}

void StartTrainerPackRefresh()
{
    if (PackListFresh())
        return;
    HANDLE thread = CreateThread(NULL, 64 * 1024, PackRefreshEntry, NULL, 0, NULL);
    if (thread != NULL)
        CloseHandle(thread);
}

int PackTrainersFor(unsigned long titleId, PackTrainer *out, int max)
{
    FILE *file = NULL;
    if (fopen_s(&file, PACK_LIST_FILE, "r") != 0 || file == NULL)
        return 0;
    int count = 0;
    char line[400];
    while (count < max && fgets(line, sizeof(line), file) != NULL)
    {
        char *end = NULL;
        if (strtoul(line, &end, 16) != titleId || end == NULL || *end != '\t')
            continue;
        const unsigned long size = strtoul(end + 1, &end, 10);
        if (end == NULL || *end != '\t')
            continue;
        char *path = end + 1;
        path[strcspn(path, "\r\n")] = '\0';
        unsigned long id = 0;
        const char *name = NULL;
        if (!ParsePackPath(path, &id, &name))
            continue;
        PackTrainer &t = out[count++];
        CopyText(t.path, sizeof(t.path), path);
        CopyText(t.file, sizeof(t.file), name);
        CopyText(t.label, sizeof(t.label), name);
        t.label[strlen(t.label) >= 4 ? strlen(t.label) - 4 : 0] = '\0'; // less ".xex"
        t.size = size;
    }
    fclose(file);
    return count;
}

// A path in the repository, for a URL: letters, digits and -_.~/ as they
// are, the rest (spaces, brackets, &) percent-encoded.
static void EncodePath(const char *in, char *out, size_t outSize)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *c = (const unsigned char *)in; *c != '\0' && o + 4 < outSize; ++c)
    {
        if ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') ||
            strchr("-_.~/", *c) != NULL)
            out[o++] = (char)*c;
        else
        {
            out[o++] = '%';
            out[o++] = hex[*c >> 4];
            out[o++] = hex[*c & 15];
        }
    }
    out[o] = '\0';
}

// Clears out a game's trainer folder - the trainer, what it said about
// itself, the cheats chosen for it - before another goes in: one trainer's
// cheat bits mean nothing to another's.
static void ClearTrainerFolder(const char *folder)
{
    char pattern[160];
    _snprintf(pattern, sizeof(pattern), "%s\\*", folder);
    pattern[sizeof(pattern) - 1] = '\0';
    WIN32_FIND_DATAA found;
    HANDLE find = FindFirstFileA(pattern, &found);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do
    {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        char path[320];
        _snprintf(path, sizeof(path), "%s\\%s", folder, found.cFileName);
        path[sizeof(path) - 1] = '\0';
        DeleteFileA(path);
    } while (FindNextFileA(find, &found));
    FindClose(find);
}

bool DownloadPackTrainer(unsigned long titleId, const PackTrainer &trainer, char *path, size_t pathSize)
{
    char encoded[400];
    EncodePath(trainer.path, encoded, sizeof(encoded));
    char *data = (char *)malloc(PACK_FILE_MAX + 1);
    HttpsSession *session = HttpsSessionOpen(QuietPrint);
    if (data == NULL || session == NULL)
    {
        free(data);
        HttpsSessionClose(session);
        return false;
    }
    unsigned long long len = PACK_FILE_MAX;
    const int status = HttpsSessionGet(session, std::string(PACK_FILE_URL) + encoded,
                                       "Accept: application/vnd.github.raw\r\n", data, &len);
    HttpsSessionClose(session);
    // A trainer is an XEX - not an error page, nor a file cut short.
    const bool ok = status == 200 && len >= 4 && len <= PACK_FILE_MAX && memcmp(data, "XEX2", 4) == 0 &&
                    (trainer.size == 0 || len == trainer.size);
    if (!ok)
    {
        dprintf("[trainer] downloading %s: status %d, %lu bytes\n", trainer.file, status, (unsigned long)len);
        free(data);
        return false;
    }

    char folder[96];
    _snprintf(folder, sizeof(folder), "%s\\%08lX", TRAINER_FOLDER, titleId);
    folder[sizeof(folder) - 1] = '\0';
    CreateDirectoryA("Hdd1:\\Omni360", NULL);
    CreateDirectoryA(TRAINER_FOLDER, NULL);
    CreateDirectoryA(folder, NULL);
    ClearTrainerFolder(folder);
    _snprintf(path, pathSize, "%s\\%s", folder, trainer.file);
    path[pathSize - 1] = '\0';
    FILE *file = NULL;
    bool written = false;
    if (fopen_s(&file, path, "wb") == 0 && file != NULL)
    {
        written = fwrite(data, 1, (size_t)len, file) == (size_t)len;
        written = fclose(file) == 0 && written;
    }
    free(data);
    dprintf("[trainer] downloaded %s for %08lX (%lu bytes): %s\n", trainer.file, titleId, (unsigned long)len,
            written ? "saved" : "NOT SAVED");
    return written;
}
