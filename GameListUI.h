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
    char storageText[96]; // e.g. "Hdd1: 120 GB free"; empty hides it
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
};

// How many rows fit - what LB/RB page by.
int LibraryPageVisibleRows(const LibraryPageView &view);

void RenderLibraryFrame(LibraryPageView &view, const UiHint *hints, int hintCount);

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
    const char **labels;
    const char **sublabels;
    int count;
    int selected;
    int scroll;
    bool focused;
    bool showCounter;
};

void RenderListFrame(ListPageView &view, const UiHint *hints, int hintCount);

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
    int libraryIndex;     // whose cover to draw, into the view's games; -1 for none
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
//   heading    - the screen heading. "DOWNLOADING" for the transfer itself;
//                the steps before it name themselves.
void RenderProgressFrame(const char *title, const char *statusLine,
                         const char *detailLine, float fraction0to1,
                         const char *heading = "DOWNLOADING");

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

// Draws a message and waits for A (returns true) or B (returns false). For
// actions that can't be undone, like removing the saved archive.org keys.
// confirmLabel names what A does, e.g. "Remove".
bool ShowConfirmUI(const char *heading, const char *message, const char *detailLine,
                   const char *confirmLabel);

#endif
