# How Omni360 compares

There are several ways to add games and manage content on a modded Xbox 360. This page compares **Omni360**, **X-Store**, **GodStix**, **Valkyrie**, **ConsoleCrate** and **ReLive**. They overlap, but they are not all trying to do the same thing: some focus on downloading games, some are broader dashboards, and ReLive is a different kind of service altogether.

This comparison combines Omni360 project details, hands-on testing of X-Store, GodStix and Valkyrie, and publicly available documentation. GodStix was tested on **version 0.9.2**; Valkyrie on a build dated **6 November 2026** (the 6.x series). Features that were not found or could not be verified are labelled that way rather than treated as definitely absent. Download speeds are individual observations, not controlled benchmarks. Last reviewed: **November 21, 2026**.

If something here is wrong or out of date, [open an issue](https://github.com/JustGillin/omni360/issues) and we'll update it.

## At a glance

| | **Omni360** | **X-Store** | **GodStix 0.9.2** | **Valkyrie** | **ConsoleCrate** | **ReLive** |
|---|---|---|---|---|---|---|
| **What it is** | Store, library and content-management app | Focused game/content downloader | Dashboard with store, library and social features | Dashboard for RGH/JTAG consoles: library, themes and content management, with a PC companion (Valhalla) | Dashboard/store project | Stealth service for connecting a modded console to Xbox Live |
| **Interface** | Controller-friendly native app | Simple, text-driven menu | Graphical dashboard with sidebar and multiple sections | Graphical dashboard with eight swappable themes; Series X is the default | Separate dashboard experience | Not a dashboard/store equivalent |
| **Game source** | Internet Archive | Internet Archive | Private service; underlying file source not confirmed | None built in: no store. Games come from your own discs and files, or are sent from a PC by Valhalla | Its own service; original source not confirmed | Not applicable / not confirmed |
| **Account needed** | Free Internet Archive account to download | Free Internet Archive account to download | Guest access is available with a daily download allowance; account needed for social features and higher-tier access | No | Not confirmed | Service account / connection requirements depend on setup |
| **Xbox 360 games** | Yes | Yes | Yes | In the library; no download catalogue | Store listed; details not fully retested | No confirmed game catalogue |
| **Xbox Live Arcade** | Yes | Yes | Yes | Its own library section; no downloads | Mentioned in prior research | No confirmed game catalogue |
| **Indie games** | Yes | No, according to its current setup guide | Not found in hands-on search; availability unconfirmed | Not confirmed | Not confirmed | No |
| **Original Xbox games** | Yes; emulator setup and compatibility guidance included | Yes | Not found in store or library during testing | Its own library section; no downloads | Not confirmed | No |
| **DLC / title updates** | Download, manage and switch title-update versions | Downloads DLC and title updates | Game details show installed DLC/TUs and allow deletion; a separate manager or version-switching workflow was not found | Lists a game's DLC; choose which title update runs; can fetch the newest from XboxUnity | DLC compatibility checking mentioned in prior research; details not retested | Not confirmed |
| **Disc ripping** | Yes; Xbox 360 and Original Xbox | No built-in disc-ripping workflow identified | No disc workflow surfaced in testing | Yes; disc to Games on Demand ("Apotheosis") | Disc tools were described in prior research | Not applicable |
| **Download queue** | Yes | Yes | Yes | No downloads; Valhalla has a transfer queue on the PC | Uses a separate installer, according to prior research | Not applicable |
| **Downloads survive closing the app?** | Yes; pause and resume on next launch | No; app must remain open | No; downloads did not survive closing the app in testing | Not applicable | Not confirmed | Not applicable |
| **Pause / resume** | Yes | Very basic pause (up to 30 seconds, according to release notes); no automatic recovery after closing | Cancel available; no pause option found | Not applicable | Not confirmed | Not applicable |
| **Download limits** | No app-imposed daily limit | No app-imposed daily limit | Guest screen showed 8 downloads per day; starting and cancelling a download still consumes one | Not applicable | Not stated | Not applicable |
| **File manager / FTP** | Yes / Yes | Not a main feature | Yes / Yes | Yes / Yes, plus a web page on the console and console-to-console copy | File manager listed in prior research | Not applicable |
| **Open source** | Yes, AGPL-3.0 | Yes, AGPL-3.0 | No public source found | No public source found; free | No public source found in prior research | No public source found in prior research |

## Download speed observations

These are the speeds observed while testing the same game where possible. They should not be read as a formal benchmark: speeds can change with server load, connection conditions, download duration and other factors.

| App / method | Observed speed |
|---|---:|
| X-Store, initial test | 2.74 MB/s |
| X-Store, later test after allowing more time | 4 MB/s |
| Omni360 direct to console | 2.8 MB/s |
| Omni360 PC helper | Over 7 MB/s |
| GodStix, one of its two server options | 8–9 MB/s |

The X-Store and Omni360 direct results were close in the initial test, and X-Store later reached 4 MB/s. It would be misleading to claim Omni360 is faster at direct console downloads based on these observations. Omni360's PC helper exceeded 7 MB/s in the test. GodStix reached 8–9 MB/s in its test, the highest direct-console result observed so far.

## The tools

### Omni360

Omni360 is intended to manage the whole path from a physical disc or downloaded content to a usable game library. It began as a way to make disc installation less awkward, then grew to include downloads, DLC, title updates, Original Xbox support, file management and a PC helper.

**Library and discovery**

- Scans multiple configurable locations, which can be assigned as Games, Apps, Homebrew or Emulators.
- Shows recently played games and a separate favourites page.
- Xbox 360 and XBLA entries can show artwork, descriptions, genres, ratings and popularity sourced from Xbox Live's public metadata.
- Original Xbox entries show artwork and community compatibility feedback rather than the same description data.
- The current library is alphabetically sorted; search, user-defined sorting and filters are not currently available.

**Downloads and content management**

- Downloads Xbox 360 games, XBLA, indie games, DLC and title updates from Internet Archive collections.
- Supports GOD and extracted-folder installation options.
- Queues downloads and processes them one at a time.
- Individual downloads can be paused, resumed or cancelled. Downloads pause when Omni360 closes and resume when it is opened again, including after a console restart.
- DLC and title updates can be downloaded for games whether or not the game is already installed.
- DLC is currently handled as a per-game batch rather than letting users select individual DLC items. Avatar items and unrelated content are filtered out.
- Title updates can be switched between downloaded versions.
- Includes game removal and rename options, a full file browser, FTP, storage overview and cleanup for leftover content and partial downloads.

**Disc ripping and Original Xbox support**

- Detects Xbox 360 discs, identifies the game, retrieves metadata and offers to install as GOD or an extracted folder.
- Checks whether the game is already installed and asks before reinstalling.
- Can rip Original Xbox discs to GOD format.
- Can create/prepare the HDDX partition and obtain the required emulator files.
- Checks official Original Xbox compatibility. For unsupported titles, it can help select an emulator configuration and show reported issues before launch.

**Other details**

- Optional PC helper for downloads; the PC helper measured over 7 MB/s in the test above.
- Seven interface languages, selected automatically from console settings or changed manually.
- Self-updater with release notes and user confirmation; migrates data/settings between versions.
- Controller-friendly interface, multiple storage devices and external USB installation.
- Shows time and can synchronize it over the internet, useful if the console's CMOS battery is no longer keeping time.
- Temperature and IP information are available through a hidden menu.
- No themes or home-screen customization yet; themes are planned.
- Public source code under AGPL-3.0.

Omni360 needs a free Internet Archive account for its downloads. It does not require an Xbox Live sign-in; Xbox Live's public servers are used for metadata only.

### [X-Store](https://github.com/951261/X-Store)

X-Store is a native Xbox 360 downloader. Its menu is intentionally straightforward and text-driven: Original Xbox, Xbox 360, XBLA/DLC/title updates, update app, download queue and Internet Archive sign-in. Its current setup guide says Xbox 360 disc games are downloaded and installed as extracted XEX folders rather than GOD packages. It also supports Original Xbox downloads and XBLA/DLC/title updates.

**What stood out in hands-on testing**

- Search is text-only and requires entering the game name. The setup guide notes that the first search in a content category can take a few minutes, with later searches usually faster.
- Downloads can be queued and run one at a time.
- It has very limited pause support: release notes describe pausing for up to 30 seconds.
- The app must remain open while downloading, decompressing and extracting. It does not automatically resume a failed or interrupted download after the app is closed.
- The initial measured speed was 2.74 MB/s; a later test reached 4 MB/s. These are observations, not a fixed speed limit.
- Internet Archive authentication can use S3 keys or email/password. The X-Store README says email/password credentials are encrypted using the console Key Vault and sent over HTTPS; it would be inaccurate to describe that method as plaintext.
- X-Store is open source under AGPL-3.0, like Omni360.

X-Store is a focused downloader rather than a full graphical dashboard. It covers several content types, including Original Xbox games, but its extracted-folder workflow and requirement to keep the app open differ from Omni360's GOD option and resumable queue.

### GodStix 0.9.2

GodStix is a broader dashboard experience. In the tested version, its left sidebar links to Home, Library, Social, Store, Downloads, Files and Settings. The interface includes a startup screen, system information and a home page with store previews, library items, curated homebrew, new arrivals, Active Rooms and team news/events.

**Dashboard and library**

- The sidebar displays CPU, GPU and RAM temperatures, the console IP address, date and time.
- The home page includes rotating store previews, favourites controls, game sizes, a weather widget, new arrivals, curated homebrew and team updates.
- Active Rooms can be viewed without signing in, including room activity and participant counts. Chat, friends, achievement tracking and other social features prompt for an account.
- The library is divided into Games and Homebrew. Games can be shown in a carousel or list, launched, opened for details or deleted.
- Game details include an overview, installed DLC/title updates, a link to the store page, achievements and tools.
- The tools include Verify Install, GOD-to-XEX conversion and game renaming.
- In testing, an existing extracted XEX game was not detected in the library. The purpose of the GOD-to-XEX tool is not clear from the interface; it may serve another workflow, but that has not been confirmed.

**Store and downloads**

- Store sections include featured content, categories, latest games and Explore pages for plugins, homebrew, Aurora themes and ports.
- The game store offers regional variants and two download-server choices.
- Halo 1 and Halo 2 were not found in the Original Xbox catalogue, and a search for the indie game CastleMiner returned no result. No Original Xbox games or indie-game category were found during this test; broader catalogue availability remains unverified.
- The source of the game files is not disclosed in the interface and has not been independently confirmed.
- The guest banner showed 0/8 downloads available for the day. Starting a download consumes one of the daily downloads even if the user cancels it. Paid membership tiers are offered, but exact pricing and benefits were not verified in this test.
- The download page shows available HDD and USB space and warns before starting if the selected device does not have enough space.
- Downloads can be cancelled, but no pause option was found. Downloads did not survive closing the app.
- The two server options produced 8–9 MB/s in the observed test.
- Download profiles are labelled Moderate and Turbo. Moderate already reached roughly 10 MB/s in the user's test, so the effect of Turbo could not be established.
- Parallel Parts appears to be an SSD-oriented option, but it could not be tested and its exact behaviour is unconfirmed.

**Game and system management**

- Includes a two-pane file browser with folder creation, copy, paste, move, rename and delete; also includes FTP and custom library scan folders.
- Game details allow installed DLC and title updates to be deleted. A separate DLC/TU manager or a workflow for browsing and switching TU versions was not found.
- No disc-ripping workflow appeared during testing. Opening the disc tray caused the app to close; relaunching with a disc inserted did not show the disc in the library or expose a disc action. This documents the tested behaviour, not proof that no future or hidden disc feature exists.
- The app includes a fan/cooler setting with Auto and manual 50–100% options.
- A Fast Wi-Fi toggle is enabled by default and references 802.11g; its purpose and effect were not established.
- Themes are supported through a `godstix-theme.ini` file placed at the root of a device. The configuration supports a theme name, author, version, colour values and optional background/logo images.
- Additional settings include navigation sounds, volume, interface motion, weather location, temporary-file cleanup and shutdown after downloads.
- A background-download plugin is still in development, may not work on every console and appears to require a separately installed plugin selected through `launch.ini`.
- The interface offers English, Spanish, French, Arabic and Brazilian Portuguese in the tested build.

GodStix's clearest strengths are its dashboard presentation, social/community features, theme support, broad homebrew/ports/plugin sections and the fastest direct-console download speed observed in these tests. Its tested download workflow is less resilient than Omni360's: no pause was found, closing the app ended the download, and cancelled downloads still use the guest's daily allowance.

### [Valkyrie](https://github.com/SafeLightDev/valkyrie)

Valkyrie is a free dashboard for RGH and JTAG consoles from SafeLight.Dev, first released publicly in late October 2026 and updated often since (6.1 on 11 November). It needs DashLaunch and a hard drive; softmodded (exploit) consoles aren't supported. It has no store: it organises and plays what's already on the console, and its PC companion **Valhalla** (Windows and Mac) sends games, DLC and title updates over the network. No source code or licence is published. It was looked at hands-on on a build dated 6 November 2026, in the Series X theme.

**Look and feel**

- Eight themes that change the whole dashboard, not just colours: Series X (default), OG Xbox, Blades, NXE, Metro, Bifrost, Darkroom and Den. The Series X theme has an accent colour (12 swatches), dark mode, header text colour, theme music, your own music from USB, interface sounds and an idle mode.
- Home has a cover flow in the middle, with the focused game's background art fading in behind it, and large tiles for the library, homebrew, disc installs and emulators. Settings › Spotlight swaps the cover flow for other layouts, such as a Bookshelf of game spines.
- Motion is a large part of why it feels polished: the focus outline glides from one item to the next in about 150 ms (including between the sidebar and the grid), the background art cross-fades as focus moves, and pages fade in. Rows themselves don't scroll smoothly.
- There's no footer: button hints sit inside a panel on the right that explains the focused item.
- A boot video picker (several Xbox start-up videos) and a Discord webhook that can post game starts, play sessions, installs and disc rips.

**Library**

- Sections in the sidebar: Games (Xbox 360 retail), Arcade, Original Xbox, Catalogue, Trainers, Recently played, Search, Franchises and Groups, with counts, and a storage bar underneath.
- Inside a section: sorting, region filters (NTSC-J, PAL, Korea, Asia), switches such as "Has Trainer" and "Stackable Achievements", and saved views you cycle with LT and RT.
- Box art uses the real case designs (green Xbox 360, orange Arcade, OG Xbox); a game without art gets a coloured tile with its first letter.
- A Manage view lists games with bars sized by how much space each takes, sorted A-Z or by size; uninstalling asks first, starting on Cancel.

**Content and system**

- Picks which title update a game runs, lists DLC, and can fetch the newest title update from XboxUnity when a game starts.
- Disc to Games on Demand ("Apotheosis"), a file manager on every drive, FTP, a web page on the console, installing from a network share, and copying between two consoles running Valkyrie.
- An Overview page with temperature cards and console details; Restart and Turn off ask for a second A press rather than a dialog.
- Signed updates that roll back if a new build doesn't start, optional modules (like plugins) that are switched off if they crash, and named start-up dashboards chosen with a button at power-on (set up through Valhalla).
- Valhalla also handles trainers (about 100), profiles and saves, disc-image checks and conversion, homebrew installs with undo, and installing Valkyrie itself.

Valkyrie's strengths are presentation and polish: themes, motion, a well-organised library and thoughtful details. It isn't a store, so it doesn't compete with Omni360 on getting games; its library, content management and file tools overlap with Omni360's.

### [ConsoleCrate](https://dashboard.consolecratelive.online/)

ConsoleCrate is a dashboard/store project that has been described as growing into ConsoleCrate Live OS. Earlier research found references to disc tools (copy, convert to Games on Demand and rip), trainers, achievements, a file manager and a Windows media server. Its downloads were described as coming from its own service, but the original file source was not identified. It is free with optional donations. These details have not been retested as part of the current hands-on comparison, so check the project's current release notes before relying on them.

### ReLive

ReLive is different from the other entries: it is a stealth service intended to connect a modded Xbox 360 to Xbox Live, rather than a game-library or download manager. Prior research also found dashboard-switching features. Claims that it can download Arcade games or DLC through the old Marketplace were not confirmed from ReLive's own documentation.

Using Xbox Live on a modified console carries account/console-ban risk. ReLive and similar services should not be treated as a guarantee against that risk. Omni360 does not use this route; it uses public metadata and Internet Archive downloads instead.

## What the comparison says

The products have different priorities:

- **Omni360** focuses on the complete game-management workflow: disc ripping, a unified library, downloads, DLC/title updates, Original Xbox compatibility and resumable downloads.
- **X-Store** focuses on searching for and downloading games and related content directly on the console. It is simple and open source, but it is not a full dashboard and requires the app to stay open during downloads.
- **GodStix** aims to be a dashboard and community hub as well as a store. It has a richer home screen, social features and customization, and it was the fastest direct downloader in these tests. Its tested guest download limits and lack of download recovery are meaningful trade-offs.
- **Valkyrie** is a dashboard without a store. It stands out for its themes, motion and library organisation (sections, filters, saved views), and leaves getting games onto the console to its PC companion, Valhalla.
- **ConsoleCrate** and **ReLive** serve different purposes and have not been retested in this round.

No single feature list tells the whole story. The best fit depends on whether someone values a dashboard/social experience, a straightforward downloader, or a tool that handles the full process of installing and maintaining a game library.

## Sources and testing notes

- **Omni360:** [GitHub repository](https://github.com/JustGillin/omni360/). Project behaviour and features described above are based on the project and hands-on knowledge from its developer.
- **X-Store:** [GitHub repository and README](https://github.com/951261/X-Store), [setup guide](https://github.com/951261/X-Store/blob/main/docs/Setup%20Guide.md), and [release notes](https://github.com/951261/X-Store/releases). The README documents Internet Archive authentication, the app-open requirement and the absence of automatic resume. The setup guide documents extracted XEX output and search behaviour; release notes document the limited pause feature.
- **GodStix:** [Speedy Games Downloads' GodStix page](https://speedygamesdownloads.com/godstix/). The detailed GodStix 0.9.2 section is primarily based on hands-on testing, with unverified behaviour clearly labelled.
- **Valkyrie and Valhalla:** [Valkyrie on GitHub](https://github.com/SafeLightDev/valkyrie) and its [releases](https://github.com/SafeLightDev/valkyrie/releases), [Valhalla on GitHub](https://github.com/SafeLightDev/valhalla) and its [releases](https://github.com/SafeLightDev/valhalla/releases), and [SafeLight.Dev's Xbox 360 help page](https://safelight.dev/x360help). The Valkyrie section is mostly from hands-on use of a build dated 6 November 2026 in the Series X theme; Valhalla's features come from its documentation and weren't tested.
- **ConsoleCrate:** [ConsoleCrate dashboard site](https://dashboard.consolecratelive.online/). Details are retained from earlier research and were not retested in this update.
- **ReLive:** [ConsoleMods' Xbox Live information](https://consolemods.org/wiki/Xbox_360:Xbox_Live). Details are retained from earlier research and were not retested in this update.

### Download test caveat

The speed figures are user-observed results from a small number of tests, not repeatable benchmark results. The X-Store result changed from 2.74 MB/s to 4 MB/s after allowing more time. Omni360 direct was measured at 2.8 MB/s, while the PC helper exceeded 7 MB/s. GodStix reached 8–9 MB/s. Different server loads and test timing may change the results.
