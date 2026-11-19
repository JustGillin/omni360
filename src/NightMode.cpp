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
#include "FileJobs.h"
#include "OutputConsole.h"

#include <xtl.h>
#include <string.h>

// The SMC - the system management controller, the small chip on the
// motherboard that is powered whenever the console is plugged in and that
// runs the power button, the fans and the disc tray. Reached through the
// kernel, by the same call DiscWorker.cpp declares for reading the tray:
// the XDK's xboxkrnl.lib exports it, though no header declares it.
extern "C" VOID HalSendSMCMessage(LPVOID input, LPVOID output);

// ---------------------------------------------------------------------------
// Switching the console off
// ---------------------------------------------------------------------------
//
// HalReturnToFirmware, which xboxkrnl.lib exports though no XDK header
// declares it, with a FIRMWARE_REENTRY routine number. It is the kernel's own
// shutdown: it winds the system down first and only then has the SMC cut the
// power - the light pulses, the screen goes, the fans run on for a couple of
// seconds - the way the dashboard and Aurora turn the console off.
//
// No XDK header gives the routine numbers either. The 360's order, from
// xkelib: Halt 0, Reboot 1, KdReboot 2, FatalErrorReboot 3, ResetSMC 4,
// PowerDown 5, RebootQuiesce 6, ForceShutdown 7, PowerCycleQuiesce 8. Xenia
// agrees on both used here: its kernel expects games to pass 1 for a reboot,
// and xenia-canary labels 5 "power off (nice)" and 4 "power off (hard)".
// (An earlier note here had sources disagreeing, power-down = 1 in one; that
// is the original Xbox's and NT's order - Halt, PowerDown, Restart, Reboot -
// not the 360's.)
#define FIRMWARE_REBOOT     1
#define FIRMWARE_POWER_DOWN 5
extern "C" VOID HalReturnToFirmware(DWORD routine);

// The SMC route stays as the fallback, for if the kernel's ever comes back.
// It is the same call with the same 16-byte message and byte 0 = command as
// the tray state in DiscWorker.cpp. The bytes are libxenon's
// xenon_smc_power_shutdown and xenon_smc_power_reboot. It works - confirmed on
// hardware 2026-11-07 - but it is a hard cut: everything stops at once, as
// pulling the plug would.
#define SMC_POWER     0x82
#define SMC_POWER_OFF 0x01

// libxenon's xenon_smc_power_reboot: the same power command, with 0x04 and
// 0x30 after it - a full power cycle, as holding the button and pressing it
// again would be, not a return to the dashboard.
#define SMC_POWER_REBOOT     0x04
#define SMC_POWER_REBOOT_ARG 0x30

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
    return RunnableDownloadCount() + RunnableGameJobCount() + PendingDiscJobCount() + PendingFileJobCount();
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
    dprintf("[night] armed - turning off once the queue drains\n");
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
        dprintf("[night] nothing left running - turning off in %d seconds\n", NIGHT_COUNTDOWN_MS / 1000);
    }

    const DWORD gone = GetTickCount() - g_countdownStart;
    if (gone >= NIGHT_COUNTDOWN_MS)
        return true;

    // Rounded up, so the last second on screen is "1" rather than "0".
    if (secondsLeft != NULL)
        *secondsLeft = (int)((NIGHT_COUNTDOWN_MS - gone + 999) / 1000);
    return false;
}

// The SMC's hard version, for when the kernel's didn't happen.
static void SmcPower(unsigned char command, unsigned char arg, const char *what)
{
    unsigned char request[16];
    unsigned char reply[16];
    memset(request, 0, sizeof(request));
    memset(reply, 0, sizeof(reply));
    request[0] = SMC_POWER;
    request[1] = command;
    request[2] = arg;

    dprintf("[power] asking the SMC to %s instead\n", what);
    HalSendSMCMessage(request, reply);

    // It doesn't come back when it works. If it does, the console is still
    // on, and the caller says so rather than leaving a black screen up.
    Sleep(3000);
    dprintf("[power] still running - the SMC didn't take it either\n");
}

void PowerOffConsole()
{
    dprintf("[power] asking the kernel to turn the console off\n");
    HalReturnToFirmware(FIRMWARE_POWER_DOWN);

    // The shutdown takes a moment once it's under way, so give it one
    // before deciding it didn't happen.
    Sleep(3000);
    dprintf("[power] still running - the kernel didn't take the power-off\n");
    SmcPower(SMC_POWER_OFF, 0, "turn it off");
}

void RestartConsole()
{
    dprintf("[power] asking the kernel to restart the console\n");
    HalReturnToFirmware(FIRMWARE_REBOOT);

    Sleep(3000);
    dprintf("[power] still running - the kernel didn't take the restart\n");
    SmcPower(SMC_POWER_REBOOT, SMC_POWER_REBOOT_ARG, "restart it");
}
