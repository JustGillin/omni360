#ifndef UI_SOUND_H
#define UI_SOUND_H

// The interface's sounds: a soft, high tick as focus moves, a short tick for
// A and a low round bump for B. Made in code when they start - sine tones
// with a quick rise and fade - so there are no sound files to ship, and
// played through XAudio2. tools/preview_ui_sounds.py renders them to WAVs.

enum UiSound
{
    UI_SOUND_MOVE,
    UI_SOUND_SELECT,
    UI_SOUND_BACK,
    UI_SOUND_COUNT
};

// False, logged, if XAudio2 won't start; PlayUiSound then does nothing.
bool StartUiSounds();

// Each sound cuts off the one before it of the same kind, so holding the
// D-pad down a list ticks with it rather than piling up.
void PlayUiSound(UiSound sound);

// Settings' Navigation sounds.
void SetUiSoundsOn(bool on);

#endif
