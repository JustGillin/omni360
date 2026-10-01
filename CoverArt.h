#ifndef COVER_ART_H
#define COVER_ART_H

#include <xtl.h> // DWORD

// ---------------------------------------------------------------------------
// Box art for the library, from xboxunity.net, cached on the hard drive
// ---------------------------------------------------------------------------
//
// Each game's tile is the front of its case art beside one fixed banner - the
// green XBOX 360 strip down the left edge, from Media/BoxArtBanner.png - which
// makes a square. The banner is the same on every tile; each cover's own
// header strip changed design over the console's life, so it isn't used. The
// art comes from xboxunity, which needs no account:
//
//   Resources/Lib/CoverInfo.php?titleid=XXXXXXXX
//       a game's covers, as JSON. Each one's CoverID is what the next URL
//       wants; the first one marked Official is used, else the first listed.
//   Resources/Lib/Cover.php?size=large&cid=N
//       the cover itself: the whole case insert - back, spine, front - as a
//       900x600 JPEG of about half a megabyte.
//
// A worker thread looks covers up, downloads them, and reads the ones already
// cached. Cutting the front out of a download happens on the UI thread
// (GameListUI's PumpCoverArt), because it decodes through D3DX, which needs
// the device; the front comes back here to be written to the cache:
//
//   game:\Covers\XXXXXXXX.bin   the front of the case, 418x512 ARGB pixels
//   game:\Covers\XXXXXXXX.none  xboxunity had nothing; asked again after a week
//
// Only the front is cached, and the banner put beside it as each cover
// loads, so a new banner never means downloading the covers again.
//
// So the first launch downloads about half a megabyte a game, and every
// launch after reads only the cache. A game with no cover, or one that
// couldn't be fetched, keeps its icon tile.

#define COVER_SIZE     512                           // the tile's square, in pixels
#define COVER_BANNER_W 94                            // the banner down its left
#define COVER_FRONT_W  (COVER_SIZE - COVER_BANNER_W) // the front of the case, beside it

// False if the thread couldn't be created; covers then never arrive.
bool StartCoverArt();

// Waits up to timeoutMs for a download in progress. See StopDownloadQueue.
bool StopCoverArt(DWORD timeoutMs);

// The games to find covers for, in the order to get them - the library's
// order, so the first screenful arrives first. Replaces any earlier list;
// covers already handed over aren't asked for again.
void RequestCoverArt(const unsigned long *titleIds, int count);

enum CoverDataKind
{
    COVER_DATA_PIXELS, // from the cache: the front, COVER_FRONT_W * COVER_SIZE ARGB pixels
    COVER_DATA_JPEG    // just downloaded: the whole case insert, to be cut down
};

struct CoverData
{
    unsigned long titleId;
    CoverDataKind kind;
    unsigned char *bytes; // malloc'd; the caller frees it
    unsigned long size;
};

// The next cover ready to draw, if there is one. The UI thread calls this.
bool TakeCoverData(CoverData *out);

// Hands back the front cut from a COVER_DATA_JPEG, to be written to the
// cache. Takes ownership of pixels (COVER_FRONT_W * COVER_SIZE, malloc'd).
void SaveCoverPixels(unsigned long titleId, unsigned long *pixels);

// A download that wouldn't decode: noted like a game with no cover, so it
// isn't downloaded again on every launch.
void MarkCoverUnusable(unsigned long titleId);

#endif
