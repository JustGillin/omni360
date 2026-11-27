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

// The community trainer pack: github.com/StonedModder/Aurora-JTAG-RGH-Trainer-Pack-Repository,
// ~550 games. Its file list is fetched once a day, on a thread of its own,
// and kept as Hdd1:\Omni360\TrainerPack.txt; a trainer is downloaded from it
// when asked for.
struct PackTrainer
{
    char path[200];  // in the repository
    char file[96];   // its file name, e.g. Trainer(RETROBYTE).xex
    char label[96];  // shown: the file name without .xex
    unsigned long size;
};
#define MAX_PACK_TRAINERS 8

void StartTrainerPackRefresh();

// The pack's trainers for this game, from the list as last fetched.
int PackTrainersFor(unsigned long titleId, PackTrainer *out, int max);

// Downloads one into this game's trainer folder, replacing what's there.
// Blocks for the download - a second or two. path gets where it went.
bool DownloadPackTrainer(unsigned long titleId, const PackTrainer &trainer, char *path, size_t pathSize);

// Whether trainers for titleId are for this game. A few title IDs are two
// games - Grand Theft Auto IV and Episodes from Liberty City are both
// 545407F2 - and a trainer only checks the title ID, so GTA IV's would start
// in Episodes and patch the wrong game.
bool TrainersFitGame(unsigned long titleId, const char *gameName);

// Whether launching this game starts its trainer with something on - what's
// chosen, or the trainer's own defaults - as StartTrainerFor decides. Reads
// a file or two, never loads the trainer: cache it, don't ask every frame.
bool TrainerSetToStart(unsigned long titleId, const char *gameName);

// The next launch goes without the trainer - "Play without it" - leaving
// what's chosen as it is for the time after.
void SkipTrainerNextLaunch();

// Loads and starts this game's trainer, if it has one and cheats are chosen -
// just before the game is launched (Launch.cpp).
void StartTrainerFor(unsigned long titleId, const char *gameName);

#endif
