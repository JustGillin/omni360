#ifndef PHONE_SIGN_IN_H
#define PHONE_SIGN_IN_H

// ---------------------------------------------------------------------------
// Signing in to Internet Archive from a phone
// ---------------------------------------------------------------------------
//
// Typing an email and password with a controller is slow, and keys slower.
// While the sign-in question is open, this serves one small page on the
// local network, at a one-time address the question shows as a QR code:
//
//     http://<console>:36065/<code>
//
// The page takes the account's email and password - or its two keys, pasted
// from archive.org/account/s3.php - and the console signs in with them
// (ArchiveOrgSignIn.h, or the keys checked as Settings checks them). The
// phone is told whether it worked; on success the question closes by itself.
//
// Only the one-time code's address answers, so nothing else on the network
// can use it, and nothing listens once the question closes. The phone to the
// console is plain HTTP on the home network, as the FTP server is; the
// console to Internet Archive is HTTPS. The password is never kept or logged.

#include <string>

#define PHONE_SIGN_IN_PORT 36065

// Starts listening, with a new code. False, with nothing listening, when the
// console has no address yet or the port can't be had. url gets the address
// to show ("http://192.168.1.20:36065/k3f9tq").
bool StartPhoneSignIn(char *url, unsigned int urlSize);

// Whether the phone has signed in. When it has, the keys and the account's
// screen name are handed over - once - for the caller to save.
bool PhoneSignInDone(std::string &accessKey, std::string &secretKey, std::string &screenName);

// Stops listening, waiting a moment for a request in flight to finish.
void StopPhoneSignIn();

#endif
