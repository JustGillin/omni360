#ifndef PC_HELPER_H
#define PC_HELPER_H

// ---------------------------------------------------------------------------
// The PC helper: downloads Store games on a PC on the console's behalf
// ---------------------------------------------------------------------------
//
// helper/ in the repository is a small PC program. It broadcasts
// "OMNI360-HELPER 1 <port> <name>" on UDP port 36060 every few seconds. A
// thread here listens for that. While a helper has been heard recently, a
// Store download asks it for the archive.org file instead of archive.org
// (http://<pc>:<port>/file/<item>/<zip>). The helper downloads with many
// connections and answers the console's byte ranges as they arrive. Nothing
// else about the install changes, and with no helper the console downloads
// by itself as before.

// Starts the listening thread. Safe to call once at startup.
void StartPcHelperListener();

// The helper's base URL ("http://192.168.1.20:36061") if one was heard in the
// last 15 seconds; false otherwise. name gets the PC's name.
bool PcHelperUrl(char *url, unsigned int urlSize, char *name, unsigned int nameSize);

#endif
