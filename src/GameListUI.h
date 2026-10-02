#ifndef GAME_LIST_UI_H
#define GAME_LIST_UI_H

#include "StfsParser.h"

// Sets up the icon/quad-rendering shader + vertex declaration + a 1x1 white
// texture (for solid-color rects), and this UI's own Font instance (Console
// keeps its own privately, so we can't reuse it - see the fork's plan notes
// on why a from-scratch render loop was needed here). Call once after
// MakeConsole(...) has run, before any Render*/Show* call below.
bool InitGameListUI();
void ShutdownGameListUI();

// ---------------------------------------------------------------------------
// The shell: a sidebar of pages, with the current page beside it
// ---------------------------------------------------------------------------
//
// main.cpp runs one loop that reads input, updates whichever page is showing
// and draws one frame with the Render*Frame calls below - so this file only
// draws, and never decides what a button does. The blocking screens further
// down (progress, messages, confirmations) are still their own loops for now;
// they draw the sidebar too, so the app reads as one place throughout.

// In the order the sidebar lists them. Library and Store are one group,
// Queue and Settings the other, with a divider between.
enum ShellPage
{
    SHELL_PAGE_LIBRARY,
    SHELL_PAGE_STORE,
    SHELL_PAGE_QUEUE,
    SHELL_PAGE_SETTINGS,
    SHELL_PAGE_COUNT
};

struct ShellSidebar
{
    ShellPage page;       // the highlighted page
    bool focused;         // the sidebar has focus, rather than the page
    int libraryCount;     // shown beside Your Library
    int queueCount;       // shown beside Queue; 0 shows nothing

    // The drive content installs to, at the foot of the sidebar: a ring for
    // how full it is, its name, and the free space in words.
    float storageUsed;      // 0..1; negative hides the whole thing
    char storageLabel[16];  // "Hdd1"
    char storageDetail[32]; // "402 GB free"
    char storageTotal[32];  // "of 931 GB"
};

// What every following frame draws in the sidebar, until it is called again.
// Kept here rather than passed to each Render call so the blocking screens,
// which main.cpp calls from deep inside a download, draw the same sidebar
// without every caller having to carry it through. They always draw it
// unfocused - while one of them is up, nothing in the sidebar can be chosen.
void SetShellSidebar(const ShellSidebar &sidebar);

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

// One frame's controller state, from the first controller.
//
//   held    - buttons down right now
//   pressed - buttons that went down this frame (edge-triggered)
//   nav     - at most ONE of the D-pad direction bits, set on the frame a
//             direction is first pushed and then repeatedly while it's held,
//             so holding down scrolls a long list. The left stick counts as
//             the D-pad here, whichever axis it's pushed further along.
struct UiInput
{
    WORD held;
    WORD pressed;
    WORD nav;
};

// Call once per frame.
UiInput PollUiInput();

// Call after anything that took over the screen for a while - a keyboard, a
// download, a message. Whatever is held at that moment is treated as already
// handled, so the A that answered a message can't also act on the page
// underneath, and a held direction doesn't jump the moment control returns.
void ResyncUiInput();

// ---------------------------------------------------------------------------
// Footer hints
// ---------------------------------------------------------------------------

enum UiButton
{
    UI_BUTTON_A,
    UI_BUTTON_B,
    UI_BUTTON_X,
    UI_BUTTON_Y,
    UI_BUTTON_LBRB,
    UI_BUTTON_START
};

// One button hint in the footer. shortLabel, when non-NULL, is used for every
// hint if the full labels don't fit the width of the page - which happens on
// a 640x480 screen, where the page beside the sidebar is narrow.
struct UiHint
{
    UiButton button;
    const WCHAR *label;
    const WCHAR *shortLabel;
};

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------

// What the disc's tile shows.
enum LibraryDiscTile
{
    DISC_TILE_READY,       // a game disc, not installed - A installs it
    DISC_TILE_INSTALLED,   // already in the games folder
    DISC_TILE_INSTALLING,  // on the Queue page now
    DISC_TILE_READING,     // just gone in
    DISC_TILE_UNREADABLE   // not a disc that can be installed
};

