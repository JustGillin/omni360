/*
FILE : QueueState.cpp
PROJECT : Omni360
DESCRIPTION : reading and writing the small text files that carry each queue's
              unfinished jobs from one run to the next. See QueueState.h.
*/

#include "QueueState.h"
#include "OutputConsole.h"

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// Larger than any queue this writes - 32 jobs of a few hundred bytes - and
// small enough that a file which isn't one of ours is refused rather than
// read into memory.
#define QUEUE_STATE_FILE_MAX (256 * 1024)

static void TempPathFor(const char *path, char *out, size_t outSize)
{
    _snprintf(out, outSize, "%s.tmp", path);
    out[outSize - 1] = '\0';
}

bool WriteQueueStateFile(const char *path, const char *text)
{
    char temp[600];
    TempPathFor(path, temp, sizeof(temp));

    // Written beside the real file and renamed over it, so a console switched
    // off mid-write leaves the old queue rather than half a new one.
    FILE *f = fopen(temp, "wb");
    if (f == NULL)
    {
        dprintf("[queue] couldn't write %s\n", temp);
        return false;
    }

    const size_t n = strlen(text);
    const bool written = (fwrite(text, 1, n, f) == n);
    if (fclose(f) != 0 || !written)
    {
        dprintf("[queue] couldn't finish writing %s\n", temp);
        remove(temp);
        return false;
    }

    remove(path); // rename won't replace an existing file
    if (rename(temp, path) != 0)
    {
        dprintf("[queue] couldn't put %s into place\n", path);
        remove(temp);
        return false;
    }

    return true;
}

char *ReadQueueStateFile(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return NULL;

    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > QUEUE_STATE_FILE_MAX)
    {
        if (size > QUEUE_STATE_FILE_MAX)
            dprintf("[queue] ignoring %s - %ld bytes is not one of ours\n", path, size);
        fclose(f);
        return NULL;
    }

    char *text = (char *)malloc((size_t)size + 1);
    if (text == NULL)
    {
        fclose(f);
        return NULL;
    }

    const size_t got = fread(text, 1, (size_t)size, f);
    fclose(f);
    text[got] = '\0';
    return text;
}

void DeleteQueueStateFile(const char *path)
{
    remove(path);
}

bool NextQueueStateLine(char **at, char **key, char **value)
{
    char *line = *at;

    for (;;)
    {
        if (line == NULL || *line == '\0')
        {
            *at = line;
            return false;
        }

        // The line ends at the next break, whichever kind it is.
        char *end = line;
        while (*end != '\0' && *end != '\n' && *end != '\r')
            end++;

        char *next = end;
        while (*next == '\n' || *next == '\r')
            next++;
        *end = '\0';

        if (*line == '\0' || *line == '#')
        {
            line = next;
            continue;
        }

        char *equals = strchr(line, '=');
        if (equals != NULL)
        {
            *equals = '\0';
            *value = equals + 1;
        }
        else
        {
            *value = end; // the empty string at the line's end
        }

        *key = line;
        *at = next;
        return true;
    }
}

// ---------------------------------------------------------------------------
// What was put back at launch - see QueueState.h
// ---------------------------------------------------------------------------

static int g_restoredFinished = 0;
static int g_restoredFailed = 0;

void NoteRestoredRecord(bool failed)
{
    g_restoredFinished++;
    if (failed)
        g_restoredFailed++;
}

void TakeRestoredTally(int *finished, int *failed)
{
    if (finished != NULL)
        *finished = g_restoredFinished;
    if (failed != NULL)
        *failed = g_restoredFailed;

    g_restoredFinished = 0;
    g_restoredFailed = 0;
}

void AppendQueueStateText(char *buffer, size_t size, size_t *at, const char *format, ...)
{
    if (*at + 1 >= size)
        return;

    va_list args;
    va_start(args, format);
    const int n = _vsnprintf(buffer + *at, size - *at - 1, format, args);
    va_end(args);

    // This toolchain returns -1 rather than the length it wanted when the
    // text doesn't fit, and doesn't terminate what it did write.
    *at = (n < 0) ? size - 1 : *at + (size_t)n;
    buffer[*at] = '\0';
}
