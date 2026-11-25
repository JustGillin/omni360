#ifndef WALLPAPERS_H
#define WALLPAPERS_H

#include <xtl.h> // DWORD

// ---------------------------------------------------------------------------
// Home wallpapers from xbox.com
// ---------------------------------------------------------------------------
//
// Xbox's wallpaper page (xbox.com/wallpapers) builds its grid from one
// script, a JSON array in all but name:
//
//   https://www.xbox.com/en-US/wallpapers/JS/wallpapers-Sheet.js
//     wallpapers = [ { "title", "category", "preview", "console", ... }, ... ]
//
// each image a path under https://assets.xboxservices.com/assets - a 960x540
// preview and a 1920x1080 "console" size, JPEG mostly, a few PNG. Nothing is
// shipped with Omni360: the list and the previews are fetched when the
// picker opens and kept in game:\Wallpapers, the list for a week.
//
// The one picked is downloaded at its console size and kept as
// game:\Wallpapers\home.jpg (or .png), with its title in home.txt, and Home
// draws it when its background is set to it.
//
// A worker thread does the fetching and the decoding; the UI thread takes
// finished pixels (TakeWallpaperArt) and makes textures of them.

enum WallpaperCategory
{
    WALLPAPER_GAMES,
    WALLPAPER_COMMUNITY,
    WALLPAPER_DEVICES,
    WALLPAPER_EVENTS,
    WALLPAPER_CATEGORY_COUNT
};

struct WallpaperEntry
{
    char title[128];      // UTF-8
    int category;         // WallpaperCategory
    char preview[160];    // paths under assets.xboxservices.com/assets, no query
    char full[160];
};

enum WallpaperListState
{
    WALLPAPER_LIST_NONE,     // not asked for yet
    WALLPAPER_LIST_LOADING,
    WALLPAPER_LIST_READY,
    WALLPAPER_LIST_FAILED    // xbox.com couldn't be reached, and nothing was kept from before
};

enum WallpaperPickState
{
    WALLPAPER_PICK_IDLE,
    WALLPAPER_PICK_WORKING,
    WALLPAPER_PICK_DONE,     // saved; the new Home texture follows through TakeWallpaperArt
    WALLPAPER_PICK_FAILED
};

// The index TakeWallpaperArt gives Home's own wallpaper; previews are 0 up.
#define WALLPAPER_HOME (-1)

bool StartWallpapers();
bool StopWallpapers(DWORD timeoutMs);

// The list: asked for once - later calls do nothing while it's loading or
// there. Entries don't change once it's READY.
void RequestWallpaperList();
WallpaperListState WallpaperListStatus();
int WallpaperCount();
const WallpaperEntry *WallpaperAt(int index);

// A preview, halved to 480x270. The newest asked for is fetched first, so
// the ones on screen win over those scrolled past.
void RequestWallpaperPreview(int index);
bool WallpaperPreviewPending(int index);

// Downloads one at its console size and saves it as Home's.
void RequestWallpaperPick(int index);
WallpaperPickState WallpaperPickStatus();
void ResetWallpaperPick();

// Home's saved wallpaper, decoded to cover 1280x720; it arrives through
// TakeWallpaperArt as WALLPAPER_HOME. Nothing comes if there's none saved.
void RequestHomeWallpaper();
bool HasHomeWallpaper();
// The saved one's title, "" if none.
void HomeWallpaperTitle(char *out, size_t outSize);

struct WallpaperArt
{
    int index;             // WALLPAPER_HOME or a preview's
    unsigned long *pixels; // ARGB, malloc'd; the taker frees it
    int width, height;
};

bool TakeWallpaperArt(WallpaperArt *out);

#endif
