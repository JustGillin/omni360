# Omni360 Setup Guide

Start-to-finish setup for a console you already have modded. Adapted from X-Store's original setup guide by 951261 — the transfer and Aurora advice is still theirs in substance; the rest describes a different application and has been rewritten.

## Prerequisites

**Required:**
- A soft-modded (BadUpdate, ABadAvatar, PeerPressure) or hard-modded (RGH, JTAG) Xbox 360.
- A working internet connection on the console. Ethernet recommended — game downloads are several gigabytes.
- A free Internet Archive account. Every collection Omni360 downloads from is marked private, so downloading needs you signed in. Browsing the Store works without it.
- Room on the drive your games go to, if you'll install games: while a game installs, both its download (around 7GB) and the installed game (3-8GB) are on the drive. The download is removed once it's installed.

**Not required:**
- Xbox Live
- Stealth server
- A PC, after the initial file transfer. That is the whole point of the app.

## 1. Get the XEX

Use a released `Omni360.xex`, or build it: `XboxTLS2.vcxproj` from `Omni360.sln`, in Visual Studio 2010 with the official Microsoft Xbox 360 XDK. The Release configuration produces `Omni360.xex`. See `COMPILING.md`.

## 2. Signing in to Internet Archive

Nothing to prepare: the first time Omni360 starts, it asks you to sign in, three ways:

