#ifndef PC_HELPER_H
#define PC_HELPER_H

// ---------------------------------------------------------------------------
// The PC helper: downloads Store games on a PC on the console's behalf
// ---------------------------------------------------------------------------
//
// helper/ in the repository is a small PC program. It broadcasts
// "OMNI360-HELPER 2 <port> <id> <name>" on UDP port 36060 every few seconds,
// and a thread here listens for that. Once the user has paired with it
// (Settings > General > PC helper - a code the helper shows), a Store download
// reads the archive.org file from the helper instead of archive.org
// (http://<pc>:<port>/file/<item>/<zip>?t=<token>). The helper downloads with
// many connections and answers the console's byte ranges as they arrive.
//
// Why it's faster: the 360 gives every connection a 255 KB receive window, so
// from archive.org, tens of milliseconds away, each connection tops out near
// 3.5 MB/s. On the LAN the round trip is about a millisecond, and two
// connections fill the console's 100 Mbps port (about 10 MB/s, measured).
//
// Nothing else about the install changes. With no paired helper running, the
// console downloads by itself as before.

// Starts the listening thread. Call once at startup, after SetPcHelperPairing.
void StartPcHelperListener();

// The pairing settings.txt holds: the helper's id and token (empty when not
// paired), and whether to use it. main.cpp sets them at startup and after a
// change in Settings.
void SetPcHelperPairing(const char *id, const char *token, bool enabled);

enum PcHelperStatus
{
    PC_HELPER_NONE,           // nothing heard, and not paired
    PC_HELPER_FOUND,          // a helper is running, but not the one paired with (or none is)
    PC_HELPER_PAIRED_ONLINE,  // paired, and it's running
    PC_HELPER_PAIRED_OFFLINE  // paired, but it isn't running
};

// What Settings shows. name gets the PC's name, when one was heard.
PcHelperStatus GetPcHelperStatus(char *name, unsigned int nameSize);

// The paired helper's base URL ("http://192.168.1.20:36061") and token, if
// it's running and turned on; false otherwise.
bool PcHelperUrl(char *url, unsigned int urlSize, char *token, unsigned int tokenSize,
                 char *name, unsigned int nameSize);

// Pairing is a request the user allows on the PC. RequestPair asks the helper
// heard most recently and gets a request id and the 4-digit code both screens
// show. WaitPair then waits up to about 20 seconds for the answer:
// PAIR_ALLOWED fills token and id with what to save; PAIR_WAITING means ask
// again; PAIR_FAILED puts why in message, in a line for a popup.
enum PcHelperPairResult
{
    PAIR_ALLOWED,
    PAIR_WAITING,
    PAIR_FAILED
};
bool PcHelperRequestPair(char *requestId, unsigned int requestIdSize, char *code, unsigned int codeSize,
                         char *message, unsigned int messageSize);
PcHelperPairResult PcHelperWaitPair(const char *requestId, char *token, unsigned int tokenSize, char *id,
                                    unsigned int idSize, char *message, unsigned int messageSize);

// Arcade games, DLC and title updates (downloadFile.cpp): an archive.org file
// URL - archive.org/download/<item>/<file>, or a storage server's
// .../items/<item>/<file> - as the paired helper's, if it's running and in use
// and hasn't just failed. Not a file inside an archive (<file>/<path>), which
// archive.org extracts itself and can't be read in ranges.
bool PcHelperRoute(const char *archiveUrl, char *helperUrl, unsigned int helperUrlSize);

// A routed request failed (HTTP status, or 0): archive.org takes the next
// minute's requests, so a helper that's gone doesn't cost every one a timeout.
void PcHelperRouteFailed(int status);

// For the calling thread only: never route. The game installer's threads set
// it - a disc game picks the helper or archive.org itself, and its archive.org
// fallback has to reach archive.org.
void PcHelperSetThreadBypass(bool bypass);

#endif
