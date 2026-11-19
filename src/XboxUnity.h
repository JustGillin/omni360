#ifndef XBOX_UNITY_H
#define XBOX_UNITY_H

// ---------------------------------------------------------------------------
// Title updates from XboxUnity
// ---------------------------------------------------------------------------
//
// XboxUnity (xboxunity.net) keeps the 360's title updates and what they're
// for - the service Aurora's own Unity menu and its Title Update Downloader
// script use. Everything is plain HTTP with JSON replies:
//
//   Resources/Lib/TitleUpdateInfo.php?titleid=<title ID>
//       every update for the title, grouped by the disc release (media ID)
//       each is for: its TitleUpdateID, version, SHA-1, size in KB, and the
//       game's base version it applies to
//   api/tu/<title ID>/<base version>
//       those for one base version, with each one's filename and the URL
//       to download it from (assets.xboxunity.net/api/gettu/<id>)
//
// Checked working from a PC on 2026-11-19. The version here is the number
// in an update's name - Halo 3's "Title Update 3" is 3 - where the package
// header's own version can be something else (42).

#define MAX_UNITY_UPDATES 32

struct UnityTitleUpdate
{
    char id[16];            // TitleUpdateID
    unsigned long mediaId;  // the release it's for
    unsigned long baseVersion;
    int version;
    char contentId[48];     // XboxUnity's "hash": the package header's content ID at 0x32C, not the file's SHA-1
    unsigned long long size; // bytes (XboxUnity gives KB, so roughly)
    bool forThisCopy;       // its media ID is the installed game's
};

enum UnityResult
{
    UNITY_OK,
    UNITY_NONE,        // XboxUnity has none for the title
    UNITY_UNREACHABLE  // no answer, or not one that could be read
};

// The title's updates, the ones for mediaId (0 for unknown) first and marked,
// newest first within each release.
UnityResult FindUnityTitleUpdates(unsigned long titleId, unsigned long mediaId, UnityTitleUpdate *out, int max,
                                  int *count);

// The filename an update installs under and the URL to fetch it from - from
// the api/tu listing for its base version. False if it isn't listed there.
bool UnityTitleUpdateFile(unsigned long titleId, const UnityTitleUpdate &update, char *fileName,
                          size_t fileNameSize, char *url, size_t urlSize);

// The file's MD5 (hex) and exact size in bytes, from api/tumd5/<id> - what
// a download is checked against. The listing's "hash" is not the file's
// SHA-1 (checked 2026-11-19: Halo 3's 23500 lists 389E4F96..., the file is
// 72824620...), and its size is only to the KB.
bool UnityTitleUpdateMd5(const UnityTitleUpdate &update, char *md5, size_t md5Size, unsigned long long *size);

#endif
