/*
FILE : GameMarks.cpp
PROJECT : Omni360
DESCRIPTION : Favourites and renamed games - see GameMarks.h.
*/

#include "GameMarks.h"
#include "OutputConsole.h"
#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_MARKS_FILE "game:\\GameMarks.txt"
#define GAME_MARKS_MAX  512
#define MARK_NAME_SIZE  128

enum MarkKind
{
    MARK_FAVOURITE,
    MARK_RENAME
};

struct GameMark
{
    MarkKind kind;
    unsigned long titleId;
    char name[MARK_NAME_SIZE];   // the package's own
    char shown[MARK_NAME_SIZE];  // a rename's; empty otherwise
};

static GameMark g_marks[GAME_MARKS_MAX];
static int g_markCount = 0;

static void CopyName(char *out, const char *from)
{
    strncpy(out, from != NULL ? from : "", MARK_NAME_SIZE - 1);
    out[MARK_NAME_SIZE - 1] = '\0';
    // Tabs and line breaks would split the line when it's read back.
    for (char *p = out; *p != '\0'; ++p)
    {
        if (*p == '\t' || *p == '\r' || *p == '\n')
            *p = ' ';
    }
}

static int FindMark(MarkKind kind, unsigned long titleId, const char *name)
{
    char key[MARK_NAME_SIZE];
    CopyName(key, name);
    for (int i = 0; i < g_markCount; ++i)
    {
        if (g_marks[i].kind == kind && g_marks[i].titleId == titleId && strcmp(g_marks[i].name, key) == 0)
            return i;
    }
    return -1;
}

void LoadGameMarks()
{
    g_markCount = 0;
    FILE *f = fopen(GAME_MARKS_FILE, "r");
    if (f == NULL)
        return;

    char line[512];
    while (fgets(line, sizeof(line), f) != NULL && g_markCount < GAME_MARKS_MAX)
    {
        line[strcspn(line, "\r\n")] = '\0';
        char *fields[4] = { NULL, NULL, NULL, NULL };
        int n = 0;
        for (char *p = line; n < 4;)
        {
            fields[n++] = p;
            char *tab = strchr(p, '\t');
            if (tab == NULL)
                break;
            *tab = '\0';
            p = tab + 1;
        }
        if (n < 3)
            continue;

        GameMark &mark = g_marks[g_markCount];
        if (strcmp(fields[0], "fav") == 0)
            mark.kind = MARK_FAVOURITE;
        else if (strcmp(fields[0], "name") == 0 && n >= 4 && fields[3][0] != '\0')
            mark.kind = MARK_RENAME;
        else
            continue;
        mark.titleId = strtoul(fields[1], NULL, 16);
        CopyName(mark.name, fields[2]);
        CopyName(mark.shown, mark.kind == MARK_RENAME ? fields[3] : "");
        g_markCount++;
    }
    fclose(f);
    dprintf("[marks] %d favourites and renames\n", g_markCount);
}

// The whole file again, to a temporary first so a failed write can't leave
// it half there.
static bool SaveGameMarks()
{
    const char *temp = GAME_MARKS_FILE ".tmp";
    FILE *f = fopen(temp, "w");
    if (f == NULL)
    {
        dprintf("[marks] couldn't write %s\n", temp);
        return false;
    }
    bool ok = true;
    for (int i = 0; i < g_markCount && ok; ++i)
    {
        const GameMark &mark = g_marks[i];
        if (mark.kind == MARK_FAVOURITE)
            ok = fprintf(f, "fav\t%08lX\t%s\n", mark.titleId, mark.name) > 0;
        else
            ok = fprintf(f, "name\t%08lX\t%s\t%s\n", mark.titleId, mark.name, mark.shown) > 0;
    }
    if (fclose(f) != 0)
        ok = false;
    if (!ok)
    {
        DeleteFileA(temp);
        dprintf("[marks] couldn't write %s\n", temp);
        return false;
    }
    DeleteFileA(GAME_MARKS_FILE);
    if (!MoveFileA(temp, GAME_MARKS_FILE))
    {
        dprintf("[marks] couldn't replace %s: %lu\n", GAME_MARKS_FILE, GetLastError());
        return false;
    }
    return true;
}

static void RemoveMark(int i)
{
    for (int j = i; j + 1 < g_markCount; ++j)
        g_marks[j] = g_marks[j + 1];
    g_markCount--;
}

bool IsFavourite(unsigned long titleId, const char *name)
{
    return FindMark(MARK_FAVOURITE, titleId, name) >= 0;
}

int FavouriteCount()
{
    int count = 0;
    for (int i = 0; i < g_markCount; ++i)
    {
        if (g_marks[i].kind == MARK_FAVOURITE)
            count++;
    }
    return count;
}

bool SetFavourite(unsigned long titleId, const char *name, bool favourite)
{
    const int at = FindMark(MARK_FAVOURITE, titleId, name);
    if (favourite == (at >= 0))
        return true;
    if (favourite)
    {
        if (g_markCount >= GAME_MARKS_MAX)
            return false;
        GameMark &mark = g_marks[g_markCount++];
        mark.kind = MARK_FAVOURITE;
        mark.titleId = titleId;
        CopyName(mark.name, name);
        mark.shown[0] = '\0';
    }
    else
    {
        RemoveMark(at);
    }
    return SaveGameMarks();
}

const char *RenamedTo(unsigned long titleId, const char *name)
{
    const int at = FindMark(MARK_RENAME, titleId, name);
    return at >= 0 ? g_marks[at].shown : NULL;
}

bool SetRename(unsigned long titleId, const char *name, const char *newName)
{
    const int at = FindMark(MARK_RENAME, titleId, name);
    char shown[MARK_NAME_SIZE];
    CopyName(shown, newName);
    // Spaces alone, or the original again, is no rename at all.
    const bool clearing = shown[strspn(shown, " ")] == '\0' || strcmp(shown, name) == 0;

    if (clearing)
    {
        if (at < 0)
            return true;
        RemoveMark(at);
    }
    else if (at >= 0)
    {
        strcpy(g_marks[at].shown, shown);
    }
    else
    {
        if (g_markCount >= GAME_MARKS_MAX)
            return false;
        GameMark &mark = g_marks[g_markCount++];
        mark.kind = MARK_RENAME;
        mark.titleId = titleId;
        CopyName(mark.name, name);
        strcpy(mark.shown, shown);
    }
    return SaveGameMarks();
}
