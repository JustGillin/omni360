/*
FILE : MissingGames.cpp
PROJECT : Omni360
DESCRIPTION : the hand-kept marks on the missing-games list, and the text file
              they live in. See MissingGames.h.
*/

#include "MissingGames.h"
#include "StoreCatalog.h"
#include "QueueState.h"
#include "OutputConsole.h"

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MISSING_MARKS_FILE "game:\\missing-games.txt"

// The catalog's rows, and room for marks on title IDs that are no longer in
// it. Those are kept deliberately: once a mark has been fed back into
// tools/make_catalog_titles.py the game it named is gone from
// CatalogTitles.h, and a file that then forgot it would undo the curation
// the next time it was written.
#define MISSING_MARKS_MAX (CATALOG_TITLE_COUNT + 256)
#define MISSING_NAME_MAX  72

// Larger than the file can be - about 570 lines of 90 bytes - and still well
// inside ReadQueueStateFile's own limit.
#define MISSING_TEXT_MAX (96 * 1024)

struct MarkEntry
{
    unsigned long titleId;
    unsigned char mark;
    char name[MISSING_NAME_MAX]; // for the file's comment column, never read back
};

static MarkEntry g_marks[MISSING_MARKS_MAX];
static int g_markCount = 0;
static bool g_marksRead = false;

// ---------------------------------------------------------------------------
// How a mark reads
// ---------------------------------------------------------------------------

struct MarkText
{
    const char *key;   // the file's word
    const char *name;  // the picker's row
    const char *brief; // short enough to sit beside a game's details
    const char *help;  // the line under the picker's row
};

static const MarkText kMarks[MISSING_MARK_COUNT] =
{
    { "none", "Not marked", "", "" },
    { "keep", "Keep - it's a real game", "Keep",
      "A game that really was released and really isn't downloadable. It stays in the list." },
    { "duplicate", "Duplicate or regional variant", "Duplicate",
      "The same game the list already has, often under a shortened name - Call of Duty: WaW beside "
      "Call of Duty: World at War - or a regional release of one." },
    { "badname", "No name, or a bad name", "Bad name",
      "The catalog's name isn't a game's: blank, a code, a line of marketing, or cut short." },
    { "test", "An Xbox test product", "Test product",
      "Something Microsoft put in the catalog to test it with, not a game anyone could buy." },
    { "dlcdisc", "A DLC or title update disc", "DLC disc",
      "A disc that carries add-ons or a title update rather than a game of its own." },
    { "other", "Something else - look at it later", "Other",
      "It doesn't belong in the list, for a reason none of the others covers." },
};

const char *MissingMarkName(int mark)
{
    return (mark > 0 && mark < MISSING_MARK_COUNT) ? kMarks[mark].name : kMarks[0].name;
}

const char *MissingMarkShort(int mark)
{
    return (mark > 0 && mark < MISSING_MARK_COUNT) ? kMarks[mark].brief : kMarks[0].brief;
}

const char *MissingMarkKey(int mark)
{
    return (mark > 0 && mark < MISSING_MARK_COUNT) ? kMarks[mark].key : kMarks[0].key;
}

const char *MissingMarkHelp(int mark)
{
    return (mark > 0 && mark < MISSING_MARK_COUNT) ? kMarks[mark].help : kMarks[0].help;
}

const char *MissingMarksPath()
{
    return MISSING_MARKS_FILE;
}

// ---------------------------------------------------------------------------
// The file
// ---------------------------------------------------------------------------

// The catalog's name for a title ID, or NULL - for the comment beside a mark
// the first time it is written.
static const char *CatalogNameOf(unsigned long titleId)
{
    for (int i = 0; i < CatalogTitleCount(); ++i)
    {
        const CatalogTitle *title = CatalogTitleAt(i);
        if (title != NULL && title->titleId == titleId)
            return title->name;
    }
    return NULL;
}

static void SetEntryName(MarkEntry &entry, const char *name)
{
    entry.name[0] = '\0';
    if (name == NULL)
        return;
    strncpy(entry.name, name, MISSING_NAME_MAX - 1);
    entry.name[MISSING_NAME_MAX - 1] = '\0';
}

// A value as the file writes it - "duplicate  # Call of Duty: WaW" - split
// into the mark and whatever the comment held. The comment is only ever
// written back out again, so a file edited by hand keeps the name its editor
// gave it.
static void SplitValue(char *value, int *outMark, const char **outName)
{
    *outMark = MISSING_MARK_NONE;
    *outName = NULL;

    char *comment = strchr(value, '#');
    if (comment != NULL)
    {
        *comment = '\0';
        char *name = comment + 1;
        while (*name == ' ' || *name == '\t')
            name++;
        size_t len = strlen(name);
        while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == '\t' || name[len - 1] == '\r'))
            name[--len] = '\0';
        if (name[0] != '\0')
            *outName = name;
    }

    // The mark is the first word of what's left.
    char *word = value;
    while (*word == ' ' || *word == '\t')
        word++;
    const size_t wordLen = strcspn(word, " \t\r");
    word[wordLen] = '\0';

    for (int m = 1; m < MISSING_MARK_COUNT; ++m)
    {
        if (_stricmp(word, kMarks[m].key) == 0)
            *outMark = m;
    }
}

