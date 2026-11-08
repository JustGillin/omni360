#ifndef NIGHT_MODE_H
#define NIGHT_MODE_H

// ---------------------------------------------------------------------------
// Night mode: queue the night's downloads, go to bed, and find the console
// switched off in the morning
// ---------------------------------------------------------------------------
//
// Armed with Y on the Queue page and nowhere else, because that is where you
// are the moment you have finished queueing. It is not a setting and it does
// not survive a restart: it arms for tonight. A console that powered itself
// off every time the queue happened to empty would be infuriating, which is
// why this is something you ask for each time.
//
// Arming blacks the screen out to the clock straight away, and that is the
// whole of the indicator - there is no armed-but-otherwise-normal screen to
// mistake for the shell. Any button press leaves night mode altogether,
// countdown or no countdown, so there is no way to think you cancelled it
// and still have the console switch off half an hour later.
//
// What it waits for is the three queues having nothing left in them that
// will finish. A paused job never finishes on its own, so it deliberately
// doesn't count: waiting on one would leave the console on all night, which
// is the thing being avoided.
//
// Then a countdown, which any button cancels, and the normal shutdown path -
// the workers stopped so staging is flushed - before the power goes.
//
// Related: the Queue page keeps what finished (QueueState.h), without which
// a night of downloads would end with the console off and nothing on screen
// in the morning saying what happened.

enum NightState
{
    NIGHT_OFF,
    NIGHT_WATCHING, // armed, waiting for the queues to drain
    NIGHT_COUNTDOWN // drained; counting down to the power going off
};

NightState GetNightState();
inline bool NightModeOn() { return GetNightState() != NIGHT_OFF; }

enum NightArmResult
{
    NIGHT_ARMED,
    NIGHT_NOTHING_RUNNING // nothing in the queues that would ever finish
};

// Refuses on a queue with nothing running, rather than blacking the screen
// out and switching the console off thirty seconds later.
NightArmResult ArmNightMode();

void LeaveNightMode();

// Once a pass of the main loop while it is on. True when the countdown has
// run out: the caller stops the workers and then calls PowerOffConsole.
// secondsLeft is what the screen shows, and 0 until the countdown starts.
bool UpdateNightMode(int *secondsLeft);

// Jobs across all three queues that will finish if left alone - what night
// mode is waiting for. Paused ones aren't among them.
int RunningJobCount();

// Asks the SMC - the motherboard's power controller, nothing to do with a
// gamepad - to switch the console off. Doesn't return when it works; see the
// note in NightMode.cpp on why it is the SMC rather than HalReturnToFirmware.
void PowerOffConsole();

#endif
