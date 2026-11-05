#ifndef UI_SOUND_H
#define UI_SOUND_H

// The interface's sounds: a soft, high tick as focus moves, a short tick for
// A, a low round bump for B, and the startup screen's score. Made in code
// when they start - sine tones with a rise and a fade, plus noise under the
// same envelope - so there are no sound files to ship, and played through
// XAudio2. tools/preview_ui_sounds.py renders them to WAVs, number for
// number, and documents the startup score.
//
// The startup screen is in three parts because the load is of unknown length
// and only the middle may stretch: HEAD as the disc arrives, SPIN looping
// once per revolution for as long as the library takes, and TAIL as the
// drive takes the disc and the wordmark washes in. See GameListUI.cpp's
// startup section, whose timings these are written against.

enum UiSound
{
    UI_SOUND_MOVE,
    UI_SOUND_SELECT,
    UI_SOUND_BACK,
    UI_SOUND_STARTUP_HEAD,
    UI_SOUND_STARTUP_SPIN, // loops until stopped
    UI_SOUND_STARTUP_TAIL,
    UI_SOUND_COUNT
};

// One revolution of the disc, which is what the spin loop is: 40 cycles of
// its own G2 exactly, so it joins itself without a click. The animation's
// turn is this long, and the startup art lands where it does because of it.
#define UI_SPIN_LOOP_SAMPLES 19592
#define UI_SPIN_LOOP_MS      408.1667f

// False, logged, if XAudio2 won't start; PlayUiSound then does nothing. The
// startup parts are built on their own thread, in the order they are needed,
// so none of it is in front of the black screen before the first frame; a
// part that is not ready yet simply does not play.
bool StartUiSounds();

// Each sound cuts off the one before it of the same kind, so holding the
// D-pad down a list ticks with it rather than piling up.
void PlayUiSound(UiSound sound);

// For crossfading the spin out under the tail - the two overlap for a few
// frames, or stopping the loop dead would click.
void FadeUiSound(UiSound sound, float volume);
void StopUiSound(UiSound sound);

// Settings' Navigation sounds.
void SetUiSoundsOn(bool on);

#endif
