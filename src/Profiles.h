#ifndef PROFILES_H
#define PROFILES_H

// ---------------------------------------------------------------------------
// Profiles on the console, and signing one in without the Guide
// ---------------------------------------------------------------------------
//
// Auto sign-in the way Aurora does it: a profile is picked once in Settings,
// and Omni360 signs it in at every start when nobody is signed in yet - no
// sign-in screen, nothing to press.
//
// None of this is in the XDK's headers, but xapilib.lib - linked already -
// exports all three XAM calls by name, so there are no ordinals to get
// wrong. Signatures and the enumerator's structure are xkelib's; the sign-in
// is Freestyle Dash's own ForcePlayerSignIn (Tools/Generic/xboxtools.cpp),
// from the team that went on to write Aurora: the profile's offline XUID in
// one of four slots, the rest INVALID_XUID, and flags 0x424.

#define PROFILE_GAMERTAG_SIZE 16 // 15 characters and the terminator

struct ProfileEntry
{
    unsigned long long xuid; // offline XUID, E000...
    char gamertag[PROFILE_GAMERTAG_SIZE];
};

// The profiles on every connected drive, up to max. Returns how many.
int ListProfiles(ProfileEntry *out, int max);

// Signs the profile in to the first free controller slot. False when it
// isn't on any drive, every slot is taken, or XAM refused - the log says
// which.
bool SignInProfile(unsigned long long xuid);

// True when any profile is signed in.
bool AnyoneSignedIn();

#endif
