#ifndef ARCHIVE_ORG_DLC_H
#define ARCHIVE_ORG_DLC_H

#include <string>

#include "downloadFile.h" // DownloadProgressFn, for DownloadDlcMember's progress callback

// Every file in msx360gcdlc is marked "private":"true" in its own metadata,
// and a real, fully anonymous (no cookies at all) request to its /download/
// URL confirmed this: a bare "401 Authorization Required" straight from
// nginx at the storage node, before any archive.org application code even
// runs - so SOME form of real authentication is genuinely required here,
// this isn't optional.
//
// This originally drove a from-scratch email/password login (POST
// /account/login, scrape the Set-Cookie session out of the response). That
// turned out to be built on the wrong endpoint entirely - archive.org's
// actual login is a JS SPA hitting a JSON API (/services/account/login/)
// with a CSRF token fetched from /services/csrf-token, whose exact request
// shape lives in a lazily-loaded JS chunk that was never worth fully
// reverse-engineering - and its login page's Content-Security-Policy
// references Google reCAPTCHA, so a scripted login could get bot-gated in a
// way a console app has no way to solve anyway.
//
// IAS3 (archive.org's official S3-like API, documented at
// archive.org/services/docs/api/ias3.html) sidesteps all of that: a
// permanent access key + secret key pair (fetched once, by the user, in a
// real logged-in browser at archive.org/account/s3.php - the exact
// mechanism archive.org's own "ia" CLI tool uses) sent as a single
// "Authorization: LOW <access>:<secret>" request header. No cookies, no
// CSRF, no session expiry, nothing to reverse-engineer further, and it
// authenticates the same private-item downloads a logged-in browser session
// does.
#define IAS3_AUTH_HEADER_MAX 160

// Formats "Authorization: LOW <access>:<secret>\r\n" into outHeader (at
// least IAS3_AUTH_HEADER_MAX bytes). Returns false if the keys don't fit.
bool BuildIas3AuthHeader(const std::string &accessKey, const std::string &secretKey,
                        char *outHeader, unsigned long long outHeaderSize);

// Whether the most recent keyed call - ListDlcMembers, DownloadDlcMember or
// DownloadTitleUpdate - failed because archive.org refused the keys (HTTP 401
// or 403), as opposed to a network error, a missing file or anything else.
// Each of those calls clears it on entry.
//
// This exists because the search itself doesn't use the keys - metadata is
// public - so wrong keys only surface once a download starts, and until
// this they surfaced as "could not read pack" or "archive.org may not serve
// this file", neither of which points at the keys.
bool ArchiveOrgKeysRejected();

// Free space on the drive holding `path` (anything starting "Hdd1:", "Usb0:"
// ...). False if it can't be found out, in which case callers shouldn't block
// a download over it.
bool DriveFreeSpace(const std::string &path, unsigned long long *outFree);

// Whether the most recent DownloadDlcMember / DownloadTitleUpdate failed for
// lack of disk space - refused up front, or run out of mid-transfer - and if
// so, how much the file needed and how much the drive had. Cleared on entry to
// each. A full drive used to surface only as a disk write error in the log,
// under a generic "download failed" on screen.
//
// Threading: both of these are plain module state, not per-call results. That
// is safe only because ListDlcMembers, DownloadDlcMember and
// DownloadTitleUpdate are all called from ONE thread - the one doing the
// downloads - and that thread reads the answer straight after its own call.
// The searches (FindDlcRarFilenames, FindTitleUpdates) and CheckArchiveOrgKeys
// never touch either, so they're fine to run on another thread. Calling any of
// the three keyed functions from a second thread would break this.
//
// In practice that one thread is the download worker (DownloadQueue.cpp).
//
// The searches have a rule of their own: they share a session cache of each
// collection's listing (see ItemListingJson), unlocked, so FindDlcRarFilenames
// and FindTitleUpdates must also stay on one thread - the search worker
// (SearchWorker.cpp).
bool ArchiveOrgDiskFull(unsigned long long *outNeeded, unsigned long long *outFree);

enum KeyCheckResult
{
    KEYS_ACCEPTED,  // archive.org says these keys are valid
    KEYS_REJECTED,  // archive.org says they are not
    KEYS_UNCHECKED  // no usable answer - offline, TLS failure, unexpected reply
};

// Asks archive.org whether authHeader's keys are valid, via its S3 service's
// check_auth endpoint - the same check archive.org's own "ia" tool makes. One
// small request, answered with {"authorized": true|false, ...}.
//
// On KEYS_REJECTED, outReason (if non-NULL) receives archive.org's own
// explanation, e.g. "The AWS Access Key Id you provided does not exist in our
// records." - worth showing, since it can say which of the two keys is wrong.
KeyCheckResult CheckArchiveOrgKeys(const char *authHeader, char *outReason, size_t outReasonSize,
                                   void printFunction(const char *_format, ...));

