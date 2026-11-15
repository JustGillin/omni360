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

// Sends code to the helper heard most recently. On success (true) token and id
// get what to save; on failure message says why, in a line for a popup.
bool PcHelperPair(const char *code, char *token, unsigned int tokenSize, char *id, unsigned int idSize,
                  char *message, unsigned int messageSize);

#endif
