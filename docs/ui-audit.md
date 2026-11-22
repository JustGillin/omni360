# Omni360 UI audit

## Goal

Make every screen and interaction feel like one deliberate product. Controller actions must be visible in a consistent footer at the bottom of the screen wherever those actions are available. The footer uses the Xbox button badge and a clear action label; it must not hide an available action or imply an unavailable one.

For dialogs, the audit will check whether action hints stay visible at the screen bottom while the dialog is open, and whether labels, ordering, focus, confirm, and cancel behavior remain consistent.

## First-pass screen map

```mermaid
flowchart TD
  Home --> Library[Games and apps]
  Home --> Store
  Home --> Queue
  Home --> Settings
  Sidebar --> Library
  Sidebar --> Favourites
  Sidebar --> Apps
  Sidebar --> Homebrew
  Sidebar --> Emulators
  Sidebar --> Store
  Sidebar --> Queue
  Sidebar --> Settings
  Library --> GameDetails[Game details]
  Store --> Browse[Browse and search]
  Browse --> GameDetails
  Settings --> Storage
  StorageRing[Storage indicator] --> Storage
  GameDetails --> Dialogs[Dialogs and progress]
  Browse --> Dialogs
  Queue --> Dialogs
  Settings --> Dialogs
  Files --> Dialogs
```

This map comes from `ShellPage` and the renderer/input branches in `src/GameListUI.h`, `src/GameListUI.cpp`, and `src/main.cpp`. Runtime exploration will add nested screens and conditional states.

## Audit record

| Area | Current evidence | Audit status |
| --- | --- | --- |
| Home | Shelf and all four top-nav icons checked on full frames | Done; see "Library walkthrough" |
| Library / Favourites | Games grid (paging), sidebar, Options dialog, Favorites grid and Options | Done; empty/loading states not reachable (every list has items) |
| Apps / Homebrew / Emulators | Apps and Homebrew sidebar and grid checked; Emulators page is hidden when no Emulators folder is set | Done, except Emulators (would need a setting change) |
| Store | Landing rows, XBLA picker and full list, Homebrew page and item, search with results, installed and uninstalled game pages, DLC / Title updates / Versions | Done; see "Store walkthrough". XBLIG, Original Xbox, Collections, Publishers lists and error/loading states not reached |
| Queue | Recently installed tiles, footer, Y (Night mode) toast | Done for what exists; active, paused, error and empty states need a real download (source only) |
| Settings | All five sections (General, Assets, Storage, Language, About) and every row type walked live; pickers opened and cancelled | Done; see "Settings walkthrough". Keyboards, Unpair/Scan, Set now/Check now/Run test/Re-sync not opened |
| Storage | Overview, Free up space, Partial downloads, Leftover DLC and updates, Measure again, Files (drive list, inside a drive, Y menu, X marks) | Done; Files delete/copy/move confirmations not run |
| Dialogs | Exit, System (BACK), toasts, every confirmation reached, Files menu, Settings and Store pickers | Done; see "Queue and dialogs walkthrough". Progress overlays not reached |
| Blocking screens | Startup, status, progress, launch, and night renderers | Verify which accept input and what their footer should show |

## What each walkthrough records

For every screen and meaningful state, record:

- Entry path and focused item or region.
- Every accepted controller action and its footer label.
- Back/cancel behavior and where focus returns.
- Empty, populated, loading, success, and error states where applicable.
- Dialog stacking, safe cancellation path, and progress feedback.
- Text clarity, localization fit, hierarchy, spacing, and any visual inconsistency.
- OBS capture and source renderer/input function for each finding.

Destructive or long-running actions will be documented at their confirmation step and canceled during discovery. They will only be executed when needed to inspect a result and explicitly authorized.

## Shared conventions to check first

1. Footer placement, order, badge scale, label length, and behavior at 640x480 and 1280x720.
2. Consistent meanings and wording for A, B, X, Y, Start, bumpers, and triggers.
3. Focus and selection visibility, including sidebar-to-content transitions.
4. Dialog placement, action hints, cancel behavior, and nested dialogs.
5. Loading/progress, empty/error states, success feedback, and toast timing.
6. Text hierarchy, capitalization, localization, truncation, and line wrapping.
7. Content density: avoid a second line that merely restates the title. Keep a subtitle only when it adds orientation, status, or an explanation the user needs to choose safely; prefer contextual help over permanent explanatory copy.
8. Plain language: labels and help text should read naturally to someone who isn't technical. Avoid jargon (paths, partitions, "re-sync", "measure", internal names) unless the user needs that word to act.
9. Clipped or hard-to-read text: anything cut off mid-word, overflowing its box, too dim, or too small at projector size.

### User direction (2026-11-20)

- Wording should not be too technical, and the same thing should be called the same thing on every screen.
- Don't overuse subtitles. Example: the Storage screen's left-hand buttons (Free up space, Partial downloads, Leftover DLC and updates, Measure again, Browse files) don't need subtitles; the name alone should be obvious. Counts or sizes that help decide may stay, but only where they add something.
- Versions should present the same way as the title update and DLC modals.
- Flag clipped text and hard-to-read text everywhere.

## Findings

### Live footer findings

These were observed on the connected Xbox through the OBS Windowed Projector on 2026-11-20. The bridge was active; navigation used short D-pad, A, B, X, and Y taps.