static void ReadMarks()
{
    if (g_marksRead)
        return;
    g_marksRead = true;
    g_markCount = 0;

    char *text = ReadQueueStateFile(MISSING_MARKS_FILE);
    if (text == NULL)
        return;

    char *at = text, *key = NULL, *value = NULL;
    int skipped = 0;
    while (NextQueueStateLine(&at, &key, &value))
    {
        // The counts the file carries for whoever reads it, not for this.
        if (_stricmp(key, "format") == 0 || _stricmp(key, "marked") == 0 || _stricmp(key, "catalog") == 0)
            continue;

        char *end = NULL;
        const unsigned long titleId = strtoul(key, &end, 16);
        if (titleId == 0 || end == NULL || *end != '\0')
        {
            skipped++;
            continue;
        }

        int mark = MISSING_MARK_NONE;
        const char *name = NULL;
        SplitValue(value, &mark, &name);
        if (mark == MISSING_MARK_NONE)
        {
            skipped++;
            continue;
        }

        if (g_markCount >= MISSING_MARKS_MAX)
            break;
        MarkEntry &entry = g_marks[g_markCount++];
        entry.titleId = titleId;
        entry.mark = (unsigned char)mark;
        SetEntryName(entry, name != NULL ? name : CatalogNameOf(titleId));
    }

    free(text);
    dprintf("[missing] %d mark%s read from %s%s\n", g_markCount, g_markCount == 1 ? "" : "s",
            MISSING_MARKS_FILE, skipped > 0 ? " - some lines made no sense and were left out" : "");
}

static bool WriteMarks()
{
    static char text[MISSING_TEXT_MAX];
    size_t at = 0;
    text[0] = '\0';

    AppendQueueStateText(text, sizeof(text), &at,
                         "# Omni360 - the missing-games list, marked by hand\n"
                         "#\n"
                         "# Each line is a title ID from src/CatalogTitles.h and why it shouldn't be\n"
                         "# in the \"games we don't have\" list. Everything but \"keep\" is something to\n"
                         "# leave out of the next generated list; \"keep\" says the row really is a\n"
                         "# game nobody has uploaded, and belongs there.\n"
                         "#\n"
                         "# The name after each '#' is only for reading - the title ID is the record.\n"
                         "format=1\n"
                         "marked=%d\n"
                         "catalog=%d\n",
                         g_markCount, CatalogTitleCount());

    // Grouped by mark, so the file reads as a list of decisions rather than a
    // list of IDs. Within a group they stay in the order they were marked.
    for (int m = 1; m < MISSING_MARK_COUNT; ++m)
    {
        int inGroup = 0;
        for (int i = 0; i < g_markCount; ++i)
        {
            if (g_marks[i].mark == m)
                inGroup++;
        }
        if (inGroup == 0)
            continue;

        AppendQueueStateText(text, sizeof(text), &at, "\n# %s (%d)\n", MissingMarkName(m), inGroup);
        for (int i = 0; i < g_markCount; ++i)
        {
            if (g_marks[i].mark != m)
                continue;
            if (g_marks[i].name[0] != '\0')
                AppendQueueStateText(text, sizeof(text), &at, "%08lX=%s  # %s\n", g_marks[i].titleId,
                                     MissingMarkKey(m), g_marks[i].name);
            else
                AppendQueueStateText(text, sizeof(text), &at, "%08lX=%s\n", g_marks[i].titleId, MissingMarkKey(m));
        }
    }

    return WriteQueueStateFile(MISSING_MARKS_FILE, text);
}

// ---------------------------------------------------------------------------
// What the screens ask
// ---------------------------------------------------------------------------

int MissingMarkOf(unsigned long titleId)
{
    ReadMarks();
    for (int i = 0; i < g_markCount; ++i)
    {
        if (g_marks[i].titleId == titleId)
            return g_marks[i].mark;
    }
    return MISSING_MARK_NONE;
}

bool SetMissingMark(unsigned long titleId, int mark)
{
    ReadMarks();
    if (titleId == 0 || mark < 0 || mark >= MISSING_MARK_COUNT)
        return false;

    int found = -1;
    for (int i = 0; i < g_markCount && found < 0; ++i)
    {
        if (g_marks[i].titleId == titleId)
            found = i;
    }

    if (mark == MISSING_MARK_NONE)
    {
        if (found < 0)
            return true; // nothing to unmark
        for (int i = found; i + 1 < g_markCount; ++i)
            g_marks[i] = g_marks[i + 1];
        g_markCount--;
    }
    else if (found >= 0)
    {
        g_marks[found].mark = (unsigned char)mark;
        if (g_marks[found].name[0] == '\0')
            SetEntryName(g_marks[found], CatalogNameOf(titleId));
    }
    else
    {
        if (g_markCount >= MISSING_MARKS_MAX)
        {
            dprintf("[missing] no room for another mark - %d is the most\n", MISSING_MARKS_MAX);
            return false;
        }
        MarkEntry &entry = g_marks[g_markCount++];
        entry.titleId = titleId;
        entry.mark = (unsigned char)mark;
        SetEntryName(entry, CatalogNameOf(titleId));
    }

    return WriteMarks();
}

int MissingMarkedCount()
{
    ReadMarks();
    int marked = 0;
    for (int i = 0; i < g_markCount; ++i)
    {
        // Only the catalog's own rows count towards "N of 312": a mark left
        // over from a list already curated isn't on the screen to be seen.
        if (g_marks[i].mark != MISSING_MARK_NONE && CatalogNameOf(g_marks[i].titleId) != NULL)
            marked++;
    }
    return marked;
}
