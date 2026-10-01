# Omni360

A homebrew Xbox 360 app that shows your **installed games** (with real box-art icons, no typing) and downloads their **DLC and title updates from archive.org** straight onto the console — no PC required.

Pick a game, then press **A** for its DLC (from the `msx360gcdlc` collection) or **X** for its title updates (from `microsoft_xbox360_title-updates`). **Y** opens Settings, where the games folder and your archive.org keys can be changed without a PC. A game disc in the drive shows up as the first tile in the library; **A** on it installs it as Games on Demand, into your games folder, so it plays without the disc. Both install to the right place on their own: DLC and lowercase `tu...` updates into `Content\`, uppercase `TU_...` updates into `Cache\`.

This is a fork of [951261/X-Store](https://github.com/951261/X-Store), reusing its proven BearSSL/TLS networking core and Direct3D rendering framework. Everything Vimm's Lair/full-game/ISO/updater-related has been stripped out; everything archive.org/DLC/installed-game-related is new. See `docs/` for X-Store's own original architecture notes (still accurate for the networking layer this fork builds on).

**Status: working on real hardware.** The full pipeline — library scan, picker, archive.org auth, content lookup, archive member walk, install — has been run end-to-end against a real 27-game library on a JTAG/RGH console, for both DLC and title updates (Skyrim's 25.5 MB update installed and was picked up by the dashboard). The **Testing order** section below is kept as a guide for bringing it up on a fresh setup.

## Why this exists instead of just using X-Store or Aurora

Two other approaches were tried and ruled out before this:

- **An Aurora Lua script** got further than expected (installed-game list, on-screen keyboard, HTTP downloads all worked), but Aurora's `Http` library doesn't persist cookies between requests and has no way to attach custom headers — confirmed on real hardware (a successful archive.org login `POST` followed by an authenticated `GET` still came back `401`). Archive.org's login wall makes this a dead end from Lua.
- **X-Store as-is** (pointed at Vimm's Lair) worked initially, then broke with `404`s from Vimm's own anti-scraping measures — outside anyone's control but that project's maintainer.

A standalone native `.xex` was the only remaining option with full control over HTTP requests (custom headers, cookies, redirects), which is exactly what archive.org's login wall requires.

## Requirements

- The official Microsoft Xbox 360 XDK + Visual Studio 2010 (`XboxTLS2.vcxproj` targets `Xbox360Proj`/`Platform=Xbox 360` directly — this is **not** buildable with the open free60/libxenon toolchain). Sourcing the XDK itself is on you.
- A JTAG/RGH/BadUpdate/ABadAvatar console, same as X-Store.
- An archive.org account, and its IAS3 access key + secret key (see Setup below) — every file in the `msx360gcdlc` collection is marked private in its own metadata, confirmed on hardware as a hard 401 even for a fully anonymous request, so real authentication is genuinely required, not optional.

## Setup

1. Open `Omni360.sln` and build `XboxTLS2.vcxproj` (Release config) once you have the XDK working. The project keeps its original name; the Release build produces `Omni360.xex`.
2. Deploy that `.xex` to your console the same way you would any other homebrew title (standalone, launched from Aurora/your dashboard like X-Store already is — this isn't an Aurora script or plugin).
3. Optional: two independent settings.txt keys control where things live, since your existing game library and where DLC actually installs can be different drives/folders:
   - `xbla-path:` (same key X-Store already uses) — where downloaded DLC gets written. Defaults to `Hdd1:\Content\0000000000000000`, the standard Xbox content layout.
   - `games-path:` — where your installed game library is scanned from for the picker. Defaults to `Hdd1:\Content\0000000000000000`, where the dashboard installs Games on Demand and arcade titles. If you keep your GOD games in a folder of their own instead (for example `Hdd1:\Games\{TitleID}\00007000\{ContentID}` — the same layout under a different root), point this there, or change it on the console in **Settings**.
4. Before first run, get your archive.org **IAS3 access key + secret key** at [archive.org/account/s3.php](https://archive.org/account/s3.php) (log in with your normal account first) — this is archive.org's official, script-friendly API key pair (the same mechanism their own `ia` command-line tool uses), not your account password.
   - **Recommended**: FTP a plain text file to `game:\ArchiveOrgKeys.txt` yourself, access key on line 1 and secret key on line 2, before ever launching the app. It's picked up silently on launch with no prompt at all - much less painful than typing a ~40-character secret key with an on-screen keyboard and a controller.
   - Otherwise, first run prompts for both values via the on-screen keyboard and saves them to that same file so you're not asked again.
   - **This file is plain text** either way — fine on your own console, delete it if anyone else has physical/FTP access.
   - (An earlier version of this tool tried logging in with your actual email/password instead — that turned out to hit the wrong endpoint entirely, since archive.org's real login is a JS-driven flow with its own CSRF handshake and a reCAPTCHA-referencing security policy a script can't satisfy. IAS3 keys sidestep all of that.)

## How it works

1. Walks `Content\0000000000000000\{TitleID}\{ContentType}\*` and reads each installed title's Title ID / display name / box-art icon straight out of its STFS package header (`StfsParser.cpp`) — entirely offline.
2. Shows that list as a grid of cover tiles on the Your Library page (`GameListUI.cpp`), in the style of the Xbox Series dashboard, beside a sidebar of pages — Your Library, Store, Queue, Settings. D-pad or left stick to move, left to reach the sidebar; on the library, A for DLC, X for title updates, Y as a shortcut to Settings. A game disc in the drive is the first tile (`DiscWorker.cpp` watches the tray and reads what's put in); A on it installs it, as a job on the Queue that runs alongside downloads. B steps back a level, and B on the sidebar exits. Searches run in the background (`SearchWorker.cpp`), and the picker opens straight away and fills in when the results arrive; each collection's listing is fetched once per session, so searches after the first are near-instant. Looking needs no keys — they're asked for the first time something is queued. Choosing a pack or update adds it to the **Queue**, which downloads on its own thread (`DownloadQueue.cpp`) while you carry on browsing; a popup says when each one finishes or fails, and X on the Queue page stops one.
3. Sends your IAS3 key pair as an `Authorization: LOW <access>:<secret>` header (`ArchiveOrgDLC.cpp`) and looks up the chosen game against the relevant item's public metadata — `msx360gcdlc` for DLC, `microsoft_xbox360_title-updates` for updates. Name-matching is soft, so a game with several separate DLC packs — e.g. Call of Duty 2's Bonus/Invasion/Skirmish Packs — picks up all of them.
4. For each matched archive, reads its internal file table via a handful of small `Range` requests. RAR interleaves headers with each file's data, so that walks the chain one entry at a time; ZIP keeps a central directory at the tail, so one request covers it.
5. Downloads each real file through archive.org's `/download/{item}/{archive}/{urlencoded/internal/path}` URL form, which serves the member **already extracted server-side** — no RAR or ZIP decompression is implemented or needed on our end.
6. Writes each file where the console expects it. DLC members already carry a `TitleID\ContentType\ContentID` internal path that matches the Content layout. Title updates are placed by filename: lowercase `tu...` into `Content\0000000000000000\{TitleID}\000B0000\`, uppercase `TU_...` into `{device}\Cache\`.
7. Avatar-item packs (STFS content type `00009000`) are filtered out — matching Sonic Generations turned up 12 files, 11 of which were avatar data rather than DLC.

## Known limitations

- **Games-on-Demand-style split packages** (a folder of numbered parts instead of one file) aren't handled by `EnumerateInstalledGames` — such titles are silently skipped rather than mis-parsed. How common that layout is on a JTAG/RGH+Aurora setup wasn't verified this session.
- **Display Name encoding**: the STFS field is documented as UTF-8 by the Free60 wiki but historically treated as UTF-16BE by real tooling (Modio, Velocity, Horizon). `StfsParser.cpp` detects which one per-file rather than assuming, but this is the first thing worth checking against your own library — if names come out garbled, that's where to look.
- **Supplementary-plane characters** (outside the BMP) decode to `?`. Everything in the BMP, including the trademark and registered signs that turn up in real title names, now survives both decode paths intact.
- **Key entry** via the on-screen keyboard is not masked (visible while typing), same caveat the earlier Aurora Lua prototype had for password entry - your secret key is visible on screen while typing it in.
- **Fuzzy DLC matching** scores candidates 0–100 (`ArchiveOrgDLC.cpp`'s `ScoreDlcMatch`) on how much of the game's name appears anywhere in the filename, weighted by word length, and shows anything above 50 in a ranked picker. This replaced a strict word-prefix match that failed on two real cases: release groups inserting words (`Halo.1.Combat.Evolved.Anniversary...` vs a title with no "1" in it), and packages whose Display Name is abbreviated (`CoD: World at War` can never prefix-match `Call.of.Duty.World.at.War...`). A missing *number* is penalised heavily, so "Halo 3" doesn't offer you "Halo 4". Because matching is deliberately loose, **you always confirm the pack yourself** — nothing downloads automatically.
- The `.vcxproj.filters` file still lists the old Vimm/7z/ISO/updater folder groupings cosmetically (Solution Explorer organization only, doesn't affect the build) — untouched since it's a large, low-value mechanical edit.

## Testing order

This is the order the app was originally brought up in, and it is still the fastest way to isolate a failure on a fresh setup — build cycles are slow and there is no emulator, so bisecting after the fact costs more than staging it does:

1. Confirm the stripped project actually builds and boots to a blank screen.
2. Test the archive.org IAS3 auth header alone (watch `game:\DebugInfo.txt` / the on-screen log) before touching the UI.
3. Test RAR header parsing against a small known file — `007.Legends.DLC.RF.X360-ZTM.rar` (17.6K) was used as the test case throughout development.
4. Test STFS icon/name extraction against a couple of your installed titles.
5. Only then exercise the full UI end-to-end.

## License

AGPL-3.0, inherited from the original X-Store project (see `LICENSE`). Vendored components keep their own licenses: BearSSL (`SSL/`, MIT), cJSON (`cJSON.c/.h`, MIT). A modified version you distribute must stay under AGPL-3.0 with copyright notices preserved and source available to anyone you give the binary to.

Omni360 is a distinct name for a distinct application, but a good deal of it is still X-Store's code. `dns.cpp`, `parsing.cpp`, `OutputConsole.cpp`, `downloadFile.cpp` and `driveMount.c` carry `PROGRAMMER : 951261` headers, and the BearSSL/TLS networking core underneath them is inherited essentially intact. Those headers, and the fork notice in the startup banner, are there on purpose — AGPL-3.0 requires copyright notices to survive and requires a modified work to say that it has been modified. Rename the product as much as you like; leave those alone.
