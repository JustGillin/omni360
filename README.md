<div align="center">

# Omni360

### Get games onto your modded Xbox 360, from the Xbox 360

Browse thousands of games, DLC and title updates, install them in the background, and play them from the dashboard or Aurora. No PC needed once it's set up.

<br>

[![Version](https://img.shields.io/github/v/release/JustGillin/omni360?include_prereleases&label=version&color=107c10)](https://github.com/JustGillin/omni360/releases)
[![Downloads](https://img.shields.io/github/downloads/JustGillin/omni360/total?color=107c10)](https://github.com/JustGillin/omni360/releases)
[![Platform: Xbox 360](https://img.shields.io/badge/platform-Xbox%20360-107c10?logo=xbox&logoColor=white)](#quick-start)
[![Built with C++](https://img.shields.io/badge/built%20with-C%2B%2B-00599c?logo=cplusplus&logoColor=white)](COMPILING.md)
[![License: AGPL-3.0](https://img.shields.io/badge/license-AGPL--3.0-blue)](LICENSE)

**[Quick start](#quick-start)** &nbsp;·&nbsp; **[Features](#what-it-does)** &nbsp;·&nbsp; **[Screenshots](#screenshots)** &nbsp;·&nbsp; **[How to use it](#how-to-use-it)** &nbsp;·&nbsp; **[Docs](docs/docs.md)**

<br>

<img src="docs/screenshots/store.png" alt="The Store: featured games and the A to Z lists" width="100%">

</div>

<br>

## What it does

| | |
|---|---|
| **Your Library** | Your installed games as a grid of covers, read straight from the hard drive: Games on Demand, arcade packages, and games kept as extracted folders (`default.xex` or `default.xbe`). |
| **Store** | About 1,500 Xbox 360 disc games from archive.org, each with its own page: wallpaper, description, screenshots and every regional version. Installs as Games on Demand, playable without the disc. |
| **Arcade and indie games** | About 740 Xbox Live Arcade games and 3,450 Xbox Live Indie Games, A to Z, installed straight into the console's content folder. |
| **Original Xbox** | About 500 Original Xbox games the 360 can play, installed as Games on Demand. |
| **DLC and title updates** | Found for any game and installed where the console expects them. |
| **Disc installs** | Put a game disc in the drive, Xbox 360 or Original Xbox, and copy it to the hard drive as Games on Demand. |
| **Queue** | Everything downloads and installs in the background while you carry on browsing. |
| **Updates itself** | Checks GitHub for a new version, shows what's new, and installs it. Only releases signed with the project's key are accepted. |

**Status: beta (0.8.0-beta), working on real hardware.** Every kind of install above has been run end to end on a modded console with a real library.

<br>

## Screenshots

<p align="center">
  <img src="docs/screenshots/library.png" alt="Your Library: installed games as a grid of covers" width="100%"><br>
  <sub><b>Your Library</b>: every installed game, Xbox 360 and Original Xbox, with its DLC and title updates marked.</sub>
</p>

<table>
  <tr>
    <td width="50%"><img src="docs/screenshots/360-game-details.png" alt="Grand Theft Auto V's page in the Store"><br><sub><b>A game's page</b>: description, ratings, screenshots and every regional version.</sub></td>
    <td width="50%"><img src="docs/screenshots/xbla-game-details.png" alt="Alan Wake's American Nightmare installing"><br><sub><b>Xbox Live Arcade</b>: arcade games get the same pages, and show their progress as they install.</sub></td>
  </tr>
  <tr>
    <td width="50%"><img src="docs/screenshots/store-popular.png" alt="The Store's A to Z, Popular and Top rated rows"><br><sub><b>Browse</b>: A to Z, or rows of Popular, Top rated and every genre.</sub></td>
    <td width="50%"><img src="docs/screenshots/dlc.png" alt="Choosing a Rock Band DLC pack"><br><sub><b>DLC</b>: find the packs for any game and pick the ones you want.</sub></td>
  </tr>
  <tr>
    <td width="50%"><img src="docs/screenshots/queue.png" alt="The Queue: Grand Theft Auto V waiting while an arcade game downloads"><br><sub><b>Queue</b>: everything downloads and installs in the background.</sub></td>
    <td width="50%"><img src="docs/screenshots/settings.png" alt="Settings: library folders, archive.org keys, disc read speed, updates"><br><sub><b>Settings</b>: library folders, your archive.org keys and updates, all on one screen.</sub></td>
  </tr>
</table>

<br>

## Quick start

**You need:**

- A soft-modded (BadUpdate, ABadAvatar) or hard-modded (RGH, JTAG) Xbox 360, connected to the internet. Ethernet is recommended.
- An archive.org account, and its IAS3 access key and secret key. Every collection Omni360 downloads from is marked private, so archive.org refuses downloads without them.
- Free space for installing games: the download and the installed game both, while it installs — around 7GB plus 3-8GB for a typical game. The download is removed once it's installed.

**Then:**

1. Download `Omni360.xex` from the [latest release](https://github.com/JustGillin/omni360/releases).
2. Get your archive.org keys at [archive.org/account/s3.php](https://archive.org/account/s3.php), logged in with your normal account. These are archive.org's official script-friendly credentials, the same pair their `ia` tool uses — not your password. Put them in a plain text file, `ArchiveOrgKeys.txt`, access key on line 1 and secret key on line 2. (Or type them in on the console, in Settings — painful with a controller, but it works.)
3. Copy `Omni360.xex` and `ArchiveOrgKeys.txt` into a folder of their own on the console, and launch it.

The [Setup Guide](docs/Setup%20Guide.md) covers the whole thing step by step.

<details>
<summary><b>Choosing where things go</b></summary>
<br>

Two optional `settings.txt` keys change where things go, and the games folder can also be changed on the console in Settings:

- `games-path:` — where games install, and where the library is scanned. Defaults to `Hdd1:\Content\0000000000000000`, where the dashboard keeps Games on Demand and arcade titles. Add more `games-path:` lines to scan more folders, such as `Usb0:\Content\0000000000000000` — the first is still where games install. See the Setup Guide.
- `xbla-path:` — where DLC, title updates, arcade games and indie games are written. Same default. (The name is inherited from X-Store, which used the same key.)

</details>

<br>

## How to use it

The sidebar on the left has four pages; left on the D-pad reaches it from any page, and **B** steps back a level (B on the sidebar exits).

- **Your Library** — **A** on a game opens its page. A game disc in the drive is the first tile: **A** installs it to the hard drive, **X** opens its page, and once it's installed **A** finds its DLC and **START** installs it again. **Y** is a shortcut to Settings.
- **Store** — featured games, A to Z, then rows of games that scroll sideways: Popular, Top rated and a row for each genre, ranked by how many people rated each game on Xbox Live (`tools/make_store_rows.py`) — no one's downloads are counted. The featured games change by themselves: a week's theme, or the series of a big new release, from `featured.json` in this repository (see `docs/featured-instructions.md` and `tools/featured.py`). **A** on a letter shows its games; **A** on a game opens its page. **XBLA**, **XBLIG** and **Original Xbox** open their own A to Z. **Search** looks through every list at once by name (or title ID); **Y** on the results searches again.
- **A game's page** — **Install** installs the chosen version, every disc of it; choose another version from the list beside the description first. The button shows Queued, Installing, Installed, or Install remaining for a multi-disc game partly installed. **Find DLC** and **Title updates** search archive.org and show what's there to pick from. **Uninstall** appears once something is installed, and removes the game but leaves its DLC and title updates.
- **Queue** — what's downloading, waiting and done. **X** stops a download or install, or clears a finished one.
- **Settings** — the library folders as a list of checkboxes, your archive.org keys, the disc read speed (left and right change it; lower it for a scratched disc that fails partway through), navigation sounds, and updates: Omni360 checks GitHub for a newer version when it starts (this can be turned off), shows what's new in it, and installs it and restarts when you choose Update. It only installs a release signed with the project's key (`tools/sign_release.py`), keeping the previous version beside it as `.old`.

Nothing downloads without you choosing it, and a popup says when each job finishes or fails.

<br>

## Known limitations

- **Downloads are slow-ish** — about 3.5MB/s on average, so a game takes half an hour or more. archive.org's servers are the limit, not the console.
- **Some games aren't in the Store.** Games archived only as compressed RARs (in the `XBOX_360_*` items, such as the DiRT series and Deadpool) aren't listed, as the installer would have to unpack them first; uncompressed RARs, like BioShock Infinite's, install like any other game. A few dozen more aren't on archive.org at all.
- **An interrupted game install starts over.** Pieces resume within a session, but quitting the app mid-download removes what was downloaded.
- **A multi-disc game installed some other way** — from real discs, or another tool — can't be uninstalled by version from its Store page, since Omni360 can't tell which disc is which. Its own library entry still can be, if the Store hasn't got it.
- **Key entry** on the on-screen keyboard isn't masked.
- **DLC matching is deliberately loose** — it has to cope with release-group names like `Halo.1.Combat.Evolved.Anniversary...` and abbreviated display names like `CoD: World at War`. It scores candidates 0-100 on how much of the game's name appears in the filename, and penalises a missing number heavily so "Halo 3" doesn't offer "Halo 4". You always confirm the pack yourself.
- **Display names** are decoded as UTF-8 or UTF-16BE, whichever each package turns out to use; characters outside the BMP become `?`.

<br>

## How it works

<details>
<summary><b>The library, the Store, installs, DLC and disc installs</b></summary>
<br>

**The library** walks `{games folder}\{TitleID}\{ContentType}\*` and reads each title's ID, name and icon out of its STFS package header (`StfsParser.cpp`) — entirely offline. Covers come from xboxunity.net, or Xbox Live's box art where xboxunity has none, and are cached in `game:\Covers` (`CoverArt.cpp`).

**The Store's games are compiled in.** `tools/make_store_titles.py` builds `StoreTitles.h` from archive.org's Redump collection (the `microsoft_xbox360_*` items, one zip per disc), with title IDs from Redump's own datfile — a disc's serial *is* its title ID: "MW-2004" is publisher code `MW` (0x4D57) and title 2004 (0x07D4). It adds games the Redump collection lacks from the `XBOX_360_2` to `_6` set, gathers each game's discs into games and versions, and writes their labels, so the console only reads tables. Wallpapers, screenshots, descriptions and ratings come from Xbox Live's servers over plain HTTP (`HttpPlain.cpp`), and are cached in `game:\Store` (`StoreArt.cpp`).

**Installing a game** (`GameInstaller.cpp`) downloads the disc's zip in 32MB pieces over two connections, from whichever of archive.org's servers holding it is fastest. archive.org stalls both connections about once a minute, so a connection that goes silent for 10 seconds, or slows below 0.5MB/s, is dropped and its piece resumed where it stopped; one server failing repeatedly moves the download to the next. As the pieces land, the disc image is decompressed once to check its CRC and to note restart points; then it's converted to Games on Demand (`GodConvert.cpp`) straight out of the zip, so the 7-8GB image is never written out — only the parts of the disc the game uses, typically 3-7GB. The package gets the game's icon from Xbox Live. Each install is noted in `game:\Store\Installed.txt`, with the disc's media ID, so the Store can tell which disc of a multi-disc game is installed. A typical game takes 30-40 minutes to download and 5-10 to install.

**DLC and title updates** (`ArchiveOrgDLC.cpp`, `DownloadQueue.cpp`) send your keys as an `Authorization: LOW <access>:<secret>` header, look the game up by name in the collection's metadata, then read each matching archive's file table with a few small `Range` requests: RAR interleaves headers with each file's data, so that walks the chain one entry at a time; ZIP keeps a central directory at the tail, so one request covers it. Each file is downloaded through archive.org's `/download/{item}/{archive}/{path}` form, which serves the member already extracted, and written where the console expects it: DLC by its own `TitleID\ContentType\ContentID` path, lowercase `tu...` updates into `Content\0000000000000000\{TitleID}\000B0000\`, and uppercase `TU_...` updates into `{device}\Cache\`. Avatar-item packs (content type `00009000`) are filtered out.

**Disc installs** (`DiscWorker.cpp`) watch the tray, read the disc that goes in, and convert it to Games on Demand the same way, reading ahead from the drive at full speed (or the speed set in Settings). An Original Xbox disc is read only where its files are, as the 360's drive refuses its security ranges.

</details>

<details>
<summary><b>Why this exists instead of just using X-Store or Aurora</b></summary>
<br>

- **An Aurora Lua script** got further than expected, but Aurora's `Http` library doesn't keep cookies between requests and can't attach custom headers — confirmed on hardware — and archive.org's login wall needs both.
- **X-Store as-is** (pointed at Vimm's Lair) broke with `404`s from Vimm's own anti-scraping measures.

A native `.xex` was the only option with full control over HTTP requests.

</details>

<br>

## Building it yourself

You need the official Microsoft Xbox 360 XDK and Visual Studio 2010 (`XboxTLS2.vcxproj` targets `Platform=Xbox 360` directly — this is **not** buildable with the open free60/libxenon toolchain). Sourcing the XDK is on you. See [`COMPILING.md`](COMPILING.md), and [`RELEASING.md`](RELEASING.md) for publishing a version people can update to.

<br>

## Supporting Omni360

Omni360 is free, and always will be: no ads, no tracking, nothing locked away. The best ways to help are free too:

- **Star the repo** so more people find it.
- **[Report a bug](https://github.com/JustGillin/omni360/issues)** or a game that won't install. It's how things get fixed.
- **Tell someone** with a modded 360 who'd use it.

If you'd like to chip in anyway, you can sponsor it on GitHub. It's entirely optional, and every feature stays free either way.

<a href="https://github.com/sponsors/JustGillin"><img src="https://img.shields.io/badge/Sponsor-Omni360-ea4aaa?style=for-the-badge&logo=githubsponsors&logoColor=white" alt="Sponsor Omni360 on GitHub"></a>

<br>

## License

AGPL-3.0, inherited from the original X-Store project (see `LICENSE`). Vendored components keep their own licenses: BearSSL (`SSL/`, MIT), zlib (`zlib/`, zlib license), cJSON (`cJSON.c/.h`, MIT), the Selawik font (OFL). A modified version you distribute must stay under AGPL-3.0 with copyright notices preserved and source available to anyone you give the binary to.

### Credits

Omni360 is a modified version of [X-Store](https://github.com/951261/X-Store) by 951261, released under AGPL-3.0; this section is its notice that it has been modified (AGPL-3.0 section 5(a)). Most of the application has since been rewritten, but X-Store's code is still at its foundation: the TLS wrapper around BearSSL (`XboxTLS.cpp/.h`), the HTTPS client (`downloadFile.cpp`), DNS (`dns.cpp`), URL and text helpers (`parsing.cpp`), drive mounting (`driveMount.c`), the log console (`OutputConsole.cpp`) and the on-screen keyboard (`Keyboard.cpp`). Several of those files carry `PROGRAMMER : 951261` headers. Those headers are copyright notices, and AGPL-3.0 requires them to survive - rename the product as much as you like, but leave them, and this section, in place.

Game data comes from [Redump](http://redump.org) (title IDs, by disc serial), [xboxunity.net](https://xboxunity.net) (covers) and Xbox Live's own servers (art, details and icons); the games themselves from the archive.org collections named above.

Built with [BearSSL](https://bearssl.org) (TLS), [zlib](https://zlib.net) (inflate), [cJSON](https://github.com/DaveGamble/cJSON) (JSON) and [stb_image](https://github.com/nothings/stb) (JPEG and PNG decoding, public domain).