| Priority | Screen/state | Observed footer | Expected / behavior | Evidence |
| --- | --- | --- | --- | --- |
| Closed | Home, top navigation focused on Games + Apps | A Open; B Exit is absent in the live frame | Source assembles A Open and B Exit. Runtime and source disagree; verify the active build and footer clipping/layout. | Live observation; `src/main.cpp` Home navigation hint branch near 12150; **not reproduced on full frame 2026-11-20: A Open, B Exit** |
| Closed | Apps, sidebar focused | No action hints visible | Source should expose A Select and B Home on sidebar focus. B/return behavior is not obvious to the user. | Live observation; `src/main.cpp` sidebar-focus hint construction; **not reproduced: A Select, B Home** |
| Closed | Homebrew, sidebar focused | A Select only; no B Home | B does return to Home, confirmed by pressing B. Add the missing B Home hint. | Live observation; `src/main.cpp` sidebar-focus hint construction; **not reproduced: A Select, B Home** |
| P2 | Games grid | A Play, X View game, Y Options, B Home, LB/RB Page | Complete mapping observed for this populated grid state. | Live observation |
| P2 | Games sidebar focus | A Select, B Home | Complete mapping observed; moving down immediately changes the page (Favorites, then Apps). | Live observation |
| P2 | Apps grid | A Start, B Home | Complete mapping observed; all apps fit, so no page hints are needed. | Live observation |
| P2 | Home game shelf | A Play, X View game, B Exit | Complete mapping observed on initial and latest Home frames. | Live OBS frames |
| P1 | Game details: Versions | Opens as a right-side panel that leaves the detail screen visible behind it; footer shows A Choose, B Done | User requests a modal presentation consistent with DLC and title updates. Adopt one shared centered modal treatment and the same bottom-screen footer placement for these related actions. | Live OBS; user direction 2026-11-20 |
| P2 | Game details: community note | Long text continues beyond the right edge and ends mid-word | Wrap text within the content column, or provide an intentional truncation/expand affordance. | Live OBS, Warrior Within |
| P2 | Game details: disabled DLC/title update actions | Focus can land on dimmed actions; on Title updates the footer contains only B Back and Y Versions | Disabled controls should not receive focus, or should explain why unavailable; ensure the footer always maps all actions actually available in the current focus state. | Live OBS, Warrior Within |
| P2 | Manage game | A Manage game opens a full-screen content selection view; A Select, Y Select all, B Back; no items selected during audit | Bottom mapping and return path are clear. Preserve the safety copy about saves and make later removal confirmation explicit. | Live OBS, Warrior Within |
| P1 | Dialog footers: Options, DLC, title updates | Hints are placed near the bottom of each centered dialog, leaving substantial screen area below them | Keep the modal content centered, but move controller hints into a shared footer anchored to the screen bottom. This applies the user’s rule literally across dialogs. | Live OBS, Games Options + Sonic DLC/title updates; user direction 2026-11-20 |
| P2 | Sonic DLC dialog | Four installed item labels visibly end early (for example, `Additional Episode "Shadow Bos.`); dialog says 7 items installed | Provide enough row width, wrap names, or ellipsize consistently while preserving a way to read the complete name. | Live OBS, Sonic the Hedgehog (Xbox 360) |
| P2 | Sonic title updates dialog | Centered modal reports no installed updates and offers Find more online; A Select, B Back | Presentation matches the DLC modal. Move hints to the shared screen-bottom footer. | Live OBS, Sonic the Hedgehog (Xbox 360) |
| P2 | Sonic details (Xbox 360) | Description wraps cleanly; screenshots and a related-games shelf are visible; footer A Play, B Back, Y Versions | The layout and body copy are legible at this projector size. Use a 360 title for representative detail flow; original Xbox titles have distinct compatibility notes. | Live OBS, Sonic the Hedgehog |
| P2 | Sonic Manage (Xbox 360) | Lists the game and installed DLC with full item names; footer A Select, Y Select all, B Back; empty selection maintained | Same interaction as the original Xbox title. User can distinguish game vs DLC by type label and storage location. | Live OBS, Sonic the Hedgehog |
| P2 | Store landing | Featured game and category tiles; footer A View game, B Home, Y Search | Footer is complete and screen-bottom. Category names are legible. | Live OBS |
| P2 | Store search keyboard | Dedicated keyboard screen, blank field, helper text says partial names search every list; LB/RB switch left/right, LT Symbols, RT Accents, stick Caps, X Backspace, Y Space, Done; screen-bottom A Select, B Back | Strong explicit control mapping. Verify any text clipping at alternate resolutions later. | Live OBS; search canceled empty |
| P2 | Queue, Recently installed | Four items initially; X Remove immediately removed focused Marvel Nemesis item from the displayed list, count changed from 4 to 3; no confirmation shown. B Home available. | Footer maps actions clearly. Review whether Remove should say "Remove from queue" or prompt, depending on whether it only dismisses the recent entry or uninstalls the content. | Live OBS; one item removed from the Queue display with user authorization |
| P1 | Settings: General, Archive.org keys row | Focus moves to Change; footer shows A Select only; pressing B returns Home | Always show B Home while available, including when a child control is focused. | Live OBS; no setting changed |
| P2 | Settings: General, Navigation sounds row | Footer shows A Turn off and B Home; contextual help for the selected row appears immediately above footer | Helpful contextual description and clear action wording. Keep shared footer complete as focus moves to other row types. | Live OBS |
| Closed | Home top navigation | Games + Apps and Queue showed A Open only; Store and Settings showed A Open and B Exit | Top-navigation footer should be consistent across destinations if Exit is available in all those states. | Live OBS; multiple nav items; **not reproduced: all four icons show A Open, B Exit** |
| Closed | Storage: Free up space | A Select, Y Select all, Manage game; no B Back hint. B does return to Storage overview. The list explains saves are kept and DLC/update removal. | Keep B Back in the screen-bottom footer across all selection-list screens. | Live OBS; no items selected or removed; **not reproduced: A Select, Y Select all, Manage game, B Back** |
| P2 | Settings: Assets | Re-sync library is the sole setting action; page otherwise empty | Add one-sentence explanation of the scan and its effects so the page feels intentional. | Live OBS; re-sync not activated |
| P2 | Settings: Storage | Lists enabled scan paths, content type, item counts, and not-scanned paths; Manage storage and Add path controls | Useful at-a-glance status; screen-bottom A/B hints appear when content is focused. Validate path text at smaller resolutions. | Live OBS; no path changed |
| P2 | Storage manager overview | Free space, per-drive usage, color legend, Free up space, Partial downloads, Leftover DLC and updates, Measure again, Browse files; A Select, B Back | Clear overview and task labels. Keep destructive work behind a visible review/confirmation step. | Live OBS |
| P1 | Storage: partial downloads confirmation | Centered "Delete partial downloads?" dialog opens with Delete focused; no dialog-specific button hints; underlying A/B hints are dimmed | Default focus to Cancel, show Delete/Cancel button mappings in the shared screen-bottom footer, and keep the safety copy readable. | Live OBS; no delete performed |
| P1 | Storage: file browser | Dual-pane browser explains Left/Right pane switching and X/Y actions at the top, while screen-bottom footer shows only A Open | Put every currently available controller action in the screen-bottom footer, including B Back and X/Y when active. Avoid relying on top-of-screen helper copy alone. | Live OBS; no files changed |
| P2 | Storage task labels | "Measure again" is paired with "Reads every drive again"; the user says the label sounds odd | Use a direct label such as "Rescan drives" or "Refresh storage info"; remove repeated subtitles that merely restate labels elsewhere. | Live OBS; user direction 2026-11-20 |

