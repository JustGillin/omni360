/*
FILE : Strings.h
PROJECT : Omni360
DESCRIPTION : Looks up the text Omni360 shows, by ID, so it can be translated.

The text itself lives in Strings.def. Call sites name a string by its ID
instead of spelling out English:

    Text(x, y, 1.0f, COL_TEXT, Tr(STR_LIB_NO_GAMES));

Tr hands back UTF-8 for the Text helpers; TrW hands back the same string as
WCHAR for the places that hold wide text (button hints). Both pointers stay
valid for the life of the program.

For text with run-time pieces, TrFormat fills {1}..{4} with strings:

    char line[128];
    TrFormat(line, sizeof(line), STR_STORE_NO_MATCH, filter);

Numbers go in as text (format them first), which keeps the one function
type-safe - a %s handed an int is a crash on this console.

Other languages come from Languages/<code>.txt, compiled in by
tools/make_languages.py (see Languages/README.md). A file of the same name
in game:\Languages\ beside the XEX is read instead of the built-in copy, so
a translation can be tried on the console without a rebuild. Any line a
translation leaves out, or gets wrong, stays English.
*/

#pragma once

#include <xtl.h> // WCHAR, size_t

enum StringId
{
#define STR(id, text) STR_##id,
#include "Strings.def"
#undef STR
    STR_COUNT
};

const char  *Tr(StringId id);
const WCHAR *TrW(StringId id);

// Fills {1}..{4} with a1..a4 (NULL reads as empty). Always terminates
// out; text past outSize is cut, never overrun. Returns out.
char *TrFormat(char *out, size_t outSize, StringId id,
               const char *a1 = NULL, const char *a2 = NULL, const char *a3 = NULL,
               const char *a4 = NULL);

// A number as text, for TrFormat. Eight buffers in turn, so up to eight can
// be in one call; UI thread only.
const char *TrNum(long n);

// TrFormat with {1} = n, picking idOne when n is 1 and idOther otherwise.
// Languages with more plural forms will need more than this; it is the
// one place to grow them.
char *TrCount(char *out, size_t outSize, StringId idOne, StringId idOther, long n,
              const char *a2 = NULL, const char *a3 = NULL);

// For English that comes from generated data rather than code - the Store's
// genre and row names, its collections' names: the current language's text
// for the Strings.def line whose English is exactly this, or the English
// unchanged when there is none (a game series' or publisher's name).
const char *TrEnglish(const char *english);

// ---- Languages -------------------------------------------------------------
// The languages there are to pick: English first, then the built-in ones,
// then any extra files in game:\Languages\. Call once at start; again to
// pick up files added since.
void FindLanguages();
int         LanguageCount();
const char *LanguageCode(int index); // "en", "fr", ...
const char *LanguageName(int index); // in its own language: "Français"
const char *LanguageContributor(int index);       // @contributor: who translated it; may be ""
bool        LanguageMachineTranslated(int index); // @machine = yes: no person has checked it yet
const char *LanguageFlag(int index);              // @flag: "br", "de"... (Flags.h); may be ""

// ---- The clock -------------------------------------------------------------
// 12- or 24-hour. By default the language says (@clock = 24 in its file;
// 12 when it doesn't); Settings can pin either.
enum ClockStyle
{
    CLOCK_FOLLOW_LANGUAGE,
    CLOCK_12_HOUR,
    CLOCK_24_HOUR,
    CLOCK_STYLE_COUNT
};
void       SetClockStyle(ClockStyle style);
ClockStyle GetClockStyle();
bool       Clock24Hour(); // what the clock is now, after the setting and the language

// The time as the header and Home show it: "9:41 PM" or "21:41".
void FormatClock(char *out, size_t outSize, const SYSTEMTIME &time);

// Switches every Tr lookup to the language with this code; an unknown code,
// or "en", is English. Returns the index now in use. Pointers Tr gave out
// before stay valid - old tables are kept, not freed - so a worker thread
// mid-sentence is never left holding freed text.
int SetLanguage(const char *code);
int CurrentLanguage();

// How many of the table's lines the current language translates.
int TranslatedCount();

// The console's own language as one of our codes ("en" when it isn't one
// we know), for the default before anyone picks.
const char *ConsoleLanguageCode();

// The Xbox Live locale for the current language ("fr-FR"), for asking the
// catalog for text in it; en-US for one Xbox Live never had.
const char *XboxLocaleForLanguage();
