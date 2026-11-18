/*
FILE : Profiles.cpp
PROJECT : Omni360
DESCRIPTION : Lists the console's profiles and signs one in - see Profiles.h.
*/

#include "Profiles.h"
#include "OutputConsole.h"
#include <xtl.h>
#include <string.h>

// xkelib's layout, packed: 0x17C and 0x188 bytes.
#pragma pack(push, 1)
struct XamAccountInfo
{
    DWORD reserved;
    DWORD liveFlags;
    WCHAR gamertag[0x10];
    XUID xuidOnline;
    DWORD cachedUserFlags;
    DWORD onlineServiceNetworkId;
    BYTE passcode[4];
    char onlineDomain[0x14];
    char onlineKerberosRealm[0x18];
    BYTE onlineKey[0x10];
    char userPassportMembername[0x72];
    char userPassportPassword[0x20];
    char ownerPassportMembername[0x72];
};
struct ProfileEnumResult
{
    XUID xuidOffline;
    XamAccountInfo account;
    DWORD deviceId;
};
#pragma pack(pop)
C_ASSERT(sizeof(XamAccountInfo) == 0x17C);
C_ASSERT(sizeof(ProfileEnumResult) == 0x188);

extern "C"
{
    // Device 0 is every device.
    DWORD XamProfileCreateEnumerator(DWORD deviceId, PHANDLE enumerator);
    DWORD XamProfileEnumerate(HANDLE enumerator, DWORD flags, ProfileEnumResult *result, PXOVERLAPPED overlapped);
    DWORD XamUserLogon(const XUID *users, DWORD flags, PXOVERLAPPED overlapped);
}

// XAMUSERLOGON_ADDUSERS | NOPOPUPNOTIFICATION | ALLOWUSERSWITHREQUIREDMESSAGE,
// the value Freestyle Dash passes. The values are xkelib's and Xenia's,
// which agree.
#define XAM_LOGON_ADD_USERS          0x00000004
#define XAM_LOGON_NO_POPUP           0x00000020
#define XAM_LOGON_ALLOW_REQUIRED_MSG 0x00000400

int ListProfiles(ProfileEntry *out, int max)
{
    HANDLE enumerator = NULL;
    const DWORD created = XamProfileCreateEnumerator(0, &enumerator);
    if (created != ERROR_SUCCESS || enumerator == NULL)
    {
        dprintf("[profile] couldn't list profiles: %08lX\n", (unsigned long)created);
        return 0;
    }

    int count = 0;
    ProfileEnumResult result;
    while (count < max)
    {
        memset(&result, 0, sizeof(result));
        if (XamProfileEnumerate(enumerator, 0, &result, NULL) != ERROR_SUCCESS)
            break;
        if (result.xuidOffline == INVALID_XUID)
            continue;

        ProfileEntry &entry = out[count];
        entry.xuid = result.xuidOffline;
        // Gamertags are ASCII; anything else shows as '?' rather than
        // being cut in half.
        int i = 0;
        for (; i < PROFILE_GAMERTAG_SIZE - 1 && result.account.gamertag[i] != 0; ++i)
        {
            const WCHAR c = result.account.gamertag[i];
            entry.gamertag[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
        entry.gamertag[i] = '\0';
        ++count;
    }
    CloseHandle(enumerator);
    return count;
}

bool AnyoneSignedIn()
{
    for (DWORD user = 0; user < XUSER_MAX_COUNT; ++user)
    {
        if (XUserGetSigninState(user) != eXUserSigninState_NotSignedIn)
            return true;
    }
    return false;
}

bool SignInProfile(unsigned long long xuid)
{
    // Only one that's still on a drive: XAM's answer for a missing one
    // isn't something to find out at every start.
    ProfileEntry profiles[32];
    const int count = ListProfiles(profiles, 32);
    const char *gamertag = NULL;
    for (int i = 0; i < count && gamertag == NULL; ++i)
    {
        if (profiles[i].xuid == xuid)
            gamertag = profiles[i].gamertag;
    }
    if (gamertag == NULL)
    {
        dprintf("[profile] %016llX isn't on any drive - not signing in\n", xuid);
        return false;
    }

    int slot = -1;
    for (DWORD user = 0; user < XUSER_MAX_COUNT && slot < 0; ++user)
    {
        if (XUserGetSigninState(user) == eXUserSigninState_NotSignedIn)
            slot = (int)user;
    }
    if (slot < 0)
    {
        dprintf("[profile] every slot is signed in already\n");
        return false;
    }

    XUID users[XUSER_MAX_COUNT] = { INVALID_XUID, INVALID_XUID, INVALID_XUID, INVALID_XUID };
    users[slot] = xuid;
    const DWORD result = XamUserLogon(users, XAM_LOGON_ADD_USERS | XAM_LOGON_NO_POPUP | XAM_LOGON_ALLOW_REQUIRED_MSG, NULL);
    dprintf("[profile] signing in %s to slot %d: %08lX\n", gamertag, slot, (unsigned long)result);
    return result == ERROR_SUCCESS;
}