### Settings walkthrough (2026-11-20, Claude)

Path: Settings opens from Home's top navigation; sidebar is General, Assets, Storage, Language, About. B from any content row goes to Home. No settings were changed; every picker was cancelled with B.

| Priority | Screen/state | Observed | Expected / suggestion |
| --- | --- | --- | --- |
| P1 | Help line, bottom-left, all sections | Long help text is cut off mid-word, with a stray period: Storage › Add path "…on the hard drive or a USB d."; General › Set clock "A wrong clock makes sec."; General › FTP server "…while Omni360 is ."; Language › Clock "…where the language is spoken - left a."; About › Speed test "…to see what slows downl." | Wrap to two lines or shorten the copy. The truncation drops the last word and adds "." even mid-word; fix that too (ellipsis at a word boundary). |
| P1 | Carousel rows: General › Disc read speed, Install disc games as; Storage › path type; Language › Clock | Left/right is the only way to change the value, but no footer hint says so. Disc read speed shows only B Home; Install disc games as and Clock show A Select, B Home, though A does nothing obvious. Path rows show A Stop scanning/Scan. | Show a ◀▶ "Change" hint whenever a carousel is focused. A should behave and read the same on every carousel, or not be offered. |
| P1 | Language list | Active language (English US) shows "A Turn off"; others show "A Turn on" while the help says "A switches Omni360 to this language". Rows use checkboxes though only one language can be on. | Radio-style marks; label "Use". No A hint on the language already in use. |
| P1 | Settings › Storage: Add folder / Downloads picker | Going back up from a folder leaves the previous dialog drawn dimmed behind the new one ("Add Folder / Hdd1:" ghost with its own faded A Select / B Back). Seen twice; persists in still frames. | Clear the old layer when going back a level. |
| P1 | Dialogs in Settings (Add folder, Downloads go here, Sign in at start) | Dialog hints sit inside the dialog while the page footer stays visible underneath, dimmed but readable ("A Select, B Home" plus the old help line): two footers. | One footer at the screen bottom showing the dialog's actions; hide the page footer (same rule as the existing dialog-footer finding). |
| P1 | About › Version info | Focus gives no outline, only slightly brighter text; nearly invisible. Footer B Home only. | Skip the row (nothing to do on it) or give it the standard focus outline. |
| P1 | Folder picker, inside a folder | Focus starts on "Use this folder", so a second A press picks the folder by accident. B climbs one level at a time (up to four presses to leave); footer only says "Back". | Start focus on the first subfolder. Offer a one-press close (e.g. Y Cancel) and show it in the footer. |
| P2 | Settings › Storage, first row | Footer "A Select" while the button reads "Change" and help says "A picks another path". | Footer label "Change". |
| P2 | Settings › Storage, path rows | "A Stop scanning" / "A Scan", while help says "A adds it to Emulators". Source (`src/main.cpp` ~12543) also uses "A Remove" for paths the user added, which looks the same as unticking. | One verb pair ("Scan" / "Stop scanning") with matching help. Make clear when A removes the folder from the list rather than unticking it. |
| P2 | Settings › Storage wording | "Manage paths" header, "Add path" button, "Add Folder" picker title (title case). Raw paths shown ("HDD1:\Content\0000000000000000", "Usb0:\Apps"); "HDD1:" vs "Hdd1:" on different rows. Drives are "Hdd1:/Usb0:" in the picker, "HDD1" on the sidebar ring, "Hard drive / USB drive 1" on Storage. | Say "folder" everywhere, sentence case ("Add folder"). Use the friendly drive names everywhere; normalise casing. |
| P2 | Settings › Storage, "Drives" header | Heading holds only a "Manage storage" button that opens a page titled "Storage", inside Settings › Storage: "Storage" names three things. | Drop the "Drives" heading, or rename the button (e.g. "Open storage"). |
| P2 | Downloads picker | Lists title-ID folders ("415407D7", "E0000437AE7C668C", "0000000000000000"); title "Downloads go here". | Jargon. Title "Download location"; start at the drive root or label known folders. |
| P2 | Settings › Storage status | "1 found" on Homebrew; other rows say "4 games", "3 apps". | "1 app". |
| P2 | General › archive.org keys | Label starts lowercase; status "Saved · access key dikO." looks truncated. | "Archive.org keys"; status "Saved" (or "dikO…" if the partial key matters). |
| P2 | General › PC helper | "Scan" button (finds the PC again) reuses the folder "Scan" word; Unpair and Scan both show generic "A Select". | Rename "Find PC"; footer names the action ("Unpair", "Find PC"). |
| P2 | General › Sign in at start | Picker (Off, then each profile) marks the current choice only by focus, no tick. | Tick the current option, as Language does. |
| P2 | General › FTP server | Status shows address and password in plain text ("ftp://<console IP> · sign in as xboxftp / xboxftp"). | Useful but technical. Consider "PC file access (FTP)" with the details in the help line only. |
| P2 | General, focus style | Single-control rows outline the whole row; multi-control rows (PC helper, Set clock, archive.org keys) outline only the button. | Acceptable; consider also lightly highlighting the row. |
| P2 | Assets | Now has help text (earlier "page otherwise empty" finding is partly fixed). "Re-sync library" row with a "Re-sync" button repeats itself; "Re-sync" and "Assets" are jargon; help says "disk" where elsewhere it's "drive". | Section "Library"; row "Refresh game list", button "Refresh"; "drive" not "disk". |
| P2 | Language › Clock | "Clock" under Language means the time format. | "Time format" (12/24-hour); consider moving it to General beside the clock setting. |
| P2 | Language › Refresh | "Refresh" alone is vague. | "Reload languages". |
| P2 | About | Version appears twice ("0.20.0-beta · up to date" and "Omni360 0.20.0-beta · Kernel 2.0.17559.0"); help says "Asks GitHub…"; "Kernel" is jargon. | One version row; "Console software 2.0.17559". |
| P2 | Help lines generally | Many repeat the footer ("A picks another path", "A stops scanning it", "A switches…"). | Drop button wording from help text; the footer carries the action. |
| P2 | Return focus | Settings → B → Home lands on the game shelf, not the Settings icon. Storage → B → Settings › Storage returns to "Manage storage", and dialogs return focus to their button (both good). | Return to the Settings icon in the top nav. |

Working as expected: sidebar focus shows A Select, B Home; checkbox rows show A Turn on/off, B Home; Storage overview shows A Select, B Back.

### Library walkthrough (2026-11-20, Claude)

