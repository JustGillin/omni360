/*
FILE : main.cpp
PROJECT : Omni360
DESCRIPTION : entry point. Flow: mount drives -> scan installed games (STFS,
              local, no network) -> icon-list picker -> archive.org IAS3 key
              auth (cached after first run) -> look up + download DLC for the
              chosen game -> write into the matching Content folder.

Rewritten from X-Store's original main.cpp, which drove a typed Vimm's Lair
search + full-game/ISO download flow - none of that is reused here since this
fork's whole point is browsing your own installed library instead of typing
a search query. CheckGameMounted() is the one piece kept close to verbatim,
since drive mounting has nothing to do with Vimm specifically.

The full pipeline below (STFS library scan -> picker -> IAS3 auth ->
archive.org lookup -> RAR member walk -> download into Content) is proven
end-to-end on real hardware against a real 27-game library.
*/

#include "OutputConsole.h"
#include "AtgConsole.h"
#include "AtgUtil.h"
#include "driveMount.h"
#include "settings.h"
#include "Keyboard.h"
#include "StfsParser.h"
#include "GameListUI.h"
#include "ArchiveOrgDLC.h"
#include "downloadFile.h" // DownloadProgressFn + FormatBytes, for the progress callback
#include "GodConvert.h"
#include "DiscSource.h"
#include "ReadAhead.h"
#include "TitleNames.h"

// Kernel exports with no XDK header.
extern "C" BOOL XexCheckExecutablePrivilege(DWORD privilege);
extern "C" NTSTATUS XexGetModuleHandle(PSZ moduleName, PHANDLE outHandle);

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#define SETTINGS_FILE "game:\\settings.txt"
#define SETTINGS_TEMP_FILE "game:\\settings.tmp"
#define GAMES_PATH_KEY "games-path: "
#define CREDENTIALS_FILE "game:\\ArchiveOrgKeys.txt"
#define CONTENT_BASE_PATH_DEFAULT "Hdd1:\\Content\\0000000000000000"
#define GAMES_PATH_DEFAULT CONTENT_BASE_PATH_DEFAULT // where the dashboard itself keeps installed games
#define MAX_INSTALLED_GAMES 256
#define MAX_DLC_MEMBERS 128

// (There is deliberately no auto-select threshold here any more - see the
// comment above the picker in DownloadDlcForGame.)

// ---------------------------------------------------------------------------
// Drive mounting - kept close to X-Store's original CheckGameMounted(),
// since this has nothing to do with Vimm specifically.
// ---------------------------------------------------------------------------

bool CheckGameMounted()
{
    FILE *fd;
    FILE *fd1;
    if (fopen_s(&fd, "game:\\test.tmp", "w") != 0)
    {
        dprintf("GAME_NOT_MOUNTED_TRYING_USB\n");
        if (mount("game:", "\\Device\\Mass0") != 0)
        {
            dprintf("GAME_NOT_MOUNTED_TRYING_HDD\n");
            if (mount("game:", "\\Device\\Harddisk0\\Partition1") != 0)
            {
                dprintf("GAME_NOT_MOUNTED\n");
            }
        }
    }
    else
    {
        fclose(fd);
        remove("game:\\test.tmp");
    }

    fd = NULL;
    fd1 = NULL;

    if (fopen_s(&fd1, "Hdd1:\\test.tmp", "w") != 0)
    {
        dprintf("Hdd1 not mounted, mounting...\n");
        if (mount("Hdd1:", "\\Device\\Harddisk0\\Partition1") != 0)
        {
            dprintf("Warning: failed to mount Hdd1:\n");
        }
    }
    else
    {
        fclose(fd1);
        remove("Hdd1:\\test.tmp");
    }

    return true;
}

// ---------------------------------------------------------------------------
// Text from the on-screen keyboard
// ---------------------------------------------------------------------------

// Strips leading and trailing whitespace. A key or path typed on the on-screen
// keyboard easily picks up a stray trailing space, and for a key that's
// enough to make every request fail with nothing on screen to show why.
static void TrimInPlace(std::string &s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
    {
        s.clear();
        return;
    }
    size_t end = s.find_last_not_of(" \t\r\n");
    s = s.substr(start, end - start + 1);
}

// For prefilling the on-screen keyboard. Everything this is used on - keys,
// drive paths - is plain ASCII, so widening byte by byte is exact.
static void NarrowToWide(const char *in, WCHAR *out, int outSize)
{
    int i = 0;
    for (; i < outSize - 1 && in[i] != '\0'; ++i)
        out[i] = (WCHAR)(unsigned char)in[i];
    out[i] = L'\0';
}

// ---------------------------------------------------------------------------
// settings.txt - two separate paths matter for this fork, and they can be
// genuinely different locations:
//   - the GAMES path: where EnumerateInstalledGames looks for your existing
//     library, to populate the picker. Defaults to the Content folder, which
//     is where the dashboard installs Games on Demand and arcade titles.
//     Some people keep GOD games in a folder of their own instead (e.g.
//     Hdd1:\Games, with the same TitleID\ContentType\ContentID layout under
//     a different root) and point Aurora at it - this key is for them.
//   - the CONTENT path: where downloaded DLC actually gets written. This is
//     always the standard Content\0000000000000000 layout regardless of
//     where the base game lives, since that's where Xbox/Aurora expect DLC
//     to be installed. Reuses the "xbla-path:" key so an existing X-Store
//     settings.txt still works without editing.
// ---------------------------------------------------------------------------

static void GetSettingsPath(const char *key, const char *defaultValue, char *outPath, size_t outPathSize)
{
    strncpy(outPath, defaultValue, outPathSize - 1);
    outPath[outPathSize - 1] = '\0';

    FILE *fd = fopen(SETTINGS_FILE, "r");
    if (fd == NULL)
        return;

    char line[512];
    size_t keyLen = strlen(key);

    while (fgets(line, sizeof(line), fd) != NULL)
    {
        if (line[0] == '#')
            continue;

        if (strncmp(line, key, keyLen) == 0)
        {
            char *value = line + keyLen;
            value[strcspn(value, "\r\n")] = '\0';

            if (strlen(value) >= 3)
            {
                strncpy(outPath, value, outPathSize - 1);
                outPath[outPathSize - 1] = '\0';
            }
            break;
        }
    }

    fclose(fd);
}

static void GetContentBasePath(char *outPath, size_t outPathSize)
{
    GetSettingsPath("xbla-path: ", CONTENT_BASE_PATH_DEFAULT, outPath, outPathSize);
}

static void GetGamesPath(char *outPath, size_t outPathSize)
{
    GetSettingsPath(GAMES_PATH_KEY, GAMES_PATH_DEFAULT, outPath, outPathSize);
}

// Writes one key into settings.txt - replacing its existing line, or adding
// one - and leaves every other line exactly as it was, comments and the
// content path included. Someone who hand-edited the file keeps their edits.
//
// Written in full to a temporary file first and only then swapped in, so a
// write that fails partway can't leave settings.txt truncated.
static bool SetSettingsValue(const char *key, const char *value)
{
    std::string contents;
    bool replaced = false;
    const size_t keyLen = strlen(key);

    FILE *in = fopen(SETTINGS_FILE, "r");
    if (in != NULL)
    {
        char line[512];
        while (fgets(line, sizeof(line), in) != NULL)
        {
            // Same matching rule as GetSettingsPath, so the line replaced here
            // is the line that would have been read.
            if (!replaced && line[0] != '#' && strncmp(line, key, keyLen) == 0)
            {
                contents += key;
                contents += value;
                contents += "\n";
                replaced = true;
                continue;
            }
            contents += line;
        }
        fclose(in);

        if (!contents.empty() && contents[contents.size() - 1] != '\n')
            contents += "\n";
    }

    if (!replaced)
    {
        contents += key;
        contents += value;
        contents += "\n";
    }

    FILE *out = fopen(SETTINGS_TEMP_FILE, "w");
    if (out == NULL)
        return false;

    bool ok = fwrite(contents.data(), 1, contents.size(), out) == contents.size();
    ok = (fclose(out) == 0) && ok;

    if (!ok)
    {
        remove(SETTINGS_TEMP_FILE);
        return false;
    }

    remove(SETTINGS_FILE); // rename won't replace an existing file
    return rename(SETTINGS_TEMP_FILE, SETTINGS_FILE) == 0;
}

// Tidies a typed folder path into the form the library scan expects: no
// surrounding spaces, backslashes rather than forward slashes, and no trailing
// backslash, since the scan appends its own separator. Returns false if what
// is left has no drive (Hdd1:, Usb0:...) and so can't be a console path.
static bool NormaliseGamesPath(const std::string &typed, char *out, size_t outSize)
{
    std::string path = typed;
    TrimInPlace(path);

    for (size_t i = 0; i < path.size(); ++i)
    {
        if (path[i] == '/')
            path[i] = '\\';
    }

    while (!path.empty() && path[path.size() - 1] == '\\')
        path.erase(path.size() - 1);

    size_t colon = path.find(':');
    if (colon == std::string::npos || colon == 0)
        return false;

    // Under 3 characters would also be ignored when settings.txt is next read
    // (see GetSettingsPath), so refusing it here keeps the two in agreement.
    if (path.size() < 3 || path.size() >= outSize)
        return false;

    memcpy(out, path.c_str(), path.size() + 1);
    return true;
}

