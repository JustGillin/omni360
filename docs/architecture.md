# Architecture

The app is a pipeline with a text UI on the front and streamed file processing on the back. The high-level shape is:

```text
main.cpp
  -> showUI()
       -> downloadFileHTTPS(search page into memory)
       -> parse_vimm_search_results()
       -> downloadFileHTTPS(selected game page into memory)
       -> parse_vimm_media_ids()
       -> return final download URL, game name, download type
  -> getSettings()
  -> getGame()
       -> downloadFileHTTPS(archive into game:\tmp.7z.001)
       -> decompressSevenZipFile()
       -> XBLA copy OR extractIso()
```

## `main.cpp`

`main.cpp` is the orchestration layer. It does not perform TLS, HTML parsing, 7z decompression, or ISO traversal directly. Instead, it glues those modules together and handles the user-facing retry loop.

Important responsibilities:

- Deletes the previous log file at startup.
- Creates the ATG-backed text console with `MakeConsole("embed:\font", ...)`.
- Tries to ensure `game:`, `Usb0:`, `Usb1:`, and `Hdd:` are mounted.
- Calls `showUI()` to collect the selected download type, final archive URL, and selected game display name.
- Reads `game:\settings.txt` into a `Settings` struct.
- Sanitizes the selected game name into a FATX-safe folder name.
- Chooses an output root based on download type.
- Calls `getGame()` with the selected URL and computed output paths.
- If something fails, waits for `Y` to restart the search or `B` to exit.

The main loop returns success immediately after a successful download/extract. It only repeats after failure and user confirmation.

## Settings Model

`main.cpp` defines a local `Settings` struct with four paths:

- `originalXboxPath`
- `xbox360Path`
- `xblaPath`
- `legacyPath`

`getSettings()` reads `game:\settings.txt` line by line and recognizes:

- `original-xbox-path: `
- `xbox-360-path: `
- `xbla-path: `
- `output-path: `

Lines beginning with `#` are skipped. If a type-specific path is missing, the code tries to fall back to the legacy `output-path`. Missing paths are logged but do not stop startup immediately.

## Download Type

`user interface/ui.h` defines:

- `ORIGINAL_XBOX = 1`
- `XBOX_360 = 2`
- `XBLA = 3`
- `AUTO_UPDATE = 4`

The value returned from `showUI()` drives both the search URL and the later extraction path.

## UI Layer

The UI is text-mode, not XUI, despite the project containing `.xur` / `.xui` files. It uses ATG console output plus XInput polling.

`showUI()` is the public entry point. It:

- waits for controller buttons to be released,
- shows the download type menu,
- optionally runs the updater,
- opens the Xbox software keyboard,
- builds a Vimm search URL,
- downloads the search results into memory,
- parses game result rows,
- lets the user select a result,
- downloads that result page,
- parses media IDs / disc versions,
- lets the user select media when more than one exists,
- writes the final archive URL to the caller's buffer.

## Download Layer

`downloadFileHTTPS()` is the single HTTPS download function used by search, metadata, game archives, and updater JSON/assets.

It has two output modes:

- file mode: pass `downloadIntoFile = true`; response body streams to `fileName`.
- memory mode: pass `downloadIntoFile = false`; response body is copied to `dataBuffer`, and `outputBufferSize` is updated to the amount written.

For archive downloads, the first output path is normally `game:\tmp.7z.001`. Because the name ends in `.001`, `DumpResponse()` treats it as a split-file target and creates `.002`, `.003`, etc. as needed.

For HTML and JSON requests, the response goes into a caller-allocated 4 MB buffer.

## TLS Layer

`XboxTLS.cpp` exposes a small C API:

- `XboxTLS_CreateContext()`
- `XboxTLS_AddTrustAnchor_RSA()`
- `XboxTLS_AddTrustAnchor_EC()`
- `XboxTLS_Connect()`
- `XboxTLS_Write()`
- `XboxTLS_Read()`
- `XboxTLS_Free()`

The implementation stores BearSSL contexts in an internal heap-allocated struct. Callers do not see BearSSL directly.

`downloadFile.cpp` owns the app's current trust anchor list. It calls `addTrustAnchors()` after context creation and before connecting.

## Extraction Layer — removed

X-Store downloaded 7z archives and Xbox ISOs, so it needed `decompressSevenZipFile()` and `extractIso()` behind it. Neither exists here: the `7zip` and `xiso extract` projects were deleted along with the full-game download path.

This fork never decompresses anything. Archive.org's virtual-path URL form, `/download/{item}/{archive}/{urlencoded member}`, resolves through `view_archive.php` and serves the member **already extracted server-side** — confirmed on hardware for both RAR and ZIP. The app only ever reads archive *headers* (to learn what is inside and how big it is) and then downloads members as ordinary files.

## Updater Layer — removed

X-Store could update itself from its own GitHub releases, using `runUpdate()`, cJSON for the release JSON and miniz for the ZIP. That path is gone, along with the `updater` and `miniz` projects; you deploy a new `.xex` by hand.

One thing outlives it and is *not* dead code: `cJSON` is now used by `ArchiveOrgDLC.cpp` to parse archive.org's item metadata. The GitHub trust anchor (`githubCert.h`) went with the updater.

## Logging

`dprintf()` writes to:

- the ATG console,
- stdout/debug output,
- `game:\DebugInfo.txt`.

`log_printf()` writes to stdout/debug output and `game:\DebugInfo.txt`, but does not display through the ATG console.

`debug_tls()` writes TLS/debug messages to stdout and `game:\DebugInfo.txt`.

For on-console user progress, use `dprintf()`. For quieter internal logs, use `log_printf()` or `debug_tls()`.
