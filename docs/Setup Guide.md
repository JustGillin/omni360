# Omni360 Setup Guide

Start-to-finish setup for a console you already have modded. Adapted from X-Store's original setup guide by 951261 — the transfer and Aurora advice is still theirs in substance; the rest describes a different application and has been rewritten.

## Prerequisites

**Required:**
- A soft-modded (BadUpdate, ABadAvatar, PeerPressure) or hard-modded (RGH, JTAG) Xbox 360.
- A working internet connection on the console. Ethernet recommended — game downloads are several gigabytes.
- An archive.org account. Every collection Omni360 downloads from is marked private, so an anonymous request gets a hard 401 — the keys below are genuinely required, not optional.
- Room on the drive your games go to, if you'll install games: while a game installs, both its download (around 7GB) and the installed game (3-8GB) are on the drive. The download is removed once it's installed.

**Not required:**
- Xbox Live
- Stealth server
- A PC, after the initial file transfer. That is the whole point of the app.

## 1. Get the XEX

Use a released `Omni360.xex`, or build it: `XboxTLS2.vcxproj` from `Omni360.sln`, in Visual Studio 2010 with the official Microsoft Xbox 360 XDK. The Release configuration produces `Omni360.xex`. See `COMPILING.md`.

## 2. Get your archive.org API keys

