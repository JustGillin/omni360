/*
FILE : ArchiveOrgDLC.cpp
PROJECT : Omni360
DESCRIPTION : archive.org IAS3 key auth, DLC lookup (msx360gcdlc item
              metadata), RAR header-only parsing (no decompression - see
              note below), and per-member download via archive.org's
              server-side archive extraction ("virtual path") URLs.

Why no RAR decompression: archive.org's own /download/{item}/{rarfile}/{path}
URL form serves an individual member of a RAR/ZIP already-extracted, streamed
server-side. We only need to know which internal paths exist, which lives in
the RAR's own header/file-table - so we parse just that (no compression
codec needed at all).

Confirmed on real hardware this session: every file in msx360gcdlc is marked
"private":"true" in its own metadata, and a fully anonymous request to a
/download/ URL gets a bare 401 straight from nginx before any archive.org
application code runs - so real authentication is required. See
BuildIas3AuthHeader's comment in ArchiveOrgDLC.h for why this uses IAS3 keys
rather than a scraped browser login session.
*/

#include "ArchiveOrgDLC.h"
#include "downloadFile.h"
#include "parsing.h"
#include "cJSON.h"
#include "settings.h"

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define ARCHIVE_ITEM "msx360gcdlc"
#define ARCHIVE_METADATA_URL "https://archive.org/metadata/" ARCHIVE_ITEM
#define ARCHIVE_DOWNLOAD_BASE "https://archive.org/download/" ARCHIVE_ITEM "/"

#define ERROR_LOG(s) (printFunction("\nERROR: %s\n", s))

// See ArchiveOrgKeysRejected. Set by NoteAuthStatus, cleared on entry to each
// public call that sends keys.
static bool g_keysRejected = false;

// Records whether a keyed request was refused, and passes the status through
// so it can wrap a request in place. 401 is archive.org not accepting the
// credentials at all; 403 is it accepting who you are but not letting you
// have the file - both are fixed in the same place, the keys, so both count.
// Requests made without keys never set it: a 401 on those means something
// else entirely.
static int NoteAuthStatus(int status, const char *authHeader)
{
    if ((status == 401 || status == 403) && authHeader != NULL && authHeader[0] != '\0')
        g_keysRejected = true;
    return status;
}

