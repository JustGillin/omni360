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
// comment above the pickers.)

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

// Set once the user confirms stopping, so the code that started the download
// can tell a cancel apart from a failure - and stays set until the next
// download resets it, so every callback after the first refusal keeps
// refusing rather than asking again.
static bool g_progressCancelled = false;
static WORD g_progressPrevButtons = 0;

static WORD AnyPadButtons(); // below, with the disc install

// Call before each pack or title update. Seeds the button state from the pad
// as it is now, so the A that started the download can't count as a press.
static void ResetDownloadCancel()
{
    g_progressCancelled = false;
    g_progressPrevButtons = AnyPadButtons();
}

// B asks before stopping, as the disc install does. Returns false once the
// user has confirmed.
//
// While the question is up nothing reads from the connection, so a long pause
// before answering "no" can leave the server to give up on it - the same as
// the old Start-to-pause had. This goes away once downloads run on their own
// thread and the UI asks without holding them up.
static bool KeepDownloading()
{
    if (g_progressCancelled)
        return false;

    WORD buttons = AnyPadButtons();
    WORD pressed = buttons & ~g_progressPrevButtons;
    g_progressPrevButtons = buttons;

    if (pressed & XINPUT_GAMEPAD_B)
    {
        if (ShowConfirmUI("STOP DOWNLOADING?", g_progressTitle,
                          "Files that already finished stay installed.", "Stop"))
        {
            dprintf("Download cancelled by the user\n");
            g_progressCancelled = true;
            return false;
        }
        g_progressPrevButtons = AnyPadButtons(); // the B that answered "no" isn't a fresh press
    }

    return true;
}

// Called from inside downloadFile.cpp's read loop, roughly every 100ms, with
// live byte counts for the file currently downloading. This replaces the old
// behaviour where the progress bar only advanced once per completed member -
// meaning it sat frozen for the entire duration of each actual download while
// the real numbers scrolled past in the debug console.
static bool DlcProgressCallback(unsigned long long bytesDone,
                                unsigned long long bytesTotal,
                                unsigned long long bytesPerSec,
                                unsigned long long secondsRemaining)
{
    if (!KeepDownloading())
        return false;

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

        _snprintf(detail, sizeof(detail), "%s / %s   %s/s   %d:%02d left   -   B to stop",
                  done, total, speed, minutesLeft, secsLeft);
    }
    else
    {
        // No Content-Length and no size hint - report what we can rather than
        // implying a percentage we don't have.
        _snprintf(detail, sizeof(detail), "%s   %s/s   -   B to stop", done, speed);
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
    return true;
}

