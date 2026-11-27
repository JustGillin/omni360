/*
FILE : Trainers.cpp
PROJECT : Omni360
DESCRIPTION : Loading Aurora's trainers (XYZEngine) for a game. See Trainers.h.
*/

#include "Trainers.h"
#include "Launch.h"        // StartSystemModule
#include "OutputConsole.h" // dprintf
#include "cJSON.h"

#include <xtl.h>
#include <stdio.h>
#include <string.h>

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

void StartTrainerFor(unsigned long titleId)
{
    char trainer[300];
    if (!FindTrainer(titleId, trainer, sizeof(trainer)))
        return; // none for this game - the usual case

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
