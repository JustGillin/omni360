#ifndef STORE_ART_H
#define STORE_ART_H

#include <xtl.h> // DWORD

// ---------------------------------------------------------------------------
// The Store's artwork and details, from Xbox Live, cached on the drive
// ---------------------------------------------------------------------------
//
// The marketplace still serves every game's images by title ID, over plain
// HTTP only:
//
//   http://download.xbox.com/content/images/66acd000-77fe-1000-9115-d802XXXXXXXX/1033/
//       background.jpg   1280x720, the dashboard's wallpaper for the game
//       screenlgN.jpg    1000x562 screenshots, numbered as the catalog lists them
//
// and the catalog its details - synopsis, developer, genre, rating - from
// catalog.xboxlive.com's FindGames query, also over HTTP (HTTPS there needs
// a root the app doesn't carry).
//
// A worker thread fetches what the Store asks for and keeps each file as it
// came, in game:\Store - XXXXXXXX.bg.jpg, XXXXXXXX.s1.jpg, XXXXXXXX.cat.xml.
// Images are decoded into textures on the UI thread (GameListUI's
// PumpStoreArt), as D3DX needs the device; details are parsed here. A game
// the marketplace has nothing for gets a .none marker, asked again after a
// week, like the covers.

// Which image. Screenshots are STORE_ART_SCREEN + the number the catalog
// gives them, 1 up.
typedef int StoreArtKind;
#define STORE_ART_BACKGROUND 0
#define STORE_ART_SCREEN     100
#define STORE_MAX_SCREENSHOTS 4

// False if the thread couldn't be created; art then never arrives.
bool StartStoreArt();

// Waits up to timeoutMs for a download in progress.
bool StopStoreArt(DWORD timeoutMs);

// Asks for one image. Asking again for one handed over already reads it
// again, from the cache - the UI lets images go when it has too many - so
// ask once each time a page opens, not every frame.
void RequestStoreArt(unsigned long titleId, StoreArtKind kind);

struct StoreArtData
{
    unsigned long titleId;
    StoreArtKind kind;
    unsigned char *bytes; // the JPEG, malloc'd; the caller frees it
    unsigned long size;
};

// The next image ready, if there is one. The UI thread calls this.
bool TakeStoreArt(StoreArtData *out);

// An image that wouldn't decode: its cached file is removed, so it's
// fetched again next launch rather than failing every time.
void DiscardStoreArt(unsigned long titleId, StoreArtKind kind);

// A game's catalog entry, as the game page shows it. Strings are UTF-8 and
// may be empty.
struct StoreDetails
{
    unsigned long titleId;
    char developer[64];
    char publisher[64];
    char genre[96];          // "Shooter", or "Action & Adventure, Shooter"
    char description[1200];  // the catalog's short one
    char players[96];        // "1-4 players  ·  online 2-16  ·  system link 2-8"
    float rating;            // 0..5; 0 for none
    unsigned long ratings;   // how many
    int screenshots[STORE_MAX_SCREENSHOTS]; // their numbers, for STORE_ART_SCREEN + n
    int screenshotCount;
};

enum StoreDetailsState
{
    STORE_DETAILS_UNKNOWN,  // not asked for
    STORE_DETAILS_LOADING,
    STORE_DETAILS_READY,
    STORE_DETAILS_NONE      // the catalog has no entry, or couldn't be reached
};

// Asks for a game's details, ahead of any images waiting.
void RequestStoreDetails(unsigned long titleId);

// Where a game's details have got to; out is filled in when they're ready.
StoreDetailsState GetStoreDetails(unsigned long titleId, StoreDetails *out);

// The game's own 64x64 icon, as a PNG, from Xbox Live's title image service
// (image.xboxlive.com, plain HTTP) - for a Games on Demand package's
// thumbnail, which the dashboard and Aurora show. Blocks, so only from a
// worker. False if there's none, it's over capacity, or it isn't a PNG.
bool FetchTitleIcon(unsigned long titleId, unsigned char *out, unsigned long capacity, unsigned long *outSize);

#endif