// Drawn after each header ListDlcMembers reads. That walk is one network round
// trip per entry, and on a large pack it went on for a long time behind a
// single static "Reading the file list" frame, looking hung.
//
// The bar is how far through the archive the walk has got. It moves in uneven
// jumps - each step skips over a whole member's data - so the counts under it
// are what show it's still going between jumps. The screen can't animate
// during a request itself: each one blocks until archive.org answers.
static bool ListMembersProgressCallback(unsigned long long bytesScanned, unsigned long long archiveSize,
                                        int filesToInstall, int avatarItemsSkipped)
{
    if (!KeepDownloading())
        return false;

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
    return true;
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

    ResetDownloadCancel();

    DlcMember members[MAX_DLC_MEMBERS];
    int memberCount = ListDlcMembers(pack.filename, pack.size, members, MAX_DLC_MEMBERS, authHeader, dprintf,
                                     ListMembersProgressCallback);

    // Stopped on purpose - straight back to the pack picker. The confirmation
    // already said what happens, so there's nothing to add.
    if (g_progressCancelled)
        return true;

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
            if (g_progressCancelled)
                return true; // not a failure - see the file-list check above

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

// ---------------------------------------------------------------------------
// The DLC pack and title update pickers
// ---------------------------------------------------------------------------
//
// A on a game searches for its DLC, X for its title updates; either way the
// results open as a picker in place of the library, and B steps back to it.
//
// ALWAYS a picker, even for a single perfectly-scored result. The DLC side
// briefly auto-selected an unambiguous match and skipped straight to
// downloading. That was removed after actually living with it: the picker is
// not friction, it is the one place you can see the real filename and size
// before committing to a download that may be gigabytes, and the one place to
// back out of a game selected by mistake. For title updates it's also the
// point: one game routinely has v1/v2/v3 across several regions, and "newest"
// is not always what someone wants - a specific version is sometimes needed
// for mods or for matchmaking compatibility.
//
// The picker stays open after a download, so a game with several separate
// packs - Call of Duty 2 has three - can have them taken one at a time.

enum PickerKind
{
    PICKER_NONE,
    PICKER_DLC,
    PICKER_TITLE_UPDATE
};

#define MAX_PICKER_ROWS (MAX_DLC_RAR_MATCHES > MAX_TITLE_UPDATE_MATCHES ? MAX_DLC_RAR_MATCHES : MAX_TITLE_UPDATE_MATCHES)

struct Picker
{
    PickerKind kind;
    int gameIndex; // into the library
    int count;
    int selected;
    int scroll;

    DlcRarMatch packs[MAX_DLC_RAR_MATCHES];
    TitleUpdateMatch updates[MAX_TITLE_UPDATE_MATCHES];

    // What the list page draws. labels point into packs/updates.
    const char *labels[MAX_PICKER_ROWS];
    const char *sublabels[MAX_PICKER_ROWS];
    char subText[MAX_PICKER_ROWS][96];
};

// Searches for the game's DLC and fills the picker with what it finds. Says so
// on screen and returns false if the search failed or found nothing.
static bool FindDlcForPicker(const InstalledGame &game, Picker &picker)
{
    RenderStatusFrame("SEARCHING", "Looking up DLC on archive.org", game.displayName);

    int matchCount = FindDlcRarFilenames(game.displayName, picker.packs, MAX_DLC_RAR_MATCHES, dprintf);

    if (matchCount < 0)
    {
        ShowMessageUI("SEARCH FAILED", "Could not reach archive.org.",
                      "Check the console's network connection and try again.");
        return false;
    }

    if (matchCount == 0)
    {
        ShowMessageUI("NOTHING FOUND", "No DLC in the collection matched this game.",
                      game.displayName);
        return false;
    }

    // The filename is what actually identifies a pack, and the size plus
    // match confidence are what let someone judge between two
    // similar-looking entries.
    for (int i = 0; i < matchCount; ++i)
    {
        picker.labels[i] = picker.packs[i].filename;

        char sizeText[64] = "";
        FormatBytes(picker.packs[i].size, sizeText, sizeof(sizeText));

        _snprintf(picker.subText[i], sizeof(picker.subText[i]), "%s   %d%% name match",
                  sizeText, picker.packs[i].score);
        picker.subText[i][sizeof(picker.subText[i]) - 1] = '\0';
        picker.sublabels[i] = picker.subText[i];
    }

    picker.kind = PICKER_DLC;
    picker.count = matchCount;
    return true;
}

static bool FindTitleUpdatesForPicker(const InstalledGame &game, Picker &picker)
{
    RenderStatusFrame("SEARCHING", "Looking up title updates", game.displayName);

    int updateCount = FindTitleUpdates(game.displayName, picker.updates, MAX_TITLE_UPDATE_MATCHES, dprintf);

    if (updateCount < 0)
    {
        ShowMessageUI("SEARCH FAILED", "Could not reach archive.org.",
                      "Check the console's network connection and try again.");
        return false;
    }

    if (updateCount == 0)
    {
        ShowMessageUI("NOTHING FOUND", "No title update matched this game.", game.displayName);
        return false;
    }

    for (int i = 0; i < updateCount; ++i)
    {
        const TitleUpdateMatch &update = picker.updates[i];

        // "score N", not "N%" - a literal percent sign does not survive this
        // logging path (see LogEscapePercent in parsing.h), which is why an
        // earlier run of this printed "100v1" instead of "100%  v1".
        dprintf("[TU]   score %d  v%d  region \"%s\"  %I64u bytes  %s\n",
                update.score, update.version, update.region, update.size, update.filename);

        picker.labels[i] = update.filename;

        char sizeText[64] = "";
        FormatBytes(update.size, sizeText, sizeof(sizeText));

        _snprintf(picker.subText[i], sizeof(picker.subText[i]), "%s   v%d   %s   %d%% name match",
                  sizeText, update.version,
                  update.region[0] != '\0' ? update.region : "unknown region",
                  update.score);
        picker.subText[i][sizeof(picker.subText[i]) - 1] = '\0';
        picker.sublabels[i] = picker.subText[i];
    }

    // Results are sorted score-first then version-descending, so the first row
    // is the newest update for the best-matching name - the right default to
    // land on, but still shown rather than assumed.
    picker.kind = PICKER_TITLE_UPDATE;
    picker.count = updateCount;
    return true;
}

// Downloads and installs one title update, saying how it went.
//
// Returns false if archive.org refused the keys, so the caller closes the
// picker: every other update would be refused the same way, and the fix is in
// Settings.
static bool InstallOneTitleUpdate(const TitleUpdateMatch &update, const InstalledGame &game,
                                  const char *contentBasePath, const char *authHeader)
{
    RenderStatusFrame("TITLE UPDATE", "Reading the update", update.filename);

    strncpy(g_progressTitle, update.filename, sizeof(g_progressTitle) - 1);
    g_progressTitle[sizeof(g_progressTitle) - 1] = '\0';
    g_progressFileIndex = 0;
    g_progressFileCount = 1;
    ResetDownloadCancel();

    unsigned long long tuNeeded = 0, tuFree = 0;
    if (DownloadTitleUpdate(update, game.titleId, contentBasePath, authHeader, dprintf, DlcProgressCallback))
    {
        ShowMessageUI("TITLE UPDATE INSTALLED", update.filename,
                      "Restart your dashboard to pick up the update.");
    }
    else if (g_progressCancelled)
    {
        // Stopped on purpose - back to the picker with nothing to report.
    }
    else if (ArchiveOrgKeysRejected())
    {
        // Not "archive.org may not serve this file" - that sends someone to
        // try a different update, which will be refused the same way.
        ShowKeysRejected();
        return false;
    }
    else if (ArchiveOrgDiskFull(&tuNeeded, &tuFree))
    {
        ShowNotEnoughSpace(contentBasePath, tuNeeded, tuFree);
    }
    else
    {
        ShowMessageUI("TITLE UPDATE FAILED", update.filename,
                      "See the log - archive.org may not serve this file directly.");
    }

    return true;
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
    // flags, the library selection) indexes into this array after the sort, so nothing
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

// The settings page: a short list whose second lines show the current state,
// so it doubles as a summary of how the app is set up. Rebuilt whenever
// something may have changed it, rather than every frame - each rebuild reads
// the keys file.
#define MAX_SETTINGS_ROWS 3

struct SettingsPage
{
    int count;
    SettingsRow rows[MAX_SETTINGS_ROWS];
    const char *labels[MAX_SETTINGS_ROWS];
    const char *sublabels[MAX_SETTINGS_ROWS];
    char gamesSub[MAX_TEXT_LENGTH + 64];
    char keysSub[128];

    int selected;
    int scroll;
};

static void BuildSettingsPage(SettingsPage &page, const Library &lib, const char *gamesPath)
{
    std::string accessKey, secretKey;
    const bool haveKeys = LoadSavedKeys(accessKey, secretKey);

    if (lib.count > 0)
        _snprintf(page.gamesSub, sizeof(page.gamesSub), "%s   -   %d game%s", gamesPath, lib.count,
                  lib.count == 1 ? "" : "s");
    else
        _snprintf(page.gamesSub, sizeof(page.gamesSub), "%s   -   no games found here", gamesPath);
    page.gamesSub[sizeof(page.gamesSub) - 1] = '\0';

    // Only the start of the access key is shown - enough to tell which keys
    // are saved. The secret key is never shown anywhere.
    if (haveKeys)
    {
        char shown[5] = "";
        strncpy(shown, accessKey.c_str(), 4);
        shown[4] = '\0';
        _snprintf(page.keysSub, sizeof(page.keysSub), "Saved   -   access key %s...", shown);
    }
    else
    {
        _snprintf(page.keysSub, sizeof(page.keysSub),
                  "Not set   -   needed to download. Get them at archive.org/account/s3.php");
    }
    page.keysSub[sizeof(page.keysSub) - 1] = '\0';

    page.count = 0;

    page.labels[page.count] = "Games folder";
    page.sublabels[page.count] = page.gamesSub;
    page.rows[page.count++] = SETTINGS_ROW_GAMES_FOLDER;

    page.labels[page.count] = haveKeys ? "Change archive.org keys" : "Add archive.org keys";
    page.sublabels[page.count] = page.keysSub;
    page.rows[page.count++] = SETTINGS_ROW_KEYS;

    // Only offered when there's something to remove.
    if (haveKeys)
    {
        page.labels[page.count] = "Remove archive.org keys";
        page.sublabels[page.count] = "Deletes the saved keys from this console";
        page.rows[page.count++] = SETTINGS_ROW_REMOVE_KEYS;
    }

    if (page.selected > page.count - 1)
        page.selected = page.count - 1; // the Remove row just went away
}

static SettingsOutcome RunSettingsRow(SettingsRow row, Library &lib, char *gamesPath, size_t gamesPathSize)
{
    SettingsOutcome outcome = {false, false};

    switch (row)
    {
    case SETTINGS_ROW_GAMES_FOLDER: ChangeGamesFolder(lib, gamesPath, gamesPathSize, outcome); break;
    case SETTINGS_ROW_KEYS:         ChangeKeys(outcome); break;
    case SETTINGS_ROW_REMOVE_KEYS:  RemoveKeys(outcome); break;
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

// ---------------------------------------------------------------------------
// The shell: a sidebar of pages, and one loop driving it
// ---------------------------------------------------------------------------

struct Shell
{
    ShellPage page;
    bool sidebarFocused;

    int librarySelected;
    int libraryScroll; // -1 until placed - see LibraryPageView

    SettingsPage settings;
    Picker picker; // open over the library while picker.kind != PICKER_NONE

    // What takes a file read or a drive query to find out, so it's refreshed
    // only when something may have changed it - on the way back from any
    // action - rather than on every frame.
    bool stale;
    bool keysSaved;
    char freeSpace[96];
};

// Static rather than on main's stack: the picker's match arrays alone are
// several KB.
static Shell g_shell;

// Store is a placeholder, and Queue has nothing to choose yet - on those, the
// sidebar keeps focus.
static bool PageTakesFocus(ShellPage page)
{
    return page == SHELL_PAGE_LIBRARY || page == SHELL_PAGE_SETTINGS;
}

static void RefreshShell(Shell &shell, Library &lib, const char *contentBasePath, const char *gamesPath)
{
    // Which titles already have DLC, and which already have a title update,
    // on the console - so a marker appears the moment a download finishes
    // rather than on the next launch.
    RefreshInstalledFlags(contentBasePath, lib.games, lib.count, lib.dlcInstalled, lib.updateInstalled);

    // The banner goes as soon as keys are added in Settings, and comes back
    // if they're removed.
    std::string savedAccess, savedSecret;
    shell.keysSaved = LoadSavedKeys(savedAccess, savedSecret);

    // Where DLC and title updates install, not where the games are - that's
    // the drive that fills up.
    FormatFreeSpaceStatus(contentBasePath, shell.freeSpace, sizeof(shell.freeSpace));

    BuildSettingsPage(shell.settings, lib, gamesPath);

    shell.stale = false;
}

// Every frame, so the blocking screens a download draws show the sidebar as
// it was when the download started.
static void PublishSidebar(const Shell &shell, const Library &lib)
{
    ShellSidebar sidebar;
    sidebar.page = shell.page;
    sidebar.focused = shell.sidebarFocused;
    sidebar.libraryCount = lib.count;
    sidebar.queueCount = 0;
    strncpy(sidebar.storageText, shell.freeSpace, sizeof(sidebar.storageText) - 1);
    sidebar.storageText[sizeof(sidebar.storageText) - 1] = '\0';

    SetShellSidebar(sidebar);
}

static LibraryPageView MakeLibraryView(const Shell &shell, const Library &lib, const char *gamesPath)
{
    LibraryPageView view;
    view.games = lib.games;
    view.count = lib.count;
    view.selected = shell.librarySelected;
    view.scroll = shell.libraryScroll;
    view.hasDlcInstalled = lib.dlcInstalled;
    view.hasUpdateInstalled = lib.updateInstalled;
    view.gamesPath = gamesPath;
    view.bannerText = shell.keysSaved ? NULL : "Add your archive.org keys in Settings to start downloading.";
    view.focused = !shell.sidebarFocused;
    return view;
}

// One row up or down for an up/down nav, clamped to the list.
static void StepSelection(WORD nav, int count, int &selected)
{
    if (nav == XINPUT_GAMEPAD_DPAD_UP && selected > 0)
        selected--;
    else if (nav == XINPUT_GAMEPAD_DPAD_DOWN && selected < count - 1)
        selected++;
}

static int AddHint(UiHint *hints, int count, UiButton button, const WCHAR *label, const WCHAR *shortLabel = NULL)
{
    hints[count].button = button;
    hints[count].label = label;
    hints[count].shortLabel = shortLabel;
    return count + 1;
}

// The auth header is built once and kept, and only rebuilt after the keys
// change or archive.org refuses them.
static bool EnsureAuthHeader(bool &haveAuth, char *authHeader, unsigned long long authHeaderSize,
                             const char *gameName)
{
    if (haveAuth)
        return true;

    // The keyboard prompt inside here draws its own system UI, so this frame
    // is only what sits behind it on a run where the keys are already saved
    // and nothing is prompted at all.
    RenderStatusFrame("SIGNING IN", "Using your saved archive.org keys", gameName);

    // GetArchiveOrgAuthHeader explains its own failures on screen, so nothing
    // more is shown here - a second message would only repeat it.
    if (!GetArchiveOrgAuthHeader(authHeader, authHeaderSize))
        return false;

    haveAuth = true;
    return true;
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

    // The shell loop: read the controller, act on it, draw a frame.
    //
    // It starts on the library, with focus in the list, since that's where
    // almost every visit is headed. B steps back a level everywhere - out of a
    // picker to the library, out of a page to the sidebar - and only B on the
    // sidebar leaves the app.
    //
    // Anything that takes over the screen - a search, a download, the
    // keyboard, a message - still blocks inside this loop for now and draws
    // its own frames. When it returns, the input is resynced so the button
    // that ended it can't also act here, and everything the action may have
    // changed is refreshed before the next frame.
    //
    // The library is scanned once (and again only if Settings changes the
    // folder) and the auth header is built once; both are held across the
    // loop, so coming back from anything costs nothing.
    Shell &shell = g_shell;
    shell.page = SHELL_PAGE_LIBRARY;
    shell.sidebarFocused = false;
    shell.librarySelected = 0;
    shell.libraryScroll = -1;
    shell.settings.selected = 0;
    shell.settings.scroll = -1;
    shell.picker.kind = PICKER_NONE;
    shell.stale = true;

    char authHeader[IAS3_AUTH_HEADER_MAX];
    bool haveAuth = false;

    ResyncUiInput();

    for (;;)
    {
        if (shell.stale)
            RefreshShell(shell, lib, contentBasePath, gamesPath);

        PublishSidebar(shell, lib);

        UiInput input = PollUiInput();
        const WORD pressed = input.pressed;

        bool acted = false; // something ran that took over the screen
        bool exitRequested = false;

        if (shell.sidebarFocused)
        {
            if (input.nav == XINPUT_GAMEPAD_DPAD_UP && shell.page > 0)
                shell.page = (ShellPage)(shell.page - 1);
            else if (input.nav == XINPUT_GAMEPAD_DPAD_DOWN && shell.page < SHELL_PAGE_COUNT - 1)
                shell.page = (ShellPage)(shell.page + 1);
            else if ((input.nav == XINPUT_GAMEPAD_DPAD_RIGHT || (pressed & XINPUT_GAMEPAD_A)) &&
                     PageTakesFocus(shell.page))
                shell.sidebarFocused = false;
            else if (pressed & XINPUT_GAMEPAD_B)
                exitRequested = true;
        }
        else if (shell.page == SHELL_PAGE_LIBRARY && shell.picker.kind != PICKER_NONE)
        {
            Picker &picker = shell.picker;
            StepSelection(input.nav, picker.count, picker.selected);

            if (pressed & XINPUT_GAMEPAD_A)
            {
                const InstalledGame &game = lib.games[picker.gameIndex];

                // False means archive.org refused the keys - every other row
                // would be refused too, so back to the library, where the fix
                // (Settings) is a step away.
                bool keepOpen = (picker.kind == PICKER_DLC)
                                    ? DownloadOnePack(picker.packs[picker.selected], contentBasePath, authHeader)
                                    : InstallOneTitleUpdate(picker.updates[picker.selected], game,
                                                            contentBasePath, authHeader);
                if (!keepOpen)
                    picker.kind = PICKER_NONE;

                // Refused keys: rebuild the header from the file next time,
                // so keys fixed outside Settings - by replacing
                // ArchiveOrgKeys.txt over FTP - are picked up without
                // restarting the app.
                if (ArchiveOrgKeysRejected())
                    haveAuth = false;

                acted = true;
            }
            else if (pressed & XINPUT_GAMEPAD_B)
            {
                picker.kind = PICKER_NONE;
            }
        }
        else if (shell.page == SHELL_PAGE_LIBRARY)
        {
            StepSelection(input.nav, lib.count, shell.librarySelected);

            // Shoulder buttons jump a full page - the fast way through a large
            // library even with auto-repeat. Skipped for an empty library,
            // where the upper clamp would land on -1.
            if (lib.count > 0 && (pressed & (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER)))
            {
                int pageRows = LibraryPageVisibleRows(MakeLibraryView(shell, lib, gamesPath));
                if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER)
                    shell.librarySelected -= pageRows;
                if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER)
                    shell.librarySelected += pageRows;
                if (shell.librarySelected > lib.count - 1) shell.librarySelected = lib.count - 1;
                if (shell.librarySelected < 0) shell.librarySelected = 0;
            }

            if (input.nav == XINPUT_GAMEPAD_DPAD_LEFT || (pressed & XINPUT_GAMEPAD_B))
            {
                shell.sidebarFocused = true;
            }
            else if ((pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_X)) && lib.count > 0)
            {
                const InstalledGame &chosen = lib.games[shell.librarySelected];
                dprintf("Selected: %s (Title ID %08lX)\n", chosen.displayName, chosen.titleId);

                if (EnsureAuthHeader(haveAuth, authHeader, sizeof(authHeader), chosen.displayName))
                {
                    bool found = (pressed & XINPUT_GAMEPAD_A) ? FindDlcForPicker(chosen, shell.picker)
                                                              : FindTitleUpdatesForPicker(chosen, shell.picker);
                    if (found)
                    {
                        shell.picker.gameIndex = shell.librarySelected;
                        shell.picker.selected = 0;
                        shell.picker.scroll = -1;
                    }
                }

                acted = true;
            }
            else if (pressed & XINPUT_GAMEPAD_Y)
            {
                // A shortcut, for the keys banner and the empty library, which
                // both send you to Settings with a Y badge.
                shell.page = SHELL_PAGE_SETTINGS;
            }
            else if (pressed & XINPUT_GAMEPAD_START)
            {
                // No archive.org keys needed: nothing here touches the network.
                InstallDiscAsGame(lib, gamesPath, shell.librarySelected);
                shell.libraryScroll = -1; // it may have moved to the game just installed
                acted = true;
            }
        }
        else if (shell.page == SHELL_PAGE_SETTINGS)
        {
            SettingsPage &settings = shell.settings;
            StepSelection(input.nav, settings.count, settings.selected);

            if (input.nav == XINPUT_GAMEPAD_DPAD_LEFT || (pressed & XINPUT_GAMEPAD_B))
            {
                shell.sidebarFocused = true;
            }
            else if ((pressed & XINPUT_GAMEPAD_A) && settings.count > 0)
            {
                SettingsOutcome changed = RunSettingsRow(settings.rows[settings.selected],
                                                         lib, gamesPath, sizeof(gamesPath));

                // A different library makes the old row number meaningless.
                if (changed.libraryChanged)
                {
                    shell.librarySelected = 0;
                    shell.libraryScroll = -1;
                }

                // The cached header was built from the old keys - or from
                // keys that no longer exist.
                if (changed.keysChanged)
                    haveAuth = false;

                acted = true;
            }
        }
        else
        {
            // A page that never takes focus. Nothing should land here, but if
            // it does, hand focus back rather than leave the pad doing nothing.
            shell.sidebarFocused = true;
        }

        if (exitRequested)
        {
            dprintf("Exiting\n");
            break;
        }

        if (acted)
        {
            ResyncUiInput();
            shell.stale = true;
            continue; // refresh first, then draw
        }

        // --- Draw ---
        UiHint hints[8];
        int hintCount = 0;

        if (shell.sidebarFocused)
        {
            if (PageTakesFocus(shell.page))
                hintCount = AddHint(hints, hintCount, UI_BUTTON_A, L"Select");
            hintCount = AddHint(hints, hintCount, UI_BUTTON_B, L"Exit");
        }

        switch (shell.page)
        {
        case SHELL_PAGE_LIBRARY:
            if (shell.picker.kind != PICKER_NONE)
            {
                hintCount = AddHint(hints, hintCount, UI_BUTTON_A, L"Download");
                hintCount = AddHint(hints, hintCount, UI_BUTTON_B, L"Back");

                ListPageView view;
                view.heading = (shell.picker.kind == PICKER_DLC) ? "CHOOSE A DLC PACK" : "CHOOSE A TITLE UPDATE";
                view.labels = shell.picker.labels;
                view.sublabels = shell.picker.sublabels;
                view.count = shell.picker.count;
                view.selected = shell.picker.selected;
                view.scroll = shell.picker.scroll;
                view.focused = true;
                view.showCounter = true;

                RenderListFrame(view, hints, hintCount);

                shell.picker.selected = view.selected;
                shell.picker.scroll = view.scroll;
            }
            else
            {
                LibraryPageView view = MakeLibraryView(shell, lib, gamesPath);

                if (!shell.sidebarFocused)
                {
                    // The console's own A, X, B order, with START - installing
                    // a disc, which works on an empty library too - ahead of
                    // them. The row actions drop out for an empty library.
                    hintCount = AddHint(hints, hintCount, UI_BUTTON_START, L"Install disc", L"Disc");
                    if (lib.count > 0)
                    {
                        hintCount = AddHint(hints, hintCount, UI_BUTTON_A, L"Search for DLC", L"DLC");
                        hintCount = AddHint(hints, hintCount, UI_BUTTON_X, L"Search for title updates", L"Updates");
                    }
                    hintCount = AddHint(hints, hintCount, UI_BUTTON_B, L"Back");
                    if (lib.count > LibraryPageVisibleRows(view))
                        hintCount = AddHint(hints, hintCount, UI_BUTTON_LBRB, L"Page");
                }

                RenderLibraryFrame(view, hints, hintCount);

                shell.librarySelected = view.selected;
                shell.libraryScroll = view.scroll;
            }
            break;

        case SHELL_PAGE_STORE:
            RenderPlaceholderFrame("STORE", "Coming soon",
                                   "Installing games straight from archive.org is on the way.",
                                   hints, hintCount);
            break;

        case SHELL_PAGE_QUEUE:
            RenderPlaceholderFrame("QUEUE", "Nothing is downloading",
                                   "DLC and title updates you choose will show here while they download.",
                                   hints, hintCount);
            break;

        case SHELL_PAGE_SETTINGS:
        {
            if (!shell.sidebarFocused)
            {
                hintCount = AddHint(hints, hintCount, UI_BUTTON_A, L"Select");
                hintCount = AddHint(hints, hintCount, UI_BUTTON_B, L"Back");
            }

            ListPageView view;
            view.heading = "SETTINGS";
            view.labels = shell.settings.labels;
            view.sublabels = shell.settings.sublabels;
            view.count = shell.settings.count;
            view.selected = shell.settings.selected;
            view.scroll = shell.settings.scroll;
            view.focused = !shell.sidebarFocused;
            view.showCounter = false;

            RenderListFrame(view, hints, hintCount);

            shell.settings.selected = view.selected;
            shell.settings.scroll = view.scroll;
            break;
        }

        default:
            break;
        }

        // Device is created with D3DPRESENT_INTERVAL_IMMEDIATE (no vsync), so
        // pace the loop by hand instead of hammering Present() as fast as the
        // CPU can spin. Not a real vsync wait - just ~60fps.
        Sleep(16);
    }

    free(lib.games);

    dprintf("Done.\n");

    ShutdownGameListUI();

    return EXIT_SUCCESS;
}