Log in to archive.org, then visit [archive.org/account/s3.php](https://archive.org/account/s3.php) and copy your **IAS3 access key and secret key**. These are archive.org's official script-friendly credentials — the same pair their own `ia` command-line tool uses. They are *not* your account password.

Create a plain text file with the access key on line 1 and the secret key on line 2. You will transfer it as `ArchiveOrgKeys.txt` alongside the XEX.

Doing this on a PC is strongly recommended over the alternative, which is typing a roughly 40-character secret key on an on-screen keyboard with a controller. You can still do it on the console: until keys are saved, the library shows a banner pointing you to **Settings**, where you can add, change or remove them. Keys entered there are checked with archive.org before they're saved, to the same file.

**This file is plain text either way.** That is fine on a console only you use. Delete it if anyone else has physical or FTP access — and never share it, or a build folder with it inside.

## 3. Optional: settings.txt

The app works with no settings file. Two keys override its defaults, and they are separate because your game library and your content folder are often on different drives. The games folder can also be changed on the console, in **Settings**, which writes this file for you:

- `games-path:` — where games install, and where your library is scanned. Defaults to `Hdd1:\Content\0000000000000000`, which is where the dashboard keeps Games on Demand and arcade titles.
- `xbla-path:` — where DLC, title updates, arcade games and indie games are written. Defaults to `Hdd1:\Content\0000000000000000`. The name is inherited from X-Store, which used the same key.

If you keep your GOD games in a folder of their own instead — `Hdd1:\Games\{TitleID}\00007000\{ContentID}` is a common choice — point `games-path:` there.

**Games on more than one drive?** In **Settings**, under the games folder, each likely folder on the hard drive and any USB drive is listed — a `Content\0000000000000000` and a `Games` folder — and **A** adds it to your library or takes it out. **Add another library folder** takes any other path. Or add a `games-path:` line for each folder yourself: the first is where games install, and the others are looked in for your library too. A USB drive formatted FAT32 is `Usb0:` or `Usb1:`, as in Aurora:

```
games-path: Hdd1:\Content\0000000000000000
games-path: Hdd1:\Games
games-path: Usb0:\Content\0000000000000000
```

Each folder needs the `{TitleID}\{ContentType}\...` layout — Games on Demand, arcade games, installed packages. Extracted XEX games (a folder with a `default.xex`) aren't found yet. A drive the dashboard formatted as Xbox storage keeps its files in a container Omni360 can't reach.

**Leaving.** **B** goes back to Home from any page; **B** on Home asks whether to exit, and leaving takes the console wherever it normally starts — whatever DashLaunch is set to. To reach another dashboard or a file manager instead, put its folder on the **Apps** page below and start it from there.

**Apps, Homebrew and Emulators.** These three pages show what's in your apps, homebrew and emulator folders as tiles, like the library does, and **A** starts one. Each page is in the **Games + Apps** sidebar only once it has a folder. Each folder in **Settings** has a value on its right, **Games**, **Homebrew**, **Apps** or **Emulators**, that says which page it feeds. Left and right change it, the same way they change Disc read speed. **Add folder** asks what's in a new folder once you've typed its path. `Homebrew`, `Apps` and `Emulators` folders on each drive are suggested too. Settings writes `homebrew-path:` (homebrew games), `apps-path:` (dashboards, file managers, tools) and `emulators-path:` lines for you, and you can also add them by hand, more than once each:

```
apps-path: Hdd1:\Apps
homebrew-path: Hdd1:\Homebrew
emulators-path: Hdd1:\Emulators
```

Each app is a folder holding `default.xex` or `default.xbe`, or just one other `.xex`, such as Aurora's `Aurora.xex`. Omni360 looks up to three folders deep, so `Hdd1:\Emulators\Nintendo\FCEUX\default.xex` is found too. The folders are read when Omni360 starts and again whenever Settings changes them.

**Homebrew in the Store.** *Not switched on in releases yet: the button shows as SOON until ConsoleMods allows Omni360 to download from its repo. Set `HOMEBREW_STORE_OPEN` to `true` in `src/main.cpp` to build it with the page on.* The Store's **Homebrew** button lists the ConsoleMods repo's apps, homebrew games and emulators, the same list Aurora's Xbox 360 Homebrew Store shows. **A** shows an item's details, and **Install** adds it to the Queue. Each install goes into the first folder of its kind in Settings. If you have none yet, it uses `Hdd1:\Apps`, `Hdd1:\Homebrew` or `Hdd1:\Emulators`, which then becomes that kind's folder. The list is built into Omni360 for now; `tools/make_homebrew.py` explains how to update it.

**Downloads from consolemods.org don't work yet.** Its Cloudflare protection only lets Aurora's script through, so Omni360's requests are refused with "Refused by ConsoleMods" until ConsoleMods allows it. To test the whole install on the console in the meantime, serve the archives from your PC:

1. Download the items' `.7z` files from consolemods.org in your browser into one folder, keeping their names. The item's details page shows where it installs, and the names are in `tools/data/homebrew/*.ini`.
2. In that folder, start a web server: `python -m http.server 8000`. Allow it through Windows Firewall on your private network if Windows asks.
3. Add `homebrew-mirror: http://192.168.1.10:8000` to `settings.txt`, with your PC's address, and restart Omni360. The Homebrew page then says it downloads from there.

Remove the line to go back to downloading from consolemods.org.

## 4. Transfer files to the console

Copy `Omni360.xex`, `ArchiveOrgKeys.txt`, and `settings.txt` if you made one, into a single folder on the console — something like `Hdd1:\Apps\Omni360\` or `Usb0:\Apps\Omni360\`. Keep them together, and do not put any other `.xex` in that folder. Omni360 keeps its caches beside them: `Covers\` and `Store\` for artwork, `Library.cache` for the game list it draws at startup, and `Recents.txt` for what Home shows. All four can be deleted safely; they rebuild.

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
5. **For DLC or title updates**, open a game's page — from the Store, or **X** on a game in Games or on Home — and press **Find DLC** or **Title updates**. It searches archive.org by name and shows what it found, ranked. **Nothing downloads automatically** — you pick the pack yourself, and anything already installed is marked as such.
6. **To install a disc**, put it in the drive: it shows up as the first tile on Home and in Games. **A** installs it as Games on Demand; you confirm its name, it checks there's room, and it copies the disc to your games folder.
7. **To remove a game**, press **Uninstall** on its page. Its DLC and title updates stay.
8. **On the Queue**, **X** stops a download or install (nothing is left behind), or clears a finished one.

Because DLC matching is deliberately loose — it has to cope with release-group naming like `Halo.1.Combat.Evolved.Anniversary` — read the filename before confirming. That check is the reason auto-selection was removed.

## Troubleshooting

`game:\DebugInfo.txt` is the log, and it keeps recording even once the drawn UI takes over the screen. Check it first.

- **Empty game list** — the games folder is pointing somewhere without games. The empty list shows which folder it searched; change it in Settings, or see step 3.
- **"Keys not accepted"** — archive.org refused your keys. This usually means keys put in `ArchiveOrgKeys.txt` by hand are wrong or in the wrong order (access key line 1, secret key line 2), or were reset on archive.org. Re-enter them in Settings.
- **"Not enough space"** — the drive needs room for the download and the installed game at once, as in Prerequisites. Free some space, or point the games folder at a bigger drive.
- **A download slows down, or seems to pause** — archive.org's servers stall every minute or so; Omni360 drops a stalled connection after 10 seconds and carries on where it stopped, and moves to another server if one keeps failing. The speed on screen is the average, pauses included.
- **A game isn't in the Store** — archive.org doesn't have every game. A few dozen aren't there at all; about 20 more are only there as compressed RARs, which install but take far longer, and the Store asks before starting one.
- **Home's Recent row is empty** — it fills as you start games from Omni360. A game started from another dashboard isn't recorded, and a game that's since been uninstalled drops out.
- **No DLC or updates for a game you know has them** — the collection may genuinely not have them, or the game's display name may be too abbreviated to match. Not every title has DLC or updates archived.
- **Game pages with no description, wallpaper or screenshots** — they come from Xbox Live's servers, and DashLaunch's `livestrong` setting blocks those (the log shows `[dns] download.xbox.com: the DNS server says there's no such name`). Box art still comes from xboxunity, and downloads from archive.org are unaffected. Turn `livestrong` off in `launch.ini` if you want the details; `liveblock` on its own doesn't block them.
- **Nothing will connect, but Aurora can** — look for `WSA error 10060` in the log: every connection is timing out, so something between the console and the internet is dropping the app's connections. Open an issue on GitHub with the log; it helps to say how the console is modded and which plugins it runs.
- **Garbled game names** — STFS display-name encoding. `StfsParser.cpp` detects UTF-8 versus UTF-16BE per file and re-encodes both to UTF-8. If a name still looks wrong, that is worth reporting rather than expected.

A game download picks up where it stopped after a stall, but not across a restart: quitting the app mid-download removes what was downloaded, and it has to be started again. DLC and title update downloads that fail have to be retried by hand.
