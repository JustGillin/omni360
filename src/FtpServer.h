#ifndef FTP_SERVER_H
#define FTP_SERVER_H

// ---------------------------------------------------------------------------
// An FTP server for the console's drives
// ---------------------------------------------------------------------------
//
// While Omni360 is open and the setting is on, a PC on the same network can
// connect to ftp://<the console's address> with FileZilla, Windows Explorer
// or any FTP client and copy files both ways - what Aurora's FTP does.
//
// The top level lists the drives - Hdd1, Usb0 to Usb2 when there's a drive in
// them, and Game, the folder Omni360 runs from - and everything under them
// is the drive as it is. Passive mode only (PASV and EPSV), which is what
// every current client uses by default; REST resumes a transfer either way.
//
// Signing in takes a username and password, xboxftp and xboxftp unless
// settings.txt says otherwise - the same as Aurora's, so a client already set
// up for it works as it is. That keeps a passer-by off the drive on a shared
// network; it is no protection beyond that - FTP sends both in the clear.

#define FTP_PORT 21

// Starts listening, on a thread of its own. Safe to call again after
// StopFtpServer. False if the thread couldn't start.
bool StartFtpServer(const char *user, const char *password);

// Stops listening and ends every connection - a transfer in progress is cut
// off, as closing the app would. Waits up to timeoutMs for the threads.
void StopFtpServer(unsigned long timeoutMs);

bool FtpServerRunning();

// Connections open now, for Settings' row.
int FtpClientCount();

#endif