Covers Home, Games, Favorites, Apps, Homebrew and the sidebar storage ring. The five P1 footer findings from cropped frames were rechecked and marked Closed above. Nothing was launched, changed or toggled.

| Priority | Screen/state | Observed | Expected / suggestion |
| --- | --- | --- | --- |
| P1 | Apps grid, tile with no art | Label reads "HDD Compatibili." (cut mid-word plus a stray period, the same truncation bug as the Settings help lines); focused it scrolls as "HDD Compatibilit". | Fix the shared truncation: ellipsis at a word boundary, or wrap. |
| P1 | Emulators | Page is hidden when no Emulators folder is set (`PageHidden()`, `src/main.cpp` ~4213), so it never appears, yet Settings › Storage help says "A adds it to Emulators". | Either mention in Settings where Emulators shows up, or show an empty Emulators entry ("No emulators found" plus how to add one), as Favorites does. |
| P2 | Favorites › Y Options | Opens with focus on "Remove from favorites", so A, A removes it with no confirmation. | Start focus on Rename, or make removal undoable. |
| P2 | Apps / Homebrew grids | Footer "A Start, B Home"; Y does nothing (correctly not shown). Games offer Options (rename, favourite, view); apps don't. | Fine if intended; otherwise offer Y Options (at least Rename). |
| P2 | Favorites spelling | UI says "Favorites" everywhere; check the en-ca pack uses one spelling throughout. | One spelling per language. |
| P2 | A verb by context | "Play" for games, "Start" for apps, "Open" in top nav, "Select" in sidebar. | Acceptable; confirm "Start" vs "Play" is deliberate. |
| P2 | Sidebar or top nav focused | The last-focused tile keeps a grey/light outline while the sidebar or nav has the white one: two outlines on screen. | Make the remembered position clearly weaker than focus (no border, or much fainter). |
| P2 | Sidebar storage ring | Footer "A Select" (it opens Storage); ring labelled "HDD1", Storage says "Hard drive" with "Hdd1"; USB is "USB drive 1" beside "Usb0". | Footer "A Storage"; one friendly drive name everywhere, consistent numbering. |
| P2 | Games grid, long focused title | Scrolling title is hard-cut at both tile edges ("mer 40,000: Spac"); reads as clipped in a still. | Fade the edges, or pause at the start before scrolling. |
| P2 | Games grid paging | "LB RB Page" shows even when one direction can't page. | Minor; consider dimming the side that doesn't apply. |
| P2 | B from a library page | Returns Home with focus on the shelf, not the Games + Apps icon; re-entering always opens Games, even after leaving from Favorites. | Return focus to the nav icon used to enter. |
| P2 | Home over a light wallpaper | "CONTINUE PLAYING" (small green caps) and white top-nav labels are low contrast on a near-white wallpaper (Soulcalibur II). | Stronger scrim or shadow behind the top band and caption. |
| P2 | Free up space | "Manage game" hint uses a grey arrow glyph that doesn't read as a controller button; small white mark in the right panel's top-right corner; "MGS 2 AND 3" in all caps. | Standard button glyph; check the panel corner. Name casing comes from metadata (optional). |
| P2 | Storage overview | "Partial downloads: 1 item, 0 B" reads oddly. While measuring, "Measuring your drives..." appears both under the page title and as the Measure again subtitle. | Hide empty partials or word it plainly; show measuring status in one place. |

### Store walkthrough (2026-11-20, Claude)

Nothing was downloaded, installed, queued, launched or changed.

| Priority | Screen/state | Observed | Expected / suggestion |
| --- | --- | --- | --- |
| P1 | Store › XBLA › All (full list); search results | Footer says "B Store", but B goes back to the A-to-Z picker. | "B Back", or name where B really goes. |
| P1 | Game details from Home (X View game) with Versions open | Opening the Versions panel squeezes the page behind it: title "Sonic .", info "SEGA · Sega · .", rating "3." (truncation bug). | Modal overlay that never re-lays out the page (see the comparison below). |
| P1 | Homebrew item page (e.g. 3DOx) | "A Install" installs straight away; page title is the row name "Emulators", not the item; bottom line "Installs to Hdd1:\Emulators\3DOx". | Item name as title; plain destination ("Installs to your hard drive") or none. |
| P1 | Store game page, Title updates button focused | Footer "A Find title updates" but A opens the installed-updates dialog; button says "Title updates". | Footer matches the button. |
| P1 | Title updates › Find more online, nothing found | "Neither XboxUnity nor archive.org has a title update for this game." Green pill "OK", no footer hints. | "No updates found for this game."; "A OK" at the bottom; same button style as other dialogs. |
| P1 | DLC dialog, installed item focused | Focus is only a slightly lighter fill, no white outline (the button gets one); no scroll cue though "7 items" don't all fit, and the "Installed" header scrolls away. | Standard white focus outline; scroll cue or "x of 7". |
| P1 | Every centered dialog (DLC, Title updates, DLC search, no results) | Dialog hints inside the dialog plus the dimmed page footer ("A Find DLC  B Back  Y Versions") still visible: two footers. | One screen-bottom footer with the dialog's actions (extends the existing dialog-footer finding). |
| P2 | Store landing rows; Homebrew emulators row | Truncation bug: "Call of Duty.", "Grand Theft.", "Need for Sp.", "Amiga360 Refresh 2026 - Feature Edition (Ami." | Fix the shared truncation. |
| P2 | Full list and search, focused tile | Scrolling name hard-cut at both edges ("0 Day Attack on Eart", "rs Racing Transfor"). | Fade the edges or pause before scrolling. |
| P2 | Category names | Tiles "XBLA", "XBLIG"; page title "Xbox Live Arcade". Jargon, and inconsistent. | One plain name everywhere: "Arcade", "Indie games", "Original Xbox", "Homebrew". |
| P2 | Genre naming | "Action & Adventure" (hero, game page) vs "Action and adventure" (row header). | One form. |
| P2 | Full list header vs A-to-Z picker | Picker "#", header "All games · 0-9"; opened from "All" the header still names a letter. | Same symbol in both; letter shown only as position. |
| P2 | Category page (XBLA) | Subtitle "743 games from archive.org"; bottom half empty; no "Y Search". | "743 games"; add Y Search. |
| P2 | Homebrew page | Subtitle "From the ConsoleMods repo, as Aurora's Homebrew Store lists it"; a row named "Homebrew" inside the Homebrew page; footer "A View" vs "View game" elsewhere. | Drop the subtitle; rename the row; one verb for A. |
| P2 | Homebrew rows | Off-screen text tiles have no name until scrolled in; most lack art; two identical "DashLaunch" tiles. | Always show names; tell duplicates apart (version or author). |
| P2 | Puzzle and trivia row | No art on any tile; "Scene It Bright Lights! Big Screen!" twice in a row. | Remove duplicates or label editions. |
| P2 | Store landing, A label | "View game", "Browse all", "Browse", "View collection", "View" on different tiles. | Varying by target is fine; "Browse" vs "Browse all" should be one word. |
| P2 | Store landing rows | No "See all" at the end of Collections, Popular or genre rows. | Optional "See all" tile. |
| P2 | Store game page info line | "SEGA · Sega · Action & Adventure" repeats publisher/developer in different case. | Show one when they match. |
| P2 | Store game page layout | Description is a narrow left column on Sonic the Hedgehog, a full-width line on Sonic Generations. | One layout. |
| P2 | Store game page buttons | "Find DLC" beside "Title updates" (verb vs none); both dialogs are titled with the game name, not "DLC" / "Updates". | Parallel labels; title each dialog by what it is. |
| P2 | DLC dialog | Subtitle "7 items installed, 2.21 MB. Manage on the game's page removes DLC." is two ideas; rows end in "Hdd1". | "7 installed · 2.21 MB"; drop the removal tip; friendly drive name or none. |
| P2 | DLC › Find more online | Raw file name "Sonic.The.Hedgehog.DLC.RF.X360-ZTM.rar", "100% name match"; long subtitle about archive.org. | Cleaned-up name and size; "Best match" instead of %; subtitle "Items you already have are skipped." |
| P2 | "Find more online" button | Label pushed right by blank space, as if an icon is missing. | Align like other buttons, or add the icon. |
| P2 | Versions rows | Tiny dim language codes "En, Ja, Fr, De, Es, It" (Japan row has none); checkbox meaning unclear. | Spell out or drop languages; clear "Installed" tag. |
| P2 | Store game page, More like this focused | Footer still shows "Y Versions" for the parent game. | Hide Y there. |
| P2 | Uninstalled store page | No Y Versions before install; picking a version before installing a multi-version game not checked. | Check a multi-version game before install. |
| P2 | Search keyboard | Blank white panel for ~1 s before the system keyboard draws. Results: "Search · 21 games · 1 of 21", footer "A View game  B Store  Y Search again  LB/RB Page". | Dark placeholder if possible. |
| P2 | Back from a related game | Search → Sonic → More like this → Sonic Generations, then B ×3 landed on Home, skipping the earlier pages. | Confirm the intended back history. |
| P2 | Home › X View game | Opens details inside Games + Apps; B goes to the Games grid, not Home. | Return to Home when opened from Home. |

