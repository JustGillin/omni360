#ifndef QUEUE_STATE_H
#define QUEUE_STATE_H

#include <stddef.h>

// ---------------------------------------------------------------------------
// The queue, kept between runs
// ---------------------------------------------------------------------------
//
// A download interrupted by the app closing keeps what it had on the drive -
// see the staging notes in GameInstaller.h, and DownloadRarRange in
// ArchiveOrgDLC.cpp - but the job itself lived only in memory, so nothing
// said what to carry on with. Each queue now writes the jobs it still has to
// do into a small text file of its own, and reads them back at launch.
//
// The file is written whenever the set of unfinished jobs changes: one added,
// one finished, one cancelled. It is never written while the app is closing.
// Closing cancels every job, which is the one moment the file must not be
// rewritten - and a console is as often switched off at the wall as closed
// politely, so the last write has to already be the truth.
//
// A job that finished is not in it, however it finished. One that failed has
// said so and is the user's to try again; one restored on every launch would
// keep failing on every launch.
//
// Keys aren't in it either. A restored job is given whatever keys are saved
// now, so changing them in Settings reaches everything still waiting.
//
// These are plain "key=value" lines, like settings.txt - readable over FTP,
// and a file that doesn't make sense costs the queue, not the app.

// Writes text to path, in one step as far as a reader is concerned: a temp
// file, then a rename over the old one. False if it couldn't be written,
// which only means this run's queue won't come back.
bool WriteQueueStateFile(const char *path, const char *text);

// The file's contents, NUL-terminated, or NULL if there is none or it is
// larger than anything this wrote. Free it with free().
char *ReadQueueStateFile(const char *path);

void DeleteQueueStateFile(const char *path);

// Walks "key=value" lines, in place: *at starts at the text and is advanced
// past the line, and both halves are NUL-terminated where they sit. False at
// the end. Blank lines and ones starting with '#' are skipped; a line with no
// '=' comes back with an empty value.
bool NextQueueStateLine(char **at, char **key, char **value);

// Adds to a buffer that never overruns: nothing is written once it is full,
// and *at stops at size - 1. Used to build the file a job at a time.
void AppendQueueStateText(char *buffer, size_t size, size_t *at, const char *format, ...);

// ---------------------------------------------------------------------------
// What finished last time, and the one line about it
// ---------------------------------------------------------------------------
//
// The queues also keep what finished, as records rather than as jobs - see
// the note on records in DownloadQueue.cpp. Each queue puts its own back, but
// the user hears about them once: three toasts at launch would stack over the
// startup screen, and what they want to know is whether anything failed while
// they were asleep.
//
// So each queue counts what it restored here, and main.cpp reads the total
// after the three restores and says it in a single toast. Launch is on the UI
// thread, one restore after another, so these need no lock of their own.

void NoteRestoredRecord(bool failed);

// The totals so far, zeroed as they're read.
void TakeRestoredTally(int *finished, int *failed);

#endif
