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
#include "DownloadQueue.h"
#include "SearchWorker.h"
#include "CoverArt.h"
#include "StoreArt.h"
#include "StoreCatalog.h"
#include "DiscWorker.h"
#include "GameInstaller.h"
#include "UpdateCheck.h"
#include "ArchiveOrgDLC.h"
#include "downloadFile.h" // DownloadProgressFn + FormatBytes, for the progress callback
#include "GodConvert.h"
#include "DiscSource.h"
#include "ReadAhead.h"
#include "TitleNames.h"
#include "dns.h"         // LogNetworkStatus

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
#define UPDATE_CHECK_KEY "check-updates: " // "off" stops the check at start
#define CREDENTIALS_FILE "game:\\ArchiveOrgKeys.txt"
#define CONTENT_BASE_PATH_DEFAULT "Hdd1:\\Content\\0000000000000000"
#define GAMES_PATH_DEFAULT CONTENT_BASE_PATH_DEFAULT // where the dashboard itself keeps installed games
#define MAX_INSTALLED_GAMES 256

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

    // The USB ports, by Aurora's names, for library folders and installs on
    // a USB drive formatted FAT32. A port with nothing in it just reads as
    // empty. (A drive the dashboard formatted as Xbox storage keeps its
    // files inside a container these don't reach.)
    static const char *const usbDrives[][2] = {
        { "Usb0:", "\\Device\\Mass0" },
        { "Usb1:", "\\Device\\Mass1" },
        { "Usb2:", "\\Device\\Mass2" },
    };
    for (int i = 0; i < 3; ++i)
        mount(usbDrives[i][0], (char *)usbDrives[i][1]);

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

// More library folders: every "games-path:" line after the first. The first
// is where games install to, and the one Settings changes; these are only
// scanned - games spread across drives, a USB drive's Content folder or a
// Games folder of Games on Demand. Added by editing settings.txt.
#define MAX_MORE_GAMES_PATHS 7
static char g_moreGamesPaths[MAX_MORE_GAMES_PATHS][MAX_TEXT_LENGTH];
static int g_moreGamesPathCount = 0;
static int g_moreGamesFound[MAX_MORE_GAMES_PATHS]; // the games each added, at the last scan

static void ReadMoreGamesPaths()
{
    g_moreGamesPathCount = 0;
    FILE *fd = fopen(SETTINGS_FILE, "r");
    if (fd == NULL)
        return;

    char line[512];
    const size_t keyLen = strlen(GAMES_PATH_KEY);
    bool first = true;
    while (fgets(line, sizeof(line), fd) != NULL && g_moreGamesPathCount < MAX_MORE_GAMES_PATHS)
    {
        if (line[0] == '#' || strncmp(line, GAMES_PATH_KEY, keyLen) != 0)
            continue;
        char *value = line + keyLen;
        value[strcspn(value, "\r\n")] = '\0';
        if (strlen(value) < 3)
            continue;
        if (first)
        {
            first = false; // GetGamesPath's
            continue;
        }
        _snprintf(g_moreGamesPaths[g_moreGamesPathCount], MAX_TEXT_LENGTH, "%s", value);
        g_moreGamesPaths[g_moreGamesPathCount][MAX_TEXT_LENGTH - 1] = '\0';
        g_moreGamesPathCount++;
    }
    fclose(fd);
}