bool ArchiveOrgKeysRejected()
{
    return g_keysRejected;
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static unsigned short ReadLE16(const unsigned char *p)
{
    return (unsigned short)(p[0] | (p[1] << 8));
}

static unsigned long ReadLE32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

// One redirect hop, reusing DumpResponse's existing Location-capture (it
// writes the redirect URL into the same buffer used for the response body
// when downloadIntoFile is false) rather than adding new redirect-parsing
// logic. archive.org's /download/ endpoint redirects to a specific storage
// node; /metadata/ does not, but this is harmless to use for both.
//
// ioCachedRedirectUrl is an optional in/out cache of the resolved datanode
// URL for this same `url`. ListDlcMembers calls this dozens/hundreds of
// times in a row against the exact same archive.org URL (walking one file's
// RAR headers via small Range requests), and archive.org's /download/
// endpoint always 302s a given item/file to the same datanode - so paying
// for a full connect+TLS handshake against archive.org itself, every single
// time, just to be told the identical Location again, was pure waste (and a
// real cost: TLS handshakes are the most CPU-expensive part of this whole
// flow on the 360's software-crypto BearSSL stack). When the cache is
// populated, this skips straight to the datanode with a single hop instead
// of two; if that ever stops working (expired/moved), it clears the cache
// and falls back to the normal two-hop path to re-resolve it.
static int HttpGetFollowRedirect(const std::string &url, char *dataBuffer, unsigned long long *dataBufferSize,
                                 const char *extraHeaderLines, const char *authHeader,
                                 void printFunction(const char *_format, ...),
                                 std::string *ioCachedRedirectUrl = NULL)
{
    char combinedHeaders[IAS3_AUTH_HEADER_MAX + 96] = "";
    if (authHeader != NULL && authHeader[0] != '\0')
    {
        _snprintf(combinedHeaders, sizeof(combinedHeaders), "%s%s",
                 authHeader, extraHeaderLines != NULL ? extraHeaderLines : "");
    }
    else if (extraHeaderLines != NULL)
    {
        strncpy(combinedHeaders, extraHeaderLines, sizeof(combinedHeaders) - 1);
    }

    unsigned long long bufSize = *dataBufferSize;

    if (ioCachedRedirectUrl != NULL && !ioCachedRedirectUrl->empty())
    {
        int cachedStatus = httpRequestHTTPS(*ioCachedRedirectUrl, HTTP_GET, NULL, combinedHeaders[0] ? combinedHeaders : NULL, "",
                                            dataBuffer, dataBufferSize, false,
                                            NULL, 0, printFunction);
        if (cachedStatus == 200 || cachedStatus == 206)
            return cachedStatus;

        // Cached datanode URL didn't pan out this time (expired, moved,
        // whatever) - fall through to the normal two-hop path below, which
        // will re-resolve and overwrite the cache with a fresh value.
        ioCachedRedirectUrl->clear();
        *dataBufferSize = bufSize;
    }

    int status = httpRequestHTTPS(url, HTTP_GET, NULL, combinedHeaders[0] ? combinedHeaders : NULL, "",
                                  dataBuffer, dataBufferSize, false,
                                  NULL, 0, printFunction);

    if (status == 302)
    {
        char redirectUrl[2048];
        strncpy(redirectUrl, dataBuffer, sizeof(redirectUrl) - 1);
        redirectUrl[sizeof(redirectUrl) - 1] = '\0';

        {
            char safeUrl[2048];
            LogEscapePercent(redirectUrl, safeUrl, sizeof(safeUrl));
            printFunction("Following redirect to: %s\n", safeUrl);
        }

        if (ioCachedRedirectUrl != NULL)
            *ioCachedRedirectUrl = redirectUrl;

        *dataBufferSize = bufSize;
        status = httpRequestHTTPS(std::string(redirectUrl), HTTP_GET, NULL, combinedHeaders[0] ? combinedHeaders : NULL, "",
                                  dataBuffer, dataBufferSize, false,
                                  NULL, 0, printFunction);
    }

    // Only the final answer counts. A failed attempt on the cached datanode
    // above is retried from scratch, so it says nothing about the keys yet.
    return NoteAuthStatus(status, authHeader);
}

// Lowercases and collapses every run of non-alphanumeric characters to a
// single space, trimming leading/trailing space. Used so "Call of Duty 2:
// Bonus Pack" and "Call.of.Duty.2.DLC.RF.X360-ZTM.rar" can be compared
// word-for-word regardless of the punctuation scene-release names use.
static void NormalizeForMatch(const char *in, char *out, size_t outSize)
{
    size_t o = 0;
    bool lastWasSpace = true; // true so we never emit a leading space

    for (size_t i = 0; in[i] != '\0' && o + 1 < outSize; ++i)
    {
        unsigned char c = (unsigned char)in[i];

        if (isalnum(c))
        {
            out[o++] = (char)tolower(c);
            lastWasSpace = false;
        }
        else if (!lastWasSpace)
        {
            out[o++] = ' ';
            lastWasSpace = true;
        }
    }

    while (o > 0 && out[o - 1] == ' ')
        --o; // trim a trailing space left by the loop above

    out[o] = '\0';
}

// Trims a trailing disc marker off an already-normalized query, in place.
//
// Multi-disc installs carry the disc in their STFS display name - real
// examples from a live library are "Battlefield 3 Disc 2 of 2" and "Max Payne
// 3 Disc 1 of 2" - but the DLC archive is named for the GAME, with no disc in
// it at all. Left alone those titles don't merely score low, they score
// themselves out of existence twice over: the unmatched words "disc", "of"
// and two copies of the disc number drag the ratio down to 60, and then the
// missing NUMBER trips the 40-point series penalty meant to keep "Halo 3" away
// from "Halo 4". Final score 20 against a threshold of 50, so a game with
// perfectly good DLC reports nothing found.
//
// Truncating at the marker rather than deleting individual words is
// deliberate: everything after it is disc bookkeeping, never part of the
// title.
//
// Two guards keep this from eating a legitimate name. The marker is only
// honoured when something precedes it, so a title that opens with the word
// never loses everything; and the following word must be a number, so
// "disc" used as an ordinary noun is left alone.
static void StripDiscSuffix(char *normalized)
{
    char *p = normalized;

    while (*p != '\0')
    {
        char *wordStart = p;
        while (*p != '\0' && *p != ' ')
            ++p;

        size_t wordLen = (size_t)(p - wordStart);

        char *next = p;
        while (*next == ' ')
            ++next;

        bool isDiscWord = (wordLen == 4 && strncmp(wordStart, "disc", 4) == 0) ||
                          (wordLen == 2 && strncmp(wordStart, "cd", 2) == 0);

        if (isDiscWord && wordStart != normalized && *next >= '0' && *next <= '9')
        {
            char *cut = wordStart;
            while (cut > normalized && cut[-1] == ' ')
                --cut;

            *cut = '\0';
            return;
        }

        p = next;
    }
}

// Pulls the next space-delimited word out of a normalized string, advancing
// the cursor. Returns false once there are none left.
static bool NextWord(const char **cursor, char *out, size_t outSize)
{
    const char *p = *cursor;

    while (*p == ' ')
        ++p;

    if (*p == '\0')
    {
        *cursor = p;
        return false;
    }

    size_t o = 0;
    while (*p != '\0' && *p != ' ')
    {
        if (o + 1 < outSize)
            out[o++] = *p;
        ++p; // keep advancing even past the buffer, so the cursor lands correctly
    }

    out[o] = '\0';
    *cursor = p;
    return true;
}

// True if word appears in haystack as a WHOLE word, not as a substring. Both
// are already normalized to lowercase words separated by single spaces, so
// the boundary test is just "space or end on each side". Substring matching
// would be far too loose here - "war" would hit "warfare", "ops" would hit
// "tops".
static bool ContainsWholeWord(const char *haystack, const char *word)
{
    size_t wlen = strlen(word);
    if (wlen == 0)
        return false;

    const char *p = haystack;
    while ((p = strstr(p, word)) != NULL)
    {
        bool atStart = (p == haystack) || (p[-1] == ' ');
        bool atEnd = (p[wlen] == '\0') || (p[wlen] == ' ');

        if (atStart && atEnd)
            return true;

        ++p;
    }

    return false;
}

static bool IsNumericWord(const char *word)
{
    if (*word == '\0')
        return false;

    for (const char *p = word; *p != '\0'; ++p)
    {
        if (*p < '0' || *p > '9')
            return false;
    }

    return true;
}

// Scores how well a candidate filename matches a game name, 0-100.
//
// This replaces a strict word-prefix match, which required the filename to
// BEGIN with the game's words and so failed on two real cases from a live
// library:
//
//   - Inserted words. "Halo: Combat Evolved Anniversary" against
//     "Halo.1.Combat.Evolved.Anniversary.DLC.RF.X360-ZTM.rar" - the "1" the
//     release group added breaks a prefix match at the second word, even
//     though every word of the title is present.
//   - Abbreviated install names. A package whose Display Name is
//     "CoD: World at War" can never prefix-match
//     "Call.of.Duty.World.at.War...", because "cod" isn't how the filename
//     starts - or appears at all.
//
// So: score by how much of the query is present ANYWHERE in the candidate,
// weighted by word length, and let the caller show a ranked list rather than
// trying to be certain. Weighting by length is what keeps "at"/"of"/"the"
// from carrying a match on their own while "anniversary" counts heavily.
static int ScoreDlcMatch(const char *candidateNorm, const char *queryNorm)
{
    int totalWeight = 0;
    int matchedWeight = 0;
    bool numberMissing = false;

    const char *cursor = queryNorm;
    char word[128];

    while (NextWord(&cursor, word, sizeof(word)))
    {
        int weight = (int)strlen(word);
        totalWeight += weight;

        if (ContainsWholeWord(candidateNorm, word))
            matchedWeight += weight;
        else if (IsNumericWord(word))
            numberMissing = true;
    }

    if (totalWeight == 0)
        return 0;

    int score = (matchedWeight * 100) / totalWeight;

    // A missing number is a much stronger signal than its one character of
    // weight suggests: it's usually the difference between entries in a
    // series. Without this, "Fallout 3" scores 87 against "Fallout New Vegas"
    // purely on the strength of "fallout", and "Halo 3" scores 80 against
    // "Halo 4".
    if (numberMissing)
        score -= 40;

    return (score < 0) ? 0 : score;
}

// Below this, a candidate isn't offered at all. Deliberately permissive -
// the user picks from a ranked list, so the cost of one extra plausible entry
// is far lower than the cost of hiding the right one.
#define DLC_MATCH_MIN_SCORE 50

// ---------------------------------------------------------------------------
// Auth
// ---------------------------------------------------------------------------

bool BuildIas3AuthHeader(const std::string &accessKey, const std::string &secretKey,
                        char *outHeader, unsigned long long outHeaderSize)
{
    if (accessKey.empty() || secretKey.empty())
        return false;

    int written = _snprintf(outHeader, outHeaderSize, "Authorization: LOW %s:%s\r\n",
                            accessKey.c_str(), secretKey.c_str());

    // _snprintf (unlike snprintf) doesn't null-terminate on truncation, and
    // returns -1 rather than the would-be length in that case.
    if (written < 0 || (unsigned long long)written >= outHeaderSize)
    {
        outHeader[outHeaderSize - 1] = '\0';
        return false;
    }

    return true;
}

// archive.org's S3 service, not archive.org itself. Its certificate chains to
// the same Go Daddy Root G2 anchor archiveOrgCert.h already carries (through a
// "GoDaddy TLS Root CA - R1" cross-signed by G2), so no new anchor is needed.
// Answers 200 with JSON either way; "authorized" is the verdict. Confirmed
// with deliberately fake keys:
//
//   {"error": "The AWS Access Key Id you provided does not exist in our
//    records.", "accesskey": "fakeaccess000000", "authorized": false}
#define ARCHIVE_S3_CHECK_AUTH_URL "https://s3.us.archive.org/?check_auth=1"

KeyCheckResult CheckArchiveOrgKeys(const char *authHeader, char *outReason, size_t outReasonSize,
                                   void printFunction(const char *_format, ...))
{
    if (outReason != NULL && outReasonSize > 0)
        outReason[0] = '\0';

    if (authHeader == NULL || authHeader[0] == '\0')
        return KEYS_REJECTED;

    char buffer[4096];
    unsigned long long bufferSize = sizeof(buffer) - 1;

    int status = HttpGetFollowRedirect(ARCHIVE_S3_CHECK_AUTH_URL, buffer, &bufferSize,
                                       NULL, authHeader, printFunction);

    // Anything but a 200 means no verdict was given - offline, a TLS failure,
    // the service down. That is "couldn't check", never "wrong keys": telling
    // someone their keys are bad because their network is would send them
    // off to fix the wrong thing.
    if (status != 200)
    {
        printFunction("Key check: no answer from archive.org (status %d)\n", status);
        return KEYS_UNCHECKED;
    }

    buffer[bufferSize < sizeof(buffer) ? bufferSize : sizeof(buffer) - 1] = '\0';

    cJSON *json = cJSON_Parse(buffer);
    if (json == NULL)
    {
        printFunction("Key check: reply was not JSON\n");
        return KEYS_UNCHECKED;
    }

    KeyCheckResult result = KEYS_UNCHECKED;

    cJSON *authorized = cJSON_GetObjectItemCaseSensitive(json, "authorized");
    if (cJSON_IsBool(authorized))
    {
        result = cJSON_IsTrue(authorized) ? KEYS_ACCEPTED : KEYS_REJECTED;

        cJSON *error = cJSON_GetObjectItemCaseSensitive(json, "error");
        if (result == KEYS_REJECTED && cJSON_IsString(error) && error->valuestring != NULL &&
            outReason != NULL && outReasonSize > 0)
        {
            strncpy(outReason, error->valuestring, outReasonSize - 1);
            outReason[outReasonSize - 1] = '\0';
        }
    }

    cJSON_Delete(json);

    printFunction("Key check: %s\n", result == KEYS_ACCEPTED ? "accepted"
                                   : result == KEYS_REJECTED ? "rejected" : "no verdict in reply");
    return result;
}

// ---------------------------------------------------------------------------
// DLC lookup (metadata)
// ---------------------------------------------------------------------------

int FindDlcRarFilenames(const std::string &gameName, DlcRarMatch *outMatches, int maxMatches,
                        void printFunction(const char *_format, ...))
{
    const unsigned long long METADATA_BUFFER_SIZE = 2 * 1024 * 1024; // metadata JSON for this item runs ~256KB; generous headroom
    char *buffer = (char *)malloc(METADATA_BUFFER_SIZE);
    if (buffer == NULL)
    {
        ERROR_LOG("malloc failed for metadata buffer");
        return -1;
    }

    unsigned long long bufferSize = METADATA_BUFFER_SIZE - 1;

    // Metadata is public - no cookie needed or sent here.
    int status = HttpGetFollowRedirect(ARCHIVE_METADATA_URL, buffer, &bufferSize, NULL, NULL, printFunction);

    if (status != 200)
    {
        printFunction("ERROR: could not fetch archive.org metadata (status %d)\n", status);
        free(buffer);
        return -1;
    }

    cJSON *json = cJSON_Parse(buffer);
    free(buffer);

    if (json == NULL)
    {
        ERROR_LOG("failed to parse archive.org metadata JSON");
        return -1;
    }

    cJSON *files = cJSON_GetObjectItemCaseSensitive(json, "files");
    if (!cJSON_IsArray(files))
    {
        ERROR_LOG("archive.org metadata had no files array");
        cJSON_Delete(json);
        return -1;
    }

    char normalizedQuery[512];
    NormalizeForMatch(gameName.c_str(), normalizedQuery, sizeof(normalizedQuery));

    // A multi-disc title carries its disc in the display name; the archive
    // never does. See StripDiscSuffix.
    size_t beforeStrip = strlen(normalizedQuery);
    StripDiscSuffix(normalizedQuery);

    if (strlen(normalizedQuery) != beforeStrip)
        printFunction("  Matching on \"%s\" (disc marker trimmed from the title)\n", normalizedQuery);

    int matchCount = 0;
    cJSON *file;
    cJSON_ArrayForEach(file, files)
    {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(file, "name");
        if (!cJSON_IsString(name) || name->valuestring == NULL)
            continue;

        // Only .rar entries are DLC packs in this collection; the item also
        // lists its own metadata/derivative files we don't want to match.
        size_t nameLen = strlen(name->valuestring);
        if (nameLen < 4 || _stricmp(name->valuestring + nameLen - 4, ".rar") != 0)
            continue;

        char normalizedName[512];
        NormalizeForMatch(name->valuestring, normalizedName, sizeof(normalizedName));

        int score = ScoreDlcMatch(normalizedName, normalizedQuery);
        if (score < DLC_MATCH_MIN_SCORE)
            continue;

        cJSON *size = cJSON_GetObjectItemCaseSensitive(file, "size");
        unsigned long long fileSize = 0;
        if (cJSON_IsString(size) && size->valuestring != NULL)
        {
            fileSize = _atoi64(size->valuestring);
        }
        else if (cJSON_IsNumber(size))
        {
            fileSize = (unsigned long long)size->valuedouble;
        }

        // Keep the BEST maxMatches, not the first maxMatches. The old code
        // stopped at the first N in archive.org's own file order, which for a
        // loose match could fill up with weak hits and drop the right one
        // simply because it appeared later in the listing.
        int slot = -1;

        if (matchCount < maxMatches)
        {
            slot = matchCount++;
        }
        else
        {
            int worst = 0;
            for (int i = 1; i < matchCount; ++i)
            {
                if (outMatches[i].score < outMatches[worst].score)
                    worst = i;
            }

            if (score > outMatches[worst].score)
                slot = worst;
        }

        if (slot < 0)
            continue;

        strncpy(outMatches[slot].filename, name->valuestring, DLC_RAR_FILENAME_LEN - 1);
        outMatches[slot].filename[DLC_RAR_FILENAME_LEN - 1] = '\0';
        outMatches[slot].size = fileSize;
        outMatches[slot].score = score;
    }

    cJSON_Delete(json);

    // Best first. Insertion sort - matchCount is capped at 16, so anything
    // cleverer would be noise.
    for (int i = 1; i < matchCount; ++i)
    {
        DlcRarMatch key = outMatches[i];
        int j = i - 1;

        while (j >= 0 && outMatches[j].score < key.score)
        {
            outMatches[j + 1] = outMatches[j];
            --j;
        }

        outMatches[j + 1] = key;
    }

    return matchCount;
}

// ---------------------------------------------------------------------------
// RAR header walk (no decompression - see file header comment)
// ---------------------------------------------------------------------------

#define RAR_PARSE_ERROR -1
#define RAR_NEED_MORE_DATA -2

// Parses whatever header data is present in [data, data+dataLen), where
// data[0] is the archive byte at whatever absolute offset the caller last
// requested (isFirstChunk means that offset is 0). Returns RAR_NEED_MORE_DATA
// if a header's fixed part or filename didn't fully fit in this chunk, or
// RAR_PARSE_ERROR if this doesn't look like a RAR4 archive at all. On
// success, *outNextPos is set to how many bytes past THE START OF THIS CHUNK
// the next header begins (0 meaning "no more entries" - headSize was 0) -
// this is a length/delta, not an absolute archive offset, since this
// function has no idea what absolute offset data[0] corresponds to. The
// caller must add its own current absolute position to it, not assign it
// directly (see the ListDlcMembers loop below - this was originally a real
// bug: assigning nextPos straight into pos caused the walk to "teleport" to
// essentially arbitrary offsets on every chunk after the first, since only
// the very first chunk starts at absolute offset 0 where local and absolute
// positions happen to coincide).
static int ParseOneRarHeader(const unsigned char *data, unsigned long dataLen, bool isFirstChunk,
                             DlcMember *outMember, bool *outIsFileEntry, unsigned long long *outNextPos)
{
    unsigned long pos = 0;

    if (isFirstChunk)
    {
        static const unsigned char SIG4[7] = {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00};
        if (dataLen < 7 || memcmp(data, SIG4, 7) != 0)
            return RAR_PARSE_ERROR;
        pos = 7;
    }

    if (pos + 7 > dataLen)
        return RAR_NEED_MORE_DATA;

    unsigned char headType = data[pos + 2];
    unsigned short headSize = ReadLE16(data + pos + 5);

    if (headSize == 0)
    {
        *outIsFileEntry = false;
        *outNextPos = 0; // caller treats 0 as "no more entries"
        return 0;
    }

    if (headType != 0x74) // not a FILE_HEAD (e.g. MAIN_HEAD) - skip it, nothing to report
    {
        *outIsFileEntry = false;
        *outNextPos = pos + headSize;
        return 0;
    }

    unsigned long fhPos = pos + 7;
    if (fhPos + 25 > dataLen)
        return RAR_NEED_MORE_DATA;

    unsigned long packSize = ReadLE32(data + fhPos + 0);
    unsigned long unpSize = ReadLE32(data + fhPos + 4);
    unsigned short nameSize = ReadLE16(data + fhPos + 19);
    unsigned long nameStart = fhPos + 25;

    if (nameStart + nameSize > dataLen)
        return RAR_NEED_MORE_DATA;

    *outIsFileEntry = (unpSize > 0); // RAR emits zero-size entries for directory path components; not real files
    if (*outIsFileEntry)
    {
        unsigned long copyLen = nameSize;
        if (copyLen >= sizeof(outMember->internalPath))
            copyLen = sizeof(outMember->internalPath) - 1;
        memcpy(outMember->internalPath, data + nameStart, copyLen);
        outMember->internalPath[copyLen] = '\0';
        outMember->packSize = packSize;
        outMember->unpSize = unpSize;
    }

    *outNextPos = pos + headSize + packSize;
    return 0;
}

// Reads the content-type segment out of a member's internal path.
//
// Paths look like "53450848\00000002\CD97F6BE...", so this is the second
// backslash-separated segment parsed as hex. Returns 0 when the path doesn't
// have that shape, which callers treat as "unknown, keep it" rather than as a
// reason to discard something.
static unsigned long MemberContentType(const char *internalPath)
{
    const char *firstSep = strchr(internalPath, '\\');
    if (firstSep == NULL)
        return 0;

    const char *typeStart = firstSep + 1;
    unsigned long value = 0;
    int digits = 0;

    for (const char *p = typeStart; *p != '\0' && *p != '\\'; ++p)
    {
        char c = *p;
        unsigned long digit;

        if (c >= '0' && c <= '9')       digit = (unsigned long)(c - '0');
        else if (c >= 'A' && c <= 'F')  digit = (unsigned long)(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f')  digit = (unsigned long)(c - 'a' + 10);
        else return 0; // not hex - this isn't the layout we expect

        value = (value << 4) | digit;

        if (++digits > 8)
            return 0; // too long to be a content type
    }

    return (digits > 0) ? value : 0;
}

int ListDlcMembers(const std::string &rarFilename, unsigned long long archiveSize,
                   DlcMember *outMembers, int maxMembers,
                   const char *authHeader, void printFunction(const char *_format, ...))
{
    g_keysRejected = false;

    std::string url = ARCHIVE_DOWNLOAD_BASE + rarFilename;

    // Generous per-request chunk: real header fixed part is 32 bytes, and
    // observed filenames are well under this; this is NOT the whole archive,
    // just enough to read one header (RAR interleaves headers with each
    // file's actual compressed data, so we can't read them all in one shot
    // without downloading everything in between).
    const unsigned long CHUNK_SIZE = 1024;
    unsigned char chunk[CHUNK_SIZE];

    unsigned long long pos = 0;
    int count = 0;
    int skippedAvatar = 0;
    bool isFirstChunk = true;

    // Resolved once (on the first Range request below) and reused for every
    // subsequent one against this same rarFilename, instead of re-resolving
    // the same archive.org -> datanode redirect from scratch (a full extra
    // TCP+TLS handshake) on every single small Range request while walking
    // this file's headers - see the comment on HttpGetFollowRedirect.
    std::string cachedDatanodeUrl;

    while (pos < archiveSize && count < maxMembers)
    {
        // Request exactly as many bytes as the buffer capacity we're about
        // to pass below (CHUNK_SIZE - 1, one byte held back so
        // DumpResponse's success-path null terminator - outputBuffer[
        // totalWritten] = '\0' - never writes at chunk[CHUNK_SIZE]). This
        // used to request CHUNK_SIZE (1024) bytes from the server while
        // only telling the download function it had 1023 bytes of room -
        // harmless against every server tried so far (a 401, or one that
        // ignored Range and got rejected some other way first), but the
        // first datanode that actually honored the Range request exactly as
        // asked (returning the full 1024 bytes, status 206) tripped our own
        // "Output Buffer Exhausted" check on its own correct response,
        // which discarded the real data yet still returned 206 - misleading
        // ListDlcMembers into parsing an untouched, stale/garbage chunk
        // buffer as if it were real RAR header bytes ("file does not look
        // like a RAR4 archive").
        unsigned long long chunkLen = CHUNK_SIZE - 1;
        unsigned long long endPos = pos + chunkLen - 1;
        if (endPos >= archiveSize)
            endPos = archiveSize - 1;

        char rangeHeader[64];
        _snprintf(rangeHeader, sizeof(rangeHeader), "Range: bytes=%I64u-%I64u\r\n", pos, endPos);

        int status = HttpGetFollowRedirect(url, (char *)chunk, &chunkLen, rangeHeader, authHeader, printFunction, &cachedDatanodeUrl);

        if (status != 206 && status != 200)
        {
            printFunction("ERROR: range request for RAR header failed (status %d) at offset %I64u\n", status, pos);
            return -1;
        }

        DlcMember member;
        bool isFileEntry = false;
        unsigned long long nextPos = 0;

        int parseResult = ParseOneRarHeader((const unsigned char *)chunk, (unsigned long)chunkLen, isFirstChunk,
                                            &member, &isFileEntry, &nextPos);

        if (parseResult == RAR_PARSE_ERROR)
        {
            ERROR_LOG("file does not look like a RAR4 archive");
            return -1;
        }

        if (parseResult == RAR_NEED_MORE_DATA)
        {
            // Shouldn't normally happen with a 1KB chunk and ~32-byte headers
            // plus short filenames, but guard against an unusually long path
            // by doubling the chunk size once and retrying from the same pos.
            printFunction("Header didn't fit in %lu bytes at offset %I64u, this file/path may be unusually large\n", CHUNK_SIZE, pos);
            return -1;
        }

        isFirstChunk = false;

        if (nextPos == 0)
            break; // headSize == 0: end of the header chain

        if (isFileEntry && count < maxMembers)
        {
            unsigned long memberType = MemberContentType(member.internalPath);

            if (memberType == STFS_CONTENT_AVATAR_ITEM)
            {
                // Avatar items, not game content. Confirmed against a real
                // pack: Sonic Generations' DLC archive holds twelve members,
                // and eleven of them are avatar items under 00009000 - so
                // without this, nearly all of a download is clothing for an
                // avatar rather than the DLC someone actually asked for.
                skippedAvatar++;
            }
            else if (memberType != STFS_CONTENT_MARKETPLACE && memberType != 0)
            {
                // Something neither DLC nor avatar data. Kept rather than
                // dropped - only one pack has been examined closely, and
                // silently discarding a content type we simply haven't seen
                // yet is a worse failure than downloading one extra file.
                // The log line is how we find out what else is out there.
                printFunction("  Note: member has content type %08lX (not marketplace content), keeping it anyway: %s\n",
                              memberType, member.internalPath);
                outMembers[count] = member;
                count++;
            }
            else
            {
                outMembers[count] = member;
                count++;
            }
        }

        // nextPos is a delta from the start of the chunk we just parsed
        // (i.e. from `pos` as it was for this iteration), not an absolute
        // archive offset - see the comment on ParseOneRarHeader. This used
        // to be `pos = nextPos;`, which only ever happened to work for the
        // very first header (where pos was already 0), and silently
        // "teleported" pos to essentially arbitrary values on every header
        // after that - explaining why a 2.3GB archive with many real DLC
        // files inside only ever turned up a single (and not even
        // necessarily correct) member.
        pos = pos + nextPos;
    }

    if (skippedAvatar > 0)
        printFunction("  Skipped %d avatar item(s); %d file(s) to install\n", skippedAvatar, count);

    return count;
}

// ---------------------------------------------------------------------------
// Fetch one member (already-extracted by archive.org server-side) and write
// it straight into the console's content folder.
// ---------------------------------------------------------------------------

// Where a member lands on disk.
//
// The RAR's own internal path already mirrors the console's layout
// ({TitleID}\{ContentType}\{ContentID}), so the destination is just that path
// under the content root. Shared by the download and the "is it already
// there?" check so the two can never disagree about where a file belongs -
// which is exactly the kind of drift that makes an install check quietly
// useless.
static std::string DlcMemberDestination(const DlcMember &member, const std::string &contentBasePath)
{
    std::string relativePath = member.internalPath;

    // A no-op for every archive seen so far - these store '\\' already - but
    // harmless insurance against a differently-packaged one.
    for (size_t i = 0; i < relativePath.size(); ++i)
    {
        if (relativePath[i] == '/')
            relativePath[i] = '\\';
    }

    return contentBasePath + "\\" + relativePath;
}

bool DlcMemberIsInstalled(const DlcMember &member, const std::string &contentBasePath)
{
    std::string destPath = DlcMemberDestination(member, contentBasePath);

    FILE *f = fopen(destPath.c_str(), "rb");
    if (f == NULL)
        return false;

    fseek(f, 0, SEEK_END);
    long onDisk = ftell(f);
    fclose(f);

    if (onDisk < 0)
        return false;

    // Size has to match, not just existence. A download cancelled partway
    // through (Start pauses, B cancels - both reachable mid-transfer) leaves a
    // short file behind, and treating that as installed would skip it forever
    // while the game stayed broken. unpSize is the real uncompressed size
    // straight from the RAR header, so this is a genuine check rather than a
    // guess.
    return ((unsigned long)onDisk == member.unpSize);
}

bool DownloadDlcMember(const std::string &rarFilename, const DlcMember &member,
                       const std::string &contentBasePath, const char *authHeader,
                       void printFunction(const char *_format, ...),
                       DownloadProgressFn progressFn)
{
    g_keysRejected = false;

    // Build the virtual-path URL: /download/{item}/{rarfile}/{urlencoded/internal/path}
    // Each path SEGMENT is percent-encoded separately so the separators
    // themselves become the literal "%2F" archive.org's own virtual-path
    // parser expects between segments (confirmed from a real "view contents"
    // link) - this splits on '\\' rather than '/' because these RAR
    // archives (packaged on Windows for Xbox 360 content) store internal
    // paths with backslash separators, confirmed on hardware from
    // member.internalPath printing as "415608D8\00000002\<hash>". Splitting
    // on '/' (as this used to) never finds a match, so the WHOLE path
    // becomes one segment, and its backslashes get percent-encoded to
    // "%5C" instead of joining segments with the "%2F" archive.org actually
    // expects there - the wrong URL entirely, not just a display glitch.
    char encodedPath[2048] = "";
    char segment[512];
    const char *p = member.internalPath;

    while (*p != '\0')
    {
        const char *slash = strchr(p, '\\');
        size_t segLen = slash ? (size_t)(slash - p) : strlen(p);

        if (segLen >= sizeof(segment))
        {
            ERROR_LOG("internal path segment too long");
            return false;
        }

        memcpy(segment, p, segLen);
        segment[segLen] = '\0';

        char encodedSegment[1024];
        if (!UrlEncodeFormValue(segment, encodedSegment, sizeof(encodedSegment)))
        {
            ERROR_LOG("failed to URL-encode internal path segment");
            return false;
        }

        if (strlen(encodedPath) + strlen(encodedSegment) + 4 >= sizeof(encodedPath))
        {
            ERROR_LOG("encoded internal path too long");
            return false;
        }

        strcat(encodedPath, encodedSegment);

        if (slash != NULL)
        {
            strcat(encodedPath, "%2F");
            p = slash + 1;
        }
        else
        {
            break;
        }
    }

    // RESOLVED, and the diagnostic that asked it has been removed: the "%2F"
    // separators here really were going missing from the LOG while being
    // perfectly correct in memory. The giveaway was that the diagnostic
    // reported len=64 - exactly right for the fully encoded path - while
    // printing something much shorter and garbled, and the downloads built
    // from it succeeded. It is a logging artifact, not a construction bug;
    // see LogEscapePercent in parsing.h for the cause and the fix.
    std::string url = ARCHIVE_DOWNLOAD_BASE + rarFilename + "/" + encodedPath;

    // Destination mirrors the RAR's own internal path, e.g.
    // {contentBasePath}\415607FF\00000002\66632C72...  - this already matches
    // Content\0000000000000000\{TitleID}\{ContentType}\{ContentID}. The '/'
    // replacement below is a no-op now that internalPath is known to already
    // use '\\' (see the encodedPath comment above) - harmless to leave in
    // case a differently-packaged archive ever does use '/', but not doing
    // any real work against the files tested this session.
    std::string destPath = DlcMemberDestination(member, contentBasePath);

    // Create the TitleID\ContentType\ subdirectories.
    size_t lastSlash = destPath.find_last_of('\\');
    if (lastSlash != std::string::npos)
    {
        std::string destDir = destPath.substr(0, lastSlash);
        // FileSystem.CreateDirectory-equivalent: XDK's CreateDirectory works
        // one level at a time, so create each parent in turn.
        std::string building = contentBasePath;
        std::string remainder = destDir.substr(contentBasePath.size());
        size_t start = 0;
        while (start < remainder.size())
        {
            size_t next = remainder.find('\\', start);
            if (next == std::string::npos)
                next = remainder.size();
            if (next > start)
            {
                building += "\\" + remainder.substr(start, next - start);
                CreateDirectoryA(building.c_str(), NULL); // ok if it already exists
            }
            start = next + 1;
        }
    }

    printFunction("Downloading DLC member: %s -> %s\n", member.internalPath, destPath.c_str());

    // HttpGetFollowRedirect (used elsewhere in this file) only works in
    // buffer mode, but a DLC file can be large, so this downloads straight
    // into destPath instead and follows one redirect manually if needed.
    int httpStatus = httpRequestHTTPS(url, HTTP_GET, NULL, authHeader,
                                      destPath, NULL, NULL, true,
                                      NULL, 0, printFunction, member.unpSize, progressFn);

    if (httpStatus == 302)
    {
        // The file-mode attempt above, on hitting a 302, already wrote the
        // raw redirect URL text as a throwaway line into destPath (that's
        // DumpResponse's existing fallback for file-mode redirects, upstream
        // of this fork) - harmless, since the real fetch below reopens
        // destPath in "wb" mode and overwrites it. We re-resolve the target
        // via a small buffer-mode request instead of trying to read it back
        // out of the file, then download the real (possibly large) file body
        // to destPath for real.
        char redirectBuffer[2048] = "";
        unsigned long long redirectSize = sizeof(redirectBuffer) - 1;
        httpRequestHTTPS(url, HTTP_GET, NULL, authHeader, "",
                        redirectBuffer, &redirectSize, false, NULL, 0, printFunction);

        // progressFn goes on this real body fetch, but deliberately NOT on the
        // small buffer-mode redirect-resolution request above - that one is a
        // couple of KB of header, and reporting its progress would briefly
        // drive the bar with numbers that have nothing to do with the file.
        httpStatus = httpRequestHTTPS(std::string(redirectBuffer), HTTP_GET, NULL,
                                      authHeader, destPath,
                                      NULL, NULL, true, NULL, 0, printFunction, member.unpSize, progressFn);
    }

    NoteAuthStatus(httpStatus, authHeader);

    if (httpStatus != 200)
    {
        printFunction("ERROR: failed to download DLC member (status %d)\n", httpStatus);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Title updates - a separate archive.org item, see ArchiveOrgDLC.h
// ---------------------------------------------------------------------------

#define TITLE_UPDATE_ITEM "microsoft_xbox360_title-updates"
#define TITLE_UPDATE_METADATA_URL "https://archive.org/metadata/" TITLE_UPDATE_ITEM
#define TITLE_UPDATE_DOWNLOAD_BASE "https://archive.org/download/" TITLE_UPDATE_ITEM "/"

// Pulls version and region out of a title-update filename.
//
// Names carry a variable number of parenthesised groups, so neither the
// version nor the region sits at a fixed position. Two real examples:
//
//   Sonic Generations (World) (v1).zip
//   Elder Scrolls V - Skyrim, The (Europe, USA) (En) (v10) (34F5B6C3) (Title Update).zip
//
// An earlier version of this read the LAST group as the version and the one
// before it as the region, which works for the first name and is badly wrong
// for the second: it reported version 0 and a region of "Title Update",
// because the trailing groups there are a language, a checksum and a label.
// Version 0 for everything also quietly broke the picker's sort, which orders
// by version so the newest update lands under the cursor.
//
// So: scan every group. The one that looks exactly like "(v<digits>)" is the
// version wherever it appears, and the FIRST group is the region - it is the
// one position that has held the region in every name observed.
static void ParseTitleUpdateName(const char *filename, int *outVersion, char *outRegion, size_t regionSize)
{
    *outVersion = 0;
    if (regionSize > 0)
        outRegion[0] = '\0';

    const char *firstOpen = NULL;

    for (const char *p = filename; *p != '\0'; ++p)
    {
        if (*p != '(')
            continue;

        if (firstOpen == NULL)
            firstOpen = p;

        const char *v = p + 1;
        if (*v != 'v' && *v != 'V')
            continue;

        int value = 0;
        const char *d = v + 1;
        bool anyDigits = false;

        while (*d >= '0' && *d <= '9')
        {
            value = value * 10 + (*d - '0');
            anyDigits = true;
            ++d;
        }

        // Must be the WHOLE group - "(v10)" is a version, "(various)" is not.
        if (anyDigits && *d == ')')
            *outVersion = value;
    }

    if (firstOpen == NULL || regionSize == 0)
        return;

    size_t o = 0;
    for (const char *r = firstOpen + 1; *r != '\0' && *r != ')' && o + 1 < regionSize; ++r)
        outRegion[o++] = *r;

    outRegion[o] = '\0';
}

int FindTitleUpdates(const std::string &gameName, TitleUpdateMatch *outMatches, int maxMatches,
                     void printFunction(const char *_format, ...))
{
    // Larger than the DLC item's buffer: this listing runs to a thousand-plus
    // entries where the DLC item is a few hundred.
    const unsigned long long METADATA_BUFFER_SIZE = 4 * 1024 * 1024;
    char *buffer = (char *)malloc(METADATA_BUFFER_SIZE);
    if (buffer == NULL)
    {
        ERROR_LOG("malloc failed for title-update metadata buffer");
        return -1;
    }

    unsigned long long bufferSize = METADATA_BUFFER_SIZE - 1;

    // Metadata is public even though the files themselves are private, so no
    // auth header is needed for this request - same as the DLC item.
    int status = HttpGetFollowRedirect(TITLE_UPDATE_METADATA_URL, buffer, &bufferSize, NULL, NULL, printFunction);

    if (status != 200)
    {
        printFunction("ERROR: could not fetch title-update metadata (status %d)\n", status);
        free(buffer);
        return -1;
    }

    cJSON *json = cJSON_Parse(buffer);
    free(buffer);

    if (json == NULL)
    {
        ERROR_LOG("failed to parse title-update metadata JSON");
        return -1;
    }

    cJSON *files = cJSON_GetObjectItemCaseSensitive(json, "files");
    if (!cJSON_IsArray(files))
    {
        ERROR_LOG("title-update metadata had no files array");
        cJSON_Delete(json);
        return -1;
    }

    char normalizedQuery[512];
    NormalizeForMatch(gameName.c_str(), normalizedQuery, sizeof(normalizedQuery));

    // A multi-disc title carries its disc in the display name; the archive
    // never does. See StripDiscSuffix.
    size_t beforeStrip = strlen(normalizedQuery);
    StripDiscSuffix(normalizedQuery);

    if (strlen(normalizedQuery) != beforeStrip)
        printFunction("  Matching on \"%s\" (disc marker trimmed from the title)\n", normalizedQuery);

    int matchCount = 0;
    cJSON *file;

    cJSON_ArrayForEach(file, files)
    {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(file, "name");
        if (!cJSON_IsString(name) || name->valuestring == NULL)
            continue;

        size_t nameLen = strlen(name->valuestring);
        if (nameLen < 4 || _stricmp(name->valuestring + nameLen - 4, ".zip") != 0)
            continue;

        char normalizedName[512];
        NormalizeForMatch(name->valuestring, normalizedName, sizeof(normalizedName));

        // Same scorer as the DLC side. It behaves better here, if anything:
        // these are ordinary game names rather than dot-separated scene
        // releases, so more of the query tends to land as whole words.
        int score = ScoreDlcMatch(normalizedName, normalizedQuery);
        if (score < DLC_MATCH_MIN_SCORE)
            continue;

        cJSON *size = cJSON_GetObjectItemCaseSensitive(file, "size");
        unsigned long long fileSize = 0;
        if (cJSON_IsString(size) && size->valuestring != NULL)
            fileSize = _atoi64(size->valuestring);
        else if (cJSON_IsNumber(size))
            fileSize = (unsigned long long)size->valuedouble;

        // Keep the best maxMatches rather than the first maxMatches - see the
        // same reasoning in FindDlcRarFilenames.
        int slot = -1;

        if (matchCount < maxMatches)
        {
            slot = matchCount++;
        }
        else
        {
            int worst = 0;
            for (int i = 1; i < matchCount; ++i)
            {
                if (outMatches[i].score < outMatches[worst].score)
                    worst = i;
            }

            if (score > outMatches[worst].score)
                slot = worst;
        }

        if (slot < 0)
            continue;

        strncpy(outMatches[slot].filename, name->valuestring, TITLE_UPDATE_FILENAME_LEN - 1);
        outMatches[slot].filename[TITLE_UPDATE_FILENAME_LEN - 1] = '\0';
        outMatches[slot].size = fileSize;
        outMatches[slot].score = score;

        ParseTitleUpdateName(outMatches[slot].filename,
                             &outMatches[slot].version,
                             outMatches[slot].region,
                             sizeof(outMatches[slot].region));
    }

    cJSON_Delete(json);

    // Best first: score descending, then version descending so the newest
    // update for the best-matching name is the one the picker lands on.
    for (int i = 1; i < matchCount; ++i)
    {
        TitleUpdateMatch key = outMatches[i];
        int j = i - 1;

        while (j >= 0 && (outMatches[j].score < key.score ||
                          (outMatches[j].score == key.score && outMatches[j].version < key.version)))
        {
            outMatches[j + 1] = outMatches[j];
            --j;
        }

        outMatches[j + 1] = key;
    }

    return matchCount;
}

// ---------------------------------------------------------------------------
// Zip central-directory probe (diagnostic - see ArchiveOrgDLC.h)
// ---------------------------------------------------------------------------

#define ZIP_EOCD_SIGNATURE 0x06054B50UL
#define ZIP_CDFH_SIGNATURE 0x02014B50UL

bool ReadTitleUpdateMember(const TitleUpdateMatch &update, const char *authHeader,
                           char *outMemberName, unsigned long outMemberNameSize,
                           unsigned long *outUnpackedSize,
                           void printFunction(const char *_format, ...))
{
    if (outMemberName == NULL || outMemberNameSize == 0)
        return false;

    outMemberName[0] = '\0';
    if (outUnpackedSize != NULL)
        *outUnpackedSize = 0;

    // Unlike the DLC filenames (dot-separated scene releases with no spaces,
    // which the existing code drops straight into a URL), title-update names
    // contain spaces and parentheses - "Ace Combat 6 (Europe) (v2).zip" - so
    // the filename itself has to be percent-encoded before it can go in a URL
    // at all. UrlEncodeFormValue encodes a space as %20 rather than '+', which
    // is what makes it usable for a path segment here.
    char encodedZip[512];
    if (!UrlEncodeFormValue(update.filename, encodedZip, sizeof(encodedZip)))
    {
        ERROR_LOG("title-update filename too long to URL-encode");
        return false;
    }

    std::string url = TITLE_UPDATE_DOWNLOAD_BASE + std::string(encodedZip);

    if (update.size < 22)
    {
        ERROR_LOG("title-update zip is too small to contain a central directory");
        return false;
    }

    std::string cachedDatanodeUrl;

    // --- Step 1: the End of Central Directory record, at the tail of the file ---
    //
    // This is what makes zip easier than the RAR walk: one request describes
    // the whole archive, instead of chaining through interleaved headers.
    const unsigned long TAIL_SIZE = 1024;
    unsigned char tail[TAIL_SIZE];

    unsigned long long tailLen = TAIL_SIZE - 1; // one byte held back, matching the RAR path's hard-won buffer rule
    unsigned long long tailStart = (update.size > tailLen) ? (update.size - tailLen) : 0;
    unsigned long long tailEnd = update.size - 1;

    char rangeHeader[64];
    _snprintf(rangeHeader, sizeof(rangeHeader), "Range: bytes=%I64u-%I64u\r\n", tailStart, tailEnd);
    rangeHeader[sizeof(rangeHeader) - 1] = '\0';

    int status = HttpGetFollowRedirect(url, (char *)tail, &tailLen, rangeHeader, authHeader,
                                       printFunction, &cachedDatanodeUrl);

    if (status != 206 && status != 200)
    {
        printFunction("ERROR: range request for zip tail failed (status %d)\n", status);
        return false;
    }

    // Scan backwards for the EOCD signature. Backwards because a zip comment,
    // if present, sits after it - and because a stray matching byte pattern is
    // likelier earlier in the data than at the true record.
    long eocd = -1;
    for (long i = (long)tailLen - 22; i >= 0; --i)
    {
        if (ReadLE32(tail + i) == ZIP_EOCD_SIGNATURE)
        {
            eocd = i;
            break;
        }
    }

    if (eocd < 0)
    {
        ERROR_LOG("no zip End of Central Directory record found in the file tail");
        return false;
    }

    unsigned short entryCount = ReadLE16(tail + eocd + 10);
    unsigned long cdSize = ReadLE32(tail + eocd + 12);
    unsigned long cdOffset = ReadLE32(tail + eocd + 16);

    printFunction("[TU] %s\n", update.filename);
    printFunction("[TU]   entries=%u, central dir at %lu (%lu bytes)\n",
                  (unsigned)entryCount, cdOffset, cdSize);

    if (entryCount == 0 || cdSize == 0)
    {
        ERROR_LOG("zip central directory is empty");
        return false;
    }

    // --- Step 2: the central directory itself ---
    //
    // Usually it is ALREADY in the tail buffer above and needs no second
    // request at all. The central directory sits immediately before the EOCD
    // record, so for a small archive - and these hold exactly one member - it
    // falls comfortably inside the last kilobyte we just fetched. Confirmed on
    // hardware: a 3084417-byte zip put its 93-byte central directory at
    // 3084280, well within the tail read starting at 3083394.
    //
    // Asking for it again was not just wasteful, it failed: the second range
    // request against the cached datanode URL came back 500, and the fallback
    // then died on a buffer that was sized to the 93 bytes wanted rather than
    // to the redirect URL the fallback had to store in it.
    const unsigned long CD_BUFFER = 2048;
    unsigned char cdFetch[CD_BUFFER];

    const unsigned char *cd = NULL;
    unsigned long long cdAvail = 0;

    if (cdOffset >= tailStart && cdOffset < update.size)
    {
        // Narrowed to unsigned long before the pointer arithmetic: the offset
        // is bounded by the tail size (a kilobyte) by the test above, and this
        // is a 32-bit target, so there is no reason to do pointer maths in 64
        // bits here.
        unsigned long into = (unsigned long)(cdOffset - tailStart);

        if (into >= tailLen)
        {
            ERROR_LOG("zip central directory offset landed outside the fetched tail");
            return false;
        }

        cd = tail + into;
        cdAvail = tailLen - into;
    }
    else
    {
        // Only for an archive whose central directory sits further back than
        // the tail we read - not expected for these, but cheap to support.
        //
        // The size passed in is the BUFFER CAPACITY, not the number of bytes
        // wanted: HttpGetFollowRedirect stores a 302's redirect URL in this
        // same buffer before following it, so sizing it to the payload is what
        // produces "Redirect URL buffer too small". The Range header is what
        // limits how much is actually requested.
        unsigned long long wanted = (cdSize < CD_BUFFER - 1) ? cdSize : (CD_BUFFER - 1);
        unsigned long long cdEnd = cdOffset + wanted - 1;

        _snprintf(rangeHeader, sizeof(rangeHeader), "Range: bytes=%lu-%I64u\r\n", cdOffset, cdEnd);
        rangeHeader[sizeof(rangeHeader) - 1] = '\0';

        unsigned long long cdLen = CD_BUFFER - 1;

        status = HttpGetFollowRedirect(url, (char *)cdFetch, &cdLen, rangeHeader, authHeader,
                                       printFunction, &cachedDatanodeUrl);

        if (status != 206 && status != 200)
        {
            printFunction("ERROR: range request for zip central directory failed (status %d)\n", status);
            return false;
        }

        cd = cdFetch;
        cdAvail = cdLen;
    }

    if (cdAvail < 46 || ReadLE32(cd) != ZIP_CDFH_SIGNATURE)
    {
        ERROR_LOG("zip central directory header signature missing");
        return false;
    }

    unsigned short method = ReadLE16(cd + 10);
    unsigned long compressedSize = ReadLE32(cd + 20);
    unsigned long uncompressedSize = ReadLE32(cd + 24);
    unsigned short nameLen = ReadLE16(cd + 28);

    char memberName[512];
    unsigned short copyLen = nameLen;
    if (copyLen > sizeof(memberName) - 1)
        copyLen = sizeof(memberName) - 1;
    if (46 + copyLen > cdAvail)
        copyLen = (unsigned short)(cdAvail - 46);

    memcpy(memberName, cd + 46, copyLen);
    memberName[copyLen] = '\0';

    // The member name is what decides where this installs: a lowercase "tu..."
    // name belongs in Content\0000000000000000\{TitleID}\000B0000\, while an
    // uppercase "TU_..." name goes to Cache instead. Both forms are in this
    // archive - which one you get depends on the update's vintage, not on
    // anything we control - so the caller has to branch per file.
    printFunction("[TU]   member: \"%s\"\n", memberName);
    printFunction("[TU]   method=%u (0=stored, 8=deflate), packed=%lu, unpacked=%lu\n",
                  (unsigned)method, compressedSize, uncompressedSize);

    strncpy(outMemberName, memberName, outMemberNameSize - 1);
    outMemberName[outMemberNameSize - 1] = '\0';

    if (outUnpackedSize != NULL)
        *outUnpackedSize = uncompressedSize;

    return true;
}

// ---------------------------------------------------------------------------
// Install one title update
// ---------------------------------------------------------------------------

// Everything before the last '/' or '\\' in a zip member name is folder
// structure inside the archive (observed: "6BD87620/TU_19KA228_..."), and none
// of it belongs in the destination - only the file itself does.
static const char *MemberBaseName(const char *memberName)
{
    const char *base = memberName;

    for (const char *p = memberName; *p != '\0'; ++p)
    {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }

    return base;
}

// The console's Cache folder, derived from the configured content path rather
// than hardcoded to Hdd1: someone who installs content to a USB drive has
// their cache on that same device, and a hardcoded Hdd1 would quietly write to
// the wrong one.
static std::string CachePathFromContentPath(const std::string &contentBasePath)
{
    size_t firstSep = contentBasePath.find('\\');

    std::string device = (firstSep != std::string::npos)
                             ? contentBasePath.substr(0, firstSep)
                             : contentBasePath;

    return device + "\\Cache";
}

bool DownloadTitleUpdate(const TitleUpdateMatch &update, unsigned long titleId,
                         const std::string &contentBasePath, const char *authHeader,
                         void printFunction(const char *_format, ...),
                         DownloadProgressFn progressFn)
{
    g_keysRejected = false;

    char memberName[512];
    unsigned long unpackedSize = 0;

    if (!ReadTitleUpdateMember(update, authHeader, memberName, sizeof(memberName),
                               &unpackedSize, printFunction))
    {
        return false;
    }

    const char *baseName = MemberBaseName(memberName);
    if (baseName[0] == '\0')
    {
        ERROR_LOG("title update member has no filename");
        return false;
    }

    // Case decides the destination, and it is genuinely the case rather than a
    // naming style: lowercase "tu..." updates are the newer per-title form and
    // live under the game's own content folder, while the older uppercase
    // "TU_..." ones go in the shared Cache. Getting this wrong installs a real
    // file to a place nothing will ever look.
    std::string destPath;

    if (baseName[0] == 't')
    {
        char titleFolder[64];
        _snprintf(titleFolder, sizeof(titleFolder), "\\%08lX\\000B0000\\", titleId);
        titleFolder[sizeof(titleFolder) - 1] = '\0';

        destPath = contentBasePath + titleFolder + baseName;
    }
    else
    {
        destPath = CachePathFromContentPath(contentBasePath) + "\\" + baseName;
    }

    printFunction("Installing title update: %s -> %s\n", baseName, destPath.c_str());

    // Create the destination folders. CreateDirectory works one level at a
    // time here, same as the DLC path does it.
    size_t lastSep = destPath.find_last_of('\\');
    if (lastSep != std::string::npos)
    {
        std::string dir = destPath.substr(0, lastSep);
        size_t at = dir.find('\\');

        while (at != std::string::npos)
        {
            CreateDirectoryA(dir.substr(0, at).c_str(), NULL); // ok if it already exists
            at = dir.find('\\', at + 1);
        }

        CreateDirectoryA(dir.c_str(), NULL);
    }

    // Same virtual-path trick the DLC side uses: archive.org extracts the
    // member server-side, so nothing here has to understand deflate. If that
    // turns out not to work for zip the request simply fails, and the caller
    // reports it rather than writing a corrupt file.
    char encodedZip[512];
    char encodedMember[1024];

    if (!UrlEncodeFormValue(update.filename, encodedZip, sizeof(encodedZip)) ||
        !UrlEncodeFormValue(memberName, encodedMember, sizeof(encodedMember)))
    {
        ERROR_LOG("title update name too long to URL-encode");
        return false;
    }

    std::string url = TITLE_UPDATE_DOWNLOAD_BASE + std::string(encodedZip) + "/" + encodedMember;

    int httpStatus = httpRequestHTTPS(url, HTTP_GET, NULL, authHeader, destPath,
                                      NULL, NULL, true, NULL, 0, printFunction,
                                      unpackedSize, progressFn);

    if (httpStatus == 302)
    {
        // File-mode requests do NOT follow redirects - httpRequestHTTPS writes
        // the redirect URL into the destination as a throwaway line and hands
        // back 302 for the caller to deal with. DownloadDlcMember has had this
        // same block from the start; leaving it out here is what made a
        // perfectly good title update report failure on a 302 that simply
        // hadn't been followed yet.
        //
        // Re-resolve through a small buffer-mode request rather than reading
        // the URL back out of the file, then fetch the real body. The throwaway
        // line is harmless - this reopens destPath in "wb" and overwrites it.
        char redirectBuffer[2048] = "";
        unsigned long long redirectSize = sizeof(redirectBuffer) - 1;
        httpRequestHTTPS(url, HTTP_GET, NULL, authHeader, "",
                         redirectBuffer, &redirectSize, false, NULL, 0, printFunction);

        // progressFn goes on the real body fetch only, not on the redirect
        // resolution above - that one is a couple of KB of header and would
        // briefly drive the progress bar with unrelated numbers.
        httpStatus = httpRequestHTTPS(std::string(redirectBuffer), HTTP_GET, NULL,
                                      authHeader, destPath,
                                      NULL, NULL, true, NULL, 0, printFunction,
                                      unpackedSize, progressFn);
    }

    NoteAuthStatus(httpStatus, authHeader);

    if (httpStatus != 200 && httpStatus != 206)
    {
        printFunction("ERROR: title update download failed (status %d)\n", httpStatus);
        printFunction("       If this is a 404, archive.org is not serving zip members\n");
        printFunction("       pre-extracted, which would need inflate support here.\n");
        return false;
    }

    return true;
}