Good: focus returns to the button that opened a dialog; B from DLC search returns to the DLC dialog; landing footer complete ("A View game  B Home  Y Search"); full list shows "1 of 743" with LB/RB and LT/RT hints.

#### Versions vs DLC vs Title updates

| | Versions (Y) | DLC (Find DLC) | Title updates |
| --- | --- | --- | --- |
| Placement | Right-side panel, ~45% wide, full height; page stays visible (and gets squeezed on the library page) | Centered dialog over dimmed page | Centered dialog over dimmed page |
| Title | "Versions" | Game name | Game name |
| Subtitle | None | "7 items installed, 2.21 MB. Manage on the game's page removes DLC." | "No title updates are installed for this game." |
| List | Region in bold + tiny language codes, size, checkbox | "Find more online" button, "Installed" header, rows with icon, name, size · Hdd1 | "Find more online" button only (when none installed) |
| Focus marker | White outline + green check on chosen version | White outline on button; rows only a lighter fill | White outline on button |
| Footer | "A Choose  B Done" inside the panel | "A Select  B Back" inside the dialog, page footer still visible | Same as DLC |
| Default focus | Current version | Find more online | Find more online |
| Close | B "Done" | B "Back" | B "Back" |

**Recommended single design:** one centered dialog for all three. Title says what it is ("Versions", "DLC", "Updates") with the game name small; one short subtitle only for a count or size; same row style (name left, size right, status tag in one place); the same white focus outline on rows and on "Find more online" as the first row; scroll cue when it overflows; one screen-bottom footer with the dialog's actions and the page footer hidden; B always "Back"; the page behind dims and never re-lays out.

### Queue and dialogs walkthrough (2026-11-20, Claude)

Nothing was removed, deleted, exited, launched or changed. Two checkboxes were ticked only to reach confirmations, then cancelled and cleared.

