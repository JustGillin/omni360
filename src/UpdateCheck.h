#ifndef UPDATE_CHECK_H
#define UPDATE_CHECK_H

// ---------------------------------------------------------------------------
// Checking GitHub for a newer Omni360
// ---------------------------------------------------------------------------
//
// One small request to GitHub's API for the repository's releases, on a
// thread of its own, compared against CURRENT_VERSION. Pre-releases count only
// while the running version is one itself ("0.2.0-beta"), so a stable build is
// only ever offered stable ones.
//
// Installing one is a separate step the user chooses (InstallUpdate): the
// release's XEX is only put in place if its signature verifies against the
// public key in UpdateKey.h, whose private half signs releases on one PC
// (tools/sign_release.py). A GitHub account taken over couldn't push a build
// to anyone through this.

#define UPDATE_RELEASES_PAGE "github.com/JustGillin/omni360/releases"

enum UpdateState
{
    UPDATE_NOT_CHECKED,
    UPDATE_CHECKING,
    UPDATE_CURRENT,   // nothing newer
    UPDATE_AVAILABLE, // see UpdateInfo
    UPDATE_FAILED     // GitHub couldn't be reached, or answered oddly
};

struct UpdateInfo
{
    char version[32];  // "0.3.0-beta", without the tag's "v"
    char title[128];   // the release's name
    char notes[2048];  // its description, Markdown taken out, '\n' between lines

    // The release's XEX and its signature, if it has both - see
    // tools/sign_release.py. Empty when it can't be installed from the app.
    char xexUrl[512];
    char sigUrl[512];
    unsigned long long xexSize;
};

// Whether InstallUpdate can install this one: it has an XEX and a signature.
inline bool UpdateInstallable(const UpdateInfo &info)
{
    return info.xexUrl[0] != '\0' && info.sigUrl[0] != '\0';
}

enum UpdateInstallResult
{
    UPDATE_INSTALL_OK,
    UPDATE_INSTALL_NOT_SIGNED,     // the release has no signature to check
    UPDATE_INSTALL_DOWNLOAD_FAILED,
    UPDATE_INSTALL_BAD_SIGNATURE,  // not signed with the key in UpdateKey.h - refused
    UPDATE_INSTALL_NOT_AN_XEX,
    UPDATE_INSTALL_NO_RUNNING_XEX, // couldn't tell which XEX in game:\ is this one
    UPDATE_INSTALL_WRITE_FAILED,   // the new one couldn't be written beside it
    UPDATE_INSTALL_SWAP_FAILED     // couldn't put it in place; the old one is still there
};

// Called as the XEX downloads, on the calling thread: bytes so far, and the
// total if known.
typedef void (*UpdateProgressFn)(unsigned long long done, unsigned long long total);

// Downloads the release's XEX and signature, checks the signature against
// the key compiled into the app and that it's an XEX, then puts it in place
// of the running one, keeping that as <name>.old. Blocks - the UI thread
// calls it, drawing progress through progress. On success outXexPath is the
// XEX to launch to run the new version. Nothing on disk changes unless every
// check passes.
UpdateInstallResult InstallUpdate(const UpdateInfo &info, UpdateProgressFn progress,
                                  char *outXexPath, unsigned long outXexPathSize);

// A sentence for each result, for the screen.
const char *UpdateInstallResultText(UpdateInstallResult result);

// Starts a check, unless one is already running. Returns at once.
void StartUpdateCheck();

// Where the last check got to; info is filled in when an update is available.
// changeCount goes up each time the state changes, for noticing one.
UpdateState GetUpdateState(UpdateInfo *info, unsigned long *changeCount);

// Compares two versions like "0.2.0-beta" and "v0.10.1": the numbers first,
// then a version without a suffix above the same one with ("0.3.0" is newer
// than "0.3.0-beta"), then the suffixes, numbers inside them compared as
// numbers ("beta.10" above "beta.2"). Negative, 0 or positive, as strcmp.
int CompareVersions(const char *a, const char *b);

#endif