// The installed library.
//
// scroll is the first visible row. It's the caller's to keep between frames,
// and the Render call moves it so the selected row stays on screen. Pass -1
// to have it start with the selection in the middle of the view.
//
// hasDlcInstalled and hasUpdateInstalled, when non-NULL, are caller-owned
// arrays of `count` flags marking which titles already have DLC, and which
// already have a title update, on the console. Those rows get an "INSTALLED"
// marker naming whichever is present. The caller works this out rather than
// this screen doing it, because it is a question about content paths on disk
// and this file has no business knowing where content lives.
//
// count may be 0. The page then says the library is empty and names gamesPath
// as the folder that was searched.
//
// bannerText, when non-NULL, is shown in a highlighted strip above the list
// with a Y badge - for something the user needs to do in Settings, like adding
// their archive.org keys.
//
// Cover art is loaded on the first frame and kept until ShutdownGameListUI or
// ReleaseGameListIcons, so drawing this every frame costs no decoding.
struct LibraryPageView
{
    const InstalledGame *games;
    int count;
    int selected;
    int scroll;
    const bool *hasDlcInstalled;
    const bool *hasUpdateInstalled;
    const char *gamesPath;
    const char *bannerText;
    bool focused; // false while the sidebar has focus

    // The disc in the drive, as the first tile, when hasDisc. selected and
    // scroll count it: game i is then item i + 1.
    bool hasDisc;
    LibraryDiscTile discTile;
    const char *discName;      // the game's, or what's wrong with the disc
    unsigned long discTitleId; // for its box art; 0 for none
    float discProgress;        // DISC_TILE_INSTALLING: 0..1, negative when unknown
};

// The library is a grid of cover tiles. How many full rows fit - LB/RB page
// by this many rows - and how many tiles there are across. scroll counts
// rows, not games.
int LibraryPageVisibleRows(const LibraryPageView &view);
int LibraryGridColumns();

void RenderLibraryFrame(LibraryPageView &view, const UiHint *hints, int hintCount);

// Turns at most one cover that CoverArt.cpp has ready into a texture for the
// tiles - decoding and cutting it first if it was just downloaded. Call once
// per frame; a download costs a few tens of milliseconds the first time, a
// cached cover next to nothing.
void PumpCoverArt();

// Drops the cached cover art, so the next library frame loads it afresh. Call
// after rescanning the library: the cache is matched to the game list by count
// alone, so a new library of the same size would otherwise show the old one's
// covers.
void ReleaseGameListIcons();

// A list of text rows with an optional second line each: Settings, and the
// DLC pack and title update pickers.
//
// labels and sublabels are caller-owned arrays of `count` strings; sublabels
// may be NULL entirely, or hold NULL for an individual row. scroll works as it
// does for LibraryPageView.
//
// showCounter controls the "3 / 12" position readout, which means something
// for a list of results and nothing for a short fixed menu.
struct ListPageView
{
    const char *heading;
    const char *subheading; // the line under the title; may be NULL
    const char **labels;
    const char **sublabels;
    int count;
    int selected;
    int scroll;
    bool focused;
    bool showCounter;
};

void RenderListFrame(ListPageView &view, const UiHint *hints, int hintCount);

// The Store's front page: three featured games - one large tile, two small -
// with their marketplace wallpaper, a row of four buttons, and the A-Z tiles.
//
// focus runs through them in that order: STORE_FOCUS_FEATURED + 0..2, then
// STORE_FOCUS_BUTTONS + 0..3, then STORE_FOCUS_LETTERS + 0..26. The page
// scrolls so the A-Z tiles come into view when one of them has focus; scroll
// is the caller's to keep between frames, and the Render call eases it.
#define STORE_FEATURED_COUNT 3
#define STORE_BUTTON_COUNT   4
#define STORE_LETTER_COUNT   27 // '#', then A to Z

#define STORE_FOCUS_FEATURED 0
#define STORE_FOCUS_BUTTONS  (STORE_FOCUS_FEATURED + STORE_FEATURED_COUNT)
#define STORE_FOCUS_LETTERS  (STORE_FOCUS_BUTTONS + STORE_BUTTON_COUNT)
#define STORE_FOCUS_COUNT    (STORE_FOCUS_LETTERS + STORE_LETTER_COUNT)

struct StoreFeaturedView
{
    unsigned long titleId; // for its wallpaper
    const char *name;
    const char *detail;    // under the name on the large tile; may be NULL
};

struct StoreButtonView
{
    const char *label;
    bool disabled;         // drawn faded, marked SOON
};