static bool FolderExists(const char *path)
{
    // A bare drive ("Hdd1:") needs its root separator to be looked up at all.
    char probe[MAX_TEXT_LENGTH + 2];
    size_t len = strlen(path);
    if (len + 2 > sizeof(probe))
        return false;

    memcpy(probe, path, len + 1);
    if (len > 0 && probe[len - 1] == ':')
    {
        probe[len] = '\\';
        probe[len + 1] = '\0';
    }

    // (DWORD)-1 is GetFileAttributes' failure value. Win32 names it
    // INVALID_FILE_ATTRIBUTES, but the XDK's headers don't define that.
    DWORD attributes = GetFileAttributesA(probe);
    return attributes != (DWORD)-1 && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// ---------------------------------------------------------------------------
// Game list ordering
// ---------------------------------------------------------------------------

// EnumerateInstalledGames returns titles in raw FATX directory order, which is
// roughly the order they were written to the drive - so the list came out
// looking shuffled, with titles sharing a name prefix scattered apart rather
// than sitting together. Sorted by display name instead, which is the only
// order someone scanning for a specific game can actually predict.
//
// Case-insensitive, because FATX preserves whatever case the package's
// metadata used and a case-sensitive sort would file every lowercase title
// after every uppercase one. Ties break on title ID so the order is total and
// stable across launches: two titles genuinely can share a display name, and
// the ID is the thing that tells them apart (which is why the row draws it).
static int CompareGamesByName(const void *a, const void *b)
{
    const InstalledGame *ga = (const InstalledGame *)a;
    const InstalledGame *gb = (const InstalledGame *)b;

    int byName = _stricmp(ga->displayName, gb->displayName);
    if (byName != 0)
        return byName;

    if (ga->titleId < gb->titleId)
        return -1;
    if (ga->titleId > gb->titleId)
        return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// "Already installed" detection for the game list
// ---------------------------------------------------------------------------

// True if this title has at least one file under the given content-type
// folder, i.e. {contentBase}\{TitleID}\{contentType}\*.
//
// Deliberately a shallow, purely LOCAL check. Answering "is this exact pack
// installed?" would mean walking every candidate archive's RAR headers over
// the network just to draw a marker - dozens of requests before the list could
// even be shown. A folder test costs one directory open per title and answers
// the question people actually have at that screen: have I already got this,
// or not.
static bool HasInstalledContent(const char *contentBasePath, unsigned long titleId,
                                unsigned long contentType)
{
    char pattern[512];
    _snprintf(pattern, sizeof(pattern), "%s\\%08lX\\%08lX\\*",
              contentBasePath, titleId, contentType);
    pattern[sizeof(pattern) - 1] = '\0';

    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(pattern, &findData);
    if (hFind == INVALID_HANDLE_VALUE)
        return false;

    bool foundFile = false;

    do
    {
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue; // skips "." and ".." along with any stray subfolder

        foundFile = true;
        break;
    } while (FindNextFileA(hFind, &findData));

    FindClose(hFind);
    return foundFile;
}

// Recomputed rather than updated incrementally, and cheap enough to just redo
// after every download - two directory opens per title, no network.
//
// KNOWN GAP on the title-update flag: this only sees updates that installed
// into {TitleID}\000B0000\. Uppercase "TU_..." updates go to {device}\Cache\
// instead, where the filenames are not reliably attributable to a title, so a
// game whose update landed in Cache shows no marker. The flag means "an update
// is definitely installed", never "no update exists" - which is the safe way
// round for a hint whose only job is to stop you re-downloading.
static void RefreshInstalledFlags(const char *contentBasePath, const InstalledGame *games,
                                  int gameCount, bool *outDlcFlags, bool *outUpdateFlags)
{
    for (int i = 0; i < gameCount; ++i)
    {
        if (outDlcFlags != NULL)
            outDlcFlags[i] = HasInstalledContent(contentBasePath, games[i].titleId,
                                                 STFS_CONTENT_MARKETPLACE);
        if (outUpdateFlags != NULL)
            outUpdateFlags[i] = HasInstalledContent(contentBasePath, games[i].titleId,
                                                    STFS_CONTENT_TITLE_UPDATE);
    }
}

// ---------------------------------------------------------------------------
// Archive.org auth - IAS3 access key + secret key (archive.org's official,
// script-friendly S3-like API auth: archive.org/services/docs/api/ias3.html),
// not a scraped browser login session.
//
// A from-scratch email/password login (POST /account/login, scrape the
// Set-Cookie session) was tried first and worked right up until the actual
// DLC download, which came back 401 even with a real, well-formed session
// cookie. Turned out every file in msx360gcdlc is marked "private":"true" in
// its own metadata, and archive.org's REAL login is a JS single-page app
// hitting a JSON API (/services/account/login/) with a CSRF token from
// /services/csrf-token - a completely different endpoint/shape than the
// classic form POST this used to do, discovered by inspecting the real
// login page's network traffic. Worse, that page's Content-Security-Policy
// references Google reCAPTCHA, so a scripted login could hit a bot check
// with no way for a console app to solve it.
//
// IAS3 keys sidestep all of that: a permanent access+secret key pair the
// user fetches once, in their own real logged-in browser, from
// archive.org/account/s3.php - the same mechanism archive.org's own "ia"
// CLI tool uses - sent as one "Authorization: LOW <access>:<secret>" header
// (built by BuildIas3AuthHeader). No cookies, no CSRF, no session expiry.
//
// Same plaintext-cache tradeoff as this project's earlier Aurora Lua
// prototype (see the fork's plan notes): convenient on your own console,
// not something to do if others have access to it.
// ---------------------------------------------------------------------------

static bool LoadSavedKeys(std::string &accessKey, std::string &secretKey)
{
    FILE *f = fopen(CREDENTIALS_FILE, "rb");
    if (f == NULL)
        return false;

    char line1[256] = "";
    char line2[256] = "";
    bool ok = (fgets(line1, sizeof(line1), f) != NULL) && (fgets(line2, sizeof(line2), f) != NULL);
    fclose(f);

    if (!ok)
        return false;

    line1[strcspn(line1, "\r\n")] = '\0';
    line2[strcspn(line2, "\r\n")] = '\0';

    if (line1[0] == '\0' || line2[0] == '\0')
        return false;

    accessKey = line1;
    secretKey = line2;
    return true;
}

static bool SaveKeys(const std::string &accessKey, const std::string &secretKey)
{
    FILE *f = fopen(CREDENTIALS_FILE, "wb");
    if (f == NULL)
        return false;

    bool ok = fprintf(f, "%s\n%s\n", accessKey.c_str(), secretKey.c_str()) > 0;
    ok = (fclose(f) == 0) && ok;
    return ok;
}

// Asks for both keys with the on-screen keyboard. accessKey and secretKey come
// in as the current keys (empty when there are none) and go out as the new
// ones.
//
// The access key is offered for editing. The secret key never is - it is not
// put on screen at all - and leaving it blank keeps the current one, so
// correcting a typo in the access key doesn't mean retyping ~40 characters of
// secret. Returns false if either prompt is cancelled or a key ends up empty,
// leaving both strings as they came in.
static bool PromptKeys(std::string &accessKey, std::string &secretKey)
{
    WCHAR currentAccess[128];
    NarrowToWide(accessKey.c_str(), currentAccess, 128);

    std::string typedAccess;
    if (OpenKeyboardToString(XUSER_INDEX_ANY, &typedAccess, L"Archive.org Access Key",
                             L"From archive.org/account/s3.php", currentAccess) != ERROR_SUCCESS)
        return false;
    TrimInPlace(typedAccess);

    const bool haveSecret = !secretKey.empty();

    std::string typedSecret;
    if (OpenKeyboardToString(XUSER_INDEX_ANY, &typedSecret, L"Archive.org Secret Key",
                             haveSecret ? L"Leave blank to keep your current secret key"
                                        : L"From archive.org/account/s3.php",
                             L"") != ERROR_SUCCESS)
        return false;
    TrimInPlace(typedSecret);

    if (typedSecret.empty())
        typedSecret = secretKey;

    if (typedAccess.empty() || typedSecret.empty())
        return false;

    accessKey = typedAccess;
    secretKey = typedSecret;
    return true;
}

enum KeyEntryOutcome
{
    KEY_ENTRY_SAVED,     // new keys are saved
    KEY_ENTRY_CANCELLED, // backed out of the keyboard - nothing shown yet, the caller may want to say so
    KEY_ENTRY_FAILED     // didn't save, and a screen already said why
};

// The one way keys get entered, from Settings or from a first download: ask
// for them, check them with archive.org, then save them.
//
// Checked before saving so a typo is caught while the keys are still on the
// user's mind, rather than as a failed download later. What happens next
// depends on the verdict:
//   accepted  - saved.
//   rejected  - not saved, since archive.org has said they won't work. Offers
//               to try again with what was typed still in the keyboard, so
//               fixing one character doesn't mean retyping both keys.
//   unchecked - saved anyway. An offline console isn't a reason to refuse
//               keys, and treating "no answer" as "wrong keys" would send
//               someone off to fix the wrong thing.
//
// accessKey and secretKey come in as the current keys (empty if none) and
// go out as whatever was last typed.
static KeyEntryOutcome EnterCheckAndSaveKeys(std::string &accessKey, std::string &secretKey)
{
    for (;;)
    {
        if (!PromptKeys(accessKey, secretKey))
            return KEY_ENTRY_CANCELLED;

        char header[IAS3_AUTH_HEADER_MAX];
        if (!BuildIas3AuthHeader(accessKey, secretKey, header, sizeof(header)))
        {
            if (ShowConfirmUI("KEYS TOO LONG", "Those are too long to be archive.org keys.",
                              "Copy them again from archive.org/account/s3.php.", "Try again"))
                continue;
            return KEY_ENTRY_FAILED;
        }

        RenderStatusFrame("CHECKING KEYS", "Asking archive.org whether these keys work", NULL);

        char reason[160] = "";
        KeyCheckResult check = CheckArchiveOrgKeys(header, reason, sizeof(reason), dprintf);

        if (check == KEYS_REJECTED)
        {
            // archive.org's own wording where it gave one - it can say which
            // of the two keys it objects to.
            if (ShowConfirmUI("KEYS NOT ACCEPTED", "archive.org didn't accept those keys.",
                              reason[0] != '\0' ? reason : "Check them at archive.org/account/s3.php.",
                              "Try again"))
                continue;
            return KEY_ENTRY_FAILED;
        }

        if (!SaveKeys(accessKey, secretKey))
        {
            ShowMessageUI("KEYS NOT SAVED", "ArchiveOrgKeys.txt could not be written.",
                          "Your previous keys, if any, are unchanged.");
            return KEY_ENTRY_FAILED;
        }

        if (check == KEYS_ACCEPTED)
            ShowMessageUI("KEYS SAVED", "archive.org accepted your keys.",
                          "They're saved on this console for your next download.");
        else
            ShowMessageUI("KEYS SAVED", "Saved, but not checked - archive.org didn't answer.",
                          "If a download fails, check them again in Settings.");

        return KEY_ENTRY_SAVED;
    }
}

// For a download that archive.org turned away because of the keys. Separate
// from the generic failure messages on purpose: those name the pack or the
// file, which is the wrong thing to go and fix.
static void ShowKeysRejected()
{
    ShowMessageUI("KEYS NOT ACCEPTED", "archive.org turned down your keys for this download.",
                  "Check them in Settings - press Y on the game list.");
}

// A full drive, said plainly with both numbers. It used to show up only as a
// disk write error in the log, under a generic "download failed" - after the
// download had already filled what space there was.
// "Hdd1:" from "Hdd1:\Content\...", or the fallback if the path names no drive.
static void DriveLabel(const char *path, char *out, size_t outSize, const char *fallback)
{
    _snprintf(out, outSize, "%s", fallback);
    out[outSize - 1] = '\0';

    const char *colon = strchr(path, ':');
    if (colon != NULL && (size_t)(colon - path) < outSize - 1)
    {
        memcpy(out, path, (size_t)(colon - path) + 1);
        out[colon - path + 1] = '\0';
    }
}

// The library footer's "Hdd1: 120 GB free". Empty if the drive can't say, so
// the footer shows nothing rather than a wrong number.
static void FormatFreeSpaceStatus(const char *path, char *out, size_t outSize)
{
    out[0] = '\0';

    unsigned long long freeSpace = 0;
    if (!DriveFreeSpace(path, &freeSpace))
        return;

    char drive[16], freeText[64] = "";
    DriveLabel(path, drive, sizeof(drive), "Drive");
    FormatBytes(freeSpace, freeText, sizeof(freeText));

    _snprintf(out, outSize, "%s %s free", drive, freeText);
    out[outSize - 1] = '\0';
}

static void ShowNotEnoughSpace(const char *path, unsigned long long needed, unsigned long long freeSpace)
{
    char drive[16];
    DriveLabel(path, drive, sizeof(drive), "The drive");

    char neededText[64] = "", freeText[64] = "";
    FormatBytes(needed, neededText, sizeof(neededText));
    FormatBytes(freeSpace, freeText, sizeof(freeText));

    char message[160];
    _snprintf(message, sizeof(message), "This needs %s, but %s has %s free.", neededText, drive, freeText);
    message[sizeof(message) - 1] = '\0';

    ShowMessageUI("NOT ENOUGH SPACE", message, "Free up some space on the drive and try again.");
}

static bool GetArchiveOrgAuthHeader(char *authHeader, unsigned long long authHeaderSize)
{
    std::string accessKey, secretKey;

    if (!LoadSavedKeys(accessKey, secretKey))
    {
        // Typing a ~40-character secret key with an on-screen keyboard and a
        // controller is painful - FTPing %s onto the console yourself ahead
        // of time (access key on line 1, secret key on line 2, plain text)
        // skips this prompt entirely, since LoadSavedKeys() above already
        // succeeds if the file is there. The keyboard is only a fallback for
        // when that's not convenient in the moment.
        dprintf("Archive.org IAS3 keys required.\n");
        dprintf("Get them (logged in to your own account) at: archive.org/account/s3.php\n");
        dprintf("Tip: FTP them directly into %s (access key on line 1, secret key on\n", CREDENTIALS_FILE);
        dprintf("line 2) ahead of time to skip typing them in on the console.\n");

        // Drawn, not printed. By this point the console no longer paints to
        // the screen (see SetConsoleQuiet), and this is the one piece of
        // guidance the user genuinely cannot act without - it tells them
        // where to get the keys they're about to be asked for.
        ShowMessageUI("ARCHIVE.ORG KEYS NEEDED",
                      "Get your access and secret key at archive.org/account/s3.php",
                      "Press A to type them in, or put them in ArchiveOrgKeys.txt beforehand.");

        KeyEntryOutcome entry = EnterCheckAndSaveKeys(accessKey, secretKey);

        if (entry == KEY_ENTRY_CANCELLED)
        {
            dprintf("ERROR: no keys entered\n");
            ShowMessageUI("NO KEYS ENTERED", "Both keys are required to download from archive.org.",
                          "You can add them any time in Settings - press Y on the game list.");
        }

        if (entry != KEY_ENTRY_SAVED)
            return false; // every other outcome has already shown its own screen
    }

    // Saved keys that can't even form a header - hand-edited into something
    // far too long. Said on screen, since the caller now leaves explaining
    // failures to this function.
    if (!BuildIas3AuthHeader(accessKey, secretKey, authHeader, authHeaderSize))
    {
        dprintf("ERROR: could not build auth header from saved keys - delete %s and re-enter them\n", CREDENTIALS_FILE);
        ShowMessageUI("SAVED KEYS UNUSABLE", "The saved keys are too long to be archive.org keys.",
                      "Re-enter them in Settings - press Y on the game list.");
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// DLC lookup + download for one chosen game
// ---------------------------------------------------------------------------

// DownloadProgressFn is a bare function pointer with no user-data parameter
// (matching printFunction, the callback convention already used throughout
// this project), so the "which pack, which file" context the callback needs
// in order to render a meaningful frame lives here rather than being threaded
// down through the HTTP layer, which has no business knowing about it.
static char g_progressTitle[256] = "";
static int g_progressFileIndex = 0;
static int g_progressFileCount = 0;

// Called from inside downloadFile.cpp's read loop, roughly every 100ms, with
// live byte counts for the file currently downloading. This replaces the old
// behaviour where the progress bar only advanced once per completed member -
// meaning it sat frozen for the entire duration of each actual download while
// the real numbers scrolled past in the debug console.
static void DlcProgressCallback(unsigned long long bytesDone,
                                unsigned long long bytesTotal,
                                unsigned long long bytesPerSec,
                                unsigned long long secondsRemaining)
{
    // Fixed format specifiers and explicit null-termination throughout: this
    // toolchain's _snprintf does not null-terminate on truncation, and its
    // dynamic-precision specifier ("%.*s") has caused a real, near-undebuggable
    // crash in this project before.
    char status[64];
    _snprintf(status, sizeof(status), "File %d of %d", g_progressFileIndex + 1, g_progressFileCount);
    status[sizeof(status) - 1] = '\0';

    char done[64] = "";
    char speed[64] = "";
    FormatBytes(bytesDone, done, sizeof(done));
    FormatBytes(bytesPerSec, speed, sizeof(speed));

    char detail[256];
    if (bytesTotal > 0)
    {
        char total[64] = "";
        FormatBytes(bytesTotal, total, sizeof(total));

        // Minutes/seconds cast down to int before formatting - they're small
        // by definition, and it avoids relying on %llu width handling here.
        int minutesLeft = (int)(secondsRemaining / 60);
        int secsLeft = (int)(secondsRemaining % 60);

        _snprintf(detail, sizeof(detail), "%s / %s   %s/s   %d:%02d left",
                  done, total, speed, minutesLeft, secsLeft);
    }
    else
    {
        // No Content-Length and no size hint - report what we can rather than
        // implying a percentage we don't have.
        _snprintf(detail, sizeof(detail), "%s   %s/s", done, speed);
    }
    detail[sizeof(detail) - 1] = '\0';

    // The bar tracks progress across the whole pack rather than the current
    // file alone, so it advances monotonically instead of snapping back to
    // zero on each of a dozen members.
    float fraction = -1.0f; // negative = indeterminate, see RenderProgressFrame
    if (g_progressFileCount > 0)
    {
        float withinFile = (bytesTotal > 0) ? ((float)bytesDone / (float)bytesTotal) : 0.0f;
        fraction = ((float)g_progressFileIndex + withinFile) / (float)g_progressFileCount;
    }

    RenderProgressFrame(g_progressTitle, status, detail, fraction);
}

// Drawn after each header ListDlcMembers reads. That walk is one network round
// trip per entry, and on a large pack it went on for a long time behind a
// single static "Reading the file list" frame, looking hung.
//
// The bar is how far through the archive the walk has got. It moves in uneven
// jumps - each step skips over a whole member's data - so the counts under it
// are what show it's still going between jumps. The screen can't animate
// during a request itself: each one blocks until archive.org answers.
static void ListMembersProgressCallback(unsigned long long bytesScanned, unsigned long long archiveSize,
                                        int filesToInstall, int avatarItemsSkipped)
{
    char detail[160];
    if (filesToInstall == 0 && avatarItemsSkipped == 0)
        _snprintf(detail, sizeof(detail), "Connecting...");
    else if (avatarItemsSkipped == 0)
        _snprintf(detail, sizeof(detail), "%d file%s to install", filesToInstall, filesToInstall == 1 ? "" : "s");
    else
        // Named, not just counted: many packs are mostly avatar items (one had
        // 11 of 12), and "skipping 10 items" with no reason reads as though
        // part of the DLC is being lost.
        _snprintf(detail, sizeof(detail), "%d file%s to install   -   %d avatar item%s skipped (outfits and props, not game content)",
                  filesToInstall, filesToInstall == 1 ? "" : "s",
                  avatarItemsSkipped, avatarItemsSkipped == 1 ? "" : "s");
    detail[sizeof(detail) - 1] = '\0';

    float fraction = (archiveSize > 0) ? (float)((double)bytesScanned / (double)archiveSize) : -1.0f;

    RenderProgressFrame(g_progressTitle, "Reading the file list", detail, fraction, "READING PACK");
}

// Downloads every file inside one chosen pack.
//
// Returns false if archive.org refused the keys, so the caller stops offering
// packs: every one of them would be refused the same way, and the fix is in
// Settings, back on the game list.
static bool DownloadOnePack(const DlcRarMatch &pack, const char *contentBasePath, const char *authHeader)
{
    // The pack name is the progress frames' title, both for the file-list
    // walk below and for the downloads after it.
    strncpy(g_progressTitle, pack.filename, sizeof(g_progressTitle) - 1);
    g_progressTitle[sizeof(g_progressTitle) - 1] = '\0';

    DlcMember members[MAX_DLC_MEMBERS];
    int memberCount = ListDlcMembers(pack.filename, pack.size, members, MAX_DLC_MEMBERS, authHeader, dprintf,
                                     ListMembersProgressCallback);

    if (memberCount <= 0)
    {
        // Reading the file list is the first thing that sends the keys, so
        // this is where wrong ones usually show up.
        if (ArchiveOrgKeysRejected())
        {
            ShowKeysRejected();
            return false;
        }

        ShowMessageUI("COULD NOT READ PACK",
                      "The file list for this pack could not be read.", pack.filename);
        return true;
    }

    // Checked once for the whole pack, before any of it downloads: running out
    // partway would leave a pack half installed, after a long wait. Files
    // already on the console don't count - they won't be fetched again.
    unsigned long long needed = 0;
    for (int f = 0; f < memberCount; ++f)
    {
        if (!DlcMemberIsInstalled(members[f], contentBasePath))
            needed += members[f].unpSize;
    }

    unsigned long long freeSpace = 0;
    if (needed > 0 && DriveFreeSpace(contentBasePath, &freeSpace) && freeSpace < needed + 4ULL * 1024 * 1024)
    {
        dprintf("Not enough space for %s: needs %I64u bytes, %I64u free\n", pack.filename, needed, freeSpace);
        ShowNotEnoughSpace(contentBasePath, needed, freeSpace);
        return true;
    }

    // Context for DlcProgressCallback, which the HTTP layer calls with nothing
    // but byte counts. (g_progressTitle is already the pack name - set above,
    // for the file-list frames.)
    g_progressFileCount = memberCount;

    int failures = 0;
    int alreadyThere = 0;

    for (int f = 0; f < memberCount; ++f)
    {
        g_progressFileIndex = f;

        // Already on disk at the right size - skip it. This is what makes
        // re-opening a pack cheap instead of a full re-download, and it makes
        // a transfer that was cancelled partway resume from where it stopped
        // rather than starting over. The size check inside
        // DlcMemberIsInstalled is what stops a half-written file from being
        // mistaken for a finished one.
        if (DlcMemberIsInstalled(members[f], contentBasePath))
        {
            alreadyThere++;
            continue;
        }

        // One frame up front so the screen reflects the new file immediately,
        // rather than showing the previous file's numbers until the first
        // callback fires ~100ms into the transfer.
        char status[64];
        _snprintf(status, sizeof(status), "File %d of %d", f + 1, memberCount);
        status[sizeof(status) - 1] = '\0';
        RenderProgressFrame(g_progressTitle, status, "Connecting...",
                            (float)f / (float)memberCount);

        if (!DownloadDlcMember(pack.filename, members[f], contentBasePath,
                               authHeader, dprintf, DlcProgressCallback))
        {
            dprintf("  Failed: %s\n", members[f].internalPath);

            // Stop at the first refusal instead of trying the rest - they'd
            // all be refused, one slow round trip each.
            if (ArchiveOrgKeysRejected())
            {
                ShowKeysRejected();
                return false;
            }

            // The rest can't fit either, so stop and say why.
            unsigned long long fileNeeded = 0, fileFree = 0;
            if (ArchiveOrgDiskFull(&fileNeeded, &fileFree))
            {
                ShowNotEnoughSpace(contentBasePath, fileNeeded, fileFree);
                return true;
            }

            failures++;
        }
    }

    if (alreadyThere > 0)
        dprintf("  %d of %d file(s) were already installed\n", alreadyThere, memberCount);

    if (failures == 0 && alreadyThere == memberCount)
    {
        // Nothing was transferred. Saying "installed" here would be true but
        // misleading - it reads as though work happened.
        ShowMessageUI("ALREADY INSTALLED", pack.filename,
                      "Every file in this pack is already on the console.");
    }
    else if (failures == 0)
    {
        ShowMessageUI("INSTALLED", pack.filename,
                      "Restart your dashboard to pick up the new content.");
    }
    else
    {
        char detail[128];
        _snprintf(detail, sizeof(detail), "%d of %d files failed to download.", failures, memberCount);
        detail[sizeof(detail) - 1] = '\0';
        ShowMessageUI("FINISHED WITH ERRORS", pack.filename, detail);
    }

    return true;
}

// Title updates: their own flow, reached with X from the game list rather than
// A. Keeping them separate is the point - someone who already has a game's DLC
// installed can fetch just its update without walking through the DLC screens.
static void InstallTitleUpdatesForGame(const InstalledGame &game, const char *contentBasePath,
                                       const char *authHeader)
{
    RenderStatusFrame("SEARCHING", "Looking up title updates", game.displayName);

    TitleUpdateMatch updates[MAX_TITLE_UPDATE_MATCHES];
    int updateCount = FindTitleUpdates(game.displayName, updates, MAX_TITLE_UPDATE_MATCHES, dprintf);

    if (updateCount < 0)
    {
        ShowMessageUI("SEARCH FAILED", "Could not reach archive.org.",
                      "Check the console's network connection and try again.");
        return;
    }

    if (updateCount == 0)
    {
        ShowMessageUI("NOTHING FOUND", "No title update matched this game.", game.displayName);
        return;
    }

    for (int i = 0; i < updateCount; ++i)
    {
        // "score N", not "N%" - a literal percent sign does not survive this
        // logging path (see LogEscapePercent in parsing.h), which is why an
        // earlier run of this printed "100v1" instead of "100%  v1".
        dprintf("[TU]   score %d  v%d  region \"%s\"  %I64u bytes  %s\n",
                updates[i].score, updates[i].version, updates[i].region,
                updates[i].size, updates[i].filename);
    }

    // Version and region are the whole reason this is a picker rather than an
    // automatic pick: one game routinely has v1/v2/v3 across several regions,
    // and "newest" is not always what someone wants - a specific update version
    // is sometimes required for mods or for matchmaking compatibility.
    const char *labels[MAX_TITLE_UPDATE_MATCHES];
    const char *sublabels[MAX_TITLE_UPDATE_MATCHES];
    char subText[MAX_TITLE_UPDATE_MATCHES][96];

    for (int i = 0; i < updateCount; ++i)
    {
        labels[i] = updates[i].filename;

        char sizeText[64] = "";
        FormatBytes(updates[i].size, sizeText, sizeof(sizeText));

        _snprintf(subText[i], sizeof(subText[i]), "%s   v%d   %s   %d%% name match",
                  sizeText, updates[i].version,
                  updates[i].region[0] != '\0' ? updates[i].region : "unknown region",
                  updates[i].score);
        subText[i][sizeof(subText[i]) - 1] = '\0';
        sublabels[i] = subText[i];
    }

    // Results are sorted score-first then version-descending, so index 0 is
    // the newest update for the best-matching name - the right default to land
    // on, but still shown rather than assumed.
    int selection = 0;

    for (;;)
    {
        int choice = ShowChoiceUI("CHOOSE A TITLE UPDATE", labels, sublabels, updateCount, selection);
        if (choice < 0)
            break; // B steps back to the game list

        selection = choice;

        RenderStatusFrame("TITLE UPDATE", "Reading the update", updates[choice].filename);

        strncpy(g_progressTitle, updates[choice].filename, sizeof(g_progressTitle) - 1);
        g_progressTitle[sizeof(g_progressTitle) - 1] = '\0';
        g_progressFileIndex = 0;
        g_progressFileCount = 1;

        unsigned long long tuNeeded = 0, tuFree = 0;
        if (DownloadTitleUpdate(updates[choice], game.titleId, contentBasePath,
                                authHeader, dprintf, DlcProgressCallback))
        {
            ShowMessageUI("TITLE UPDATE INSTALLED", updates[choice].filename,
                          "Restart your dashboard to pick up the update.");
        }
        else if (ArchiveOrgKeysRejected())
        {
            // Not "archive.org may not serve this file" - that sends someone
            // to try a different update, which will be refused the same way.
            ShowKeysRejected();
            break;
        }
        else if (ArchiveOrgDiskFull(&tuNeeded, &tuFree))
        {
            ShowNotEnoughSpace(contentBasePath, tuNeeded, tuFree);
        }
        else
        {
            ShowMessageUI("TITLE UPDATE FAILED", updates[choice].filename,
                          "See the log - archive.org may not serve this file directly.");
        }
    }
}

static void DownloadDlcForGame(const InstalledGame &game, const char *contentBasePath, const char *authHeader)
{
    RenderStatusFrame("SEARCHING", "Looking up DLC on archive.org", game.displayName);

    DlcRarMatch matches[MAX_DLC_RAR_MATCHES];
    int matchCount = FindDlcRarFilenames(game.displayName, matches, MAX_DLC_RAR_MATCHES, dprintf);

    if (matchCount < 0)
    {
        ShowMessageUI("SEARCH FAILED", "Could not reach archive.org.",
                      "Check the console's network connection and try again.");
        return;
    }

    if (matchCount == 0)
    {
        ShowMessageUI("NOTHING FOUND", "No DLC in the collection matched this game.",
                      game.displayName);
        return;
    }

    // Labels for the picker. The filename is what actually identifies a pack,
    // and the size plus match confidence are what let someone judge between
    // two similar-looking entries.
    const char *labels[MAX_DLC_RAR_MATCHES];
    const char *sublabels[MAX_DLC_RAR_MATCHES];
    char subText[MAX_DLC_RAR_MATCHES][96];

    for (int i = 0; i < matchCount; ++i)
    {
        labels[i] = matches[i].filename;

        char sizeText[64] = "";
        FormatBytes(matches[i].size, sizeText, sizeof(sizeText));

        _snprintf(subText[i], sizeof(subText[i]), "%s   %d%% name match", sizeText, matches[i].score);
        subText[i][sizeof(subText[i]) - 1] = '\0';
        sublabels[i] = subText[i];
    }

    // ALWAYS ask, even for a single perfectly-scored result.
    //
    // This screen briefly auto-selected an unambiguous match and skipped
    // straight to downloading. That was removed after actually living with it:
    // the confirmation is not friction, it is the one place you can see the
    // real filename and size before committing to a download that may be
    // gigabytes, and the one place to back out of a game selected by mistake.
    // Saving a button press is not worth either of those.
    //
    // Looping rather than returning after one download: a game can genuinely
    // have several separate packs - Call of Duty 2 has three - and this lets
    // someone take them one at a time instead of the original behaviour, which
    // downloaded every match in one go.
    int pickerSelection = 0;

    for (;;)
    {
        int choice = ShowChoiceUI("CHOOSE A DLC PACK", labels, sublabels, matchCount, pickerSelection);
        if (choice < 0)
            break; // B here steps back to the game list, not out of the app

        pickerSelection = choice; // come back to the pack they just took

        if (!DownloadOnePack(matches[choice], contentBasePath, authHeader))
            break; // keys refused - back to the game list, where Settings is
    }
}

// ---------------------------------------------------------------------------
// The installed library
// ---------------------------------------------------------------------------

// The scanned library and its per-title markers, kept together because they
// are only meaningful together: the marker arrays index into games, and all
// three are replaced whenever the games folder changes.
struct Library
{
    InstalledGame *games; // MAX_INSTALLED_GAMES entries, allocated once
    int count;
    bool dlcInstalled[MAX_INSTALLED_GAMES];
    bool updateInstalled[MAX_INSTALLED_GAMES];
};

static void ScanLibrary(Library &lib, const char *gamesPath)
{
    RenderStatusFrame("SCANNING", "Reading your installed games", gamesPath);

    int found = EnumerateInstalledGames(gamesPath, lib.games, MAX_INSTALLED_GAMES, dprintf);
    lib.count = (found > 0) ? found : 0;

    // Sorted here rather than inside EnumerateInstalledGames - that
    // function's job is to walk the filesystem, and leaving presentation order
    // to the caller keeps it that way. Everything downstream (the installed
    // flags, listSelection) indexes into this array after the sort, so nothing
    // else has to know it happened.
    if (lib.count > 1)
        qsort(lib.games, lib.count, sizeof(InstalledGame), CompareGamesByName);

    // The cover cache belongs to whatever list was there before.
    ReleaseGameListIcons();

    if (lib.count > 0)
        dprintf("Found %d installed games under %s\n", lib.count, gamesPath);
    else
        dprintf("No installed games found under %s\n", gamesPath);
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

struct SettingsOutcome
{
    bool libraryChanged; // the games folder changed and the library was rescanned
    bool keysChanged;    // the saved keys were replaced or removed
};

enum SettingsRow
{
    SETTINGS_ROW_GAMES_FOLDER,
    SETTINGS_ROW_KEYS,
    SETTINGS_ROW_REMOVE_KEYS
};

static void ChangeGamesFolder(Library &lib, char *gamesPath, size_t gamesPathSize, SettingsOutcome &outcome)
{
    WCHAR current[MAX_TEXT_LENGTH];
    NarrowToWide(gamesPath, current, MAX_TEXT_LENGTH);

    std::string typed;
    if (OpenKeyboardToString(XUSER_INDEX_ANY, &typed, L"Games Folder",
                             L"Where your installed games are - usually Hdd1:\\Content\\0000000000000000", current) != ERROR_SUCCESS)
        return; // cancelled

    char newPath[MAX_TEXT_LENGTH];
    if (!NormaliseGamesPath(typed, newPath, sizeof(newPath)) || strlen(newPath) >= gamesPathSize)
    {
        ShowMessageUI("NOT A FOLDER PATH", "Include the drive, like Hdd1:\\Games or Usb0:\\Games.",
                      typed.c_str());
        return;
    }

    if (_stricmp(newPath, gamesPath) == 0)
        return; // unchanged - nothing to save or rescan

    // Checked before saving, so a typo is caught while the old, working path
    // is still in place rather than after it has been overwritten.
    if (!FolderExists(newPath))
    {
        ShowMessageUI("FOLDER NOT FOUND", "There is no folder at that path.", newPath);
        return;
    }

    bool saved = SetSettingsValue(GAMES_PATH_KEY, newPath);
    dprintf("Games path changed to %s (%s)\n", newPath, saved ? "saved" : "NOT saved");

    // Used for this session either way - the user asked for it and the folder
    // exists. Only remembering it is in question if the write failed.
    memcpy(gamesPath, newPath, strlen(newPath) + 1);

    ScanLibrary(lib, gamesPath);
    outcome.libraryChanged = true;

    if (!saved)
    {
        ShowMessageUI("NOT SAVED", "Using this folder for now, but settings.txt could not be written.",
                      "It will go back to the old folder next time Omni360 starts.");
    }
}

static void ChangeKeys(SettingsOutcome &outcome)
{
    std::string accessKey, secretKey;
    LoadSavedKeys(accessKey, secretKey); // leaves both empty if there are none yet

    // Cancelling here needs no message - the user is already on the screen
    // that says what's saved.
    if (EnterCheckAndSaveKeys(accessKey, secretKey) == KEY_ENTRY_SAVED)
        outcome.keysChanged = true;
}

static void RemoveKeys(SettingsOutcome &outcome)
{
    if (!ShowConfirmUI("REMOVE KEYS", "Remove the archive.org keys saved on this console?",
                       "You'll need to add them again before you can download.", "Remove"))
        return;

    if (remove(CREDENTIALS_FILE) != 0)
    {
        ShowMessageUI("COULD NOT REMOVE", "ArchiveOrgKeys.txt could not be deleted.", NULL);
        return;
    }

    dprintf("Saved archive.org keys removed\n");
    outcome.keysChanged = true;
    ShowMessageUI("KEYS REMOVED", "Your archive.org keys have been removed from this console.", NULL);
}

// The settings screen: a short list whose second lines show the current
// state, so it doubles as a summary of how the app is set up. Rebuilt on every
// pass, since each action changes what it should say.
static SettingsOutcome RunSettingsUI(Library &lib, char *gamesPath, size_t gamesPathSize)
{
    SettingsOutcome outcome = {false, false};
    int selection = 0;

    for (;;)
    {
        std::string accessKey, secretKey;
        const bool haveKeys = LoadSavedKeys(accessKey, secretKey);

        char gamesSub[MAX_TEXT_LENGTH + 64];
        if (lib.count > 0)
            _snprintf(gamesSub, sizeof(gamesSub), "%s   -   %d game%s", gamesPath, lib.count,
                      lib.count == 1 ? "" : "s");
        else
            _snprintf(gamesSub, sizeof(gamesSub), "%s   -   no games found here", gamesPath);
        gamesSub[sizeof(gamesSub) - 1] = '\0';

        // Only the start of the access key is shown - enough to tell which
        // keys are saved. The secret key is never shown anywhere.
        char keysSub[128];
        if (haveKeys)
        {
            char shown[5] = "";
            strncpy(shown, accessKey.c_str(), 4);
            shown[4] = '\0';
            _snprintf(keysSub, sizeof(keysSub), "Saved   -   access key %s...", shown);
        }
        else
        {
            _snprintf(keysSub, sizeof(keysSub),
                      "Not set   -   needed to download. Get them at archive.org/account/s3.php");
        }
        keysSub[sizeof(keysSub) - 1] = '\0';

        const char *labels[3];
        const char *sublabels[3];
        SettingsRow rows[3];
        int rowCount = 0;

        labels[rowCount] = "Games folder";
        sublabels[rowCount] = gamesSub;
        rows[rowCount++] = SETTINGS_ROW_GAMES_FOLDER;

        labels[rowCount] = haveKeys ? "Change archive.org keys" : "Add archive.org keys";
        sublabels[rowCount] = keysSub;
        rows[rowCount++] = SETTINGS_ROW_KEYS;

        // Only offered when there's something to remove.
        if (haveKeys)
        {
            labels[rowCount] = "Remove archive.org keys";
            sublabels[rowCount] = "Deletes the saved keys from this console";
            rows[rowCount++] = SETTINGS_ROW_REMOVE_KEYS;
        }

        if (selection > rowCount - 1)
            selection = rowCount - 1; // the Remove row just went away

        int choice = ShowChoiceUI("SETTINGS", labels, sublabels, rowCount, selection, "Select", false);
        if (choice < 0)
            break; // B, back to the game list

        selection = choice;

        switch (rows[choice])
        {
        case SETTINGS_ROW_GAMES_FOLDER: ChangeGamesFolder(lib, gamesPath, gamesPathSize, outcome); break;
        case SETTINGS_ROW_KEYS:         ChangeKeys(outcome); break;
        case SETTINGS_ROW_REMOVE_KEYS:  RemoveKeys(outcome); break;
        }
    }

    return outcome;
}

// ---------------------------------------------------------------------------
// Installing a disc as Games on Demand
// ---------------------------------------------------------------------------

// UTF-8 into the keyboard's WCHAR default text. Characters outside the BMP
// become '?'.
static void Utf8ToWideText(const char *in, WCHAR *out, int outSize)
{
    int n = 0;
    const unsigned char *s = (const unsigned char *)in;
    while (*s != 0 && n < outSize - 1)
    {
        unsigned long c = *s++;
        int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
        if (extra > 0)
            c &= (0x3F >> extra);
        else if (c >= 0x80)
            c = '?';
        for (; extra > 0 && (*s & 0xC0) == 0x80; --extra)
            c = (c << 6) | (*s++ & 0x3F);
        out[n++] = (extra > 0 || c > 0xFFFF) ? L'?' : (WCHAR)c;
    }
    out[n] = L'\0';
}

// What OpenKeyboardToString hands back for text: one byte per character, and
// '?' for anything past U+00FF. Used to tell whether a name came back from
// the keyboard unchanged.
static std::string Utf8AsKeyboardText(const char *utf8)
{
    WCHAR wide[128];
    Utf8ToWideText(utf8, wide, 128);
    std::string out;
    for (int i = 0; wide[i] != L'\0'; ++i)
        out += (wide[i] <= 0xFF) ? (char)wide[i] : '?';
    return out;
}

static std::string Latin1ToUtf8(const std::string &in)
{
    std::string out;
    for (size_t i = 0; i < in.length(); ++i)
    {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x80)
            out += (char)c;
        else
        {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

static WORD AnyPadButtons()
{
    WORD buttons = 0;
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i)
    {
        XINPUT_STATE state;
        ZeroMemory(&state, sizeof(state));
        if (XInputGetState(i, &state) == ERROR_SUCCESS)
            buttons |= state.Gamepad.wButtons;
    }
    return buttons;
}

struct DiscInstallProgress
{
    const char *title;
    DWORD startTick;
    DWORD lastFrameTick;
    WORD prevButtons;
};

// Called by GodConvert after every 816KB. Redraws at most ten times a second
// - the copy runs at the drive's pace, and a frame costs time the drive could
// be reading - and offers B to stop.
static bool DiscInstallProgressCallback(unsigned long long done, unsigned long long total, void *context)
{
    DiscInstallProgress *p = (DiscInstallProgress *)context;

    WORD buttons = AnyPadButtons();
    WORD pressed = buttons & ~p->prevButtons;
    p->prevButtons = buttons;

    if (pressed & XINPUT_GAMEPAD_B)
    {
        if (ShowConfirmUI("STOP INSTALLING?", "The game won't be installed.",
                          "What has been copied so far is removed.", "Stop"))
            return false;
        p->prevButtons = AnyPadButtons(); // the B that answered "no" isn't a fresh press
    }

    DWORD now = GetTickCount();
    if (now - p->lastFrameTick < 100 && done < total)
        return true;
    p->lastFrameTick = now;

    DWORD elapsedMs = now - p->startTick;
    unsigned long long bytesPerSec = (elapsedMs > 0) ? done * 1000ULL / elapsedMs : 0;

    char doneText[64] = "", totalText[64] = "", speedText[64] = "";
    FormatBytes(done, doneText, sizeof(doneText));
    FormatBytes(total, totalText, sizeof(totalText));
    FormatBytes(bytesPerSec, speedText, sizeof(speedText));

    char detail[256];
    if (bytesPerSec > 0 && elapsedMs > 3000) // the first seconds' rate is mostly spin-up
    {
        unsigned long long secondsLeft = (total - done) / bytesPerSec;
        _snprintf(detail, sizeof(detail), "%s / %s   %s/s   %d:%02d left   -   B to stop",
                  doneText, totalText, speedText, (int)(secondsLeft / 60), (int)(secondsLeft % 60));
    }
    else
    {
        _snprintf(detail, sizeof(detail), "%s / %s   -   B to stop", doneText, totalText);
    }
    detail[sizeof(detail) - 1] = '\0';

    RenderProgressFrame(p->title, "Copying from the disc", detail,
                        (total > 0) ? (float)((double)done / (double)total) : -1.0f, "INSTALLING");
    return true;
}

// An install in progress, recorded before the first byte is copied and
// removed once it has either finished or cleaned up after itself. Anything
// that ends the app mid-copy without either - the Guide button to the
// dashboard, the power, a crash - leaves it behind, and the next launch
// removes the partial package without asking: half a game is never worth
// keeping, since an install can't be resumed.
//
// Lines: games folder, title ID, media ID, the finished package's size, and
// the name (for the log and the status line).
#define INSTALL_MARKER_FILE "game:\\InstallInProgress.txt"

static void WriteInstallMarker(const char *gamesPath, const GodImageInfo &info, const char *name)
{
    FILE *f = fopen(INSTALL_MARKER_FILE, "w");
    if (f == NULL)
    {
        dprintf("[disc] couldn't write %s - an interrupted install won't be cleaned up automatically\n",
                INSTALL_MARKER_FILE);
        return;
    }
    fprintf(f, "%s\n%08lX\n%08lX\n%I64u\n%s\n", gamesPath, info.title.titleId, info.title.mediaId,
            info.outputSize, name);
    fclose(f);
}

static void ClearInstallMarker()
{
    remove(INSTALL_MARKER_FILE);
}

// At startup: if the last install never finished, take its partial package
// away. A package whose files already add up to the full size did finish -
// the app just ended before it could clear the marker - and is kept.
static void CleanUpInterruptedInstall()
{
    FILE *f = fopen(INSTALL_MARKER_FILE, "r");
    if (f == NULL)
        return;

    char root[MAX_TEXT_LENGTH] = "", idLine[32] = "", mediaLine[32] = "", sizeLine[32] = "", name[256] = "";
    bool ok = fgets(root, sizeof(root), f) && fgets(idLine, sizeof(idLine), f) &&
              fgets(mediaLine, sizeof(mediaLine), f) && fgets(sizeLine, sizeof(sizeLine), f);
    if (ok && fgets(name, sizeof(name), f) == NULL)
        name[0] = '\0';
    fclose(f);

    root[strcspn(root, "\r\n")] = '\0';
    name[strcspn(name, "\r\n")] = '\0';

    unsigned long titleId = strtoul(idLine, NULL, 16);
    unsigned long mediaId = strtoul(mediaLine, NULL, 16);
    unsigned long long expected = _strtoui64(sizeLine, NULL, 10);

    if (!ok || root[0] == '\0' || titleId == 0)
    {
        dprintf("[disc] %s is unreadable; removing it\n", INSTALL_MARKER_FILE);
        ClearInstallMarker();
        return;
    }

    // Its drive isn't here - a USB drive that has been unplugged. Kept for a
    // launch where it is, rather than forgetting the partial copy on it.
    if (!FolderExists(root))
    {
        dprintf("[disc] an unfinished install of \"%s\" is on %s, which isn't available; will retry next launch\n",
                name, root);
        return;
    }

    unsigned long long onDisk = GodPackageSizeOnDisk(root, titleId, mediaId);
    if (expected > 0 && onDisk == expected)
    {
        dprintf("[disc] \"%s\" had finished installing (%I64u bytes); keeping it\n", name, onDisk);
        ClearInstallMarker();
        return;
    }

    dprintf("[disc] removing the unfinished install of \"%s\" (%08lX, media %08lX) from %s\n",
            name, titleId, mediaId, root);
    RenderStatusFrame("CLEANING UP", "Removing a game install that didn't finish", name);
    GodRemovePackage(root, titleId, mediaId);
    ClearInstallMarker();
}

// Measures whether the drive reads faster with more requests waiting on it,
// to the log. Installs run at about 5.6 MB/s - half what the XDK gives for
// the drive - with the drive audibly slower than under Aurora's Disc to
// GOD, and reading through the file system instead of raw made no
// difference. The one remaining difference found so far is that every read
// here waits for the one before: if the drive only spins up when it sees a
// queue, more reads in flight will show it.
//
// Uses the file system's overlapped reads (documented XDK behaviour) on
// the disc's largest file, 1MB per read: 1, 2, 4 and 8 reads in flight, then
// 1 again - if that last one is fast too, the drive simply sped up over
// time, not because of the queue. Each run covers the next stretch of the
// same file, so all sit at about the same radius. Temporary: it adds about
// half a minute to an install.
#define SPEED_TEST_CHUNK    (1024UL * 1024)
#define SPEED_TEST_MAX_MB   32UL
#define SPEED_TEST_MAX_DEPTH 8

// Reads `bytes` from `start` with `depth` reads in flight, completing them
// in order. Returns MB/s, or 0 on a failed read.
static double TimeQueuedReads(HANDLE f, unsigned long long start, unsigned long bytes, int depth,
                              unsigned char *buffers, HANDLE *events)
{
    OVERLAPPED ov[SPEED_TEST_MAX_DEPTH];
    const unsigned long count = bytes / SPEED_TEST_CHUNK;
    unsigned long issued = 0, completed = 0;
    bool ok = true;

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    for (; ok && completed < count; )
    {
        // Keep `depth` reads outstanding.
        while (ok && issued < count && issued - completed < (unsigned long)depth)
        {
            int slot = (int)(issued % depth);
            unsigned long long at = start + (unsigned long long)issued * SPEED_TEST_CHUNK;
            memset(&ov[slot], 0, sizeof(ov[slot]));
            ov[slot].Offset = (DWORD)at;
            ov[slot].OffsetHigh = (DWORD)(at >> 32);
            ov[slot].hEvent = events[slot];
            ResetEvent(events[slot]);
            if (!ReadFile(f, buffers + slot * SPEED_TEST_CHUNK, SPEED_TEST_CHUNK, NULL, &ov[slot]) &&
                GetLastError() != ERROR_IO_PENDING)
            {
                ok = false; // never started, so never waited on
                break;
            }
            issued++;
        }
        if (!ok || completed == issued)
            break;

        // Wait for the oldest.
        int slot = (int)(completed % depth);
        DWORD got = 0;
        if (!GetOverlappedResult(f, &ov[slot], &got, TRUE) || got != SPEED_TEST_CHUNK)
            ok = false;
        completed++;
    }

    // Anything still in flight after a failure has to finish before its
    // buffer and OVERLAPPED go away.
    for (; completed < issued; ++completed)
    {
        DWORD got = 0;
        GetOverlappedResult(f, &ov[completed % depth], &got, TRUE);
    }

    QueryPerformanceCounter(&t1);
    double secs = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    return (ok && secs > 0) ? (double)bytes / (1024.0 * 1024.0) / secs : 0.0;
}

static void DiscSpeedTest(const GodImageInfo &info)
{
    static const int kDepths[] = { 1, 2, 4, 8, 1 };
    const int runs = sizeof(kDepths) / sizeof(kDepths[0]);

    // Up to 32MB per run, less on a disc with no file that big.
    unsigned long perRunMB = info.largestFileSize / (1024 * 1024) / runs;
    if (perRunMB > SPEED_TEST_MAX_MB)
        perRunMB = SPEED_TEST_MAX_MB;
    if (perRunMB < 8 || info.largestFile[0] == '\0')
    {
        dprintf("[speed] no file on this disc is big enough to measure with (largest %lu bytes)\n",
                info.largestFileSize);
        return;
    }
    const unsigned long perRun = perRunMB * 1024 * 1024;

    RenderStatusFrame("INSTALL DISC", "Measuring the drive's speed", "About half a minute, once, before copying.");

    mount("OmniDvd:", "\\Device\\Cdrom0");
    char path[300];
    _snprintf(path, sizeof(path), "OmniDvd:%s", info.largestFile);
    path[sizeof(path) - 1] = '\0';

    HANDLE f = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                          FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING, NULL);
    if (f == INVALID_HANDLE_VALUE)
    {
        dprintf("[speed] couldn't open %s (error %lu)\n", path, GetLastError());
        return;
    }

    unsigned char *buffers = (unsigned char *)VirtualAlloc(NULL, SPEED_TEST_MAX_DEPTH * SPEED_TEST_CHUNK,
                                                           MEM_COMMIT, PAGE_READWRITE);
    HANDLE events[SPEED_TEST_MAX_DEPTH];
    bool eventsOk = true;
    for (int i = 0; i < SPEED_TEST_MAX_DEPTH; ++i)
    {
        events[i] = CreateEvent(NULL, TRUE, FALSE, NULL);
        eventsOk = eventsOk && events[i] != NULL;
    }

    if (buffers != NULL && eventsOk)
    {
        dprintf("[speed] using %s (%lu MB), %lu MB per run\n", info.largestFile,
                info.largestFileSize / (1024 * 1024), perRunMB);

        for (int r = 0; r < runs; ++r)
        {
            double mbps = TimeQueuedReads(f, (unsigned long long)r * perRun, perRun, kDepths[r], buffers, events);
            dprintf("[speed] %d read%s in flight: %s%.2f MB/s\n", kDepths[r], kDepths[r] == 1 ? " " : "s",
                    mbps > 0 ? "" : "(read failed) ", mbps);
        }
    }

    for (int i = 0; i < SPEED_TEST_MAX_DEPTH; ++i)
        if (events[i] != NULL)
            CloseHandle(events[i]);
    if (buffers != NULL)
        VirtualFree(buffers, 0, MEM_RELEASE);
    CloseHandle(f);
}

// START on the library: reads the disc in the drive, asks for the name the
// dashboard should show, and installs it into the games folder - the same
// place the library is read from, so the game appears there straight after.
static void InstallDiscAsGame(Library &lib, const char *gamesPath, int &listSelection)
{
    RenderStatusFrame("INSTALL DISC", "Reading the disc", "The drive may take a few seconds to spin up.");

    DiscSource disc;
    if (!disc.Open(dprintf))
    {
        ShowMessageUI("NO DISC", "Put an Xbox 360 game disc in the drive and try again.",
                      "If one is in, it couldn't be read - the log says why.");
        return;
    }
    disc.LogProbe(dprintf);

    GodImageInfo info;
    GodResult result = GodInspect(&disc, &info);
    if (result != GOD_OK)
    {
        dprintf("[disc] not installable: %s\n", GodResultText(result));
        ShowMessageUI("CAN'T INSTALL THIS DISC", GodResultText(result),
                      (result == GOD_NOT_A_DISC_IMAGE) ? "Only Xbox 360 game discs can be installed." : NULL);
        return;
    }

    dprintf("[disc] %s disc: title %08lX, media %08lX, disc %u of %u, %I64u bytes used, package %I64u bytes in %lu parts\n",
            info.imageType, info.title.titleId, info.title.mediaId, info.title.discNumber, info.title.discCount,
            info.usedSize, info.outputSize, info.partCount);

    // The name. The bundled list only suggests it - its names are
    // community-edited - and the keyboard lets it be fixed before it goes on
    // the dashboard for good.
    char fallbackName[32];
    _snprintf(fallbackName, sizeof(fallbackName), "Title %08lX", info.title.titleId);
    fallbackName[sizeof(fallbackName) - 1] = '\0';

    const char *listed = LookupTitleName(info.title.titleId);
    const char *suggested = (listed != NULL) ? listed : fallbackName;

    WCHAR wideSuggested[128];
    Utf8ToWideText(suggested, wideSuggested, 128);

    WCHAR description[160];
    if (info.title.discCount > 1)
        swprintf_s(description, 160, L"The name the dashboard and Aurora show. This is disc %u of %u.",
                   (unsigned)info.title.discNumber, (unsigned)info.title.discCount);
    else
        swprintf_s(description, 160, L"The name the dashboard and Aurora show.");

    std::string typed;
    if (OpenKeyboardToString(XUSER_INDEX_ANY, &typed, L"Game Name", description, wideSuggested) != ERROR_SUCCESS)
        return; // cancelled
    TrimInPlace(typed);

    // Unchanged, the original is kept: the keyboard hands text back one byte
    // per character, which would turn a name like "Modern Warfare® 3" or a
    // Japanese title into question marks.
    std::string name;
    if (typed.empty() || typed == Utf8AsKeyboardText(suggested))
        name = suggested;
    else
        name = Latin1ToUtf8(typed);

    char headerPath[MAX_TEXT_LENGTH];
    _snprintf(headerPath, sizeof(headerPath), "%s\\%08lX\\00007000\\%08lX", gamesPath, info.title.titleId, info.title.mediaId);
    headerPath[sizeof(headerPath) - 1] = '\0';

    FILE *existing = fopen(headerPath, "rb");
    if (existing != NULL)
    {
        fclose(existing);
        if (!ShowConfirmUI("ALREADY INSTALLED", "This disc is already installed.",
                           "Installing it again replaces the copy on the drive.", "Reinstall"))
            return;
    }

    unsigned long long freeSpace = 0;
    if (DriveFreeSpace(gamesPath, &freeSpace) && freeSpace < info.outputSize + 4ULL * 1024 * 1024)
    {
        ShowNotEnoughSpace(gamesPath, info.outputSize, freeSpace);
        return;
    }

    char sizeText[64] = "";
    FormatBytes(info.outputSize, sizeText, sizeof(sizeText));

    char message[200], detail[MAX_TEXT_LENGTH + 64];
    _snprintf(message, sizeof(message), "Install %s?", name.c_str());
    message[sizeof(message) - 1] = '\0';
    _snprintf(detail, sizeof(detail), "%s, to %s", sizeText, gamesPath);
    detail[sizeof(detail) - 1] = '\0';

    if (!ShowConfirmUI("INSTALL DISC", message, detail, "Install"))
        return;

    DiscInstallProgress progress;
    progress.title = name.c_str();
    progress.startTick = GetTickCount();
    progress.lastFrameTick = 0;
    progress.prevButtons = AnyPadButtons(); // the A that confirmed is still down

    DiscSpeedTest(info);

    // From here until GodConvert returns, the marker is what cleans up if
    // the app is ended mid-copy. GodConvert handles every failure it sees -
    // cancelling, a read or write error - by removing its own output first,
    // so once it returns, by any path, there is nothing left to clean.
    WriteInstallMarker(gamesPath, info, name.c_str());

    char packagePath[MAX_TEXT_LENGTH] = "";
    GodTimings timings;
    memset(&timings, 0, sizeof(timings));
    {
        // The drive keeps reading while each group is hashed and written -
        // see ReadAhead.h. 8MB ahead, in 1MB reads. Scoped so its thread has
        // finished with the drive before the drive is closed.
        ReadAheadSource ahead(&disc, 1024 * 1024, 8);
        result = GodConvert(&ahead, info, gamesPath, name.c_str(), NULL, 0,
                            DiscInstallProgressCallback, &progress, packagePath, sizeof(packagePath), &timings);
    }

    ClearInstallMarker();

    DWORD seconds = (GetTickCount() - progress.startTick) / 1000;
    dprintf("[disc] install %s after %lu:%02lu: %s\n", GodResultText(result),
            (unsigned long)(seconds / 60), (unsigned long)(seconds % 60), packagePath);

    // Which part is the limit: reading the disc, hashing, or writing. With
    // read-ahead, "waiting for the disc" is only the time the drive couldn't
    // keep up.
    if (seconds > 0)
        dprintf("[disc] %I64u MB at %.2f MB/s - waiting for the disc %.0fs, hashing %.0fs, writing %.0fs, progress screen %.0fs\n",
                info.usedSize / (1024 * 1024), (double)info.usedSize / (1024.0 * 1024.0) / (double)seconds,
                timings.readMs / 1000.0, timings.hashMs / 1000.0, timings.writeMs / 1000.0, timings.progressMs / 1000.0);

    disc.Close();

    if (result == GOD_CANCELLED)
    {
        ShowMessageUI("INSTALL STOPPED", "Nothing was installed.", "What had been copied was removed.");
        return;
    }
    if (result != GOD_OK)
    {
        const char *hint = (result == GOD_READ_FAILED) ? "The disc may be dirty or scratched - clean it and try again."
                         : (result == GOD_WRITE_FAILED) ? "Check the drive the games folder is on, then try again."
                         : NULL;
        ShowMessageUI("INSTALL FAILED", GodResultText(result), hint);
        return;
    }

    char doneDetail[160];
    if (info.title.discCount > 1 && info.title.discNumber < info.title.discCount)
        _snprintf(doneDetail, sizeof(doneDetail), "That was disc %u of %u - put in disc %u and press START to install it.",
                  (unsigned)info.title.discNumber, (unsigned)info.title.discCount, (unsigned)info.title.discNumber + 1);
    else
        _snprintf(doneDetail, sizeof(doneDetail), "Play it from the dashboard or Aurora - the disc isn't needed.");
    doneDetail[sizeof(doneDetail) - 1] = '\0';

    _snprintf(message, sizeof(message), "%s is installed.", name.c_str());
    message[sizeof(message) - 1] = '\0';
    ShowMessageUI("INSTALLED", message, doneDetail);

    // Into the library, and onto its row.
    ScanLibrary(lib, gamesPath);
    for (int i = 0; i < lib.count; ++i)
    {
        if (lib.games[i].titleId == info.title.titleId)
        {
            listSelection = i;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

static void WaitForExit()
{
    dprintf("\nPress B to exit\n");

    XINPUT_STATE state;
    for (;;)
    {
        ZeroMemory(&state, sizeof(state));
        if (XInputGetState(0, &state) == ERROR_SUCCESS && (state.Gamepad.wButtons & XINPUT_GAMEPAD_B))
            break;
        Sleep(50);
    }
}

int main()
{
    remove(LOG_FILE_PATH);

    MakeConsole("embed:\\font", CONSOLE_COLOR_BLACK, CONSOLE_COLOR_WHITE);

    if (!CheckGameMounted())
        dprintf("Warning: Some paths may not be mounted\n");

    // The "fork of X-Store" half of this line is not decoration - AGPL-3.0
    // section 5(a) wants a modified work to say prominently that it has been
    // modified. Rename the product freely; leave the attribution alone.
    dprintf("Omni360 " CURRENT_VERSION " (fork of X-Store, https://github.com/951261/X-Store)\n");

    // Whether opening the disc tray should leave the app running (see
    // xex.xml), and whether DashLaunch is loaded - the first thing to check
    // if the app is ever closed by an eject again.
    {
        HANDLE dashLaunch = NULL;
        bool haveDashLaunch = XexGetModuleHandle("launch.xex", &dashLaunch) >= 0 && dashLaunch != NULL;
        dprintf("Stays open when the disc tray opens (no-force-reboot privilege): %s; DashLaunch %s\n",
                XexCheckExecutablePrivilege(0) ? "yes" : "NO", haveDashLaunch ? "loaded" : "not loaded");
    }

    if (!InitGameListUI())
    {
        dprintf("ERROR: failed to initialize the icon-list UI (D3D device not ready?)\n");
        WaitForExit();
        return EXIT_FAILURE;
    }

    // The drawn UI is up and confirmed working, so hand it the screen. Every
    // dprintf from here on still writes the log and still reaches the debug
    // channel - it just stops repainting the console over whatever the UI has
    // presented. Deliberately AFTER the check above, so a UI that failed to
    // initialize still has a visible way to say so.
    SetConsoleQuiet(true);

    char contentBasePath[MAX_TEXT_LENGTH];
    GetContentBasePath(contentBasePath, sizeof(contentBasePath));
    dprintf("Content path (DLC installs here): %s\n", contentBasePath);

    char gamesPath[MAX_TEXT_LENGTH];
    GetGamesPath(gamesPath, sizeof(gamesPath));
    dprintf("Games path (scanned for your library): %s\n", gamesPath);

    // First thing, so the space a half-installed game from last time took is
    // back before anything else checks for room. (It was never listed: the
    // header is written last, and without one the library doesn't see it.)
    CleanUpInterruptedInstall();

    // From here on the screen belongs to the UI rather than the debug console.
    // The dprintf calls stay - they're the log, and the log is still how
    // anything that goes wrong gets diagnosed - but every phase the user
    // actually waits on now draws a real frame instead of leaving whatever
    // was last on screen.
    Library lib;
    lib.count = 0;
    lib.games = (InstalledGame *)malloc(sizeof(InstalledGame) * MAX_INSTALLED_GAMES);

    if (lib.games == NULL)
    {
        dprintf("ERROR: out of memory for the game list\n");
        ShutdownGameListUI();
        return EXIT_FAILURE;
    }

    // An empty library is NOT a reason to quit any more. It used to end the
    // app with a message, which left someone whose games folder was simply
    // wrong no way to fix it from the console - the game list now says it is
    // empty, names the folder it searched, and offers Settings.
    ScanLibrary(lib, gamesPath);

    // Session loop. The game list is the app's root screen, and B steps BACK a
    // screen everywhere else - out of the pack picker to here, out of here to
    // the dashboard. Previously every B unwound straight out of main(), so
    // finishing one download, or changing your mind at any point, dropped you
    // out of the app entirely and made you relaunch to fetch a second pack.
    //
    // The library is scanned once (and again only if Settings changes the
    // folder) and the auth header is built once; both are held across the loop
    // so returning here costs nothing.
    char authHeader[IAS3_AUTH_HEADER_MAX];
    bool haveAuth = false;
    int listSelection = 0;

    for (;;)
    {
        // Which titles already have DLC, and which already have a title
        // update, on the console. Refreshed on every pass rather than only at
        // startup, so the markers appear the moment someone comes back from a
        // download instead of on the next launch.
        RefreshInstalledFlags(contentBasePath, lib.games, lib.count, lib.dlcInstalled, lib.updateInstalled);

        // Checked on every pass, not once at startup, so the banner goes away
        // as soon as keys are added in Settings - and comes back if they're
        // removed. One small file read per return to this screen.
        std::string savedAccess, savedSecret;
        const bool keysSaved = LoadSavedKeys(savedAccess, savedSecret);

        // Read here, on every return to the list, so it reflects the download
        // that just finished. Where DLC and title updates install, not where
        // the games are - that's the drive that fills up.
        char freeSpaceStatus[96];
        FormatFreeSpaceStatus(contentBasePath, freeSpaceStatus, sizeof(freeSpaceStatus));

        GameListUIResult pick = ShowGameListUI(lib.games, lib.count, listSelection,
                                               lib.dlcInstalled, lib.updateInstalled, gamesPath,
                                               keysSaved ? NULL : "Add your archive.org keys in Settings to start downloading.",
                                               freeSpaceStatus);

        if (pick.action == GAMELIST_EXIT)
        {
            dprintf("Exiting\n");
            break; // B on the root screen is the way out
        }

        if (pick.action == GAMELIST_SETTINGS)
        {
            if (pick.selectedIndex >= 0)
                listSelection = pick.selectedIndex;

            SettingsOutcome changed = RunSettingsUI(lib, gamesPath, sizeof(gamesPath));

            // A different library makes the old row number meaningless.
            if (changed.libraryChanged)
                listSelection = 0;

            // The cached header was built from the old keys - or from keys
            // that no longer exist.
            if (changed.keysChanged)
                haveAuth = false;

            continue;
        }

        if (pick.action == GAMELIST_INSTALL_DISC)
        {
            if (pick.selectedIndex >= 0)
                listSelection = pick.selectedIndex;

            // No archive.org keys needed: nothing here touches the network.
            InstallDiscAsGame(lib, gamesPath, listSelection);
            continue;
        }

        listSelection = pick.selectedIndex; // return them to the same row afterwards

        const InstalledGame &chosen = lib.games[pick.selectedIndex];
        dprintf("Selected: %s (Title ID %08lX)\n", chosen.displayName, chosen.titleId);

        if (!haveAuth)
        {
            // The keyboard prompt inside here draws its own system UI, so this
            // frame is only what sits behind it on a run where the keys are
            // already cached and nothing is prompted at all.
            RenderStatusFrame("SIGNING IN", "Using your saved archive.org keys", chosen.displayName);

            // GetArchiveOrgAuthHeader explains its own failures on screen, so
            // nothing more is shown here - a second message would only repeat it.
            if (!GetArchiveOrgAuthHeader(authHeader, sizeof(authHeader)))
                continue; // back to the list, so they can fix it and retry rather than being thrown out

            haveAuth = true;
        }

        if (pick.action == GAMELIST_TITLE_UPDATES)
            InstallTitleUpdatesForGame(chosen, contentBasePath, authHeader);
        else
            DownloadDlcForGame(chosen, contentBasePath, authHeader);

        // Refused keys: rebuild the header from the file next time, so keys
        // fixed outside Settings - by replacing ArchiveOrgKeys.txt over FTP -
        // are picked up without restarting the app.
        if (ArchiveOrgKeysRejected())
            haveAuth = false;
    }

    free(lib.games);

    dprintf("Done.\n");

    ShutdownGameListUI();

    return EXIT_SUCCESS;
}