- **With your phone** — scan the QR code on screen and sign in on the page it opens. The page only exists while the question is on screen, and only on your home network.
- **On the console** — your email and password, typed with the controller.
- **With keys** — your IAS3 access key and secret key from [archive.org/account/s3.php](https://archive.org/account/s3.php), the pair Internet Archive's own `ia` tool uses. The phone page takes them too, pasted.

Or choose **Not now**: browsing works without signing in, and Omni360 asks again when you download something. You can sign in, change account or sign out any time from the top of the Store or **Settings → Internet Archive**.

Your password is only sent to Internet Archive, and never saved. What Omni360 keeps is the account's keys, in `ArchiveOrgKeys.txt` beside the XEX. You can also make that file yourself on a PC — access key on line 1, secret key on line 2 — and copy it over with the XEX.

**That file is plain text.** That is fine on a console only you use. Delete it if anyone else has physical or FTP access — and never share it, or a build folder with it inside.

## 3. Folders: where things go, and where Omni360 looks

The app works with no settings file. Everything here is set on the console in **Settings → Library**, which writes `settings.txt` for you. The page has two parts.

**Where new things go** has one row for each kind of thing Omni360 installs. **A** on a row changes it:

| Row | Default | What goes there |
|---|---|---|
| **Games** | `Hdd1:\Content\0000000000000000` | Store downloads and disc installs |
| **Arcade, DLC & updates** | the Content folder of the Games drive | Arcade and indie games, DLC, title updates |
| **Apps**, **Homebrew**, **Emulators** | `Hdd1:\Apps`, `Hdd1:\Homebrew`, `Hdd1:\Emulators` | Installs from the Store's Homebrew section |

**Arcade, DLC & updates** asks for a drive, not a folder: the console only finds them in a drive's `Content\0000000000000000` folder, so that's where they go. A row's folder doesn't have to exist yet. "(made on first install)" means Omni360 creates it the first time something installs there.

**Where Omni360 looks** lists every folder that's read for your games and apps:

- Every drive's **Content** folder, and every folder from **Where new things go**, is always looked in. These say **Always looked in**, and the help line underneath says why. They can't be removed here; change the row above instead.
- **Add folder** adds any other folder on the hard drive or a USB drive, and asks what's in it: games, homebrew, apps or emulators. **A** on a folder you added opens its options (**Scan depth**, **Change folder**, **Remove**), **X** removes it, and left and right change what it holds. Removing a folder only takes it off the list; nothing on the drive is deleted.
- One folder can be looked in for more than one kind. Its row counts each, like "48 games · 2 apps".

Folders are scanned for Games on Demand, arcade games and installed packages (the `{TitleID}\{ContentType}\...` layout, even when it's kept under folders of its own like `Games\Xbox 360\Halo 3\...`), and for extracted games (a folder with a `default.xex`). Apps are folders holding `default.xex` or `default.xbe`, or just one other `.xex`, such as Aurora's `Aurora.xex`.

**Scan depth** is how many folders down from a path things are looked for: 0 means the folder is a game itself, 1 means games are in folders right inside it, and so on up to 6. It's 3 unless you change it, and a row only shows it when it isn't.

**AutoScan**, beside **Scan now**, reads your folders again each time Omni360 starts. Turn it off and Omni360 starts with the list from last time, which is quicker for big libraries; **Scan now** finds anything new. While a scan runs, the line under the clock shows how far it's got, and a message says what it found when it's done.

A drive the dashboard formatted as Xbox storage keeps its files in a container Omni360 can't reach. A USB drive formatted FAT32 is `Usb0:` or `Usb1:`, as in Aurora.

<details>
<summary><b>The settings.txt lines, for editing by hand</b></summary>
<br>

```
games-path: Hdd1:\Games                      # the first is where games install
games-path: Usb0:\Games|5                    # more folders of games; |5 is a scan depth
xbla-path: Hdd1:\Content\0000000000000000    # where arcade games, DLC and updates go
apps-install: Hdd1:\Apps                     # where each kind installs
homebrew-install: Hdd1:\Homebrew
emulators-install: Hdd1:\Emulators
apps-path: Usb0:\Apps                        # more folders of each kind, more than once each
homebrew-path: Hdd1:\Homebrew
emulators-path: Hdd1:\Emulators
library-autoscan: off
```

Without `xbla-path:`, arcade games, DLC and updates go to the Content folder of the drive `games-path:` names. Without an `-install:` line, a kind installs to the first of its `-path:` folders, or the default.

</details>

**Leaving.** **B** goes back to Home from any page; **B** on Home asks whether to exit, and leaving takes the console wherever it normally starts — whatever DashLaunch is set to. To reach another dashboard or a file manager instead, put it in your Apps folder and start it from the **Apps** page.

**Apps, Homebrew and Emulators.** These three pages show what's in those folders as tiles, like the library does, and **A** starts one. Each page appears in the **Games + Apps** sidebar once it has something to show.

**Homebrew in the Store.** The Store's **Homebrew** button lists the ConsoleMods repo's apps, homebrew games and emulators, the same list Aurora's Xbox 360 Homebrew Store shows. **A** shows an item's details, and **Install** adds it to the Queue. Each install goes to its kind's folder under **Where new things go**. The list is built into Omni360 for now; `tools/make_homebrew.py` explains how to update it.

ConsoleMods' bot protection used to refuse Omni360 outright. They now let it through by the user agent it sends, agreed with them in November 2026, so downloads work as they are — please don't change that agent. A "Refused by ConsoleMods" message means something has changed at their end; it's worth reporting.

<details>
<summary><b>Serving the archives from your PC instead</b></summary>
<br>

Useful for testing an install without going out to consolemods.org:

1. Download the items' `.7z` files from consolemods.org in your browser into one folder, keeping their names. The item's details page shows where it installs, and the names are in `tools/data/homebrew/*.ini`.
2. In that folder, start a web server: `python -m http.server 8000`. Allow it through Windows Firewall on your private network if Windows asks.
3. Add `homebrew-mirror: http://192.168.1.10:8000` to `settings.txt`, with your PC's address, and restart Omni360. The Homebrew page then says it downloads from there.

Remove the line to go back to downloading from consolemods.org.

</details>

## 4. Transfer files to the console

Copy `Omni360.xex`, plus `ArchiveOrgKeys.txt` and `settings.txt` if you made them, into a single folder on the console — something like `Hdd1:\Apps\Omni360\` or `Usb0:\Apps\Omni360\`. Keep them together, and do not put any other `.xex` in that folder. Omni360 keeps its caches beside them: `Covers\` and `Store\` for artwork, `Library.cache` for the game list it draws at startup, and `Recents.txt` for what Home shows. All four can be deleted safely; they rebuild.

Three ways to get them across:

- **USB drive** — easiest. Copy to a FAT32 drive from your PC or phone, then either run from the USB or use a file manager (Aurora, XEX Menu) to move the files to the internal HDD.
- **FTP** — copy straight to the HDD or a USB over the network.
- **Xbox 360 Neighborhood** — works, but is prone to crashing and failed transfers. Not recommended.

## 5. Point Aurora at the right folders

If you use Aurora, add the paths you configured to **Settings → Content → Path**, and set scan depth to at least 4. Without this, installed games and downloaded content will not show up in Aurora even though they installed correctly. With the defaults, that's `Hdd1:\Content\0000000000000000`.

While you are there, make sure Aurora can see `Omni360.xex` so you can launch it from the home screen.

## 6. Run it

Launch `Omni360.xex` from a file manager or your dashboard's home screen.

1. The app mounts drive aliases, then scans your library and reads each title's name and icon out of its package header. This is offline and takes a moment on a large library; covers fill in over the next few seconds, and are cached after the first run.
2. **Home** is where it starts: the games you played most recently, and a row of icons across the top for **Games + Apps**, **Store**, **Queue** and **Settings**. Move with the **D-pad or left stick**, **up** from the games to reach the icons. **B** goes back to Home from anywhere, and **B** on Home asks whether to exit.
3. **Games + Apps** has a sidebar of its own — **Games**, and **Apps**, **Homebrew** or **Emulators** once those have folders. Left from the first column reaches it. The Store, the Queue and Settings have no sidebar; they open full width.
4. **To install a game**, open the **Store**, pick a letter, then a game. Its page has its description, screenshots and versions — USA and World versions first, and a multi-disc game's discs together as one version. Choose one in the list if you want a different version, then press **Install**. It joins the **Queue**: a typical game takes 30-40 minutes to download and 5-10 to install, and you can keep browsing meanwhile. A popup says when it's done.
5. **For DLC or title updates**, open a game's page — from the Store, or **X** on a game in Games or on Home — and press **Find DLC** or **Title updates**. It searches Internet Archive by name and shows what it found, ranked. **Nothing downloads automatically** — you pick the pack yourself, and anything already installed is marked as such.
6. **To install a disc**, put it in the drive: it shows up as the first tile on Home and in Games. **A** installs it as Games on Demand; you confirm its name, it checks there's room, and it copies the disc to your games folder.
7. **To remove a game**, press **Uninstall** on its page. Its DLC and title updates stay.
8. **On the Queue**, **X** stops a download or install (nothing is left behind), or clears a finished one.

Because DLC matching is deliberately loose — it has to cope with release-group naming like `Halo.1.Combat.Evolved.Anniversary` — read the filename before confirming. That check is the reason auto-selection was removed.

## Troubleshooting

`game:\DebugInfo.txt` is the log, and it keeps recording even once the drawn UI takes over the screen. Check it first.

- **Empty game list** — the games folder is pointing somewhere without games. The empty list shows which folder it searched; change it under **Settings → Library → Where new things go**, or add the folder your games are in under **Where Omni360 looks** (step 3).
- **"Keys not accepted"** — Internet Archive refused your keys. This usually means keys put in `ArchiveOrgKeys.txt` by hand are wrong or in the wrong order (access key line 1, secret key line 2), or were reset at archive.org. Sign in again in **Settings → Internet Archive**.
- **"Not enough space"** — the drive needs room for the download and the installed game at once, as in Prerequisites. Free some space, or point the games folder at a bigger drive.
- **A download slows down, or seems to pause** — Internet Archive's servers stall every minute or so; Omni360 drops a stalled connection after 10 seconds and carries on where it stopped, and moves to another server if one keeps failing. The speed on screen is the average, pauses included.
- **A game isn't in the Store** — Internet Archive doesn't have every game. A few dozen aren't there at all; about 20 more are only there as compressed RARs, which install but take far longer, and the Store asks before starting one.
- **Home's Recent row is empty** — it fills as you start games from Omni360. A game started from another dashboard isn't recorded, and a game that's since been uninstalled drops out.
- **No DLC or updates for a game you know has them** — the collection may genuinely not have them, or the game's display name may be too abbreviated to match. Not every title has DLC or updates archived.
- **Game pages with no description, wallpaper or screenshots** — they come from Xbox Live's servers, and DashLaunch's `livestrong` setting blocks those (the log shows `[dns] download.xbox.com: the DNS server says there's no such name`). Box art still comes from xboxunity, and downloads from Internet Archive are unaffected. Turn `livestrong` off in `launch.ini` if you want the details; `liveblock` on its own doesn't block them.
- **Nothing will connect, but Aurora can** — look for `WSA error 10060` in the log: every connection is timing out, so something between the console and the internet is dropping the app's connections. Open an issue on GitHub with the log; it helps to say how the console is modded and which plugins it runs.
- **Garbled game names** — STFS display-name encoding. `StfsParser.cpp` detects UTF-8 versus UTF-16BE per file and re-encodes both to UTF-8. If a name still looks wrong, that is worth reporting rather than expected.

A game download picks up where it stopped after a stall, but not across a restart: quitting the app mid-download removes what was downloaded, and it has to be started again. DLC and title update downloads that fail have to be retried by hand.