struct StorePageView
{
    StoreFeaturedView featured[STORE_FEATURED_COUNT];
    StoreButtonView buttons[STORE_BUTTON_COUNT];
    const char *letters;   // STORE_LETTER_COUNT characters
    int focus;
    float scroll;          // pixels the page has moved up; starts at 0
    bool focused;
};

// How many A-Z tiles fit across: they wrap onto as many rows as it takes.
int StoreLettersPerRow();

void RenderStoreFrame(StorePageView &view, const UiHint *hints, int hintCount);

// A section of the Store that is only its A-Z tiles - Xbox Live Arcade's -
// laid out as the front page's are. focus is the letter, 0 to
// STORE_LETTER_COUNT - 1.
struct StoreSectionView
{
    const char *title;    // the header's
    const char *subtitle;
    const char *letters;  // STORE_LETTER_COUNT characters
    const int *counts;    // the games under each; a letter with none is drawn faded
    int focus;
    bool focused;
};

void RenderStoreSectionFrame(StoreSectionView &view, const UiHint *hints, int hintCount);

// Turns at most one image StoreArt.cpp has ready into a texture. Call once
// per frame, as PumpCoverArt.
void PumpStoreArt();

// One letter's games: a grid of cover tiles like the library's, each with
// its regions in the corner. The covers on screen and the row after are
// asked for as they come into view, and only those are kept.
struct StoreTileView
{
    unsigned long titleId;  // for its cover; 0 for none
    const char *name;
    unsigned short regions; // STORE_REGION_*
};

struct StoreLetterView
{
    char letter;
    const char *section; // before the count in the header - "Xbox Live Arcade"; NULL for none
    const StoreTileView *tiles;
    int count;
    int selected;
    int scroll;  // rows, as LibraryPageView's
    bool focused;
};

// Full rows on screen - LB/RB page by this many.
int StoreLetterVisibleRows();

void RenderStoreLetterFrame(StoreLetterView &view, const UiHint *hints, int hintCount);

// A game's page: its wallpaper across the top, the box, the catalog's
// details, its buttons, the synopsis, its versions and screenshots.
//
// focus: STORE_GAME_FOCUS_BUTTONS + 0..3 is a button, STORE_GAME_FOCUS_VERSIONS
// + i is version i. versionScroll is the first version row shown; the Render
// call keeps the focused one on screen. The buttons are Install, Find DLC,
// Title updates and Uninstall; a NULL one isn't shown, and only the last may
// be - Uninstall, while there's nothing installed to remove.
#define STORE_GAME_BUTTONS        4
#define STORE_GAME_FOCUS_BUTTONS  0
#define STORE_GAME_FOCUS_VERSIONS STORE_GAME_BUTTONS

struct StoreVersionView
{
    const char *label;  // "USA, Europe  ·  Disc 2"
    const char *detail; // "En, Fr, De  ·  Rev 1"; may be empty
    const char *size;   // "6.2 GB"
};

// Whether the game on a game page can be installed, or is on its way or
// already there.
enum StoreInstallState
{
    STORE_INSTALL_AVAILABLE,  // green - A installs the chosen version
    STORE_INSTALL_QUEUED,     // waiting its turn on the Queue page
    STORE_INSTALL_INSTALLING, // the button fills as it goes
    STORE_INSTALL_INSTALLED   // in the library
};

struct StoreGameView
{
    unsigned long titleId;  // for the wallpaper, box and screenshots
    const char *name;
    const char *meta;       // "Bungie Studios  ·  Microsoft  ·  Shooter"; NULL while loading or for none
    const char *players;    // may be NULL
    float rating;           // 0..5; 0 hides the stars
    unsigned long ratings;
    const char *description; // NULL: the page explains there's no catalog entry
    bool loading;           // the details haven't arrived yet
    const int *screenshots; // their numbers, for STORE_ART_SCREEN + n
    int screenshotCount;

    const char *buttons[STORE_GAME_BUTTONS];
    bool buttonDisabled[STORE_GAME_BUTTONS];
    StoreInstallState install; // how the first button, Install, is drawn
    float installFraction;     // while installing: 0..1, negative when unknown

    const StoreVersionView *versions;
    int versionCount;
    int versionChosen;      // the one Install installs
    int versionScroll;
    int focus;
    bool focused;
};

void RenderStoreGameFrame(StoreGameView &view, const UiHint *hints, int hintCount);