// STFS content types, from the Free60 wiki's STFS page. A DLC pack's members
// are laid out as {TitleID}\{ContentType}\{ContentID}, so the middle path
// segment says what each member actually is - and the same values name the
// folders content sits in on the console itself.
//
// Confirmed against a real pack: Sonic Generations' DLC archive holds twelve
// members, eleven of which are avatar items rather than game content.
#define STFS_CONTENT_MARKETPLACE  0x00000002UL // Marketplace Content - real DLC
#define STFS_CONTENT_AVATAR_ITEM  0x00009000UL // Avatar Item - clothing/props, not game content
#define STFS_CONTENT_TITLE_UPDATE 0x000B0000UL // Title Update - the folder lowercase "tu..." updates install to

// One matched DLC file discovered inside an archive.org item's RAR, with the
// internal member path we want to fetch (see FindDlcMemberPaths).
//
// Some archives keep the TitleID\ContentType\ContentID folders under one of
// their own - every Xbox Live Arcade RAR does, the game's name - so where a
// member installs is the path from its TitleID folder on: see
// DlcMemberDestination.
struct DlcMember
{
    char internalPath[512]; // e.g. "415607FF/00000002/66632C72BEC2A85B643F365E76BA3B2D68F7D0AF41"
    unsigned long packSize;  // RAR-compressed size (informational only - we never decompress it ourselves)
    unsigned long unpSize;   // real, uncompressed size - this is what we actually download via the virtual-path URL
};

#define MAX_DLC_RAR_MATCHES 16
#define DLC_RAR_FILENAME_LEN 256

struct DlcRarMatch
{
    char item[32];                        // the archive.org item it's in; empty for the DLC collection
    char filename[DLC_RAR_FILENAME_LEN]; // e.g. "007.Legends.DLC.RF.X360-ZTM.rar"
    unsigned long long size;              // whole-archive size in bytes, from the metadata listing
    int score;                            // 0-100 name-match confidence; results come back sorted by this, best first
};

// Soft-matches gameName against the msx360gcdlc item's file listing (via the
// public /metadata/ endpoint - no login required for this part). A game can
// have more than one separate DLC pack in this collection (e.g. "Call of
// Duty 2" has three: Bonus/Invasion/Skirmish Pack), so this fills
// outMatches (caller-allocated, up to maxMatches entries) and returns how
// many were found, or -1 on a request/parse failure (0 just means no match).
int FindDlcRarFilenames(const std::string &gameName, DlcRarMatch *outMatches, int maxMatches,
                        void printFunction(const char *_format, ...));

// Walks the pack's header chain (RAR4 or RAR5 - see RarHeaders.h) one entry at a time, each via a small,
// precisely-targeted Range request (RAR stores headers interleaved with each
// file's compressed data, not in one central directory, so this is a series
// of small requests rather than one big peek) - never downloads or
// decompresses real archive content. archiveSize (from DlcRarMatch::size)
// tells it where the archive ends. Fills outMembers (caller-allocated array)
// and returns the number of entries found, or -1 on failure.
//
// That walk is one network round trip per header, and a large pack has
// dozens - long enough that it needs to visibly move. progressFn, if
// non-NULL, is called once before the first request and again after every
// header, with:
//   bytesScanned / archiveSize - how far through the archive the walk is.
//                  It advances in uneven jumps, since each step skips a whole
//                  member's data, so it shows position rather than time left.
//   filesToInstall - files found so far that will be downloaded
//   avatarItemsSkipped - files found so far that are avatar items (clothing
//                  and props for an Xbox avatar, not game content) and so
//                  won't be - counted separately so the screen can say so
//
// Return true to carry on, false to cancel - ListDlcMembers then returns -1.
// It is only asked between headers, so a cancel lands once the request in
// flight has come back.
typedef bool (*ListMembersProgressFn)(unsigned long long bytesScanned, unsigned long long archiveSize,
                                      int filesToInstall, int avatarItemsSkipped);

int ListDlcMembers(const DlcRarMatch &pack,
                   DlcMember *outMembers, int maxMembers,
                   const char *authHeader, void printFunction(const char *_format, ...),
                   ListMembersProgressFn progressFn = NULL);

