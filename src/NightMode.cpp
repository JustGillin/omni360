/*
FILE : NightMode.cpp
PROJECT : Omni360
DESCRIPTION : Waiting for the queues to drain overnight and then switching the
              console off. See NightMode.h.
*/

#include "NightMode.h"
#include "DownloadQueue.h"
#include "GameInstaller.h"
#include "DiscWorker.h"
#include "OutputConsole.h"

#include <xtl.h>
#include <string.h>

// The system controller, through the kernel. The same call DiscWorker.cpp
// declares for reading the disc tray: the XDK's xboxkrnl.lib exports it,
// though no header declares it.
extern "C" VOID HalSendSMCMessage(LPVOID input, LPVOID output);

// ---------------------------------------------------------------------------
// Switching the console off
// ---------------------------------------------------------------------------
//
// The usual route in homebrew is HalReturnToFirmware, which xboxkrnl.lib also
// exports, with a FIRMWARE_REENTRY routine number for powering down. The
// number is the problem. No XDK header declares that enum - grepping the
// whole of %XEDK%\include for FIRMWARE_REENTRY and HalReturnToFirmware finds
// nothing - and the two community sources that give values disagree with each
// other: one orders the enum so that power-down is 1, the other has 1 as the
// reboot. A wrong value here reboots the console at three in the morning,
// which is the opposite of the point.
//
// So the system controller is asked directly instead. That is the same call
// with the same 16-byte message and the same byte 0 = command as the tray
// state in DiscWorker.cpp, which has been working on hardware for a while -
// so the calling convention is one this codebase has already proven. The
// bytes are libxenon's xenon_smc_power_shutdown, whose tray command is 0x0A,
// the same as ours.
#define SMC_POWER     0x82
#define SMC_POWER_OFF 0x01

// Long enough to catch the screen going black and reach for a controller,
// short enough not to be a wait.
#define NIGHT_COUNTDOWN_MS 30000

static NightState g_state = NIGHT_OFF;
static DWORD g_countdownStart = 0;

int RunningJobCount()
{
    // A disc install has no pause - there is nothing to keep and nothing to
    // carry on from - so every one of its pending jobs is one that will
    // finish.
    return RunnableDownloadCount() + RunnableGameJobCount() + PendingDiscJobCount();
}

NightState GetNightState()
{
    return g_state;
}

NightArmResult ArmNightMode()
{
    if (RunningJobCount() == 0)
        return NIGHT_NOTHING_RUNNING;

    g_state = NIGHT_WATCHING;
    dprintf("[night] armed - switching off once the queue drains\n");
    return NIGHT_ARMED;
}

void LeaveNightMode()
{
    if (g_state == NIGHT_OFF)
        return;

    dprintf("[night] off again\n");
    g_state = NIGHT_OFF;
    g_countdownStart = 0;
}

bool UpdateNightMode(int *secondsLeft)
{
    if (secondsLeft != NULL)
        *secondsLeft = 0;

    if (g_state == NIGHT_OFF)
        return false;

    if (g_state == NIGHT_WATCHING)
    {
        if (RunningJobCount() > 0)
            return false;

        g_state = NIGHT_COUNTDOWN;
        g_countdownStart = GetTickCount();
        dprintf("[night] nothing left running - switching off in %d seconds\n", NIGHT_COUNTDOWN_MS / 1000);
    }

    const DWORD gone = GetTickCount() - g_countdownStart;
    if (gone >= NIGHT_COUNTDOWN_MS)
        return true;

    // Rounded up, so the last second on screen is "1" rather than "0".
    if (secondsLeft != NULL)
        *secondsLeft = (int)((NIGHT_COUNTDOWN_MS - gone + 999) / 1000);
    return false;
}

void PowerOffConsole()
{
    unsigned char request[16];
    unsigned char reply[16];
    memset(request, 0, sizeof(request));
    memset(reply, 0, sizeof(reply));
    request[0] = SMC_POWER;
    request[1] = SMC_POWER_OFF;

    dprintf("[night] asking the system controller to switch off\n");
    HalSendSMCMessage(request, reply);

    // It doesn't come back when it works. If it does, the console is still
    // on, and the caller says so rather than leaving a black screen up.
    Sleep(3000);
    dprintf("[night] still running - the controller didn't take the power-off\n");
}
