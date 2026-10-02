#ifndef UPDATE_CHECK_H
#define UPDATE_CHECK_H

// ---------------------------------------------------------------------------
// Checking GitHub for a newer Omni360
// ---------------------------------------------------------------------------
//
// One small request to GitHub's API for the repository's releases, on a
// thread of its own, compared against CURRENT_VERSION. Nothing is downloaded
// or installed here - this only says whether there's something newer, and
// what it's called. Pre-releases count only while the running version is one
// itself ("0.2.0-beta"), so a stable build is only ever offered stable ones.

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
};

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