// Downloads one already-identified DLC member (via archive.org's
// /download/{item}/{rarfile}/{urlencoded internal path} virtual-path URL,
// which serves the file already-extracted server-side - no RAR decompression
// needed on our end) and writes it to contentBasePath + "\" + member's own
// internal path from its TitleID folder on (which mirrors the
// Content\0000000000000000\{TitleID}\{ContentType}\{ContentID} layout).
//
// progressFn, if non-NULL, is forwarded down to the HTTP layer and called
// periodically with live byte counts for this one member, so the caller can
// draw a progress bar. See DownloadProgressFn in downloadFile.h.
bool DownloadDlcMember(const DlcRarMatch &pack, const DlcMember &member,
                       const std::string &contentBasePath, const char *authHeader,
                       void printFunction(const char *_format, ...),
                       DownloadProgressFn progressFn = NULL);

// True if this member is already sitting at its destination at the expected
// size. Purely local - no network - so it is cheap enough to call for every
// member of a pack before downloading anything.
//
// Checks the SIZE as well as existence, because a transfer cancelled partway
// leaves a short file behind and treating that as installed would skip it
// permanently while the content stayed broken.
bool DlcMemberIsInstalled(const DlcMember &member, const std::string &contentBasePath);

// ---------------------------------------------------------------------------
// Title updates (a SEPARATE archive.org item from the DLC collection)
// ---------------------------------------------------------------------------
//
// Item: microsoft_xbox360_title-updates. Confirmed from its public metadata:
//
//   - Files are named by GAME NAME, region and version, not Title ID, e.g.
//     "Ace Combat 6 (Europe) (v2).zip", "Aliens Vs. Predator (World) (v3).zip".
//     So this needs the same fuzzy scoring as the DLC side - though these are
//     tidier names than scene releases, so it should do better here.
//   - They are .zip, NOT .rar, so none of the RAR header-walk above applies.
//     Zip is easier: the central directory sits at the END of the file, so one
//     Range request for the last few KB describes the whole archive instead of
//     a chain of requests.
//   - Each zip reports "filecount": "1" - exactly one member, so there is no
//     member list to enumerate or choose between.
//   - Marked "private": "true", same as the DLC item, so the same IAS3 auth
//     header is required.
//
// A game can have several versions (v1/v2/v3) and several regions in this
// item, which is exactly why these are offered in a picker rather than
// installed automatically - "install everything that matched" would mean
// three conflicting title updates.

#define MAX_TITLE_UPDATE_MATCHES 12
#define TITLE_UPDATE_FILENAME_LEN 256

struct TitleUpdateMatch
{
    char filename[TITLE_UPDATE_FILENAME_LEN]; // e.g. "Halo 3 (World) (v3).zip"
    unsigned long long size;
    int score;                                 // 0-100, same scale as DlcRarMatch
    int version;                               // parsed from the trailing "(vN)"; 0 if absent
    char region[40];                           // parsed from the "(Region)" group; empty if absent
};

// Soft-matches gameName against the title-update item's file listing. Fills
// outMatches (caller-allocated) sorted best-first - by score, then by version
// descending, so the newest update for the best-matching name comes first.
// Returns how many were found, or -1 on a request/parse failure.
int FindTitleUpdates(const std::string &gameName, TitleUpdateMatch *outMatches, int maxMatches,
                     void printFunction(const char *_format, ...));

// Reads the zip's single member: its name, and its uncompressed size (which
// also serves as the progress/ETA hint for the download). One Range request in
// the normal case - the central directory of a one-member zip falls inside the
// tail this already fetches.
//
// The member NAME is the interesting part: it decides where the update
// installs, and the two forms go to completely different places (see
// DownloadTitleUpdate).
bool ReadTitleUpdateMember(const TitleUpdateMatch &update, const char *authHeader,
                           char *outMemberName, unsigned long outMemberNameSize,
                           unsigned long *outUnpackedSize,
                           void printFunction(const char *_format, ...));

// Downloads and installs one title update.
//
// Destination depends on the member's filename, and both forms really do occur
// in this archive depending on the update's vintage:
//
//   tu00000002_00000000  (lowercase) -> {contentBase}\{TitleID}\000B0000\
//   TU_19KA228_00000...  (uppercase) -> {device}\Cache\
//
// Any folder structure inside the zip is dropped - only the file itself is
// installed.
//
// Like the DLC path, this relies on archive.org serving the member
// pre-extracted, so nothing here needs to understand deflate. If that turns
// out not to hold for zip, the request fails and this returns false rather
// than writing a corrupt file - the log then says so explicitly.
bool DownloadTitleUpdate(const TitleUpdateMatch &update, unsigned long titleId,
                         const std::string &contentBasePath, const char *authHeader,
                         void printFunction(const char *_format, ...),
                         DownloadProgressFn progressFn = NULL);

#endif
