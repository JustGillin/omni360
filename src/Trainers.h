#ifndef TRAINERS_H
#define TRAINERS_H

// ---------------------------------------------------------------------------
// Trainers: cheats for a game, from Aurora's trainer packs (XYZEngine)
// ---------------------------------------------------------------------------
//
// A trainer is a DLL XEX kept in Hdd1:\Omni360\Trainers\<title ID>\. It runs
// as a system module beside the game; Omni360 loads it and starts it just
// before launching the game, with the cheats chosen in its picker. See
// Trainers.cpp for how one works.

#define MAX_TRAINER_CHEATS 32
#define MAX_TRAINER_TUS    16

struct TrainerInfo
{
    char path[300];          // the trainer's .xex
    char name[64];           // as the trainer gives it - usually the game's
    char author[64];
    char description[128];
    char version[16];
    unsigned long titleId;
    long mediaId;            // -1: any
    long baseVersion;        // -1: any
    long disc;               // -1: any
    int tuCount;             // the title updates it works with (0 is none)
    long tus[MAX_TRAINER_TUS];
    int cheatCount;
    char cheats[MAX_TRAINER_CHEATS][64]; // UTF-8
    bool defaults[MAX_TRAINER_CHEATS];   // on unless turned off
};

// The trainer for this game, if there's one: its .xex. False without.
bool FindTrainer(unsigned long titleId, char *path, size_t pathSize);

// What the trainer at path says about itself. It has to be loaded to say, so
// this loads it, reads it and unloads it again - a second or so. False if it
// couldn't be read; the log says why.
bool ReadTrainerInfo(const char *path, TrainerInfo *out);

// The cheats chosen for this game, as bits (bit n is cheat n): false if none
// have been chosen yet, when the trainer's own defaults apply. Kept as
// cheats.txt beside the trainer.
bool LoadTrainerCheats(unsigned long titleId, unsigned long *bits);
bool SaveTrainerCheats(unsigned long titleId, unsigned long bits);

// Loads and starts this game's trainer, if it has one and cheats are chosen -
// just before the game is launched (Launch.cpp).
void StartTrainerFor(unsigned long titleId);

#endif
