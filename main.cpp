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

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#define SETTINGS_FILE "game:\\settings.txt"
#define CREDENTIALS_FILE "game:\\ArchiveOrgKeys.txt"
#define CONTENT_BASE_PATH_DEFAULT "Hdd1:\\Content\\0000000000000000"
#define GAMES_PATH_DEFAULT "Hdd1:\\Games"
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
// settings.txt - two separate paths matter for this fork, and they can be
// genuinely different locations:
//   - the GAMES path: where EnumerateInstalledGames looks for your existing
//     library, to populate the picker. On a setup like Aurora's, disc-based/
//     GOD games often live in their own dedicated folder (e.g. Hdd1:\Games),
//     separate from the shared Content partition - confirmed against a real
//     console this session (00007000 = Game on Demand, same TitleID\
//     ContentType\ContentID layout, just a different root).
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
    GetSettingsPath("games-path: ", GAMES_PATH_DEFAULT, outPath, outPathSize);
}

// ---------------------------------------------------------------------------
// "Already has DLC" detection for the game list
// ---------------------------------------------------------------------------

// True if this title has at least one file under its Marketplace Content
// folder, i.e. {contentBase}\{TitleID}\00000002\*.
//
// Deliberately a shallow, purely LOCAL check. Answering "is this exact pack
// installed?" would mean walking every candidate archive's RAR headers over
// the network just to draw a marker - dozens of requests before the list could
// even be shown. A folder test costs one directory open per title and answers
// the question people actually have at that screen: have I already got DLC for
// this, or not.
static bool HasInstalledDlc(const char *contentBasePath, unsigned long titleId)
{
    char pattern[512];
    _snprintf(pattern, sizeof(pattern), "%s\\%08lX\\%08lX\\*",
              contentBasePath, titleId, (unsigned long)STFS_CONTENT_MARKETPLACE);
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
// after every download - a directory open per title, no network.
static void RefreshDlcInstalledFlags(const char *contentBasePath, const InstalledGame *games,
                                     int gameCount, bool *outFlags)
{
    for (int i = 0; i < gameCount; ++i)
        outFlags[i] = HasInstalledDlc(contentBasePath, games[i].titleId);
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

static void SaveKeys(const std::string &accessKey, const std::string &secretKey)
{
    FILE *f = fopen(CREDENTIALS_FILE, "wb");
    if (f == NULL)
        return;
    fprintf(f, "%s\n%s\n", accessKey.c_str(), secretKey.c_str());
    fclose(f);
}

static bool PromptKeys(std::string &accessKey, std::string &secretKey)
{
    if (OpenKeyboardToString(XUSER_INDEX_ANY, &accessKey, L"Archive.org Access Key",
                             L"From archive.org/account/s3.php", L"") != ERROR_SUCCESS)
        return false;

    if (OpenKeyboardToString(XUSER_INDEX_ANY, &secretKey, L"Archive.org Secret Key",
                             L"From archive.org/account/s3.php", L"") != ERROR_SUCCESS)
        return false;

    return !accessKey.empty() && !secretKey.empty();
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

        if (!PromptKeys(accessKey, secretKey))
        {
            dprintf("ERROR: no keys entered\n");
            ShowMessageUI("NO KEYS ENTERED", "Both keys are required to download from archive.org.",
                          "See the README for how to set up ArchiveOrgKeys.txt.");
            return false;
        }

        SaveKeys(accessKey, secretKey);
    }

    if (!BuildIas3AuthHeader(accessKey, secretKey, authHeader, authHeaderSize))
    {
        dprintf("ERROR: could not build auth header from saved keys - delete %s and re-enter them\n", CREDENTIALS_FILE);
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

// Downloads every file inside one chosen pack.
static void DownloadOnePack(const DlcRarMatch &pack, const char *contentBasePath, const char *authHeader)
{
    RenderStatusFrame("READING PACK", "Reading the file list", pack.filename);

    DlcMember members[MAX_DLC_MEMBERS];
    int memberCount = ListDlcMembers(pack.filename, pack.size, members, MAX_DLC_MEMBERS, authHeader, dprintf);

    if (memberCount <= 0)
    {
        ShowMessageUI("COULD NOT READ PACK",
                      "The file list for this pack could not be read.", pack.filename);
        return;
    }

    // Context for DlcProgressCallback, which the HTTP layer calls with nothing
    // but byte counts.
    strncpy(g_progressTitle, pack.filename, sizeof(g_progressTitle) - 1);
    g_progressTitle[sizeof(g_progressTitle) - 1] = '\0';
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
}

// Title updates: their own flow, reached with Y from the game list rather than
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

        if (DownloadTitleUpdate(updates[choice], game.titleId, contentBasePath,
                                authHeader, dprintf, DlcProgressCallback))
        {
            ShowMessageUI("TITLE UPDATE INSTALLED", updates[choice].filename,
                          "Restart your dashboard to pick up the update.");
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

        DownloadOnePack(matches[choice], contentBasePath, authHeader);
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

    // From here on the screen belongs to the UI rather than the debug console.
    // The dprintf calls stay - they're the log, and the log is still how
    // anything that goes wrong gets diagnosed - but every phase the user
    // actually waits on now draws a real frame instead of leaving whatever
    // was last on screen.
    RenderStatusFrame("SCANNING", "Reading your installed games", gamesPath);

    InstalledGame *games = (InstalledGame *)malloc(sizeof(InstalledGame) * MAX_INSTALLED_GAMES);
    int gameCount = EnumerateInstalledGames(gamesPath, games, MAX_INSTALLED_GAMES, dprintf);

    if (gameCount <= 0)
    {
        dprintf("No installed games found under %s\n", gamesPath);
        ShowMessageUI("NO GAMES FOUND", "Nothing was found in your games folder.", gamesPath);
        free(games);
        ShutdownGameListUI();
        return EXIT_FAILURE;
    }

    dprintf("Found %d installed games\n", gameCount);

    // Session loop. The game list is the app's root screen, and B steps BACK a
    // screen everywhere else - out of the pack picker to here, out of here to
    // the dashboard. Previously every B unwound straight out of main(), so
    // finishing one download, or changing your mind at any point, dropped you
    // out of the app entirely and made you relaunch to fetch a second pack.
    //
    // The library is scanned once and the keys are fetched once; both are held
    // across the loop so returning here costs nothing.
    char authHeader[IAS3_AUTH_HEADER_MAX];
    bool haveAuth = false;
    int listSelection = 0;

    // Which titles already have DLC on the console. Refreshed on every pass
    // rather than only at startup, so the marker appears the moment someone
    // comes back from a download instead of on the next launch.
    bool *dlcInstalled = (bool *)malloc(sizeof(bool) * gameCount);

    for (;;)
    {
        if (dlcInstalled != NULL)
            RefreshDlcInstalledFlags(contentBasePath, games, gameCount, dlcInstalled);

        GameListUIResult pick = ShowGameListUI(games, gameCount, listSelection, dlcInstalled);

        if (!pick.selected)
        {
            dprintf("Exiting\n");
            break; // B on the root screen is the way out
        }

        listSelection = pick.selectedIndex; // return them to the same row afterwards

        const InstalledGame &chosen = games[pick.selectedIndex];
        dprintf("Selected: %s (Title ID %08lX)\n", chosen.displayName, chosen.titleId);

        if (!haveAuth)
        {
            // The keyboard prompt inside here draws its own system UI, so this
            // frame is only what sits behind it on a run where the keys are
            // already cached and nothing is prompted at all.
            RenderStatusFrame("SIGNING IN", "Using your saved archive.org keys", chosen.displayName);

            if (!GetArchiveOrgAuthHeader(authHeader, sizeof(authHeader)))
            {
                ShowMessageUI("SIGN-IN FAILED", "No usable archive.org keys.",
                              "See the README for how to set up ArchiveOrgKeys.txt.");
                continue; // back to the list, so they can fix it and retry rather than being thrown out
            }

            haveAuth = true;
        }

        if (pick.titleUpdates)
            InstallTitleUpdatesForGame(chosen, contentBasePath, authHeader);
        else
            DownloadDlcForGame(chosen, contentBasePath, authHeader);
    }

    free(dlcInstalled);
    free(games);

    dprintf("Done.\n");

    ShutdownGameListUI();

    return EXIT_SUCCESS;
}