| Priority | Screen/state | Observed | Expected / suggestion |
| --- | --- | --- | --- |
| P1 | **Every confirmation** (`ShowConfirmUI` → `ShowAlertUI` → `RunAlert`, `src/GameListUI.cpp`) | Focus opens on the confirm button: seen on "Delete partial downloads?" (Delete), Leftover and Manage game "Remove from your drives?" (Remove). Confirmed in source: `RunAlert` starts at button 0 and `ShowConfirmUI` puts the confirm label first, so Files delete, Stop downloading/installing, Remove keys and Remove folder do the same. A double A press destroys data. | Open on Cancel; fix once in `RunAlert`/`ShowAlertUI`. Replaces the single partial-downloads finding. |
| P1 | Exit Omni360? (B on Home) | Buttons "Turn off", "Restart", "Exit"; focus on Exit; no Cancel; no hints, only the dimmed page footer. Turn off and Restart have no second confirmation. B cancels. | Add Cancel and start there; show "A Select, B Cancel". |
| P1 | Exit dialog wording | "Exit goes back to the dashboard. Restart and Turn off do that to the console." Title asks about exiting, but two buttons power-cycle the console. | Title "Leave Omni360" or "Power"; "Go back to the Xbox dashboard, restart the console or turn it off." |
| P1 | Leftover DLC and updates › Remove | "This removes Marvel: Ultimate Alliance 2 and frees 157 MB." reads as removing the game; detail "Saves are kept. A game's DLC and updates on the same drive are removed with it." doesn't fit leftovers. | "This removes 2 DLC and 1 update for Marvel: Ultimate Alliance 2 (157 MB)." Leftover-specific note or none. |
| P1 | Queue, Y | Arms Night mode (turns the console off when downloads finish) with no footer hint and no confirmation. | Show "Y Night mode" while something downloads; confirm or show a clear armed state. |
| P1 | Queue toast | "Nothing is downloading / Night mode turns the console off when the queue empties. It." (truncation bug; full text ends "It's empty already."). | Wrap toast text to two lines. |
| P1 | Queue recent tiles | Unfocused names "Left 4 Dead." (drops the "2", looks like another game), "Marvel: Ulti.", "Naruto: Ris."; focused bold label overhangs the tile's left edge. | Truncation fix; never drop a sequel number. |
| P1 | Queue recent tiles, X | Footer "X Remove, B Home"; X clears the entry instantly. X means "View game" on identical-looking Home tiles. A does nothing. | "X Clear from list" (it doesn't uninstall) or move to Y; consider "A View game". |
| P1 | Manage game, loading | "Loading..." with the sidebar drawn and no footer; then the layout jumps to full width without the sidebar. | "B Back" while loading; same layout before and after. |
| P2 | Home, BACK | Opens a "System" panel (IP, Signed in, Kernel, Omni360, FTP, temperature bars). Its hints are inside the panel bottom-left ("B Close, X Fahrenheit"), unlike every other dialog. BACK isn't hinted anywhere; does nothing on Settings or Games. | Hint it or document it; one footer position for all dialogs; "Kernel" → "Console software"; "X Show °F". |
| P2 | Home, START | Nothing on the shelf; on a disc tile it starts a disc install (source, `HomeTileAction`). | Make sure the disc tile shows a START hint. |
| P2 | Queue header | Subtitle "3 installed" repeats the "Recently installed" row below. | Drop it when only recents exist. |
| P2 | Queue vs Storage | Queue shows "Marvel: Ultimate Alliance 2" with box art as recently installed; Storage lists it as "No game installed · 2 DLC · 1 update". | Label DLC/update installs as such in Queue ("· 2 DLC"). |
| P2 | Queue empty state (source) | "DLC and title updates you choose will wait here while they download." Games queue too. | "Games, DLC and updates you download wait here." |
| P2 | Partial downloads confirmation | "This frees 0 B."; "A download started again begins from the start." | Hide the action at 0 B; "If you download it again, it starts over." |
| P2 | Manage game from Leftover | Help still says "Remove a single DLC or update, or just the game and keep its extras." with no game present; the same sentence repeats as the confirmation detail. | Context-specific help; don't repeat it in the confirmation. |
| P2 | Selection wording | Storage lists "A Deselect", Files "X Unselect"; Files' Y menu has "Select all" but no clear. | One verb ("Deselect"); add "Clear selection" to Files. |
| P2 | Leftover / Free up space footer | "Manage game" glyph is grey and is actually START. | Standard START glyph. |
| P2 | Files › Y menu | Title is the item name ("Apps") even with focus elsewhere; Copy uses a file icon, Move and New folder share a folder icon; Delete has no icon or caution styling; Copy/Move offered when the other pane shows only Drives. | Title "1 item"; distinct icons; caution Delete; disable Copy/Move without a target folder. |
| P2 | Files, marked row | Tick replaces the folder icon. | Keep the type icon; show the tick separately. |
| P2 | Files, inside a drive | Footer complete ("A Open, X Select, Y Actions, B Back"); top helper still clipped ("Y acts on what's selecte"); drives "Hdd1:", "HddX:", "Usb0:"; title "Files" vs button "Browse files". | Friendly drive names (HddX is jargon); drop or shorten the helper now the footer is complete. |
| P2 | Files, inactive pane | Keeps a grey outline: two outlines on screen. | Fainter, as in the library finding. |
| P2 | Measure again (pressed once) | Footer drops to "B Back"; status shows in header and row. | Status in one place. |
| P2 | Return paths | Walking the sidebar down to the storage ring passes Homebrew and switches page, so B from Storage lands on Homebrew. View game from Home → B lands in the Games grid; details highlight "Games" in the sidebar. | Return to where the user came from; the ring shouldn't switch pages when passed over. |
| P2 | Game details footer | "A Play, B Back, Y Versions": B in the middle (last elsewhere); button "Manage" vs hint "A Manage game"; "SEGA · Sega". | B last; match labels; dedupe publisher/developer. |
| P2 | Manage game / Leftover right panel | Small white mark at the panel's top-right corner. | Check the corner rendering. |

#### Dialogs seen

| Dialog | Default focus | Buttons | Hints | B |
| --- | --- | --- | --- | --- |
| Exit Omni360? | **Exit** | Turn off, Restart, Exit (no Cancel) | None; dimmed page footer | Cancels |
| System (BACK on Home) | Nothing focusable | None | Inside panel, bottom-left: "B Close, X Fahrenheit" | Closes |
| Night mode toast (Queue, Y) | n/a | n/a | n/a; body clipped | Times out |
| Delete partial downloads? | **Delete** | Cancel, Delete | None; dimmed page footer | Cancels |
| Remove from your drives? (Leftover, Manage game) | **Remove** | Cancel, Remove | None; dimmed page footer | Cancels, selection kept |
| Files › Actions (Y) | Copy to the other side | Copy, Move, Rename, Delete, New folder, Select all | Inside dialog plus dimmed page footer | Closes |

Three different hint patterns: confirmations show none, menus and pickers show them inside the dialog, and the System panel puts them bottom-left.

## Fix first

Ordered by risk, then reach:

1. **Confirmations open on Cancel**, including Exit (add Cancel). One change in `RunAlert`/`ShowAlertUI` covers every delete, remove, stop and power action. **Done in 0e985de**, checked on the console.
2. **Fix the shared truncation** (word boundary + ellipsis, or wrap): settings help lines, toasts, tile names, store rows, the squeezed Versions page. Dozens of instances; some mislead ("Left 4 Dead."). **Done:** XUI's ellipsis style cut mid-word and drew "…" as a single full stop in Selawik. `UiFont::DrawText` now shortens the text itself, ending at a word with "..." (checked on the console: "…to see what slows..."). Places that are simply too narrow (the Versions page) still need layout work under item 3.
3. **One dialog component and one footer rule**: centered dialog, page footer hidden, the dialog's own actions in a single screen-bottom footer. Make Versions, DLC and Title updates the same dialog (see the comparison under Store). **Done (b11, checked on the console):** every dialog drawn by `RunAlert` (confirmations, choices, DLC, Title updates, pickers, Files actions), the progress dialog and the System panel now put their hints in the screen's footer, over a dark band that hides the page's own footer. Button dialogs gained hints: "A Select, B Cancel", or "A <label>" for a lone button. Versions is no longer a side panel: Y opens the same list dialog as DLC (game name, "Install gets the ticked one.", tick on the chosen version, languages and size under each). Still open from the comparison: titling the three by what they are ("Versions", "DLC", "Updates") rather than the game name, and the DLC subtitle wording - both need copy decisions (item 5).
4. **Footer labels that match what the button does**: "B Store" that goes back, "A Find title updates", carousels with no ◀▶ hint, "A Turn off" on the active language, Queue "X Remove", Queue "Y" Night mode. **Done (b16, checked on the console):** Store sub-pages say "B Back"; the game page says "A Title updates", hides "Y Versions" on More like this, and puts B last; Settings value rows show a new D-pad ◀▶ "Change" icon and A no longer acts on them (Install disc games as used to toggle); Settings buttons name their action in the footer ("A Change", "A Unpair", "A Run test"...) instead of "A Select"; the language in use has no A hint, the others "A Use"; Settings footer adds "LB RB Section"; Queue X reads "Clear from list"; the sidebar storage ring says "A Storage". Queue "Y Night mode" already shows while something downloads. New from this pass: on a Settings › Storage folder row, Left changes the folder type, so the sidebar can only be reached from a row without a value (P2, focus trap).
5. **Plain names, used the same everywhere**: drives ("Hard drive", "USB drive 1" instead of Hdd1/HDD1/Usb0/HddX), store categories (no XBLA/XBLIG), folders not paths, "Kernel", "Re-sync", "Assets", raw file names and match percentages.
6. **Trim subtitles and help lines** that repeat the label or the footer (Storage's left buttons, Queue "3 installed", Homebrew and category subtitles, DLC subtitle).
7. **Return focus to where the user came from** (nav icons, Home after View game, the storage ring not switching pages).

### Batch 1 for review (b17, 2026-11-21)

Fix-first items 5-7 plus open P2s, in one build. Naming choices are the user's (2026-11-21); drives keep their real names (Hdd1:, Usb0:) and only get consistent casing. **Checked on the console by Claude:** Settings › Storage rows, folder-row Left, Storage buttons, return to the Settings icon, View game from Home then B. The rest is in the code and built, for the user to look at first.

| Where | What changed |
| --- | --- |
| Everywhere | "HDD1:" and "hdd1:" now show as "Hdd1:" (also "Usb0:", "HddX:"); the sidebar ring says "Hdd1" |
| Store | Tiles and page titles "Arcade" and "Indie games"; category subtitle "743 games"; Homebrew page subtitle gone |
| Game page | Button "DLC" beside "Title updates"; "SEGA · Sega" shows once |
| DLC / Title updates / Versions dialogs | Titled "DLC · Game", "Title updates · Game", "Versions · Game"; DLC subtitle "7 installed · 2.21 MB"; online search "Find DLC · Game"; help lines no longer say what A does |
| Title updates, nothing found | "No updates found for this game." |
| Settings › General | "Archive.org keys" (status just "Saved"); PC helper "Find PC"; disc speed help without "left and right change it"; Sign in at start ticks the current choice |
| Settings › Library (was Assets) | "Refresh game list", button "Refresh", "drives" not "disk" |
| Settings › Storage | "Manage storage" is a row with an "Open" button (no "Drives" heading); "Folders", "Add folder"; homebrew row says "1 app"; help lines drop "A picks…"; Left on a folder row's first type goes to the sidebar |
| Settings › Language | "Time format" (was Clock); "Reload" (was Refresh) |
| Settings › About | "Console software 2.0.17559" replaces Version info (the version is on Updates) |
| Folder pickers | Inside a folder, focus starts on the first subfolder, not "Use this folder"; "Download location" title |
| Storage | Left buttons have no subtitles except counts; "Rescan drives"; partials with 0 B say "1 item" |
| Leftover DLC and updates › Remove | "This removes the DLC and updates for <game> and frees 157 MB."; no "Saves are kept" note |
| Exit (B on Home) | Title "Leave Omni360", "Go back to the Xbox dashboard, restart the console or turn it off." |
| System panel (BACK) | "Console software"; X "Show °F" / "Show °C" |
| Queue | No "3 installed" subtitle when only recents; empty state "Games, DLC and updates you download wait here."; Night mode toast reworded |
| Favorites › Y | Opens on Rename when the game is a favourite |
| Messages | One-button messages show "A OK" instead of "B Continue" |
| Speed test | Progress "Step 6 of 7" (no "B stops it"); when the helper is 30%+ faster than archive.org the verdict says to download through it |
| Return focus | B from a page opened from the top nav lands on that icon; a game opened with X on Home goes back to Home |
| Translations | en-ca regenerated; the machine-translated languages keep their old wording for changed lines until retranslated |

Not done in this batch: Cancel while pairing waits (needs the wait to stop mid-request), telling a PC-side timeout from a No, Manage game's loading screen (it blocks), the folder picker's leftover layer and one-press close, Files menu icons, Home BACK hint, Y Search on category pages, Language radio marks.

#### Found during batch 1 review (user, 2026-11-21)

| Priority | Screen/state | Observed | Expected / suggestion |
| --- | --- | --- | --- |
| Fixed (b18) | Settings › PC helper › Find PC | Takes over the whole screen: "Looking for your PC" progress (`ScanProgress` → `RenderStatusFrame`), then the result as a full-screen message (`ShowMessageUI`), in `ScanForPcHelper` (`src/main.cpp`). | Run it in a centered dialog over Settings, as other dialogs are: progress, then the result, in the same dialog with its hints in the screen footer. Pairing ("Allow it on your PC", `PairPcHelper`) uses the same full-screen frames and should move with it. |
| Fixed (b18) | Find PC › nothing found | Text on the "No helper found" result is cut off. The message ("Make sure Omni360 Helper is running on a PC on the same network as this Xbox.") and the detail line ("If it is, allow it through…") are drawn on single lines by the status frame, so the ends are cut. | Wrap the message and detail. Moving it into a dialog (above) should fix it; check both lines fit at 640x480. |

### Batch 2 for review (b18, 2026-11-21)

**Checked on the console by Claude:** Find PC (search progress and "No helper found" as dialogs over Settings, full text wrapped, focus back on Find PC), and the folder picker (going into Hdd1: and back leaves no leftover layer; focus starts on the first subfolder).

| Where | What changed |
| --- | --- |
| Settings › PC helper › Find PC | Runs in a dialog over Settings: a progress bar with "Checked 24 of 253 addresses", then the outcome as an OK dialog. No button hint while it searches, since nothing can be pressed |
| Pairing (after Find PC finds a new PC, or Pair) | "Talking to your PC" and "Allow it on your PC" in the same dialog; the bar shows the time the PC has left to answer; Paired / Not paired as an OK dialog |
| Full-screen messages everywhere | The message wraps to up to three lines and the detail to two, and the card grows to fit, instead of cutting the end off with "..." |
| Folder pickers (Add folder, Download location) | Every level draws over the page it was opened from, so going back up leaves no dimmed copy behind |
| Store category pages (Arcade, Indie games, Original Xbox) | Y Search, as on the Store's front page; B from the results comes back to the category |
| List dialogs (DLC, Title updates) | Installed rows that can't be picked now get the white focus outline too |
| Grids, Home shelf, Files | The remembered position (while the sidebar, top nav or other Files pane has focus) is a much fainter outline, so there's one clear focus on screen |

Not changed: the Homebrew item card already carries the item's name (the header shows its kind); "Find more online" is indented to line up with the labels of rows that have icons.

### Batch 3 for review (b19, 2026-11-21)

**Checked on the console by Claude:** BACK hint on Home, Settings › About skipping the Console software row, Language's round marks.

| Where | What changed |
| --- | --- |
| Home | Footer adds "BACK System" (new BACK button icon: START's twin, arrow pointing left) |
| Home over light wallpapers | Stronger shade behind the top bar and the "Continue playing" line |
| Settings › Language | Languages use round marks (one at a time) instead of checkboxes |
| Settings rows that only tell you something | Passed over by up/down (Console software in About) |
| Queue › Recently installed | A DLC or update install is named "DLC: Game" / "Update: Game", so the game's box art doesn't read as the game installing (new installs only; ones already listed keep their names) |
| Files | A ticked row keeps its folder or file icon, turned green, beside the green edge |
| Storage › Partial downloads | With nothing downloaded yet, the confirmation says so instead of "This frees 0 B" |
| Manage game from Leftover DLC and updates | Note says the game isn't installed and these are its DLC and updates; the remove confirmation says "Saves are kept." instead of repeating the note |

Already covered, no change: Night mode has its own screen when armed (the indicator the finding asked for); scrolling names already fade at the edges; the Emulators help now says scanning shows it in Emulators; Files says "Deselect" like Storage; the grey glyph on "Manage game" is the controller's own START button.

Still open after batch 3: Cancel while pairing waits and telling a timeout from a No (need the wait to stop mid-request); Manage game's blocking loading screen; Files menu icons and a caution style for Delete (no icons for them yet); Y Options (Rename) on Apps and Homebrew; walking the sidebar to the storage ring switches pages; Versions rows' language codes; Store data duplicates (Scene It, DashLaunch); description layout differences between game pages.

### Found while checking the speed test and PC helper (2026-11-20, Claude)

| Priority | Screen/state | Observed | Expected / suggestion |
| --- | --- | --- | --- |
| Fixed | Settings › PC helper › Scan | "Scan found no helper" although the helper answered `/hello`. Cause: each batch waited 700 ms, and a PC on Wi-Fi that had dozed off answered later than that (the same helper answered in 7 ms on another scan). **Fixed:** 2.5 s per batch; found 3 of 3 times on the console, against 0 of 3 before. | Done. |
| P2 | PC helper after the PC's address changes | The saved `helper-url` only updates once the helper is heard or found, so a new DHCP address leaves it "not running" until Scan is pressed. Broadcasts from an old helper build (11-16) didn't reach the console; the current build's do. | Scan now recovers it. Optionally scan once automatically when a paired helper hasn't been heard for a while. |
| Fixed | Settings › PC helper row | The row kept its old state (e.g. "Paired · DESKTOP-…" with no Scan) after the helper stopped, even after reopening Settings, until another sidebar section was visited; the clock icon updated correctly. "No helper found" was also shown while the clock icon said connected. **Fixed:** while General is open, the row and its buttons are rebuilt whenever the helper's state or name changes. | Done. |
| Fixed | Speed test › Stop | Pressing B during "Checking archive.org (3 of 4)" showed "Stopping…" for about 3 minutes with the whole screen frozen (clock stuck), then returned. Cause: the archive.org connections used the HTTPS client's 2-minute read timeout, and only hear Stop when data arrives, so one stalled connection held everything. **Fixed:** those connections time out after 6 s, and the dialog closes within 1.5 s of B; anything left finishes stopping in the background (Run test again waits for it, B backs out). | Done. |
| P2 | Speed test verdict | With "Through the PC helper 11.3 MB/s" beside "archive.org 2.6 MB/s", the verdict still says "…so this is as fast as it gets." | When the helper is faster, say downloads go faster through it. |
| P2 | Speed test progress | "Step 6 of 7 - B stops it" repeats the "B Stop" hint below it. | Drop "B stops it" from the line. |
| P2 | "No helper found" | Message clipped: "running on a PC on the same netwo."; footer "B Continue". | Truncation fix; "B Back" or "A OK". |
| P2 | Pairing wait ("Allow it on your PC") | No footer at all, so no visible way out. The helper's PC prompt expires after 2 minutes and counts as No; the console then says "Pairing wasn't allowed on your PC", which reads as if the user refused. | Show "B Cancel"; say "Nobody answered on the PC in time" when it timed out. |
| P2 | Unpair | Unpairs at once, no confirmation. | Fine, since pairing again is easy; noting it. |
| P2 | Title updates dialog, long game name | "Hasbro Family Game Night 4: The Game ." (truncation bug). | Truncation fix. |

Checked and working: the speed test's PC helper row (11.3 MB/s with a current helper; "Update the helper to check" with one built before `/speed`); Title updates on a Store entry with no title ID (opens the dialog; "Find more online" reports nothing found, no crash).

**Capture correction (2026-11-20, Claude):** the first window captures were cropped on the right and bottom by a DPI-scaling bug in the capture script (fixed in `tools/capture_window.ps1`). With the full frame, the Storage file browser footer shows **A Open, B Back** (not only A Open), and the Storage overview shows A Select, B Back. Earlier footer omissions recorded from cropped frames need rechecking. The file browser's top helper line is genuinely clipped: "Y acts on what's selecte".

The observed Home navigation omission and the Apps sidebar blank footer conflict with their source hint branches. Check whether the projector is running the current executable before treating those specific observations as render defects. The confirmed Homebrew omission is present in the captured live state even though B works.

No implementation changes have been made during the audit. Continue recording all accepted actions and their footer labels, then fix shared footer rules so every available action is represented along the physical bottom edge, including while dialogs are open.