// Whether to ask GitHub for a newer version at start. On unless turned off.
static bool UpdateChecksOn()
{
    char value[16];
    GetSettingsPath(UPDATE_CHECK_KEY, "yes", value, sizeof(value));
    return _stricmp(value, "off") != 0;
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

// Rewrites settings.txt's library folders: the folder games install to,
// first, then the others - in place of every "games-path:" line there was,
// where the first of them was, or at the end. Every other line stays as it
// was. Written to a temporary file and swapped in, as SetSettingsValue does.
static bool WriteGamesPaths(const char *first, char more[][MAX_TEXT_LENGTH], int moreCount)
{
    std::string paths = std::string(GAMES_PATH_KEY) + first + "\n";
    for (int i = 0; i < moreCount; ++i)
        paths += std::string(GAMES_PATH_KEY) + more[i] + "\n";

    std::string contents;
    bool placed = false;
    const size_t keyLen = strlen(GAMES_PATH_KEY);
    FILE *in = fopen(SETTINGS_FILE, "r");
    if (in != NULL)
    {
        char line[512];
        while (fgets(line, sizeof(line), in) != NULL)
        {
            if (line[0] != '#' && strncmp(line, GAMES_PATH_KEY, keyLen) == 0)
            {
                if (!placed)
                    contents += paths;
                placed = true;
                continue;
            }
            contents += line;
        }
        fclose(in);
        if (!contents.empty() && contents[contents.size() - 1] != '\n')
            contents += "\n";
    }
    if (!placed)
        contents += paths;

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

// The library folders Settings lists, after the one games install to: those
// added, then the likely places on each drive that aren't - a Content folder
// and a Games folder, on the hard drive and each USB port - that exist.
struct LibraryFolderRow
{
    char path[MAX_TEXT_LENGTH];
    bool scanned; // a "games-path:" line has it
    bool exists;
    int games;    // found there at the last scan, when scanned
};

#define MAX_FOLDER_ROWS (MAX_MORE_GAMES_PATHS + 8)
static LibraryFolderRow g_folderRows[MAX_FOLDER_ROWS];
static int g_folderRowCount = 0;

static bool FolderListed(const char *path, const char *gamesPath)
{
    if (_stricmp(path, gamesPath) == 0)
        return true;
    for (int i = 0; i < g_folderRowCount; ++i)
    {
        if (_stricmp(g_folderRows[i].path, path) == 0)
            return true;
    }
    return false;
}

static void FindLibraryFolders(const char *gamesPath)
{
    g_folderRowCount = 0;
    for (int p = 0; p < g_moreGamesPathCount && g_folderRowCount < MAX_FOLDER_ROWS; ++p)
    {
        if (FolderListed(g_moreGamesPaths[p], gamesPath))
            continue;
        LibraryFolderRow &row = g_folderRows[g_folderRowCount++];
        _snprintf(row.path, sizeof(row.path), "%s", g_moreGamesPaths[p]);
        row.path[sizeof(row.path) - 1] = '\0';
        row.scanned = true;
        row.exists = FolderExists(row.path);
        row.games = g_moreGamesFound[p];
    }

    static const char *const drives[] = { "Hdd1:", "Usb0:", "Usb1:", "Usb2:" };
    static const char *const folders[] = { "\\Content\\0000000000000000", "\\Games" };
    for (int d = 0; d < 4; ++d)
    {
        for (int f = 0; f < 2 && g_folderRowCount < MAX_FOLDER_ROWS; ++f)
        {
            char path[MAX_TEXT_LENGTH];
            _snprintf(path, sizeof(path), "%s%s", drives[d], folders[f]);
            path[sizeof(path) - 1] = '\0';
            if (FolderListed(path, gamesPath) || !FolderExists(path))
                continue;
            LibraryFolderRow &row = g_folderRows[g_folderRowCount++];
            memcpy(row.path, path, strlen(path) + 1);
            row.scanned = false;
            row.exists = true;
            row.games = 0;
        }
    }
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
            if (ShowConfirmUI("Keys too long", "Those are too long to be archive.org keys.",
                              "Copy them again from archive.org/account/s3.php.", "Try again"))
                continue;
            return KEY_ENTRY_FAILED;
        }

        RenderStatusFrame("Checking keys", "Asking archive.org whether these keys work", NULL);

        char reason[160] = "";
        KeyCheckResult check = CheckArchiveOrgKeys(header, reason, sizeof(reason), dprintf);

        if (check == KEYS_REJECTED)
        {
            // archive.org's own wording where it gave one - it can say which
            // of the two keys it objects to.
            if (ShowConfirmUI("Keys not accepted", "archive.org didn't accept those keys.",
                              reason[0] != '\0' ? reason : "Check them at archive.org/account/s3.php.",
                              "Try again"))
                continue;
            return KEY_ENTRY_FAILED;
        }

        if (!SaveKeys(accessKey, secretKey))
        {
            ShowMessageUI("Keys not saved", "ArchiveOrgKeys.txt could not be written.",
                          "Your previous keys, if any, are unchanged.");
            return KEY_ENTRY_FAILED;
        }

        if (check == KEYS_ACCEPTED)
            ShowMessageUI("Keys saved", "archive.org accepted your keys.",
                          "They're saved on this console for your next download.");
        else
            ShowMessageUI("Keys saved", "Saved, but not checked - archive.org didn't answer.",
                          "If a download fails, check them again in Settings.");

        return KEY_ENTRY_SAVED;
    }
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

// The sidebar's storage: how full the drive content installs to is, its name
// ("Hdd1"), "402 GB free" and "of 931 GB" - three short lines, since one
// long one doesn't fit beside the ring. used stays negative if the drive
// can't say, which hides it rather than showing a wrong number.
struct StorageStatus
{
    float used;
    char label[16];
    char detail[32];
    char total[32];
};

static void ReadStorageStatus(const char *path, StorageStatus *out)
{
    out->used = -1.0f;
    out->label[0] = '\0';
    out->detail[0] = '\0';
    out->total[0] = '\0';

    const char *colon = strchr(path, ':');
    if (colon == NULL)
        return;

    size_t nameLen = (size_t)(colon - path);
    if (nameLen == 0 || nameLen >= sizeof(out->label) || nameLen + 3 > 32)
        return;

    char root[32];
    memcpy(root, path, nameLen + 1); // through the colon
    root[nameLen + 1] = '\\';
    root[nameLen + 2] = '\0';

    ULARGE_INTEGER freeToCaller, total;
    if (!GetDiskFreeSpaceExA(root, &freeToCaller, &total, NULL) || total.QuadPart == 0)
        return;

    memcpy(out->label, path, nameLen);
    out->label[nameLen] = '\0';

    char freeText[64] = "", totalText[64] = "";
    FormatBytes(freeToCaller.QuadPart, freeText, sizeof(freeText));
    FormatBytes(total.QuadPart, totalText, sizeof(totalText));
    _snprintf(out->detail, sizeof(out->detail), "%s free", freeText);
    out->detail[sizeof(out->detail) - 1] = '\0';
    _snprintf(out->total, sizeof(out->total), "of %s", totalText);
    out->total[sizeof(out->total) - 1] = '\0';

    out->used = (float)(1.0 - (double)freeToCaller.QuadPart / (double)total.QuadPart);
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

    ShowMessageUI("Not enough space", message, "Free up some space on the drive and try again.");
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
        ShowMessageUI("archive.org keys needed",
                      "Get your access and secret key at archive.org/account/s3.php",
                      "Press A to type them in, or put them in ArchiveOrgKeys.txt beforehand.");

        KeyEntryOutcome entry = EnterCheckAndSaveKeys(accessKey, secretKey);

        if (entry == KEY_ENTRY_CANCELLED)
        {
            dprintf("ERROR: no keys entered\n");
            ShowMessageUI("No keys entered", "Both keys are required to download from archive.org.",
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
        ShowMessageUI("Saved keys unusable", "The saved keys are too long to be archive.org keys.",
                      "Re-enter them in Settings - press Y on the game list.");
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// The DLC pack and title update pickers
// ---------------------------------------------------------------------------
//
// A game page's Find DLC and Title updates buttons - over the Store or the
// library - and the installed disc's tile search for a game's DLC or title
// updates; either way a picker opens over the page straight away, fills in
// when the search comes back, and B steps back to the page at any point.
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
// A in the picker adds the row to the download queue and leaves the picker
// open, so a game with several separate packs - Call of Duty 2 has three -
// can have them all queued in one visit.

enum PickerKind
{
    PICKER_NONE,
    PICKER_DLC,
    PICKER_TITLE_UPDATE
};

// The picker opens the moment A or X is pressed, while the search runs on the
// search worker, and fills in when the result arrives.
enum PickerStatus
{
    PICKER_SEARCHING,
    PICKER_READY,       // results to choose from
    PICKER_NOTHING,     // the search worked, and matched nothing
    PICKER_UNREACHABLE  // archive.org couldn't be reached - A tries again
};

#define MAX_PICKER_ROWS (MAX_DLC_RAR_MATCHES > MAX_TITLE_UPDATE_MATCHES ? MAX_DLC_RAR_MATCHES : MAX_TITLE_UPDATE_MATCHES)

struct Picker
{
    PickerKind kind;
    PickerStatus status;
    int requestId; // the search it's waiting on, while PICKER_SEARCHING
    ShellPage page; // the page it's open over: the library, or a game in the Store

    // The game it's for - copied, since it may be the disc in the drive,
    // which isn't in the library.
    char gameName[256];
    unsigned long titleId;
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

// Where SearchResults land - static, as they're 8KB.
static SearchResult g_searchResult;

// Opens the picker for a game and starts its search.
static void OpenPicker(Picker &picker, PickerKind kind, const char *gameName, unsigned long titleId,
                       ShellPage page = SHELL_PAGE_LIBRARY)
{
    picker.kind = kind;
    picker.page = page;
    strncpy(picker.gameName, gameName, sizeof(picker.gameName) - 1);
    picker.gameName[sizeof(picker.gameName) - 1] = '\0';
    picker.titleId = titleId;
    picker.count = 0;
    picker.selected = 0;
    picker.scroll = -1;
    picker.requestId = BeginSearch(kind == PICKER_DLC ? SEARCH_DLC : SEARCH_TITLE_UPDATES, picker.gameName);

    // 0 means the search worker never started; the log says why.
    picker.status = (picker.requestId != 0) ? PICKER_SEARCHING : PICKER_UNREACHABLE;
}

// The rows for a DLC search. The filename is what actually identifies a pack,
// and the size plus match confidence are what let someone judge between two
// similar-looking entries.
static void FillDlcRows(Picker &picker)
{
    for (int i = 0; i < picker.count; ++i)
    {
        picker.labels[i] = picker.packs[i].filename;

        char sizeText[64] = "";
        FormatBytes(picker.packs[i].size, sizeText, sizeof(sizeText));

        _snprintf(picker.subText[i], sizeof(picker.subText[i]), "%s   %d%% name match",
                  sizeText, picker.packs[i].score);
        picker.subText[i][sizeof(picker.subText[i]) - 1] = '\0';
        picker.sublabels[i] = picker.subText[i];
    }
}

static void FillTitleUpdateRows(Picker &picker)
{
    for (int i = 0; i < picker.count; ++i)
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
}

// Collects the picker's search result, if it has arrived. Called every frame
// while the picker is searching.
static void PollPickerSearch(Picker &picker)
{
    if (picker.kind == PICKER_NONE || picker.status != PICKER_SEARCHING)
        return;

    if (!TakeSearchResult(picker.requestId, &g_searchResult))
        return;

    const SearchResult &result = g_searchResult;

    if (result.count < 0)
    {
        picker.status = PICKER_UNREACHABLE;
        return;
    }

    if (result.count == 0)
    {
        picker.status = PICKER_NOTHING;
        return;
    }

    picker.count = result.count;

    if (picker.kind == PICKER_DLC)
    {
        memcpy(picker.packs, result.packs, sizeof(picker.packs));
        FillDlcRows(picker);
    }
    else
    {
        // Sorted score-first then version-descending, so the first row is the
        // newest update for the best-matching name - the right default to
        // land on, but still shown rather than assumed.
        memcpy(picker.updates, result.updates, sizeof(picker.updates));
        FillTitleUpdateRows(picker);
    }

    picker.status = PICKER_READY;
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

// The disc in the drive's title ID, while there's a game disc in it - its
// box art is asked for ahead of the library's, as it's the first tile.
static unsigned long g_coverDiscTitleId = 0;

static void RequestLibraryCovers(const Library &lib)
{
    static unsigned long titleIds[MAX_INSTALLED_GAMES + 1];
    int n = 0;
    if (g_coverDiscTitleId != 0)
        titleIds[n++] = g_coverDiscTitleId;
    for (int i = 0; i < lib.count; ++i)
        titleIds[n++] = lib.games[i].titleId;
    RequestCoverArt(titleIds, n);
}

static void ScanLibrary(Library &lib, const char *gamesPath)
{
    RenderStatusFrame("Scanning", "Reading your installed games", gamesPath);

    // The cover cache was built from the old scan's images - drop both.
    ReleaseGameListIcons();
    FreeInstalledGames(lib.games, lib.count);
    lib.count = 0;

    int found = EnumerateInstalledGames(gamesPath, lib.games, MAX_INSTALLED_GAMES, dprintf);
    lib.count = (found > 0) ? found : 0;

    // Then each more folder from settings.txt. A game in two of them -
    // copied to a USB drive, say - is listed once, from the first.
    ReadMoreGamesPaths();
    for (int p = 0; p < g_moreGamesPathCount && lib.count < MAX_INSTALLED_GAMES; ++p)
    {
        const int before = lib.count;
        found = EnumerateInstalledGames(g_moreGamesPaths[p], lib.games + lib.count, MAX_INSTALLED_GAMES - lib.count,
                                        dprintf);
        g_moreGamesFound[p] = 0;
        if (found <= 0)
        {
            dprintf("No installed games found under %s\n", g_moreGamesPaths[p]);
            continue;
        }
        int kept = before;
        for (int i = before; i < before + found; ++i)
        {
            bool seen = false;
            for (int j = 0; j < before && !seen; ++j)
                seen = (lib.games[j].titleId == lib.games[i].titleId);
            if (seen)
            {
                FreeInstalledGames(&lib.games[i], 1);
                continue;
            }
            if (kept != i)
                lib.games[kept] = lib.games[i];
            kept++;
        }
        lib.count = kept;
        g_moreGamesFound[p] = kept - before;
        dprintf("Found %d more installed games under %s\n", kept - before, g_moreGamesPaths[p]);
    }

    // Sorted here rather than inside EnumerateInstalledGames - that
    // function's job is to walk the filesystem, and leaving presentation order
    // to the caller keeps it that way. Everything downstream (the installed
    // flags, the library selection) indexes into this array after the sort, so nothing
    // else has to know it happened.
    if (lib.count > 1)
        qsort(lib.games, lib.count, sizeof(InstalledGame), CompareGamesByName);

    if (lib.count > 0)
        dprintf("Found %d installed games under %s\n", lib.count, gamesPath);
    else
        dprintf("No installed games found under %s\n", gamesPath);

    // Box art, in the library's order so the first screenful comes first.
    RequestLibraryCovers(lib);
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
    SETTINGS_ROW_REMOVE_KEYS,
    SETTINGS_ROW_UPDATES,      // the version, and what's newer
    SETTINGS_ROW_UPDATE_CHECKS, // checking at start, on or off
    SETTINGS_ROW_LIBRARY_FOLDER, // one of g_folderRows: A adds or removes it
    SETTINGS_ROW_ADD_FOLDER    // another library folder, typed
};

// Rescans with the library folders now in settings.txt.
static void RescanFolders(Library &lib, const char *gamesPath, SettingsOutcome &outcome)
{
    ScanLibrary(lib, gamesPath);
    outcome.libraryChanged = true;
}

// A on a library folder's row: scanned, it's removed - its games stay where
// they are - and not, it's added.
static void ToggleLibraryFolder(int index, Library &lib, const char *gamesPath, SettingsOutcome &outcome)
{
    if (index < 0 || index >= g_folderRowCount)
        return;
    const LibraryFolderRow row = g_folderRows[index]; // a copy - the rescan rebuilds the list

    static char more[MAX_MORE_GAMES_PATHS][MAX_TEXT_LENGTH];
    int n = 0;
    if (row.scanned)
    {
        char message[MAX_TEXT_LENGTH + 64];
        _snprintf(message, sizeof(message), "Stop looking for games in %s?", row.path);
        message[sizeof(message) - 1] = '\0';
        if (!ShowConfirmUI("Library folder", message, "Its games stay where they are.", "Remove"))
            return;
        for (int p = 0; p < g_moreGamesPathCount; ++p)
        {
            if (_stricmp(g_moreGamesPaths[p], row.path) != 0)
                memcpy(more[n++], g_moreGamesPaths[p], MAX_TEXT_LENGTH);
        }
    }
    else
    {
        if (g_moreGamesPathCount >= MAX_MORE_GAMES_PATHS)
        {
            ShowMessageUI("Too many folders", "Remove a library folder first.", "Up to 7 can be added.");
            return;
        }
        for (int p = 0; p < g_moreGamesPathCount; ++p)
            memcpy(more[n++], g_moreGamesPaths[p], MAX_TEXT_LENGTH);
        memcpy(more[n++], row.path, MAX_TEXT_LENGTH);
    }

    if (!WriteGamesPaths(gamesPath, more, n))
    {
        ShowMessageUI("Not saved", "settings.txt could not be written.", row.path);
        return;
    }
    dprintf("Library folder %s %s\n", row.path, row.scanned ? "removed" : "added");
    RescanFolders(lib, gamesPath, outcome);
}

// "Add another library folder": one typed, for a folder somewhere the list
// doesn't look.
static void AddLibraryFolder(Library &lib, const char *gamesPath, SettingsOutcome &outcome)
{
    if (g_moreGamesPathCount >= MAX_MORE_GAMES_PATHS)
    {
        ShowMessageUI("Too many folders", "Remove a library folder first.", "Up to 7 can be added.");
        return;
    }

    std::string typed;
    if (OpenKeyboardToString(XUSER_INDEX_ANY, &typed, L"Library Folder",
                             L"Another folder to look for games in, like Usb0:\\Games", L"Usb0:\\") != ERROR_SUCCESS)
        return; // cancelled

    char newPath[MAX_TEXT_LENGTH];
    if (!NormaliseGamesPath(typed, newPath, sizeof(newPath)))
    {
        ShowMessageUI("Not a folder path", "Include the drive, like Hdd1:\\Games or Usb0:\\Games.", typed.c_str());
        return;
    }
    if (!FolderExists(newPath))
    {
        ShowMessageUI("Folder not found", "There is no folder at that path.", newPath);
        return;
    }
    bool listed = (_stricmp(newPath, gamesPath) == 0);
    for (int p = 0; p < g_moreGamesPathCount && !listed; ++p)
        listed = (_stricmp(g_moreGamesPaths[p], newPath) == 0);
    if (listed)
    {
        ShowMessageUI("Already in your library", "That folder is looked in already.", newPath);
        return;
    }

    static char more[MAX_MORE_GAMES_PATHS][MAX_TEXT_LENGTH];
    int n = 0;
    for (int p = 0; p < g_moreGamesPathCount; ++p)
        memcpy(more[n++], g_moreGamesPaths[p], MAX_TEXT_LENGTH);
    memcpy(more[n++], newPath, MAX_TEXT_LENGTH);
    if (!WriteGamesPaths(gamesPath, more, n))
    {
        ShowMessageUI("Not saved", "settings.txt could not be written.", newPath);
        return;
    }
    dprintf("Library folder %s added\n", newPath);
    RescanFolders(lib, gamesPath, outcome);
}

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
        ShowMessageUI("Not a folder path", "Include the drive, like Hdd1:\\Games or Usb0:\\Games.",
                      typed.c_str());
        return;
    }

    if (_stricmp(newPath, gamesPath) == 0)
        return; // unchanged - nothing to save or rescan

    // Checked before saving, so a typo is caught while the old, working path
    // is still in place rather than after it has been overwritten.
    if (!FolderExists(newPath))
    {
        ShowMessageUI("Folder not found", "There is no folder at that path.", newPath);
        return;
    }

    bool saved = SetSettingsValue(GAMES_PATH_KEY, newPath);
    dprintf("Games path changed to %s (%s)\n", newPath, saved ? "saved" : "NOT saved");

    // Used for this session either way - the user asked for it and the folder
    // exists. Only remembering it is in question if the write failed.
    memcpy(gamesPath, newPath, strlen(newPath) + 1);
    SetDiscWorkerGamesPath(gamesPath);
    SetGameInstallerGamesPath(gamesPath);

    ScanLibrary(lib, gamesPath);
    outcome.libraryChanged = true;

    if (!saved)
    {
        ShowMessageUI("Not saved", "Using this folder for now, but settings.txt could not be written.",
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

// The download's progress, drawn while InstallUpdate works.
static char g_updatingTo[32];

static void DrawUpdateProgress(unsigned long long done, unsigned long long total)
{
    char message[64], detail[96], doneText[32] = "", totalText[32] = "";
    _snprintf(message, sizeof(message), "Downloading Omni360 %s", g_updatingTo);
    message[sizeof(message) - 1] = '\0';
    FormatBytes(done, doneText, sizeof(doneText));
    if (total > 0)
    {
        FormatBytes(total, totalText, sizeof(totalText));
        _snprintf(detail, sizeof(detail), "%s of %s", doneText, totalText);
    }
    else
    {
        _snprintf(detail, sizeof(detail), "%s", doneText);
    }
    detail[sizeof(detail) - 1] = '\0';
    RenderStatusFrame("Updating", message, detail);
}

// Restarting ends whatever's running, so updating waits for the Queue.
static bool AnythingQueued()
{
    return PendingDownloadCount() > 0 || PendingGameJobCount() > 0 || PendingDiscJobCount() > 0;
}

// The Updates row: what's new in a newer version - and, for a signed one,
// installing it and restarting into it - or a check now.
static void ShowUpdate()
{
    UpdateInfo info;
    if (GetUpdateState(&info, NULL) != UPDATE_AVAILABLE)
    {
        StartUpdateCheck();
        ShowShellToast("Checking for updates", "Asking GitHub for the latest version", UI_TOAST_INFO);
        return;
    }

    char heading[64];
    _snprintf(heading, sizeof(heading), "Omni360 %s", info.version);
    heading[sizeof(heading) - 1] = '\0';

    const bool installable = UpdateInstallable(info);
    const bool busy = AnythingQueued();
    const char *foot = !installable ? "Get it from " UPDATE_RELEASES_PAGE
                     : busy        ? "To update, let what's in the Queue finish, or stop it, first."
                                   : "Update installs it and restarts Omni360. Your settings, keys and games stay.";
    if (!ShowNotesUI(heading, info.title, info.notes, foot, installable && !busy ? L"Update" : NULL))
        return;

    _snprintf(g_updatingTo, sizeof(g_updatingTo), "%s", info.version);
    g_updatingTo[sizeof(g_updatingTo) - 1] = '\0';
    DrawUpdateProgress(0, info.xexSize);

    char xexPath[MAX_PATH + 16] = "";
    const UpdateInstallResult result = InstallUpdate(info, DrawUpdateProgress, xexPath, sizeof(xexPath));
    if (result != UPDATE_INSTALL_OK)
    {
        ShowMessageUI("Couldn't update", UpdateInstallResultText(result), "Omni360 hasn't changed.");
        return;
    }

    char message[96];
    _snprintf(message, sizeof(message), "Omni360 %s is installed.", info.version);
    message[sizeof(message) - 1] = '\0';
    ShowMessageUI("Updated", message, "It starts when you continue. The previous version is kept as .old beside it.");

    dprintf("Restarting into %s\n", xexPath);
    XLaunchNewImage(xexPath, 0);
}

static void ToggleUpdateChecks()
{
    const bool on = !UpdateChecksOn();
    if (!SetSettingsValue(UPDATE_CHECK_KEY, on ? "yes" : "off"))
        ShowMessageUI("Not saved", "settings.txt could not be written.", NULL);
}

static void RemoveKeys(SettingsOutcome &outcome)
{
    if (!ShowConfirmUI("Remove keys", "Remove the archive.org keys saved on this console?",
                       "You'll need to add them again before you can download.", "Remove"))
        return;

    if (remove(CREDENTIALS_FILE) != 0)
    {
        ShowMessageUI("Could not remove", "ArchiveOrgKeys.txt could not be deleted.", NULL);
        return;
    }

    dprintf("Saved archive.org keys removed\n");
    outcome.keysChanged = true;
    ShowMessageUI("Keys removed", "Your archive.org keys have been removed from this console.", NULL);
}

// The settings page: a short list whose second lines show the current state,
// so it doubles as a summary of how the app is set up. Rebuilt whenever
// something may have changed it, rather than every frame - each rebuild reads
// the keys file.
#define MAX_SETTINGS_ROWS (6 + MAX_FOLDER_ROWS)

struct SettingsPage
{
    int count;
    SettingsRow rows[MAX_SETTINGS_ROWS];
    int args[MAX_SETTINGS_ROWS]; // a library folder row's index into g_folderRows
    const char *labels[MAX_SETTINGS_ROWS];
    const char *sublabels[MAX_SETTINGS_ROWS];
    const char *sections[MAX_SETTINGS_ROWS]; // a heading over the row that starts each part, else NULL
    char gamesSub[MAX_TEXT_LENGTH + 96];
    char folderSubs[MAX_FOLDER_ROWS][96];
    char keysSub[128];
    char updatesSub[160];

    int selected;
    int scroll;
};

static void BuildSettingsPage(SettingsPage &page, const Library &lib, const char *gamesPath)
{
    std::string accessKey, secretKey;
    const bool haveKeys = LoadSavedKeys(accessKey, secretKey);

    // Where games install, and how many the library has in all - each other
    // folder it looks in has a row of its own, below.
    if (lib.count > 0)
        _snprintf(page.gamesSub, sizeof(page.gamesSub), "%s   -   where games install   -   %d game%s in your library",
                  gamesPath, lib.count, lib.count == 1 ? "" : "s");
    else
        _snprintf(page.gamesSub, sizeof(page.gamesSub), "%s   -   where games install   -   no games found",
                  gamesPath);
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
    for (int i = 0; i < MAX_SETTINGS_ROWS; ++i)
        page.sections[i] = NULL;

    page.sections[page.count] = "Library location";
    page.labels[page.count] = "Games folder";
    page.sublabels[page.count] = page.gamesSub;
    page.rows[page.count++] = SETTINGS_ROW_GAMES_FOLDER;

    // The other folders the library looks in, and the likely ones it could.
    FindLibraryFolders(gamesPath);
    for (int i = 0; i < g_folderRowCount; ++i)
    {
        const LibraryFolderRow &row = g_folderRows[i];
        char *sub = page.folderSubs[i];
        const size_t subSize = sizeof(page.folderSubs[i]);
        if (!row.scanned)
            _snprintf(sub, subSize, "Not in your library   -   A to look for games here");
        else if (!row.exists)
            _snprintf(sub, subSize, "In your library, but the folder isn't there   -   A to remove");
        else
            _snprintf(sub, subSize, "In your library   -   %d game%s here   -   A to remove", row.games,
                      row.games == 1 ? "" : "s");
        sub[subSize - 1] = '\0';
        page.labels[page.count] = row.path;
        page.sublabels[page.count] = sub;
        page.args[page.count] = i;
        page.rows[page.count++] = SETTINGS_ROW_LIBRARY_FOLDER;
    }
    page.labels[page.count] = "Add another library folder";
    page.sublabels[page.count] = "Type the path of a folder to look for games in";
    page.rows[page.count++] = SETTINGS_ROW_ADD_FOLDER;

    page.sections[page.count] = "archive.org keys";
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

    // The version, and whether there's a newer one.
    UpdateInfo update;
    switch (GetUpdateState(&update, NULL))
    {
    case UPDATE_AVAILABLE:
        _snprintf(page.updatesSub, sizeof(page.updatesSub), "%s is available   -   you have %s. A for what's new",
                  update.version, CURRENT_VERSION);
        break;
    case UPDATE_CHECKING:
        _snprintf(page.updatesSub, sizeof(page.updatesSub), "Version %s   -   checking for a newer one...",
                  CURRENT_VERSION);
        break;
    case UPDATE_CURRENT:
        _snprintf(page.updatesSub, sizeof(page.updatesSub), "Version %s   -   up to date", CURRENT_VERSION);
        break;
    case UPDATE_FAILED:
        _snprintf(page.updatesSub, sizeof(page.updatesSub), "Version %s   -   couldn't reach GitHub. A to try again",
                  CURRENT_VERSION);
        break;
    default:
        _snprintf(page.updatesSub, sizeof(page.updatesSub), "Version %s   -   A to check for a newer one",
                  CURRENT_VERSION);
        break;
    }
    page.updatesSub[sizeof(page.updatesSub) - 1] = '\0';

    page.sections[page.count] = "Updates";
    page.labels[page.count] = "Check for updates";
    page.sublabels[page.count] = page.updatesSub;
    page.rows[page.count++] = SETTINGS_ROW_UPDATES;

    const bool checksOn = UpdateChecksOn();
    page.labels[page.count] = checksOn ? "Check for updates at start: On" : "Check for updates at start: Off";
    page.sublabels[page.count] = checksOn ? "Asks GitHub for the latest version each time Omni360 starts"
                                          : "Only when you choose Check for updates";
    page.rows[page.count++] = SETTINGS_ROW_UPDATE_CHECKS;

    if (page.selected > page.count - 1)
        page.selected = page.count - 1; // the Remove row just went away
}

static SettingsOutcome RunSettingsRow(SettingsRow row, int arg, Library &lib, char *gamesPath, size_t gamesPathSize)
{
    SettingsOutcome outcome = {false, false};

    switch (row)
    {
    case SETTINGS_ROW_GAMES_FOLDER: ChangeGamesFolder(lib, gamesPath, gamesPathSize, outcome); break;
    case SETTINGS_ROW_KEYS:         ChangeKeys(outcome); break;
    case SETTINGS_ROW_REMOVE_KEYS:  RemoveKeys(outcome); break;
    case SETTINGS_ROW_UPDATES:      ShowUpdate(); break;
    case SETTINGS_ROW_UPDATE_CHECKS: ToggleUpdateChecks(); break;
    case SETTINGS_ROW_LIBRARY_FOLDER: ToggleLibraryFolder(arg, lib, gamesPath, outcome); break;
    case SETTINGS_ROW_ADD_FOLDER:   AddLibraryFolder(lib, gamesPath, outcome); break;
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
    RenderStatusFrame("Cleaning up", "Removing a game install that didn't finish", name);
    GodRemovePackage(root, titleId, mediaId);
    ClearInstallMarker();
}

// The name the dashboard will show for a disc about to be installed. The
// bundled list only suggests one - its names are community-edited - so when
// there's no listed name the keyboard asks, with "Title XXXXXXXX" filled in.
// False if the keyboard was cancelled.
static bool AskDiscName(const char *suggested, unsigned int discNumber, unsigned int discCount, std::string &name)
{
    WCHAR wideSuggested[128];
    Utf8ToWideText(suggested, wideSuggested, 128);

    WCHAR description[160];
    if (discCount > 1)
        swprintf_s(description, 160, L"The name the dashboard and Aurora show. This is disc %u of %u.",
                   discNumber, discCount);
    else
        swprintf_s(description, 160, L"The name the dashboard and Aurora show.");

    std::string typed;
    if (OpenKeyboardToString(XUSER_INDEX_ANY, &typed, L"Game Name", description, wideSuggested) != ERROR_SUCCESS)
        return false;
    TrimInPlace(typed);

    // Unchanged, the original is kept: the keyboard hands text back one byte
    // per character, which would turn a name like "Modern Warfare® 3" or a
    // Japanese title into question marks.
    if (typed.empty() || typed == Utf8AsKeyboardText(suggested))
        name = suggested;
    else
        name = Latin1ToUtf8(typed);
    return true;
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

// Downloads and disc installs, on one Queue page.
#define MAX_QUEUE_ROWS (MAX_QUEUE_JOBS + MAX_DISC_JOBS + MAX_GAME_JOBS)

struct Shell
{
    ShellPage page;
    bool sidebarFocused;

    int librarySelected;
    int libraryScroll; // -1 until placed - see LibraryPageView

    SettingsPage settings;
    Picker picker; // open over the library while picker.kind != PICKER_NONE

    // The queue as of this frame, copied out of DownloadQueue at the top of
    // each pass, and the row text built from it. The selection is held as a
    // job id as well as a row, because the rows reorder as jobs start and
    // finish - and pressing X should act on the job you were looking at.
    QueueJobSnapshot queueJobs[MAX_QUEUE_ROWS];
    int queueCount;
    int queueSelected;
    int queueSelectedId;
    int queueScroll;
    QueueRowView queueRows[MAX_QUEUE_ROWS];
    char queueStatus[MAX_QUEUE_ROWS][256];
    char queueNumbers[MAX_QUEUE_ROWS][128];

    // The disc in the drive, as the library's first tile - see UpdateDisc.
    // librarySelected counts it: with a disc, 0 is the disc and game i is
    // item i + 1.
    DiscInfo disc;
    bool hasDisc;
    bool discInstalled;
    char discName[128];

    // A disc install finished: read the library again at the top of the
    // next pass, so the game is in it.
    bool rescanLibrary;

    // What takes a file read or a drive query to find out, so it's refreshed
    // only when something may have changed it - on the way back from any
    // action, or when a download finishes - rather than on every frame.
    bool stale;
    bool keysSaved;
    StorageStatus storage;

    // The Store's front page: which tile has focus (STORE_FOCUS_*), how far
    // it has scrolled, and whether its wallpapers have been asked for yet.
    int storeFocus;
    float storeScroll;
    bool storeArtRequested;

    // A section's A-Z page - Xbox Live Arcade's or the Indie Games' - open
    // over the front page, and which letter has focus on each. A letter's
    // grid and a game's page open over it in turn.
    bool storeInSection;
    ArcadeSet storeSection;
    int sectionFocus[2];   // by ArcadeSet

    // A letter's games, while one is open over the front page - or over a
    // section's page, storeLetterArcade, its games then in g_arcadeGames.
    bool storeInLetter;
    bool storeLetterArcade;
    char storeLetter;
    int storeGameCount; // in g_storeGames
    int storeSelected;
    int storeLetterScroll;

    // A game's page, open over the letter or the front page - or over the
    // library, with libraryInGame. The page is the same either way.
    bool storeInGame;
    bool libraryInGame;
    int storeGameFocus;     // STORE_GAME_FOCUS_*
    int storeVersionChosen;
    int storeVersionScroll;
    bool storeShotsAsked;   // its screenshots, once its details came
};

// One letter's games and their tiles - as many as the biggest letter has.
#define MAX_STORE_GAMES (STORE_MAX_LETTER_GAMES > ARCADE_MAX_LETTER_GAMES ? STORE_MAX_LETTER_GAMES \
                                                                       : ARCADE_MAX_LETTER_GAMES)
static StoreGame g_storeGames[MAX_STORE_GAMES];
static const XblaGame *g_arcadeGames[MAX_STORE_GAMES];
static StoreTileView g_storeTiles[MAX_STORE_GAMES];

// How many games each letter of a section has, counted when its page first
// opens - a letter with none is drawn faded. By ArcadeSet.
static int g_sectionLetterCounts[2][STORE_LETTER_COUNT];
static bool g_sectionCounted[2] = { false, false };

// The game whose page is open, and its version rows - the labels are the
// table's, the sizes formatted here.
static StoreGame g_storeGame;
static StoreVersionView g_storeVersions[STORE_MAX_VERSIONS];
static char g_storeVersionSize[STORE_MAX_VERSIONS][24];
static StoreDetails g_storeDetails;
static char g_storeMeta[256];

// The arcade or indie game whose page is open, or NULL for a disc's. Its one
// version row is the RAR - the game, and anything packed with it.
static const XblaGame *g_storeArcade = NULL;

// The content folder, where arcade and indie games install - main's.
static const char *g_contentBase = "";

// Static rather than on main's stack: the picker's match arrays and the
// queue's snapshot are tens of KB between them.
static Shell g_shell;

// An empty Queue has nothing to choose - on it, the sidebar keeps focus.
static bool PageTakesFocus(const Shell &shell, ShellPage page)
{
    if (page == SHELL_PAGE_QUEUE)
        return shell.queueCount > 0;
    return page == SHELL_PAGE_LIBRARY || page == SHELL_PAGE_STORE || page == SHELL_PAGE_SETTINGS;
}

// ---------------------------------------------------------------------------
// The Store's front page
// ---------------------------------------------------------------------------

#define STORE_MIDDOT "  \xC2\xB7  "

// Chosen by hand; the details are the Xbox Live catalog's.
static const StoreFeaturedView kStoreFeatured[STORE_FEATURED_COUNT] = {
    { 0x4D5307E6, "Halo 3", "Bungie Studios" STORE_MIDDOT "Shooter" },
    { 0x545407D8, "BioShock", "2K Boston" STORE_MIDDOT "Shooter" },
    { 0x4D530AA4, "Forza Horizon 2", "Sumo Digital" STORE_MIDDOT "Racing" },
};

// The other ways in - those still to come marked SOON. DLC and title updates
// are on each game's page instead.
static const StoreButtonView kStoreButtons[STORE_BUTTON_COUNT] = {
    { "Search", true },
    { "XBLA", false },
    { "XBLIG", false },
    { "Original Xbox", true },
};

// What each will be, for the toast when one's pressed before then.
#define STORE_BUTTON_XBLA  1
#define STORE_BUTTON_XBLIG 2
static const char *const kStoreButtonsSoon[STORE_BUTTON_COUNT] = {
    "Search every game by name",
    "Xbox Live Arcade games, A to Z",
    "Xbox Live Indie Games, A to Z",
    "Original Xbox games, A to Z",
};

static const char kStoreLetters[] = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ";

static StorePageView MakeStoreView(const Shell &shell)
{
    StorePageView view;
    memset(&view, 0, sizeof(view));
    for (int i = 0; i < STORE_FEATURED_COUNT; ++i)
        view.featured[i] = kStoreFeatured[i];
    for (int i = 0; i < STORE_BUTTON_COUNT; ++i)
        view.buttons[i] = kStoreButtons[i];
    view.letters = kStoreLetters;
    view.focus = shell.storeFocus;
    view.scroll = shell.storeScroll;
    view.focused = !shell.sidebarFocused;
    return view;
}

// The D-pad on the front page. The featured tiles are one large one with
// two stacked beside it; the buttons are one row under them; the letters
// wrap, perRow to a row. Left from the first column goes to the sidebar.
static void StepStoreFocus(WORD nav, Shell &shell)
{
    const int perRow = StoreLettersPerRow();
    int i = shell.storeFocus;

    if (i < STORE_FOCUS_BUTTONS)
    {
        const int f = i - STORE_FOCUS_FEATURED;
        if (nav == XINPUT_GAMEPAD_DPAD_RIGHT && f == 0)
            i = STORE_FOCUS_FEATURED + 1;
        else if (nav == XINPUT_GAMEPAD_DPAD_LEFT)
        {
            if (f == 0)
                shell.sidebarFocused = true;
            else
                i = STORE_FOCUS_FEATURED;
        }
        else if (nav == XINPUT_GAMEPAD_DPAD_DOWN)
        {
            if (f == 1)
                i = STORE_FOCUS_FEATURED + 2;
            else
                i = STORE_FOCUS_BUTTONS + (f == 0 ? 0 : STORE_BUTTON_COUNT - 1);
        }
        else if (nav == XINPUT_GAMEPAD_DPAD_UP && f == 2)
            i = STORE_FOCUS_FEATURED + 1;
    }
    else if (i < STORE_FOCUS_LETTERS)
    {
        const int b = i - STORE_FOCUS_BUTTONS;
        if (nav == XINPUT_GAMEPAD_DPAD_RIGHT && b + 1 < STORE_BUTTON_COUNT)
            i++;
        else if (nav == XINPUT_GAMEPAD_DPAD_LEFT)
        {
            if (b == 0)
                shell.sidebarFocused = true;
            else
                i--;
        }
        else if (nav == XINPUT_GAMEPAD_DPAD_UP)
            i = STORE_FOCUS_FEATURED + (b + 1 < STORE_BUTTON_COUNT ? 0 : 2); // the last is under the small tiles
        else if (nav == XINPUT_GAMEPAD_DPAD_DOWN)
            i = STORE_FOCUS_LETTERS + (b * perRow * 2 + STORE_BUTTON_COUNT) / (STORE_BUTTON_COUNT * 2);
    }
    else
    {
        const int l = i - STORE_FOCUS_LETTERS;
        const int col = l % perRow;
        if (nav == XINPUT_GAMEPAD_DPAD_RIGHT && col + 1 < perRow && l + 1 < STORE_LETTER_COUNT)
            i++;
        else if (nav == XINPUT_GAMEPAD_DPAD_LEFT)
        {
            if (col == 0)
                shell.sidebarFocused = true;
            else
                i--;
        }
        else if (nav == XINPUT_GAMEPAD_DPAD_UP)
        {
            if (l >= perRow)
                i -= perRow;
            else
                i = STORE_FOCUS_BUTTONS + col * STORE_BUTTON_COUNT / perRow;
        }
        else if (nav == XINPUT_GAMEPAD_DPAD_DOWN && l / perRow < (STORE_LETTER_COUNT - 1) / perRow)
        {
            // Down onto a shorter last row lands on its last letter.
            i = (l + perRow < STORE_LETTER_COUNT) ? i + perRow : STORE_FOCUS_LETTERS + STORE_LETTER_COUNT - 1;
        }
    }

    shell.storeFocus = i;
}

// A section's name, for its page and its letters' headers.
static const char *SectionName(ArcadeSet set)
{
    return set == ARCADE_XBLIG ? "Xbox Live Indie Games" : "Xbox Live Arcade";
}

// A letter's grid - the discs', or with arcade, the open section's.
static void OpenStoreLetter(Shell &shell, char letter, bool arcade)
{
    if (arcade)
    {
        // Indie games have no covers to ask for: they're drawn with the
        // indie banner and their names instead (see IndieTile).
        const bool indie = (shell.storeSection == ARCADE_XBLIG);
        shell.storeGameCount = ArcadeGamesForLetter(shell.storeSection, letter, g_arcadeGames, MAX_STORE_GAMES);
        for (int i = 0; i < shell.storeGameCount; ++i)
        {
            g_storeTiles[i].titleId = indie ? 0 : g_arcadeGames[i]->titleId;
            g_storeTiles[i].name = g_arcadeGames[i]->name;
            g_storeTiles[i].regions = 0; // the RARs don't say
            g_storeTiles[i].indie = indie;
        }
    }
    else
    {
        shell.storeGameCount = StoreGamesForLetter(letter, g_storeGames, MAX_STORE_GAMES);
        for (int i = 0; i < shell.storeGameCount; ++i)
        {
            g_storeTiles[i].titleId = g_storeGames[i].titleId;
            g_storeTiles[i].name = g_storeGames[i].name;
            g_storeTiles[i].regions = g_storeGames[i].regions;
            g_storeTiles[i].indie = false;
        }
    }
    shell.storeLetterArcade = arcade;
    shell.storeLetter = letter;
    shell.storeSelected = 0;
    shell.storeLetterScroll = 0;
    shell.storeInLetter = true;
}

static StoreLetterView MakeStoreLetterView(const Shell &shell)
{
    StoreLetterView view;
    view.letter = shell.storeLetter;
    view.section = shell.storeLetterArcade ? SectionName(shell.storeSection) : NULL;
    view.tiles = g_storeTiles;
    view.count = shell.storeGameCount;
    view.selected = shell.storeSelected;
    view.scroll = shell.storeLetterScroll;
    view.focused = !shell.sidebarFocused;
    return view;
}

// The D-pad and shoulders on a letter's grid, as on the library's.
static void StepStoreLetter(const UiInput &input, Shell &shell)
{
    const int cols = LibraryGridColumns();
    const int count = shell.storeGameCount;
    int &sel = shell.storeSelected;

    if (input.nav == XINPUT_GAMEPAD_DPAD_LEFT)
    {
        if (sel % cols == 0)
            shell.sidebarFocused = true;
        else
            sel--;
    }
    else if (input.nav == XINPUT_GAMEPAD_DPAD_RIGHT)
    {
        if (sel % cols < cols - 1 && sel + 1 < count)
            sel++;
    }
    else if (input.nav == XINPUT_GAMEPAD_DPAD_UP)
    {
        if (sel >= cols)
            sel -= cols;
    }
    else if (input.nav == XINPUT_GAMEPAD_DPAD_DOWN)
    {
        if (sel + cols < count)
            sel += cols;
        else if (sel / cols < (count - 1) / cols)
            sel = count - 1;
    }

    if (input.pressed & (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER))
    {
        const int jump = StoreLetterVisibleRows() * cols;
        if (input.pressed & XINPUT_GAMEPAD_LEFT_SHOULDER)
            sel -= jump;
        if (input.pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER)
            sel += jump;
        if (sel > count - 1) sel = count - 1;
        if (sel < 0) sel = 0;
    }
}

// A letter's first screenful of covers, asked for as its tile takes focus, so
// they're arriving before it's opened. The letter's page asks again for what
// it shows once it is. Indie games have none.
static void PrefetchLetterCovers(char letter, bool arcade, ArcadeSet set)
{
    unsigned long ids[24];
    int idCount = 0;
    if (arcade)
    {
        if (set == ARCADE_XBLIG)
            return;
        const int n = ArcadeGamesForLetter(set, letter, g_arcadeGames, 24);
        for (int i = 0; i < n; ++i)
            ids[idCount++] = g_arcadeGames[i]->titleId;
    }
    else
    {
        const int n = StoreGamesForLetter(letter, g_storeGames, MAX_STORE_GAMES);
        for (int i = 0; i < n && idCount < 24; ++i)
        {
            if (g_storeGames[i].titleId != 0)
                ids[idCount++] = g_storeGames[i].titleId;
        }
    }
    RequestStoreCoverArt(ids, idCount);
}

// A section's A-Z page, over the front page.
static void OpenSection(Shell &shell, ArcadeSet set)
{
    if (!g_sectionCounted[set])
    {
        static const XblaGame *scratch[MAX_STORE_GAMES];
        for (int i = 0; i < STORE_LETTER_COUNT; ++i)
            g_sectionLetterCounts[set][i] = ArcadeGamesForLetter(set, kStoreLetters[i], scratch, MAX_STORE_GAMES);
        g_sectionCounted[set] = true;
    }
    shell.storeSection = set;
    shell.storeInSection = true;
    PrefetchLetterCovers(kStoreLetters[shell.sectionFocus[set]], true, set);
}

static StoreSectionView MakeSectionView(const Shell &shell)
{
    const ArcadeSet set = shell.storeSection;
    static char subtitle[64];
    _snprintf(subtitle, sizeof(subtitle), "%d games from archive.org",
              set == ARCADE_XBLIG ? XBLIG_GAME_COUNT : XBLA_GAME_COUNT);
    subtitle[sizeof(subtitle) - 1] = '\0';

    StoreSectionView view;
    view.title = SectionName(set);
    view.subtitle = subtitle;
    view.letters = kStoreLetters;
    view.counts = g_sectionLetterCounts[set];
    view.focus = shell.sectionFocus[set];
    view.focused = !shell.sidebarFocused;
    return view;
}

// The D-pad on a section's letters, as on the front page's.
static void StepSectionFocus(WORD nav, Shell &shell)
{
    const int perRow = StoreLettersPerRow();
    int &l = shell.sectionFocus[shell.storeSection];
    const int col = l % perRow;
    if (nav == XINPUT_GAMEPAD_DPAD_RIGHT && col + 1 < perRow && l + 1 < STORE_LETTER_COUNT)
        l++;
    else if (nav == XINPUT_GAMEPAD_DPAD_LEFT)
    {
        if (col == 0)
            shell.sidebarFocused = true;
        else
            l--;
    }
    else if (nav == XINPUT_GAMEPAD_DPAD_UP && l >= perRow)
        l -= perRow;
    else if (nav == XINPUT_GAMEPAD_DPAD_DOWN && l / perRow < (STORE_LETTER_COUNT - 1) / perRow)
        l = (l + perRow < STORE_LETTER_COUNT) ? l + perRow : STORE_LETTER_COUNT - 1; // a shorter last row
}

// The wallpapers for the featured tiles - again on the way back to the front
// page, as a game page may have pushed them out.
static void RequestFeaturedArt()
{
    for (int i = 0; i < STORE_FEATURED_COUNT; ++i)
        RequestStoreArt(kStoreFeatured[i].titleId, STORE_ART_BACKGROUND);
}

static void AppendText(char *out, size_t outSize, const char *separator, const char *text)
{
    size_t used = strlen(out);
    _snprintf(out + used, outSize - used, "%s%s", used > 0 ? separator : "", text);
    out[outSize - 1] = '\0';
}

// Whether the page showing is a game's page, from the Store or the library.
static bool GamePageOpen(const Shell &shell)
{
    return (shell.page == SHELL_PAGE_STORE && shell.storeInGame) ||
           (shell.page == SHELL_PAGE_LIBRARY && shell.libraryInGame);
}

// Opens a game's page over the current page - the Store's, or the library's.
static void OpenStoreGame(Shell &shell, const StoreGame &game)
{
    g_storeGame = game;
    g_storeArcade = NULL; // OpenArcadeGame sets it after
    for (int v = 0; v < g_storeGame.versionCount; ++v)
    {
        // The labels are the generator's; only the size is formatted here.
        const StoreRelease *release = StoreReleaseOf(&g_storeGame, v);
        g_storeVersionSize[v][0] = '\0';
        if (release != NULL)
            FormatBytes(release->size, g_storeVersionSize[v], sizeof(g_storeVersionSize[v]));
        g_storeVersions[v].label = release != NULL ? release->label : "";
        g_storeVersions[v].detail = release != NULL ? release->detail : "";
        g_storeVersions[v].size = g_storeVersionSize[v];
    }

    if (shell.page == SHELL_PAGE_LIBRARY)
        shell.libraryInGame = true;
    else
        shell.storeInGame = true;
    shell.storeGameFocus = STORE_GAME_FOCUS_BUTTONS;
    shell.storeVersionChosen = 0;
    shell.storeVersionScroll = 0;
    shell.storeShotsAsked = false;

    // The details first - the screenshots wait on them - then the art.
    const unsigned long titleId = g_storeGame.titleId;
    if (titleId != 0)
    {
        RequestStoreDetails(titleId);
        RequestStoreArt(titleId, STORE_ART_BACKGROUND);
        RequestStoreCoverArt(&titleId, 1);
    }
}

// Whether the page open is an indie game's.
static bool StoreGameIsIndie()
{
    return g_storeArcade != NULL && ArcadeSetOf(g_storeArcade) == ARCADE_XBLIG;
}

// An arcade or indie game's page: a disc game's, with one version - its RAR.
// An indie game's has no title ID of its own to find art or details by -
// every indie game shares one - so it's opened without.
static void OpenArcadeGame(Shell &shell, const XblaGame &game)
{
    const bool indie = (ArcadeSetOf(&game) == ARCADE_XBLIG);
    StoreGame page;
    memset(&page, 0, sizeof(page));
    page.name = game.name;
    page.titleId = indie ? 0 : game.titleId;
    OpenStoreGame(shell, page);

    static char detail[48];
    detail[0] = '\0';
    if (game.packages > 1)
    {
        if (indie)
            _snprintf(detail, sizeof(detail), "%u packages", (unsigned)game.packages);
        else
            _snprintf(detail, sizeof(detail), "With %u add-on%s", (unsigned)(game.packages - 1),
                      game.packages == 2 ? "" : "s");
        detail[sizeof(detail) - 1] = '\0';
    }
    FormatBytes(game.size, g_storeVersionSize[0], sizeof(g_storeVersionSize[0]));
    g_storeVersions[0].label = indie ? "Indie game" : "Xbox Live Arcade";
    g_storeVersions[0].detail = detail;
    g_storeVersions[0].size = g_storeVersionSize[0];
    g_storeArcade = &game;
}

// The version rows the page shows: an arcade or indie game's one, or a disc
// game's.
static int StoreVersionCount()
{
    return g_storeArcade != NULL ? 1 : g_storeGame.versionCount;
}

// Whether a game from either list is on the drive: its biggest package, in
// its title's folder under the content folder. Checked at most once a second
// - the page asks every frame.
static bool ArcadePackageOnDrive(const XblaGame &game)
{
    static const XblaGame *checked = NULL;
    static DWORD checkedAt = 0;
    static bool onDrive = false;
    if (checked == &game && GetTickCount() - checkedAt < 1000)
        return onDrive;

    char file[64], path[512];
    const char *space = strchr(game.files, ' ');
    const size_t n = (space != NULL) ? (size_t)(space - game.files) : strlen(game.files);
    onDrive = false;
    if (n > 0 && n < sizeof(file))
    {
        memcpy(file, game.files, n);
        file[n] = '\0';
        _snprintf(path, sizeof(path), "%s\\%08lX\\%08lX\\%s", g_contentBase, game.titleId, game.contentType, file);
        path[sizeof(path) - 1] = '\0';
        onDrive = (GetFileAttributesA(path) != (DWORD)-1); // INVALID_FILE_ATTRIBUTES, which the XDK lacks
    }
    checked = &game;
    checkedAt = GetTickCount();
    return onDrive;
}

// The game's install: on its way in the download queue, where it's a pack
// like DLC, or installed - its package on the drive, or for an arcade game
// one the library has, installed some other way.
static StoreInstallState ArcadeInstallState(const Library &lib, float *outFraction)
{
    static QueueJobSnapshot jobs[MAX_QUEUE_JOBS];
    const int jobCount = SnapshotDownloadQueue(jobs, MAX_QUEUE_JOBS);
    for (int i = 0; i < jobCount; ++i)
    {
        const QueueJobSnapshot &job = jobs[i];
        if (job.kind != QUEUE_JOB_DLC_PACK || job.state == QUEUE_FINISHED || strcmp(job.title, g_storeArcade->rar) != 0)
            continue;
        if (job.state == QUEUE_WAITING)
            return STORE_INSTALL_QUEUED;
        *outFraction = job.fraction;
        return STORE_INSTALL_INSTALLING;
    }
    if (ArcadePackageOnDrive(*g_storeArcade))
        return STORE_INSTALL_INSTALLED;
    for (int i = 0; !StoreGameIsIndie() && i < lib.count; ++i)
    {
        if (lib.games[i].titleId == g_storeArcade->titleId)
            return STORE_INSTALL_INSTALLED;
    }
    return STORE_INSTALL_AVAILABLE;
}

// Removes a game's own packages - those its RAR held for its title and
// type. How many were removed.
static int RemoveArcadePackages(const XblaGame &game)
{
    int removed = 0;
    const char *p = game.files;
    while (*p != '\0')
    {
        const char *space = strchr(p, ' ');
        const size_t n = (space != NULL) ? (size_t)(space - p) : strlen(p);
        char file[64], path[512];
        if (n > 0 && n < sizeof(file))
        {
            memcpy(file, p, n);
            file[n] = '\0';
            _snprintf(path, sizeof(path), "%s\\%08lX\\%08lX\\%s", g_contentBase, game.titleId, game.contentType, file);
            path[sizeof(path) - 1] = '\0';
            if (DeleteFileA(path))
                removed++;
        }
        p += n;
        while (*p == ' ')
            p++;
    }
    return removed;
}

// Where one disc of a version is: waiting or running in the game installer,
// installed, or neither.
enum StoreDiscProgress
{
    STORE_DISC_NOT_INSTALLED,
    STORE_DISC_QUEUED,
    STORE_DISC_INSTALLING,
    STORE_DISC_INSTALLED
};

// A disc counts as installed once the installer has installed it from this
// zip - or, for a single-disc version, once the library has its game, however
// it got there. A title ID can't say which disc of several is installed.
static StoreDiscProgress ProgressOfDisc(const Library &lib, const StoreRelease *release, const StoreDisc *disc,
                                        const QueueJobSnapshot *jobs, int jobCount, float *outFraction)
{
    for (int i = 0; i < jobCount; ++i)
    {
        if (jobs[i].state == QUEUE_FINISHED || strcmp(jobs[i].title, disc->zip) != 0)
            continue;
        if (jobs[i].state == QUEUE_WAITING)
            return STORE_DISC_QUEUED;
        if (outFraction != NULL)
            *outFraction = jobs[i].fraction;
        return STORE_DISC_INSTALLING;
    }

    if (IsGameZipInstalled(disc->zip))
        return STORE_DISC_INSTALLED;
    if (release->discCount == 1 && disc->titleId != 0)
    {
        for (int i = 0; i < lib.count; ++i)
        {
            if (lib.games[i].titleId == disc->titleId)
                return STORE_DISC_INSTALLED;
        }
    }
    return STORE_DISC_NOT_INSTALLED;
}

// The chosen version as a whole: installing while any disc is, queued while
// any waits, installed once every disc is. outPartial: some discs are
// installed and the rest aren't on their way.
static StoreInstallState StoreVersionState(const Library &lib, int version, float *outFraction, bool *outPartial)
{
    *outFraction = -1.0f;
    *outPartial = false;
    if (g_storeArcade != NULL)
        return ArcadeInstallState(lib, outFraction);
    const StoreRelease *release = StoreReleaseOf(&g_storeGame, version);
    if (release == NULL || release->discCount == 0)
    {
        // A game the Store hasn't got, opened from the library: installed,
        // as that's where it came from.
        for (int i = 0; g_storeGame.titleId != 0 && i < lib.count; ++i)
        {
            if (lib.games[i].titleId == g_storeGame.titleId)
                return STORE_INSTALL_INSTALLED;
        }
        return STORE_INSTALL_AVAILABLE;
    }

    static QueueJobSnapshot jobs[MAX_GAME_JOBS];
    const int jobCount = SnapshotGameJobs(jobs, MAX_GAME_JOBS);

    int installed = 0, queued = 0, installing = 0;
    float activeFraction = 0.0f;
    for (int d = 0; d < release->discCount; ++d)
    {
        const StoreDisc *disc = StoreReleaseDisc(release, d);
        if (disc == NULL)
            continue;
        float fraction = -1.0f;
        switch (ProgressOfDisc(lib, release, disc, jobs, jobCount, &fraction))
        {
        case STORE_DISC_INSTALLED:
            installed++;
            break;
        case STORE_DISC_QUEUED:
            queued++;
            break;
        case STORE_DISC_INSTALLING:
            installing++;
            activeFraction = fraction > 0.0f ? fraction : 0.0f;
            break;
        default:
            break;
        }
    }

    if (installing > 0)
    {
        // The whole version's progress: the discs done, and the one going.
        *outFraction = ((float)installed + activeFraction) / (float)release->discCount;
        return STORE_INSTALL_INSTALLING;
    }
    if (queued > 0)
        return STORE_INSTALL_QUEUED;
    if (installed == release->discCount)
        return STORE_INSTALL_INSTALLED;
    *outPartial = (installed > 0);
    return STORE_INSTALL_AVAILABLE;
}

// Whether the game page offers Uninstall: something of the chosen version is
// installed, and none of it is on its way.
static bool CanUninstallStoreVersion(const Library &lib, const Shell &shell)
{
    float fraction = 0.0f;
    bool partial = false;
    const StoreInstallState state = StoreVersionState(lib, shell.storeVersionChosen, &fraction, &partial);
    return state == STORE_INSTALL_INSTALLED || (state == STORE_INSTALL_AVAILABLE && partial);
}

// "<root>\TITLEID\00007000\MEDIAID" split up: the content root, and the
// media ID. False for a path that isn't a Games on Demand package's.
static bool ParseGodPackagePath(const char *path, char *root, size_t rootSize, unsigned long *mediaId)
{
    const char *name = strrchr(path, '\\');
    if (name == NULL || strlen(name + 1) != 8)
        return false;
    const char *type = name - 9; // "\00007000"
    if (type < path || _strnicmp(type, "\\00007000", 9) != 0)
        return false;
    const char *title = type - 9; // "\TITLEID"
    if (title < path || title[0] != '\\')
        return false;
    char *end = NULL;
    *mediaId = strtoul(name + 1, &end, 16);
    if (end == NULL || *end != '\0')
        return false;

    size_t n = (size_t)(title - path);
    if (n >= rootSize)
        return false;
    memcpy(root, path, n);
    root[n] = '\0';
    return true;
}

// Every package in a library game's own content-type folder - all the discs
// of a Games on Demand game, or an arcade game's one package. Its DLC and
// title updates are in other folders, and stay.
static int RemoveLibraryGamePackages(const InstalledGame &game)
{
    char root[512];
    unsigned long mediaId = 0;
    if (!ParseGodPackagePath(game.packagePath, root, sizeof(root), &mediaId))
    {
        // Not Games on Demand: one package file, nothing beside it.
        return DeleteFileA(game.packagePath) ? 1 : 0;
    }

    // Each disc's header in the 00007000 folder, by media ID.
    char folder[600], pattern[620];
    _snprintf(folder, sizeof(folder), "%s\\%08lX\\00007000", root, game.titleId);
    folder[sizeof(folder) - 1] = '\0';
    _snprintf(pattern, sizeof(pattern), "%s\\*", folder);
    pattern[sizeof(pattern) - 1] = '\0';

    unsigned long mediaIds[16];
    int count = 0;
    WIN32_FIND_DATAA found;
    HANDLE h = FindFirstFileA(pattern, &found);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            char *end = NULL;
            unsigned long id = strtoul(found.cFileName, &end, 16);
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && strlen(found.cFileName) == 8 &&
                end != NULL && *end == '\0' && count < 16)
                mediaIds[count++] = id;
        } while (FindNextFileA(h, &found));
        FindClose(h);
    }
    for (int i = 0; i < count; ++i)
        GodRemovePackage(root, game.titleId, mediaIds[i]);
    return count;
}

// Uninstall on the game page, once confirmed: the chosen version's discs - by
// the installer's note of each, or for a single disc the library's package -
// or, for a game the Store hasn't got, everything of it in the library. True
// if anything was removed.
static bool UninstallStoreVersion(Shell &shell, const Library &lib, const char *gamesPath)
{
    char message[200];
    _snprintf(message, sizeof(message), "Remove %s from the hard drive?", g_storeGame.name);
    message[sizeof(message) - 1] = '\0';
    if (!ShowConfirmUI("Uninstall", message, StoreGameIsIndie() ? "Only its own packages are removed."
                                                                : "Its DLC and title updates stay.", "Uninstall"))
        return false;

    RenderStatusFrame("Uninstalling", g_storeGame.name, NULL);

    int removed = 0;
    const StoreRelease *release = StoreReleaseOf(&g_storeGame, shell.storeVersionChosen);
    if (g_storeArcade != NULL)
    {
        // Its own packages; for an arcade game installed some other way -
        // another version - the library's.
        removed = RemoveArcadePackages(*g_storeArcade);
        for (int i = 0; removed == 0 && !StoreGameIsIndie() && i < lib.count; ++i)
        {
            if (lib.games[i].titleId == g_storeArcade->titleId)
                removed += RemoveLibraryGamePackages(lib.games[i]);
        }
    }
    else if (release == NULL)
    {
        for (int i = 0; i < lib.count; ++i)
        {
            if (lib.games[i].titleId == g_storeGame.titleId)
                removed += RemoveLibraryGamePackages(lib.games[i]);
        }
    }
    else
    {
        for (int d = 0; d < release->discCount; ++d)
        {
            const StoreDisc *disc = StoreReleaseDisc(release, d);
            if (disc == NULL)
                continue;

            unsigned long titleId = 0, mediaId = 0;
            if (GameZipInstalledAs(disc->zip, &titleId, &mediaId))
            {
                GodRemovePackage(gamesPath, titleId, mediaId);
                ForgetInstalledZip(disc->zip);
                removed++;
                continue;
            }
            // One the installer didn't note: the library's, for a single disc.
            for (int i = 0; release->discCount == 1 && disc->titleId != 0 && i < lib.count; ++i)
            {
                char root[512];
                if (lib.games[i].titleId == disc->titleId &&
                    ParseGodPackagePath(lib.games[i].packagePath, root, sizeof(root), &mediaId))
                {
                    GodRemovePackage(root, disc->titleId, mediaId);
                    removed++;
                }
            }
        }
    }

    dprintf("[store] uninstalled %d package(s) of \"%s\"\n", removed, g_storeGame.name);
    shell.rescanLibrary = true;
    shell.stale = true;
    if (removed > 0)
        ShowShellToast("Uninstalled", g_storeGame.name, UI_TOAST_SUCCESS);
    else
        ShowShellToast("Nothing to uninstall", "Its package wasn't where the library said.", UI_TOAST_ERROR);
    return removed > 0;
}

// A game's page over the library, by its title ID: as the Store has it - on
// the version that's installed, where it can tell - or, for a game the Store
// hasn't got (an arcade game, homebrew, a disc it lacks), a page of its own
// under the name given, with no versions to install. Find DLC and Title
// updates work either way. For a library game, and the disc in the drive.
static void OpenGameByTitleId(Shell &shell, const Library &lib, unsigned long titleId, const char *name)
{
    static char ownName[256];
    StoreGame game;
    if (!StoreGameByTitleId(titleId, &game))
    {
        // An arcade game the Store has: its page from there.
        const XblaGame *xbla = XblaGameByTitleId(titleId);
        if (xbla != NULL)
        {
            OpenArcadeGame(shell, *xbla);
            return;
        }
        _snprintf(ownName, sizeof(ownName), "%s", name != NULL ? name : "");
        ownName[sizeof(ownName) - 1] = '\0';
        memset(&game, 0, sizeof(game));
        game.name = ownName;
        game.titleId = titleId;
    }
    OpenStoreGame(shell, game);

    for (int v = 0; v < g_storeGame.versionCount; ++v)
    {
        float fraction = 0.0f;
        bool partial = false;
        if (StoreVersionState(lib, v, &fraction, &partial) == STORE_INSTALL_INSTALLED)
        {
            shell.storeVersionChosen = v;
            break;
        }
    }
}

// A on a library game.
static void OpenLibraryGame(Shell &shell, const Library &lib, const InstalledGame &chosen)
{
    OpenGameByTitleId(shell, lib, chosen.titleId, chosen.displayName);
}

static StoreGameView MakeStoreGameView(Shell &shell, const Library &lib)
{
    StoreGameView view;
    memset(&view, 0, sizeof(view));
    view.titleId = g_storeGame.titleId;
    view.indie = StoreGameIsIndie();
    view.name = g_storeGame.name;

    const StoreDetailsState state = GetStoreDetails(g_storeGame.titleId, &g_storeDetails);
    view.loading = (state == STORE_DETAILS_LOADING);
    if (state == STORE_DETAILS_READY)
    {
        const StoreDetails &d = g_storeDetails;
        g_storeMeta[0] = '\0';
        AppendText(g_storeMeta, sizeof(g_storeMeta), "", d.developer);
        if (strcmp(d.publisher, d.developer) != 0 && d.publisher[0] != '\0')
            AppendText(g_storeMeta, sizeof(g_storeMeta), STORE_MIDDOT, d.publisher);
        if (d.genre[0] != '\0')
            AppendText(g_storeMeta, sizeof(g_storeMeta), STORE_MIDDOT, d.genre);

        view.meta = g_storeMeta[0] != '\0' ? g_storeMeta : NULL;
        view.players = d.players;
        view.rating = d.rating;
        view.ratings = d.ratings;
        view.description = d.description;
        view.screenshots = d.screenshots;
        view.screenshotCount = d.screenshotCount;

        if (!shell.storeShotsAsked)
        {
            for (int i = 0; i < d.screenshotCount; ++i)
                RequestStoreArt(g_storeGame.titleId, STORE_ART_SCREEN + d.screenshots[i]);
            shell.storeShotsAsked = true;
        }
    }

    // Install speaks for the chosen version, all of its discs.
    static char installLabel[32];
    bool partial = false;
    view.install = StoreVersionState(lib, shell.storeVersionChosen, &view.installFraction, &partial);
    switch (view.install)
    {
    case STORE_INSTALL_QUEUED:
        view.buttons[0] = "Queued";
        break;
    case STORE_INSTALL_INSTALLING:
        if (view.installFraction >= 0.0f)
        {
            _snprintf(installLabel, sizeof(installLabel), "Installing %d%%", (int)(view.installFraction * 100.0f));
            installLabel[sizeof(installLabel) - 1] = '\0';
            view.buttons[0] = installLabel;
        }
        else
        {
            view.buttons[0] = "Installing";
        }
        break;
    case STORE_INSTALL_INSTALLED:
        view.buttons[0] = "Installed";
        break;
    default:
        view.buttons[0] = partial ? "Install remaining" : "Install";
        view.buttonDisabled[0] = (StoreVersionCount() == 0);
        break;
    }
    view.buttons[1] = "Find DLC";
    view.buttons[2] = "Title updates";
    if (view.indie)
    {
        // Indie games had neither, and Xbox Live's catalog doesn't list them.
        view.buttonDisabled[1] = view.buttonDisabled[2] = true;
        view.loading = false;
        view.description = "An Xbox Live Indie Game. Xbox Live's catalog doesn't list indie games, "
                           "so there's no description or screenshots for it here.";
    }
    if (view.install == STORE_INSTALL_INSTALLED || (view.install == STORE_INSTALL_AVAILABLE && partial))
        view.buttons[3] = "Uninstall"; // else not shown

    view.versions = g_storeVersions;
    view.versionCount = StoreVersionCount();
    view.versionChosen = shell.storeVersionChosen;
    view.versionScroll = shell.storeVersionScroll;
    view.focus = shell.storeGameFocus;
    view.focused = !shell.sidebarFocused;
    return view;
}

// The D-pad on a game page: along the buttons, and down the versions beside
// the synopsis. Left from Install, as from any first column, is the sidebar.
static void StepStoreGame(WORD nav, Shell &shell, const Library &lib)
{
    int &f = shell.storeGameFocus;
    const int versions = StoreVersionCount();
    const int buttons = CanUninstallStoreVersion(lib, shell) ? STORE_GAME_BUTTONS : STORE_GAME_BUTTONS - 1;

    if (f < STORE_GAME_FOCUS_VERSIONS)
    {
        const int b = f - STORE_GAME_FOCUS_BUTTONS;
        if (nav == XINPUT_GAMEPAD_DPAD_LEFT)
        {
            if (b == 0)
                shell.sidebarFocused = true;
            else
                f--;
        }
        else if (nav == XINPUT_GAMEPAD_DPAD_RIGHT && b + 1 < buttons)
            f++;
        else if (nav == XINPUT_GAMEPAD_DPAD_DOWN && versions > 0)
            f = STORE_GAME_FOCUS_VERSIONS + shell.storeVersionChosen;
    }
    else
    {
        const int v = f - STORE_GAME_FOCUS_VERSIONS;
        if (nav == XINPUT_GAMEPAD_DPAD_UP)
            f = (v == 0) ? STORE_GAME_FOCUS_BUTTONS : f - 1;
        else if (nav == XINPUT_GAMEPAD_DPAD_DOWN && v + 1 < versions)
            f++;
        else if (nav == XINPUT_GAMEPAD_DPAD_LEFT)
            f = STORE_GAME_FOCUS_BUTTONS;
    }
}

static void ReportEnqueue(EnqueueResult result, const char *filename);

// Install on an arcade or indie game's page: its RAR, queued as a pack - the
// download queue lists the RAR and fetches each package in it, as it does
// DLC's.
// The indie games' runtime: the Indie Games title's own update, without
// which a console offline from Xbox Live won't start one ("you need to
// download and apply the available update"). The only copy in archive.org's
// indie items is loose in Castle Miner Z's RAR - so that's where it's fetched
// from, that one file alone.
#define INDIE_RUNTIME_RAR  "Castle Miner Z.rar"
#define INDIE_RUNTIME_FILE "tu32000100_00000000"
#define INDIE_RUNTIME_NAME "Indie Games update"

// Queues the runtime if it isn't on the drive. True if it was queued.
static bool QueueIndieRuntime(const char *authHeader)
{
    char path[512];
    _snprintf(path, sizeof(path), "%s\\584E07D2\\000B0000\\" INDIE_RUNTIME_FILE, g_contentBase);
    path[sizeof(path) - 1] = '\0';
    if (GetFileAttributesA(path) != (DWORD)-1)
        return false;

    const XblaGame *carrier = ArcadeGameByRar(INDIE_RUNTIME_RAR);
    if (carrier == NULL)
        return false;

    DlcRarMatch pack;
    memset(&pack, 0, sizeof(pack));
    _snprintf(pack.item, sizeof(pack.item), "%s", ArcadeItemOf(carrier));
    pack.item[sizeof(pack.item) - 1] = '\0';
    _snprintf(pack.filename, sizeof(pack.filename), "%s", carrier->rar);
    pack.filename[sizeof(pack.filename) - 1] = '\0';
    _snprintf(pack.only, sizeof(pack.only), "%s", INDIE_RUNTIME_FILE);
    pack.size = carrier->size;
    pack.score = 100;
    return EnqueueDlcPack(pack, INDIE_RUNTIME_NAME, carrier->titleId, authHeader) == ENQUEUE_ADDED;
}

static void InstallArcadeGame(const char *authHeader)
{
    // An indie game's runtime goes first, if it's needed.
    const bool runtime = StoreGameIsIndie() && QueueIndieRuntime(authHeader);

    DlcRarMatch pack;
    memset(&pack, 0, sizeof(pack));
    _snprintf(pack.item, sizeof(pack.item), "%s", ArcadeItemOf(g_storeArcade));
    pack.item[sizeof(pack.item) - 1] = '\0';
    _snprintf(pack.filename, sizeof(pack.filename), "%s", g_storeArcade->rar);
    pack.filename[sizeof(pack.filename) - 1] = '\0';
    pack.size = g_storeArcade->size;
    pack.score = 100;
    const EnqueueResult result = EnqueueDlcPack(pack, g_storeArcade->name, g_storeArcade->titleId, authHeader);
    if (runtime && result == ENQUEUE_ADDED)
    {
        char message[192];
        _snprintf(message, sizeof(message), "%s, and the " INDIE_RUNTIME_NAME " indie games need to start",
                  g_storeArcade->name);
        message[sizeof(message) - 1] = '\0';
        ShowShellToast("Added to the queue", message, UI_TOAST_INFO);
    }
    else
    {
        ReportEnqueue(result, g_storeArcade->name);
    }
}

// Install on a game page: every disc of the chosen version that isn't
// installed or on its way, in disc order, queued for GameInstaller.
static void InstallStoreVersion(const Shell &shell, const Library &lib, const char *authHeader)
{
    if (g_storeArcade != NULL)
    {
        InstallArcadeGame(authHeader);
        return;
    }

    const StoreRelease *release = StoreReleaseOf(&g_storeGame, shell.storeVersionChosen);
    if (release == NULL)
        return;

    static QueueJobSnapshot jobs[MAX_GAME_JOBS];
    const int jobCount = SnapshotGameJobs(jobs, MAX_GAME_JOBS);

    int added = 0;
    GameEnqueueResult failure = GAME_QUEUED;
    for (int d = 0; d < release->discCount && failure == GAME_QUEUED; ++d)
    {
        const StoreDisc *disc = StoreReleaseDisc(release, d);
        if (disc == NULL || ProgressOfDisc(lib, release, disc, jobs, jobCount, NULL) != STORE_DISC_NOT_INSTALLED)
            continue;

        GameRequest request;
        memset(&request, 0, sizeof(request));
        _snprintf(request.item, sizeof(request.item), "%s", StoreItemOf(disc));
        _snprintf(request.zipName, sizeof(request.zipName), "%s", disc->zip);
        // The disc's number in its name, so the Queue page and the dashboard
        // tell the discs apart.
        if (release->discCount > 1 && disc->disc > 0)
            _snprintf(request.name, sizeof(request.name), "%s (Disc %u)", g_storeGame.name, (unsigned)disc->disc);
        else
            _snprintf(request.name, sizeof(request.name), "%s", g_storeGame.name);
        request.item[sizeof(request.item) - 1] = '\0';
        request.zipName[sizeof(request.zipName) - 1] = '\0';
        request.name[sizeof(request.name) - 1] = '\0';
        request.zipSize = disc->zipSize;
        request.titleId = disc->titleId;

        const GameEnqueueResult result = EnqueueGameInstall(request, authHeader);
        if (result == GAME_QUEUED)
            added++;
        else if (result != GAME_ALREADY_QUEUED)
            failure = result;
    }

    if (failure == GAME_QUEUE_FULL)
        ShowShellToast("The queue is full", added > 0 ? "Not every disc fit - try again when one finishes."
                                                      : "Wait for an install to finish first.", UI_TOAST_ERROR);
    else if (failure != GAME_QUEUED)
        ShowShellToast("Couldn't add it", "The game installer isn't running.", UI_TOAST_ERROR);
    else if (added > 1)
    {
        char message[64];
        _snprintf(message, sizeof(message), "%d discs of %s", added, g_storeGame.name);
        message[sizeof(message) - 1] = '\0';
        ShowShellToast("Added to the queue", message, UI_TOAST_INFO);
    }
    else if (added == 1)
        ShowShellToast("Added to the queue", g_storeGame.name, UI_TOAST_INFO);
    else
        ShowShellToast("Already in the queue", g_storeGame.name, UI_TOAST_INFO);
}

// A on the front page.
static void ActOnStoreFocus(Shell &shell)
{
    const int i = shell.storeFocus;
    if (i < STORE_FOCUS_BUTTONS)
    {
        const StoreFeaturedView &featured = kStoreFeatured[i - STORE_FOCUS_FEATURED];
        if (StoreGameByTitleId(featured.titleId, &g_storeGame))
            OpenStoreGame(shell, g_storeGame);
        else
            ShowShellToast("Not in the collection", featured.name, UI_TOAST_ERROR);
    }
    else if (i < STORE_FOCUS_LETTERS)
    {
        if (i - STORE_FOCUS_BUTTONS == STORE_BUTTON_XBLA)
            OpenSection(shell, ARCADE_XBLA);
        else if (i - STORE_FOCUS_BUTTONS == STORE_BUTTON_XBLIG)
            OpenSection(shell, ARCADE_XBLIG);
        else
            ShowShellToast("Coming soon", kStoreButtonsSoon[i - STORE_FOCUS_BUTTONS], UI_TOAST_INFO);
    }
    else
    {
        OpenStoreLetter(shell, kStoreLetters[i - STORE_FOCUS_LETTERS], false);
    }
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
    ReadStorageStatus(contentBasePath, &shell.storage);

    // An install, or a new games folder, can change whether the disc is in it.
    if (shell.hasDisc && shell.disc.state == DISC_READY)
        shell.discInstalled = IsDiscInstalled(gamesPath, shell.disc.titleId, shell.disc.mediaId);

    BuildSettingsPage(shell.settings, lib, gamesPath);

    shell.stale = false;
}

// Every frame, so the blocking screens - a search, a key check - show the
// sidebar as it was when they started.
static void PublishSidebar(const Shell &shell, const Library &lib)
{
    ShellSidebar sidebar;
    sidebar.page = shell.page;
    sidebar.focused = shell.sidebarFocused;
    sidebar.libraryCount = lib.count;
    sidebar.queueCount = PendingDownloadCount() + PendingDiscJobCount() + PendingGameJobCount();
    sidebar.storageUsed = shell.storage.used;
    strncpy(sidebar.storageLabel, shell.storage.label, sizeof(sidebar.storageLabel) - 1);
    sidebar.storageLabel[sizeof(sidebar.storageLabel) - 1] = '\0';
    strncpy(sidebar.storageDetail, shell.storage.detail, sizeof(sidebar.storageDetail) - 1);
    sidebar.storageDetail[sizeof(sidebar.storageDetail) - 1] = '\0';
    strncpy(sidebar.storageTotal, shell.storage.total, sizeof(sidebar.storageTotal) - 1);
    sidebar.storageTotal[sizeof(sidebar.storageTotal) - 1] = '\0';

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

    view.hasDisc = shell.hasDisc;
    view.discTitleId = (shell.disc.state == DISC_READY) ? shell.disc.titleId : 0;
    view.discName = shell.discName;
    view.discProgress = -1.0f;
    if (shell.disc.state == DISC_READING)
        view.discTile = DISC_TILE_READING;
    else if (shell.disc.state == DISC_UNREADABLE)
        view.discTile = DISC_TILE_UNREADABLE;
    else if (shell.disc.installing)
        view.discTile = DISC_TILE_INSTALLING;
    else if (shell.discInstalled)
        view.discTile = DISC_TILE_INSTALLED;
    else
        view.discTile = DISC_TILE_READY;

    // The bar across an installing disc's tile, from its job on the Queue.
    for (int i = 0; i < shell.queueCount; ++i)
    {
        const QueueJobSnapshot &job = shell.queueJobs[i];
        if (job.kind == QUEUE_JOB_DISC_INSTALL && job.state == QUEUE_ACTIVE)
            view.discProgress = job.fraction;
    }
    return view;
}

// The disc in the drive, as of this frame. When it changes - in, out, or a
// different one - its name is looked up, whether it's installed is checked,
// and its box art is asked for; the selection moves with the games, so the
// one you were on stays selected as the disc's tile comes and goes.
static void UpdateDisc(Shell &shell, const Library &lib, const char *gamesPath)
{
    DiscInfo now;
    GetDiscInfo(&now);

    const bool changed = (now.changeCount != shell.disc.changeCount);
    const bool hadDisc = shell.hasDisc;

    shell.disc = now;
    shell.hasDisc = (now.state == DISC_READY || now.state == DISC_READING || now.state == DISC_UNREADABLE);

    if (changed)
    {
        if (now.state == DISC_READY)
        {
            const char *listed = LookupTitleName(now.titleId);
            if (listed != NULL)
                _snprintf(shell.discName, sizeof(shell.discName), "%s", listed);
            else
                _snprintf(shell.discName, sizeof(shell.discName), "Title %08lX", now.titleId);
            shell.discInstalled = IsDiscInstalled(gamesPath, now.titleId, now.mediaId);
        }
        else if (now.state == DISC_READING)
        {
            _snprintf(shell.discName, sizeof(shell.discName), "Reading the disc");
            shell.discInstalled = false;
        }
        else if (now.state == DISC_UNREADABLE)
        {
            _snprintf(shell.discName, sizeof(shell.discName), "%s", now.reason);
            shell.discInstalled = false;
        }
        shell.discName[sizeof(shell.discName) - 1] = '\0';

        unsigned long coverTitle = (now.state == DISC_READY) ? now.titleId : 0;
        if (coverTitle != g_coverDiscTitleId)
        {
            g_coverDiscTitleId = coverTitle;
            RequestLibraryCovers(lib);
        }
    }

    if (hadDisc != shell.hasDisc)
    {
        if (shell.hasDisc)
            shell.librarySelected++;
        else if (shell.librarySelected > 0)
            shell.librarySelected--;
    }
}

// What A, X and START do on the disc's tile. Returns true if it took over
// the screen (the keyboard, a message), so input is resynced after.
static bool DiscTileAction(Shell &shell, const Library &lib, const char *gamesPath, WORD pressed)
{
    const DiscInfo &disc = shell.disc;

    if (disc.state == DISC_UNREADABLE)
    {
        if (pressed & XINPUT_GAMEPAD_A)
        {
            ShowMessageUI("Can't install this disc", disc.reason, "Only Xbox 360 game discs can be installed.");
            return true;
        }
        return false;
    }

    if (disc.state != DISC_READY)
        return false;

    if (disc.installing)
    {
        // It's on the Queue page; A goes there.
        if (pressed & XINPUT_GAMEPAD_A)
        {
            shell.page = SHELL_PAGE_QUEUE;
            shell.sidebarFocused = false;
        }
        return false;
    }

    // X is the game's page, installed or not - its details, versions, DLC
    // and updates. Once installed, A is its DLC and START installs it again.
    if (pressed & XINPUT_GAMEPAD_X)
    {
        OpenGameByTitleId(shell, lib, disc.titleId, shell.discName);
        return false;
    }

    const bool install = (!shell.discInstalled && (pressed & XINPUT_GAMEPAD_A)) ||
                         (shell.discInstalled && (pressed & XINPUT_GAMEPAD_START));

    if (!install)
    {
        if (shell.discInstalled && (pressed & XINPUT_GAMEPAD_A))
            OpenPicker(shell.picker, PICKER_DLC, shell.discName, disc.titleId);
        return false;
    }

    if (shell.discInstalled &&
        !ShowConfirmUI("Install again?", shell.discName,
                       "Installing it again replaces the copy on the drive.", "Reinstall"))
        return true;

    unsigned long long freeSpace = 0;
    if (DriveFreeSpace(gamesPath, &freeSpace) && freeSpace < disc.outputSize + 4ULL * 1024 * 1024)
    {
        ShowNotEnoughSpace(gamesPath, disc.outputSize, freeSpace);
        return true;
    }

    // A name from the bundled list is used as it is; without one, the
    // keyboard asks rather than installing it as "Title XXXXXXXX".
    std::string name = shell.discName;
    bool tookScreen = shell.discInstalled; // the confirm above
    if (LookupTitleName(disc.titleId) == NULL)
    {
        tookScreen = true;
        if (!AskDiscName(shell.discName, disc.discNumber, disc.discCount, name))
            return true;
    }

    char sizeText[64] = "";
    FormatBytes(disc.outputSize, sizeText, sizeof(sizeText));

    char detail[160];
    switch (QueueDiscInstall(disc.titleId, disc.mediaId, name.c_str()))
    {
    case DISC_INSTALL_QUEUED:
        _snprintf(detail, sizeof(detail), "%s" "  \xC2\xB7  " "%s, on the Queue page", name.c_str(), sizeText);
        detail[sizeof(detail) - 1] = '\0';
        ShowShellToast("Installing", detail, UI_TOAST_INFO);
        break;
    case DISC_INSTALL_BUSY:
        ShowShellToast("Already installing", "One disc installs at a time.", UI_TOAST_INFO);
        break;
    case DISC_INSTALL_NO_DISC:
        ShowShellToast("Couldn't install", "The disc in the drive changed. Try again in a moment.", UI_TOAST_ERROR);
        break;
    default:
        ShowShellToast("Couldn't install", "The disc worker didn't start - the log says why.", UI_TOAST_ERROR);
        break;
    }
    return tookScreen;
}

// One row up or down for an up/down nav, clamped to the list.
static void StepSelection(WORD nav, int count, int &selected)
{
    if (nav == XINPUT_GAMEPAD_DPAD_UP && selected > 0)
        selected--;
    else if (nav == XINPUT_GAMEPAD_DPAD_DOWN && selected < count - 1)
        selected++;
}

// ---------------------------------------------------------------------------
// The queue, from the UI's side
// ---------------------------------------------------------------------------

// Whose cover a job shows. By title ID rather than a row number, so it still
// finds the game after the library is rescanned.
static int LibraryIndexForTitle(const Library &lib, unsigned long titleId)
{
    for (int i = 0; i < lib.count; ++i)
    {
        if (lib.games[i].titleId == titleId)
            return i;
    }
    return -1;
}

static QueueRowTone ToneForOutcome(QueueOutcome outcome)
{
    switch (outcome)
    {
    case QUEUE_OUTCOME_INSTALLED:
    case QUEUE_OUTCOME_ALREADY_INSTALLED:
        return QUEUE_ROW_DONE;
    case QUEUE_OUTCOME_CANCELLED:
        return QUEUE_ROW_WAITING; // dim - it simply isn't happening
    default:
        return QUEUE_ROW_FAILED;
    }
}

// "12.4 MB of 48.0 MB   1.2 MB/s   3:21 left", or as much of it as is known.
// Fixed format specifiers and explicit termination throughout: this
// toolchain's _snprintf doesn't terminate on truncation, and its
// dynamic-precision specifier has crashed this project before.
static void FormatTransferNumbers(const QueueJobSnapshot &job, char *out, size_t outSize)
{
    out[0] = '\0';
    if (job.bytesDone == 0 && job.bytesTotal == 0)
        return;

    char done[64] = "", speed[64] = "";
    FormatBytes(job.bytesDone, done, sizeof(done));
    FormatBytes(job.bytesPerSec, speed, sizeof(speed));

    if (job.bytesTotal > 0)
    {
        char total[64] = "";
        FormatBytes(job.bytesTotal, total, sizeof(total));

        // Minutes/seconds cast down to int before formatting - small by
        // definition, and it avoids relying on %llu width handling here.
        int minutesLeft = (int)(job.secondsLeft / 60);
        int secsLeft = (int)(job.secondsLeft % 60);

        _snprintf(out, outSize, "%s of %s   %s/s   %d:%02d left", done, total, speed, minutesLeft, secsLeft);
    }
    else
    {
        // No size to go on - say what's known rather than implying a
        // percentage there isn't.
        _snprintf(out, outSize, "%s   %s/s", done, speed);
    }
    out[outSize - 1] = '\0';
}

// Copies the queue out and turns it into rows for the Queue page: a disc
// install and game installs still to do first, then the downloads as the
// download queue orders them, then finished game and disc installs.
static void SnapshotQueue(Shell &shell, const Library &lib)
{
    static QueueJobSnapshot discJobs[MAX_DISC_JOBS];
    static QueueJobSnapshot gameJobs[MAX_GAME_JOBS];
    const int discCount = SnapshotDiscJobs(discJobs, MAX_DISC_JOBS);
    const int gameCount = SnapshotGameJobs(gameJobs, MAX_GAME_JOBS);

    int n = 0;
    for (int i = 0; i < discCount; ++i)
    {
        if (discJobs[i].state != QUEUE_FINISHED)
            shell.queueJobs[n++] = discJobs[i];
    }
    for (int i = 0; i < gameCount; ++i)
    {
        if (gameJobs[i].state != QUEUE_FINISHED)
            shell.queueJobs[n++] = gameJobs[i];
    }
    n += SnapshotDownloadQueue(shell.queueJobs + n, MAX_QUEUE_JOBS);
    for (int i = 0; i < gameCount && n < MAX_QUEUE_ROWS; ++i)
    {
        if (gameJobs[i].state == QUEUE_FINISHED)
            shell.queueJobs[n++] = gameJobs[i];
    }
    for (int i = 0; i < discCount && n < MAX_QUEUE_ROWS; ++i)
    {
        if (discJobs[i].state == QUEUE_FINISHED)
            shell.queueJobs[n++] = discJobs[i];
    }
    shell.queueCount = n;

    // Follow the selected job to wherever it now sits.
    for (int i = 0; i < shell.queueCount; ++i)
    {
        if (shell.queueJobs[i].id == shell.queueSelectedId)
        {
            shell.queueSelected = i;
            break;
        }
    }
    if (shell.queueSelected > shell.queueCount - 1) shell.queueSelected = shell.queueCount - 1;
    if (shell.queueSelected < 0) shell.queueSelected = 0;
    shell.queueSelectedId = (shell.queueCount > 0) ? shell.queueJobs[shell.queueSelected].id : 0;

    for (int i = 0; i < shell.queueCount; ++i)
    {
        const QueueJobSnapshot &job = shell.queueJobs[i];
        QueueRowView &row = shell.queueRows[i];
        char *status = shell.queueStatus[i];
        char *numbers = shell.queueNumbers[i];
        const size_t statusSize = sizeof(shell.queueStatus[i]);

        row.title = job.title;
        row.gameName = job.gameName;
        row.status = status;
        row.numbers = NULL;
        row.libraryIndex = LibraryIndexForTitle(lib, job.titleId);
        row.titleId = job.titleId;
        numbers[0] = '\0';

        if (job.state == QUEUE_WAITING)
        {
            _snprintf(status, statusSize, "Waiting");
            row.tone = QUEUE_ROW_WAITING;
            row.showBar = true;
            row.fraction = -1.0f;
        }
        else if (job.state == QUEUE_ACTIVE)
        {
            _snprintf(status, statusSize, "%s", job.phase[0] != '\0' ? job.phase : "Starting");
            FormatTransferNumbers(job, numbers, sizeof(shell.queueNumbers[i]));
            row.numbers = numbers;
            row.tone = QUEUE_ROW_ACTIVE;
            row.showBar = true;
            row.fraction = job.fraction;
        }
        else
        {
            if (job.resultDetail[0] != '\0')
                _snprintf(status, statusSize, "%s   -   %s", job.resultText, job.resultDetail);
            else
                _snprintf(status, statusSize, "%s", job.resultText);
            row.tone = ToneForOutcome(job.outcome);
            row.showBar = false;
            row.fraction = -1.0f;
        }
        status[statusSize - 1] = '\0';
    }
}

// Notices the update check finishing: the Settings row says so, and a newer
// version gets one popup a session.
static void PollUpdateCheck(Shell &shell)
{
    static unsigned long seen = 0;
    static bool announced = false;
    unsigned long changes = 0;
    UpdateInfo info;
    const UpdateState state = GetUpdateState(&info, &changes);
    if (changes == seen)
        return;
    seen = changes;
    shell.stale = true; // the Settings row

    if (state == UPDATE_AVAILABLE && !announced)
    {
        announced = true;
        char title[64];
        _snprintf(title, sizeof(title), "Omni360 %s is available", info.version);
        title[sizeof(title) - 1] = '\0';
        ShowShellToast(title, "See Settings for what's new", UI_TOAST_INFO);
    }
}

// Drains the jobs that finished since the last frame. Each one may have
// changed what's on disk - the installed markers, the free space - so they're
// refreshed; the ones worth hearing about get a popup.
static void HandleFinishedDownloads(Shell &shell, bool &haveAuth)
{
    QueueJobSnapshot job;

    // A disc or game install: the game is new in the library, or it isn't.
    while (TakeFinishedDiscJob(&job) || TakeFinishedGameJob(&job))
    {
        shell.stale = true;
        if (job.outcome == QUEUE_OUTCOME_INSTALLED)
            shell.rescanLibrary = true;

        if (!job.notify)
            continue;

        char message[192];
        if (job.outcome == QUEUE_OUTCOME_INSTALLED)
            _snprintf(message, sizeof(message), "%s", job.gameName);
        else
            _snprintf(message, sizeof(message), "%s   -   %s", job.gameName, job.resultDetail);
        message[sizeof(message) - 1] = '\0';

        ShowShellToast(job.resultText, message,
                       job.outcome == QUEUE_OUTCOME_INSTALLED ? UI_TOAST_SUCCESS : UI_TOAST_ERROR);
    }

    while (TakeFinishedQueueJob(&job))
    {
        shell.stale = true;

        // Refused keys: rebuild the header from the file next time, so keys
        // fixed outside Settings - by replacing ArchiveOrgKeys.txt over FTP -
        // are picked up without restarting the app.
        if (job.outcome == QUEUE_OUTCOME_KEYS_REJECTED)
            haveAuth = false;

        // An arcade or indie game, by its RAR. An arcade game is new in the
        // library; an indie game isn't one the library lists.
        const XblaGame *arcade = (job.kind == QUEUE_JOB_DLC_PACK) ? ArcadeGameByRar(job.title) : NULL;
        const bool arcadeGame = (arcade != NULL);
        if (arcadeGame && ArcadeSetOf(arcade) == ARCADE_XBLA && job.outcome == QUEUE_OUTCOME_INSTALLED)
            shell.rescanLibrary = true;

        if (!job.notify)
            continue;

        char heading[64];
        _snprintf(heading, sizeof(heading), "%s", job.resultText);
        heading[sizeof(heading) - 1] = '\0';

        // The pack's filename says which download this was - an arcade
        // game's name, for one of those; the detail is in the Queue page for
        // anyone who wants it.
        const char *what = arcadeGame ? job.gameName : job.title;
        char message[192];
        if (job.outcome == QUEUE_OUTCOME_INSTALLED || job.outcome == QUEUE_OUTCOME_ALREADY_INSTALLED)
            _snprintf(message, sizeof(message), "%s", what);
        else
            _snprintf(message, sizeof(message), "%s   -   %s", what, job.resultDetail);
        message[sizeof(message) - 1] = '\0';

        ShowShellToast(heading, message,
                       ToneForOutcome(job.outcome) == QUEUE_ROW_DONE ? UI_TOAST_SUCCESS : UI_TOAST_ERROR);
    }
}

// What adding a row from a picker says. Never a blocking message - the point
// of the queue is that choosing a download doesn't stop you.
static void ReportEnqueue(EnqueueResult result, const char *filename)
{
    switch (result)
    {
    case ENQUEUE_ADDED:
        ShowShellToast("Added to the queue", filename, UI_TOAST_INFO);
        break;
    case ENQUEUE_ALREADY_QUEUED:
        ShowShellToast("Already in the queue", filename, UI_TOAST_INFO);
        break;
    case ENQUEUE_FULL:
        ShowShellToast("The queue is full", "Wait for a download to finish, or remove finished ones from the Queue.",
                       UI_TOAST_ERROR);
        break;
    default:
        ShowShellToast("Downloads aren't available", "The download worker didn't start - see the log.",
                       UI_TOAST_ERROR);
        break;
    }
}

static int AddHint(UiHint *hints, int count, UiButton button, const WCHAR *label, const WCHAR *shortLabel = NULL)
{
    hints[count].button = button;
    hints[count].label = label;
    hints[count].shortLabel = shortLabel;
    return count + 1;
}

// The DLC pack or title update picker, over whichever page opened it: a
// search in progress or what it found.
// A game's page, over the Store or the library, with the buttons that apply
// to what has focus.
static void RenderGamePageFrame(Shell &shell, const Library &lib, UiHint *hints, int hintCount)
{
    if (!shell.sidebarFocused)
    {
        const int f = shell.storeGameFocus;
        if (f >= STORE_GAME_FOCUS_VERSIONS)
            hintCount = AddHint(hints, hintCount, UI_BUTTON_A, L"Choose this version", L"Choose");
        else if (f == STORE_GAME_FOCUS_BUTTONS && StoreVersionCount() > 0)
            hintCount = AddHint(hints, hintCount, UI_BUTTON_A, L"Install", L"Install");
        else if (f == STORE_GAME_FOCUS_BUTTONS + 1 && !StoreGameIsIndie())
            hintCount = AddHint(hints, hintCount, UI_BUTTON_A, L"Find DLC", L"DLC");
        else if (f == STORE_GAME_FOCUS_BUTTONS + 2 && !StoreGameIsIndie())
            hintCount = AddHint(hints, hintCount, UI_BUTTON_A, L"Find title updates", L"Updates");
        else if (f == STORE_GAME_FOCUS_BUTTONS + 3)
            hintCount = AddHint(hints, hintCount, UI_BUTTON_A, L"Uninstall");
        hintCount = AddHint(hints, hintCount, UI_BUTTON_B, L"Back");
    }

    StoreGameView view = MakeStoreGameView(shell, lib);
    RenderStoreGameFrame(view, hints, hintCount);
    shell.storeGameFocus = view.focus;
    shell.storeVersionScroll = view.versionScroll;
}

static void RenderPickerFrame(Shell &shell, UiHint *hints, int hintCount)
{
    if (shell.picker.kind != PICKER_NONE && shell.picker.status != PICKER_READY)
    {
        const Picker &picker = shell.picker;
        const char *heading = (picker.kind == PICKER_DLC) ? "Choose a DLC pack" : "Choose a title update";
        const char *gameName = picker.gameName;

        if (picker.status == PICKER_UNREACHABLE)
            hintCount = AddHint(hints, hintCount, UI_BUTTON_A, L"Try again");
        hintCount = AddHint(hints, hintCount, UI_BUTTON_B, L"Back");

        if (picker.status == PICKER_SEARCHING)
        {
            // The dots step about three times a second - something
            // has to move, or a slow answer reads as a hang.
            static const char *const kSearching[] = {
                "Searching archive.org", "Searching archive.org.",
                "Searching archive.org..", "Searching archive.org..."};
            const char *message = kSearching[(GetTickCount() / 333) % 4];
            RenderPlaceholderFrame(heading, message, gameName, hints, hintCount);
        }
        else if (picker.status == PICKER_NOTHING)
        {
            RenderPlaceholderFrame(heading,
                                   picker.kind == PICKER_DLC ? "No DLC in the collection matched this game."
                                                             : "No title update matched this game.",
                                   gameName, hints, hintCount);
        }
        else
        {
            RenderPlaceholderFrame(heading, "Could not reach archive.org.",
                                   "Check the console's network connection and try again.",
                                   hints, hintCount);
        }
    }
    else
    {
        hintCount = AddHint(hints, hintCount, UI_BUTTON_A, L"Add to queue", L"Queue");
        hintCount = AddHint(hints, hintCount, UI_BUTTON_B, L"Back");

        ListPageView view;
        view.heading = (shell.picker.kind == PICKER_DLC) ? "Choose a DLC pack" : "Choose a title update";
        view.subheading = shell.picker.gameName;
        view.labels = shell.picker.labels;
        view.sublabels = shell.picker.sublabels;
        view.sections = NULL;
        view.count = shell.picker.count;
        view.selected = shell.picker.selected;
        view.scroll = shell.picker.scroll;
        view.focused = true;
        view.showCounter = true;

        RenderListFrame(view, hints, hintCount);

        shell.picker.selected = view.selected;
        shell.picker.scroll = view.scroll;
    }
}

// The auth header is built once and kept, and only rebuilt after the keys
// change or archive.org refuses them.
static bool EnsureAuthHeader(bool &haveAuth, char *authHeader, unsigned long long authHeaderSize)
{
    if (haveAuth)
        return true;

    // Nothing is drawn for saved keys - reading them is instant, and a frame
    // here only flashed up between A and the toast. GetArchiveOrgAuthHeader
    // draws its own screens when there's something to ask.
    //
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

    // X-Store, which this began as a fork of, is credited in the README's
    // Credits section - the modified-version notice AGPL-3.0 asks for.
    dprintf("Omni360 " CURRENT_VERSION "\n");

    // Whether opening the disc tray should leave the app running (see
    // xex.xml), and whether DashLaunch is loaded - the first thing to check
    // if the app is ever closed by an eject again.
    {
        HANDLE dashLaunch = NULL;
        bool haveDashLaunch = XexGetModuleHandle("launch.xex", &dashLaunch) >= 0 && dashLaunch != NULL;
        dprintf("Stays open when the disc tray opens (no-force-reboot privilege): %s; DashLaunch %s\n",
                XexCheckExecutablePrivilege(0) ? "yes" : "NO", haveDashLaunch ? "loaded" : "not loaded");
    }

    // The network as the app sees it - the first thing to read when a
    // download won't connect.
    LogNetworkStatus();

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
    g_contentBase = contentBasePath;
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
    // Before the first scan, which asks it for the library's box art.
    if (!StartCoverArt())
        dprintf("ERROR: the cover worker didn't start - tiles keep their icons\n");

    // Waits until the Store is first opened to fetch anything.
    if (!StartStoreArt())
        dprintf("ERROR: the Store's art worker didn't start - its tiles stay plain\n");

    // Reads what's in the drive straight away, for the library's first tile.
    if (!StartDiscWorker(gamesPath))
        dprintf("ERROR: the disc worker didn't start - discs can't be installed\n");

    // Also clears out what an install interrupted last time left staged.
    if (!StartGameInstaller(gamesPath))
        dprintf("ERROR: the game installer didn't start - games can't be installed\n");

    ScanLibrary(lib, gamesPath);

    // One request to GitHub, on its own thread; the Settings page says how it
    // went, and a newer version gets a popup.
    if (UpdateChecksOn())
        StartUpdateCheck();

    // The shell loop: read the controller, act on it, draw a frame.
    //
    // It starts on the library, with focus in the list, since that's where
    // almost every visit is headed. B steps back a level everywhere - out of a
    // picker to the library, out of a page to the sidebar - and only B on the
    // sidebar leaves the app.
    //
    // Downloads run on the queue's worker thread, so the loop carries on
    // while they do - it only reads their progress each frame. What still
    // takes over the screen - a search, the keyboard, a message, a disc
    // install - blocks inside this loop and draws its own frames. When it
    // returns, the input is resynced so the button that ended it can't also
    // act here, and everything it may have changed is refreshed before the
    // next frame.
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
    shell.queueCount = 0;
    shell.queueSelected = 0;
    shell.queueSelectedId = 0;
    shell.queueScroll = -1;
    shell.stale = true;

    char authHeader[IAS3_AUTH_HEADER_MAX];
    bool haveAuth = false;

    // Not fatal if it fails: everything but downloading still works, and
    // choosing a download then says why it can't.
    if (!StartDownloadQueue(contentBasePath))
        dprintf("ERROR: the download worker didn't start - downloads are unavailable\n");

    // Likewise: without it, a search says archive.org can't be reached.
    if (!StartSearchWorker())
        dprintf("ERROR: the search worker didn't start - searches are unavailable\n");

    ResyncUiInput();

    // For spotting stutter: a frame taking more than this is logged. Screens
    // that wait on the user - a confirmation, the keyboard - show up here too,
    // as one long frame; those can be ignored.
    const DWORD slowFrameMs = 50;
    DWORD lastFrame = 0;

    for (;;)
    {
        const DWORD frameStart = GetTickCount();
        if (lastFrame != 0 && frameStart - lastFrame > slowFrameMs)
            dprintf("[timing] slow frame: %lu ms (page %d)\n", (unsigned long)(frameStart - lastFrame), (int)shell.page);
        lastFrame = frameStart;

        // Finished downloads first, since they can make the rest stale.
        HandleFinishedDownloads(shell, haveAuth);
        PollUpdateCheck(shell);

        if (shell.rescanLibrary)
        {
            shell.rescanLibrary = false;
            ScanLibrary(lib, gamesPath);
            shell.stale = true;
        }

        UpdateDisc(shell, lib, gamesPath);

        if (shell.stale)
            RefreshShell(shell, lib, contentBasePath, gamesPath);

        SnapshotQueue(shell, lib);
        PollPickerSearch(shell.picker);
        PumpCoverArt();
        PumpStoreArt();

        // The featured wallpapers, the first time the Store is shown.
        if (shell.page == SHELL_PAGE_STORE && !shell.storeArtRequested)
        {
            for (int i = 0; i < STORE_FEATURED_COUNT; ++i)
                RequestStoreArt(kStoreFeatured[i].titleId, STORE_ART_BACKGROUND);
            shell.storeArtRequested = true;
        }

        // Removing the last finished job leaves nothing on the Queue to have
        // focus.
        if (shell.page == SHELL_PAGE_QUEUE && !shell.sidebarFocused && shell.queueCount == 0)
            shell.sidebarFocused = true;

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
                     PageTakesFocus(shell, shell.page))
                shell.sidebarFocused = false;
            else if (pressed & XINPUT_GAMEPAD_B)
            {
                // Leaving stops the queue, so it asks first when there's
                // anything in it still to do.
                int pending = PendingDownloadCount() + PendingDiscJobCount() + PendingGameJobCount();
                if (pending == 0)
                {
                    exitRequested = true;
                }
                else
                {
                    char message[96];
                    _snprintf(message, sizeof(message), "%d job%s still in the queue.",
                              pending, pending == 1 ? " is" : "s are");
                    message[sizeof(message) - 1] = '\0';

                    exitRequested = ShowConfirmUI("Leave Omni360?", message,
                                                  "Leaving stops them. Files that already finished stay installed; "
                                                  "a disc install is removed.",
                                                  "Leave");
                    acted = !exitRequested;
                }
            }
        }
        else if (shell.picker.kind != PICKER_NONE && shell.page == shell.picker.page)
        {
            Picker &picker = shell.picker;

            if (picker.status == PICKER_READY)
                StepSelection(input.nav, picker.count, picker.selected);

            if (pressed & XINPUT_GAMEPAD_B)
            {
                // Backing out of a search that's still running just leaves its
                // result uncollected.
                picker.kind = PICKER_NONE;
            }
            else if ((pressed & XINPUT_GAMEPAD_A) && picker.status == PICKER_UNREACHABLE)
            {
                OpenPicker(picker, picker.kind, picker.gameName, picker.titleId, picker.page); // try again
            }
            else if ((pressed & XINPUT_GAMEPAD_A) && picker.status == PICKER_READY)
            {
                // The keys, the first time something is queued. Usually
                // they're saved and this costs nothing; if they're not, it's
                // the keyboard, which takes over the screen.
                // If that took over the screen, resync once it's done.
                const bool hadAuth = haveAuth;
                const bool signedIn = EnsureAuthHeader(haveAuth, authHeader, sizeof(authHeader));
                if (!hadAuth)
                    acted = true;

                // Queued, not downloaded: this returns at once and the picker
                // stays open, so the next pack can be queued straight after.
                // How it goes turns up on the Queue page and as a popup.
                if (signedIn && picker.kind == PICKER_DLC)
                {
                    const DlcRarMatch &pack = picker.packs[picker.selected];
                    ReportEnqueue(EnqueueDlcPack(pack, picker.gameName, picker.titleId, authHeader),
                                  pack.filename);
                }
                else if (signedIn)
                {
                    const TitleUpdateMatch &update = picker.updates[picker.selected];
                    ReportEnqueue(EnqueueTitleUpdate(update, picker.gameName, picker.titleId, authHeader),
                                  update.filename);
                }
            }
        }
        else if (shell.page == SHELL_PAGE_LIBRARY && !shell.libraryInGame)
        {
            // Left from the first column goes to the sidebar, as B does.
            bool toSidebar = (pressed & XINPUT_GAMEPAD_B) != 0;

            // The disc's tile, when there is one, is item 0 and the games
            // follow it.
            const int discItems = shell.hasDisc ? 1 : 0;
            const int itemCount = lib.count + discItems;
            const bool onDisc = shell.hasDisc && shell.librarySelected == 0;

            if (itemCount == 0)
            {
                if (input.nav == XINPUT_GAMEPAD_DPAD_LEFT)
                    toSidebar = true;
            }
            else
            {
                const int cols = LibraryGridColumns();
                int &sel = shell.librarySelected;

                if (input.nav == XINPUT_GAMEPAD_DPAD_LEFT)
                {
                    if (sel % cols == 0)
                        toSidebar = true;
                    else
                        sel--;
                }
                else if (input.nav == XINPUT_GAMEPAD_DPAD_RIGHT)
                {
                    if (sel % cols < cols - 1 && sel + 1 < itemCount)
                        sel++;
                }
                else if (input.nav == XINPUT_GAMEPAD_DPAD_UP)
                {
                    if (sel >= cols)
                        sel -= cols;
                }
                else if (input.nav == XINPUT_GAMEPAD_DPAD_DOWN)
                {
                    // Down from a row with nothing under it lands on the last
                    // game, rather than doing nothing.
                    if (sel + cols < itemCount)
                        sel += cols;
                    else if (sel / cols < (itemCount - 1) / cols)
                        sel = itemCount - 1;
                }

                // Shoulder buttons jump a screenful of rows - the fast way
                // through a large library even with auto-repeat.
                if (pressed & (XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER))
                {
                    int jump = LibraryPageVisibleRows(MakeLibraryView(shell, lib, gamesPath)) * cols;
                    if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER)
                        sel -= jump;
                    if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER)
                        sel += jump;
                    if (sel > itemCount - 1) sel = itemCount - 1;
                    if (sel < 0) sel = 0;
                }
            }

            if (toSidebar)
            {
                shell.sidebarFocused = true;
            }
            else if (onDisc && (pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_START)))
            {
                acted = DiscTileAction(shell, lib, gamesPath, pressed);
            }
            else if ((pressed & XINPUT_GAMEPAD_A) && lib.count > 0 && !onDisc)
            {
                // Its page, with its DLC and title updates a button away.
                const InstalledGame &chosen = lib.games[shell.librarySelected - discItems];
                dprintf("Selected: %s (Title ID %08lX)\n", chosen.displayName, chosen.titleId);
                OpenLibraryGame(shell, lib, chosen);
            }
            else if (pressed & XINPUT_GAMEPAD_Y)
            {
                // A shortcut, for the keys banner and the empty library, which
                // both send you to Settings with a Y badge.
                shell.page = SHELL_PAGE_SETTINGS;
            }
        }
        else if (GamePageOpen(shell))
        {
            const int f = shell.storeGameFocus;
            if ((pressed & XINPUT_GAMEPAD_B) && shell.page == SHELL_PAGE_LIBRARY)
            {
                shell.libraryInGame = false; // back to the library's grid
            }
            else if (pressed & XINPUT_GAMEPAD_B)
            {
                // Back to the letter it was opened from, or the front page.
                shell.storeInGame = false;
                if (!shell.storeInLetter && !shell.storeInSection)
                    RequestFeaturedArt();
            }
            else if (pressed & XINPUT_GAMEPAD_A)
            {
                float installFraction = 0.0f;
                bool partial = false;
                const StoreInstallState installState =
                    StoreVersionState(lib, shell.storeVersionChosen, &installFraction, &partial);
                if (f >= STORE_GAME_FOCUS_VERSIONS)
                {
                    shell.storeVersionChosen = f - STORE_GAME_FOCUS_VERSIONS;
                }
                else if (f == STORE_GAME_FOCUS_BUTTONS && StoreVersionCount() > 0 &&
                         installState != STORE_INSTALL_AVAILABLE)
                {
                    // On its way or already there.
                    ShowShellToast(installState == STORE_INSTALL_INSTALLED ? "Already installed" : "Already in the queue",
                                   g_storeGame.name, UI_TOAST_INFO);
                }
                else if (f == STORE_GAME_FOCUS_BUTTONS && StoreVersionCount() > 0)
                {
                    // The keys, the first time - the keyboard, if they aren't saved.
                    const bool hadAuth = haveAuth;
                    if (EnsureAuthHeader(haveAuth, authHeader, sizeof(authHeader)))
                        InstallStoreVersion(shell, lib, authHeader);
                    if (!hadAuth)
                        acted = true;
                }
                else if ((f == STORE_GAME_FOCUS_BUTTONS + 1 || f == STORE_GAME_FOCUS_BUTTONS + 2) && !StoreGameIsIndie())
                {
                    // The library's pickers, open over this page.
                    OpenPicker(shell.picker, f == STORE_GAME_FOCUS_BUTTONS + 1 ? PICKER_DLC : PICKER_TITLE_UPDATE,
                               g_storeGame.name, g_storeGame.titleId, shell.page);
                }
                else if (f == STORE_GAME_FOCUS_BUTTONS + 3 && CanUninstallStoreVersion(lib, shell))
                {
                    // The confirmation takes over the screen, so resync after.
                    const bool removed = UninstallStoreVersion(shell, lib, gamesPath);
                    acted = true;
                    shell.storeGameFocus = STORE_GAME_FOCUS_BUTTONS;

                    // A game the Store hasn't got has no page once it's gone.
                    if (removed && shell.page == SHELL_PAGE_LIBRARY && StoreVersionCount() == 0)
                        shell.libraryInGame = false;
                }
            }
            else if (input.nav != 0)
            {
                StepStoreGame(input.nav, shell, lib);
            }
        }
        else if (shell.page == SHELL_PAGE_STORE && shell.storeInLetter)
        {
            if (pressed & XINPUT_GAMEPAD_B)
            {
                // Back to the page it was opened from, on the same letter.
                shell.storeInLetter = false;
                if (!shell.storeInSection)
                    RequestFeaturedArt();
            }
            else if ((pressed & XINPUT_GAMEPAD_A) && shell.storeGameCount > 0)
            {
                if (shell.storeLetterArcade)
                    OpenArcadeGame(shell, *g_arcadeGames[shell.storeSelected]);
                else
                    OpenStoreGame(shell, g_storeGames[shell.storeSelected]);
            }
            else
                StepStoreLetter(input, shell);
        }
        else if (shell.page == SHELL_PAGE_STORE && shell.storeInSection)
        {
            const ArcadeSet set = shell.storeSection;
            const int focusBefore = shell.sectionFocus[set];
            if (pressed & XINPUT_GAMEPAD_B)
            {
                shell.storeInSection = false; // back to the front page, on its button
                RequestFeaturedArt();
            }
            else if (pressed & XINPUT_GAMEPAD_A)
            {
                const int l = shell.sectionFocus[set];
                if (g_sectionLetterCounts[set][l] > 0)
                    OpenStoreLetter(shell, kStoreLetters[l], true);
                else
                    ShowShellToast("No games here", "Nothing in the collection starts with that.", UI_TOAST_INFO);
            }
            else if (input.nav != 0)
                StepSectionFocus(input.nav, shell);

            if (shell.storeInSection && !shell.storeInLetter && shell.sectionFocus[set] != focusBefore)
                PrefetchLetterCovers(kStoreLetters[shell.sectionFocus[set]], true, set);
        }
        else if (shell.page == SHELL_PAGE_STORE)
        {
            const int focusBefore = shell.storeFocus;
            if (pressed & XINPUT_GAMEPAD_B)
                shell.sidebarFocused = true;
            else if (pressed & XINPUT_GAMEPAD_A)
                ActOnStoreFocus(shell);
            else if (input.nav != 0)
                StepStoreFocus(input.nav, shell);

            if (!shell.storeInLetter && shell.storeFocus != focusBefore && shell.storeFocus >= STORE_FOCUS_LETTERS)
                PrefetchLetterCovers(kStoreLetters[shell.storeFocus - STORE_FOCUS_LETTERS], false, ARCADE_XBLA);
        }
        else if (shell.page == SHELL_PAGE_QUEUE)
        {
            StepSelection(input.nav, shell.queueCount, shell.queueSelected);
            if (shell.queueCount > 0)
                shell.queueSelectedId = shell.queueJobs[shell.queueSelected].id;

            if (input.nav == XINPUT_GAMEPAD_DPAD_LEFT || (pressed & XINPUT_GAMEPAD_B))
            {
                shell.sidebarFocused = true;
            }
            else if ((pressed & XINPUT_GAMEPAD_X) && shell.queueCount > 0)
            {
                const QueueJobSnapshot &job = shell.queueJobs[shell.queueSelected];

                if (job.state == QUEUE_FINISHED)
                {
                    if (IsGameJobId(job.id))
                        RemoveGameJob(job.id);
                    else if (IsDiscJobId(job.id))
                        RemoveDiscJob(job.id);
                    else
                        RemoveQueueJob(job.id);
                }
                else if (IsGameJobId(job.id))
                {
                    if (ShowConfirmUI("Stop installing?", job.gameName,
                                      "The game won't be installed. What has been downloaded is removed.",
                                      "Stop"))
                        CancelGameJob(job.id);
                    acted = true;
                }
                else if (IsDiscJobId(job.id))
                {
                    if (ShowConfirmUI("Stop installing?", job.gameName,
                                      "The game won't be installed. What has been copied so far is removed.",
                                      "Stop"))
                        CancelDiscJob(job.id);
                    acted = true;
                }
                else
                {
                    // Asked, like stopping a disc install: a big pack can be
                    // most of the way there. The download carries on while the
                    // question is up - it's on its own thread now.
                    if (ShowConfirmUI("Stop downloading?", job.title,
                                      job.kind == QUEUE_JOB_DLC_PACK ? "Files that already finished stay installed."
                                                                     : "The update won't be installed.",
                                      "Stop"))
                        CancelQueueJob(job.id);
                    acted = true;
                }
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
                                                         settings.args[settings.selected], lib, gamesPath,
                                                         sizeof(gamesPath));

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
            if (PageTakesFocus(shell, shell.page))
                hintCount = AddHint(hints, hintCount, UI_BUTTON_A, L"Select");
            hintCount = AddHint(hints, hintCount, UI_BUTTON_B, L"Exit");
        }

        switch (shell.page)
        {
        case SHELL_PAGE_LIBRARY:
            if (shell.picker.kind != PICKER_NONE && shell.picker.page == SHELL_PAGE_LIBRARY)
            {
                RenderPickerFrame(shell, hints, hintCount);
            }
            else if (shell.libraryInGame)
            {
                RenderGamePageFrame(shell, lib, hints, hintCount);
            }
            else
            {
                LibraryPageView view = MakeLibraryView(shell, lib, gamesPath);

                if (!shell.sidebarFocused)
                {
                    // The console's own A, X, B order. On the disc's tile they
                    // say what the disc's state allows.
                    const int itemCount = lib.count + (shell.hasDisc ? 1 : 0);
                    const bool onDisc = shell.hasDisc && shell.librarySelected == 0;

                    if (onDisc)
                    {
                        switch (view.discTile)
                        {
                        case DISC_TILE_READY:
                            hintCount = AddHint(hints, hintCount, UI_BUTTON_A, L"Install to hard drive", L"Install");
                            hintCount = AddHint(hints, hintCount, UI_BUTTON_X, L"View game", L"View");
                            break;
                        case DISC_TILE_INSTALLED:
                            hintCount = AddHint(hints, hintCount, UI_BUTTON_A, L"Find DLC", L"DLC");
                            hintCount = AddHint(hints, hintCount, UI_BUTTON_X, L"View game", L"View");
                            hintCount = AddHint(hints, hintCount, UI_BUTTON_START, L"Install again", L"Reinstall");
                            break;
                        case DISC_TILE_INSTALLING:
                            hintCount = AddHint(hints, hintCount, UI_BUTTON_A, L"View in the Queue", L"Queue");
                            break;
                        case DISC_TILE_UNREADABLE:
                            hintCount = AddHint(hints, hintCount, UI_BUTTON_A, L"Why?");
                            break;
                        default:
                            break;
                        }
                    }
                    else if (lib.count > 0)
                    {
                        hintCount = AddHint(hints, hintCount, UI_BUTTON_A, L"View game", L"View");
                    }
                    hintCount = AddHint(hints, hintCount, UI_BUTTON_B, L"Back");
                    if (itemCount > LibraryPageVisibleRows(view) * LibraryGridColumns())
                        hintCount = AddHint(hints, hintCount, UI_BUTTON_LBRB, L"Page");
                }

                RenderLibraryFrame(view, hints, hintCount);

                shell.librarySelected = view.selected;
                shell.libraryScroll = view.scroll;
            }
            break;

        case SHELL_PAGE_STORE:
            if (shell.picker.kind != PICKER_NONE && shell.picker.page == SHELL_PAGE_STORE)
            {
                RenderPickerFrame(shell, hints, hintCount);
            }
            else if (shell.storeInGame)
            {
                RenderGamePageFrame(shell, lib, hints, hintCount);
            }
            else if (shell.storeInLetter)
            {
                if (!shell.sidebarFocused)
                {
                    if (shell.storeGameCount > 0)
                        hintCount = AddHint(hints, hintCount, UI_BUTTON_A, L"View game", L"View");
                    hintCount = AddHint(hints, hintCount, UI_BUTTON_B, L"Store");
                    if (shell.storeGameCount > StoreLetterVisibleRows() * LibraryGridColumns())
                        hintCount = AddHint(hints, hintCount, UI_BUTTON_LBRB, L"Page");
                }

                StoreLetterView view = MakeStoreLetterView(shell);
                RenderStoreLetterFrame(view, hints, hintCount);
                shell.storeSelected = view.selected;
                shell.storeLetterScroll = view.scroll;
            }
            else if (shell.storeInSection)
            {
                const int l = shell.sectionFocus[shell.storeSection];
                if (!shell.sidebarFocused)
                {
                    static WCHAR browse[32];
                    if (g_sectionLetterCounts[shell.storeSection][l] > 0)
                    {
                        _snwprintf(browse, 32, L"Browse %c", (WCHAR)kStoreLetters[l]);
                        browse[31] = L'\0';
                        hintCount = AddHint(hints, hintCount, UI_BUTTON_A, browse, L"Browse");
                    }
                    hintCount = AddHint(hints, hintCount, UI_BUTTON_B, L"Store");
                }

                StoreSectionView view = MakeSectionView(shell);
                RenderStoreSectionFrame(view, hints, hintCount);
                shell.sectionFocus[shell.storeSection] = view.focus;
            }
            else
            {
                if (!shell.sidebarFocused)
                {
                    // What A does where the focus is - nothing, on a button
                    // that isn't ready.
                    static WCHAR browse[32];
                    const int i = shell.storeFocus;
                    if (i < STORE_FOCUS_BUTTONS)
                    {
                        hintCount = AddHint(hints, hintCount, UI_BUTTON_A, L"View game", L"View");
                    }
                    else if (i < STORE_FOCUS_LETTERS)
                    {
                        if (!kStoreButtons[i - STORE_FOCUS_BUTTONS].disabled)
                            hintCount = AddHint(hints, hintCount, UI_BUTTON_A, L"Browse", L"Browse");
                    }
                    else
                    {
                        _snwprintf(browse, 32, L"Browse %c", (WCHAR)kStoreLetters[i - STORE_FOCUS_LETTERS]);
                        browse[31] = L'\0';
                        hintCount = AddHint(hints, hintCount, UI_BUTTON_A, browse, L"Browse");
                    }
                    hintCount = AddHint(hints, hintCount, UI_BUTTON_B, L"Back");
                }

                StorePageView view = MakeStoreView(shell);
                RenderStoreFrame(view, hints, hintCount);
                shell.storeFocus = view.focus;
                shell.storeScroll = view.scroll;
            }
            break;

        case SHELL_PAGE_QUEUE:
            if (shell.queueCount == 0)
            {
                RenderPlaceholderFrame("Queue", "Nothing is downloading",
                                       "DLC and title updates you choose will wait here while they download.",
                                       hints, hintCount);
            }
            else
            {
                if (!shell.sidebarFocused)
                {
                    bool finished = (shell.queueJobs[shell.queueSelected].state == QUEUE_FINISHED);
                    hintCount = finished ? AddHint(hints, hintCount, UI_BUTTON_X, L"Remove")
                                         : AddHint(hints, hintCount, UI_BUTTON_X, L"Stop download", L"Stop");
                    hintCount = AddHint(hints, hintCount, UI_BUTTON_B, L"Back");
                }

                QueuePageView view;
                view.games = lib.games;
                view.gameCount = lib.count;
                view.rows = shell.queueRows;
                view.count = shell.queueCount;
                view.selected = shell.queueSelected;
                view.scroll = shell.queueScroll;
                view.focused = !shell.sidebarFocused;

                RenderQueueFrame(view, hints, hintCount);

                shell.queueSelected = view.selected;
                shell.queueScroll = view.scroll;
            }
            break;

        case SHELL_PAGE_SETTINGS:
        {
            if (!shell.sidebarFocused)
            {
                hintCount = AddHint(hints, hintCount, UI_BUTTON_A, L"Select");
                hintCount = AddHint(hints, hintCount, UI_BUTTON_B, L"Back");
            }

            ListPageView view;
            view.heading = "Settings";
            view.subheading = NULL;
            view.labels = shell.settings.labels;
            view.sublabels = shell.settings.sublabels;
            view.sections = shell.settings.sections;
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

    // Stop the worker before anything it uses goes away. A transfer stops at
    // its next progress report; a request already waiting on archive.org
    // can't be interrupted, so this is bounded rather than waited out.
    if (PendingDownloadCount() + PendingDiscJobCount() + PendingGameJobCount() > 0)
        RenderStatusFrame("Stopping", "Stopping downloads and installs", "Files that already finished stay installed.");
    StopDownloadQueue(15000);

    // An install stops at its next 816KB and removes what it copied.
    StopDiscWorker(15000);

    // A game install's connections stop at their next progress report, and
    // the converter at its next 816KB; the staging folder goes next launch
    // if it can't be removed now.
    StopGameInstaller(20000);

    // A search still out would only be thrown away - a short wait is plenty.
    StopSearchWorker(3000);

    // Likewise a cover download: it's fetched again next time.
    StopCoverArt(3000);
    StopStoreArt(3000);

    free(lib.games);

    dprintf("Done.\n");

    ShutdownGameListUI();

    return EXIT_SUCCESS;
}
