# UI Handoff Notes

Written 2026-09-25, handing UI work over to a fresh session/model. This isn't
a replacement for the README or `docs/` - read those too - it's specifically
the stuff that only exists in prior conversation history and real-hardware
trial and error, not yet written down anywhere else.

## Status: what's actually proven working on real hardware

The non-UI pipeline is done and tested end-to-end (install a real game's
DLC from archive.org onto the console):

- IAS3 access-key/secret-key auth (`Authorization: LOW` header) - no login
  flow, no cookies, no CSRF/reCAPTCHA to fight.
- Installed-game enumeration via STFS header parsing (`StfsParser.cpp`) -
  scanned a real 27-game library correctly.
- archive.org metadata lookup + fuzzy DLC-pack name matching
  (`ArchiveOrgDLC.cpp`).
- RAR4 header walk to find each file inside a `.rar` DLC pack, via small
  `Range` requests (no decompression needed - archive.org serves members
  pre-extracted).
- Downloading and writing a real DLC member file into the console's Content
  folder, with a working progress/ETA readout.

The basic list UI (`GameListUI.cpp`) already renders on hardware too - icon
grid, selection highlight, D-pad navigation, per-row text - confirmed
working via staged diagnostic renders (clear -> text -> highlight -> icons).
So this isn't a blank-canvas UI task; it's extending something that already
draws correctly.

**Open item from the original design plan, never resolved:** whether
`GameListUI.cpp`'s render path is a real per-frame `Present()` loop, or
closer to one-shot blocking calls. Given it already renders an interactive,
navigable grid, this is probably moot - but confirm by reading the render
loop before assuming either way, especially before adding anything
animated (a progress bar, transitions, etc.).

## Hardware landmines (all found the hard way, via real-device trial and error)

- **D3D9 with `D3DRS_VIEWPORTENABLE=FALSE`** (used here) requires **raw
  pixel coordinates** out of the vertex shader, not normalized -1..1
  device coordinates. Easy to get this wrong if copying patterns from
  typical D3D9 code.
- **`Console::Add(WCHAR)` treats a bare `\r` as a real carriage return** -
  it wipes the current line back to blank. Any text containing `\r\n`
  passed through in one `%s` substitution silently vanishes with no error.
  If a print "does nothing," check this before assuming a logic bug.
- **Character-by-character console rendering is genuinely expensive** -
  every character goes through `MultiByteToWideChar` plus a font
  glyph-width measurement. This measurably throttled download throughput
  this session; it'll matter even more for anything UI/animation-related
  that touches text every frame.
- **`_snprintf`/`vsnprintf_s` on this XDK toolchain are not fully
  trustworthy**: `_snprintf` doesn't null-terminate on truncation, and the
  `%.*s` dynamic-precision specifier caused a real, very hard to diagnose
  crash (empty/unwalkable call stack in the VS debugger). Prefer fixed
  format specifiers or manual bounds-checked copies over dynamic
  width/precision.
- **Never print raw/binary response bytes through the console renderer** -
  confirmed crash risk, already bit us once.
- **The VS debugger's call stack can go completely empty on a real crash.**
  When that happens, don't trust stepping through the debugger - add
  temporary bisection print markers, narrow it down from the log, then
  remove them once resolved (see git history / prior session for the
  pattern used).
- **`XShowKeyboardUI` fails if called immediately after a previous keyboard
  instance closes** - needs a retry-with-backoff (already handled in
  `Keyboard.cpp`, just noting it exists in case similar timing issues show
  up elsewhere).

## What's actually left for UI

- A real progress bar / percentage display during DLC downloads - the
  data's there now (`downloadFile.cpp`'s chunked-download progress reporting
  gives % complete and ETA), it's just text-only in the debug console
  today, not a drawn widget.
- General UI polish/expansion beyond the current bare-bones list+grid.
- Broader testing across the full 27-game library and multi-pack DLC cases
  once UI work is far enough along to test through it.

## Where to look first

- `README.md` - setup, requirements, testing order.
- `docs/` - architecture, HTTP/download internals.
- `.claude/plans/wise-wishing-avalanche.md` - original design plan (some
  items in it are now done; treat as historical context, not a live task
  list).
- Inline comments in `GameListUI.cpp`, `ArchiveOrgDLC.cpp`,
  `downloadFile.cpp`, `XboxTLS.cpp` - deliberately left detailed wherever a
  fix wasn't obvious from the code alone, explaining the *why*, not just
  the *what*.

## Testing workflow reminder

No emulator - everything is tested on real JTAG/RGH hardware via VS2010 +
Xbox 360 XDK, so build/deploy/test cycles are slow. When a debug log is too
long to paste directly, it gets saved to `build_errors.txt` in the project
root and read from there instead.