// A page with nothing to choose on it - a heading and a line or two of text.
// For Store, which is a placeholder, and Queue while nothing is downloading.
// detailLine may be NULL.
void RenderPlaceholderFrame(const char *heading, const char *message, const char *detailLine,
                            const UiHint *hints, int hintCount);

// The download queue. Each row is one pack or title update, with the cover of
// the game it's for, two lines of text and, while it's waiting or
// downloading, a progress bar.
enum QueueRowTone
{
    QUEUE_ROW_WAITING,
    QUEUE_ROW_ACTIVE,
    QUEUE_ROW_DONE,
    QUEUE_ROW_FAILED
};

struct QueueRowView
{
    const char *title;    // the pack's or update's filename
    const char *gameName;
    const char *status;   // coloured by tone: "Waiting", "File 2 of 5", "Installed - restart your dashboard..."
    const char *numbers;  // right-aligned beside the status while downloading; may be NULL
    float fraction;       // the bar, 0..1; negative for no fill (unknown, or not started)
    bool showBar;
    int libraryIndex;     // whose icon to draw, into the view's games; -1 for none
    unsigned long titleId; // whose box art - found even for a game that isn't in the library
    QueueRowTone tone;
};

// games/gameCount are the library, for the covers - the same array the
// library page draws, so they share its cover cache.
struct QueuePageView
{
    const InstalledGame *games;
    int gameCount;
    const QueueRowView *rows;
    int count;
    int selected;
    int scroll;
    bool focused;
};

void RenderQueueFrame(QueuePageView &view, const UiHint *hints, int hintCount);

// ---------------------------------------------------------------------------
// Popup
// ---------------------------------------------------------------------------

// A short notice at the top right of the page - a download finishing or
// failing, a pack added to the queue - that goes away on its own after a few
// seconds and never takes input, so it can turn up on any page without
// getting in the way. A new one replaces whatever is showing.
//
// Drawn by the shell's pages (the Render*Frame calls above), not by the
// blocking screens, which carry their own messages.
enum UiToastTone
{
    UI_TOAST_INFO,
    UI_TOAST_SUCCESS,
    UI_TOAST_ERROR
};

void ShowShellToast(const char *heading, const char *message, UiToastTone tone);

// ---------------------------------------------------------------------------
// Blocking screens
// ---------------------------------------------------------------------------

// Draws one progress-bar frame. Intended to be called repeatedly from inside
// a download loop - see main.cpp's DlcProgressCallback, which drives it from
// downloadFile.cpp's own read loop via DownloadProgressFn.
//
//   title      - what's being downloaded (the game or pack name)
//   statusLine - which piece, e.g. "file 3 of 12"
//   detailLine - live numbers, e.g. "12.4 MB / 48.0 MB - 1.2 MB/s - 3:21 left".
//                May be NULL/empty when there's nothing to say yet.
//   fraction0to1 - bar fill; clamped internally. Pass a negative value for an
//                indeterminate download (unknown total size), which draws an
//                empty trough rather than a misleading 0%.
//   heading    - the screen heading. "Downloading" for the transfer itself;
//                the steps before it name themselves.
void RenderProgressFrame(const char *title, const char *statusLine,
                         const char *detailLine, float fraction0to1,
                         const char *heading = "Downloading");

// Draws one non-interactive status frame and returns immediately - for the
// blocking phases between screens (scanning the library, talking to
// archive.org, reading a pack's file list), which otherwise leave the last
// screen frozen or fall back to raw console text.
//
// detailLine may be NULL. Call it before starting the slow work, not after.
void RenderStatusFrame(const char *heading, const char *message, const char *detailLine);

// Draws a message and waits for B. For terminal states (nothing found, an
// error, finished) that previously just printed a line and dropped the user
// back to a console screen.
void ShowMessageUI(const char *heading, const char *message, const char *detailLine);

// Draws a title, a few wrapped paragraphs and a line beneath them - for an
// update's release notes. body may hold '\n's; what doesn't fit is cut off.
// With an actionLabel, A does that and returns true; B, or A without one,
// returns false.
bool ShowNotesUI(const char *heading, const char *title, const char *body, const char *footLine,
                 const WCHAR *actionLabel = NULL);

// Draws a message and waits for A (returns true) or B (returns false). For
// actions that can't be undone, like removing the saved archive.org keys.
// confirmLabel names what A does, e.g. "Remove".
bool ShowConfirmUI(const char *heading, const char *message, const char *detailLine,
                   const char *confirmLabel);

#endif
