#ifndef PARSING_H
#define PARSING_H

struct ChunkedDecodeState
{
    unsigned long remaining;
    char sizeLine[32];
    int sizeLineLen;
    int state;
};

void InitChunkedDecodeState(ChunkedDecodeState *state);
bool ParseChunkSize(const char *line, int len, unsigned long *size);
int HexValue(char c);
bool HeaderContainsToken(const char *headers, int headerLen, const char *name, const char *token);
int FindHeaderEnd(const char *data, int len);
bool IncrementSplitFilename(char *filename);
bool ContainsNoCase(const char *data, int dataLen, const char *needle);
bool EqualsNoCase(const char *a, int aLen, const char *b);
char LowerAscii(char c);
int parseURL(const char *URL, char *domain, char *path);

// Scans a raw HTTP header block for one or more "Set-Cookie:" lines and
// appends each cookie's "name=value" pair (the part before the first ';')
// into cookieJar as "name=value; name2=value2; ". Safe to call repeatedly
// across multiple requests to accumulate a session's cookies; existing
// cookieJar content is preserved and appended to, not overwritten.
bool ExtractSetCookies(const char *headers, int headerLen, char *cookieJar, unsigned long long cookieJarSize);

// URL-encodes a single form value (application/x-www-form-urlencoded rules)
// from src into dst. Returns false if dst is too small.
bool UrlEncodeFormValue(const char *src, char *dst, unsigned long long dstSize);

// Doubles every '%' so a string survives being LOGGED.
//
// Strings containing percent signs - percent-encoded URLs above all - come out
// mangled in this project's log, and confirmed on hardware it is the log that
// is wrong, not the string: a path measured at the correct 64 characters
// printed as garbage while the request built from it downloaded fine.
//
// Two observations pin the cause down to the formatted buffer being run
// through a formatter a SECOND time:
//
//   - "%%" in a format string prints nothing. Pass one turns it into a lone
//     '%', and pass two consumes that as the start of a conversion.
//   - A URL passed as a plain %s argument gets eaten. It lands in the buffer
//     intact, then pass two reads "%20G" as a width-20 float conversion and
//     prints 7.90505E-323 in the middle of the URL.
//
// Doubling each '%' in the ARGUMENT survives pass one untouched and collapses
// back to a single '%' in pass two. If that theory is ever wrong, the symptom
// is obvious and harmless - doubled percent signs in the log rather than
// missing ones.
//
// Returns false if dst is too small; dst is always null-terminated.
bool LogEscapePercent(const char *src, char *dst, unsigned long long dstSize);

#endif
