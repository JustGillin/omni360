/*
FILE : OutputConsole.cpp
PROJECT : Omni360
PROGRAMMER : 951261
DESCRIPTION : Writes text to the screen. Also writes to debug file
*/

#include "AtgConsole.h"
#include "AtgUtil.h"
#include "OutputConsole.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEXTBUFFER_SIZE (1024 * 10) // 2KB buffer

ATG::Console g_console;// console for output

// See SetConsoleQuiet in OutputConsole.h for why this exists.
static bool g_consoleQuiet = false;

// dprintf and log_printf format into the shared buf below and append to the
// same log file, and downloads are moving onto a worker thread - so two lines
// logged at once from different threads would otherwise overwrite each
// other's text mid-format. A static object rather than lazy setup, so the
// lock exists before main() and before any thread could reach it.
struct LogLock
{
	CRITICAL_SECTION cs;
	LogLock() { InitializeCriticalSection(&cs); }
};
static LogLock g_logLock;

// Once the drawn UI is up, lines go into memory and a thread on core 5
// writes them out every quarter second. Opening, appending and closing the
// file for each line, under the lock above, held the UI thread behind the
// hard drive whenever something else had it busy - a library scan at
// startup - and a slow frame's own "slow frame" line slowed the next. Before
// the UI (and if the thread can't start) each line is written as it comes,
// so a crash during startup still leaves its last line in the file. LogFlush
// writes what's waiting now: before the app ends or starts something else.
#define LOG_PENDING_SIZE (64 * 1024)
static char g_logPending[LOG_PENDING_SIZE];
static size_t g_logPendingLen = 0;
static bool g_logBuffered = false;

// Only one writer at a time, so lines reach the file in order. Taken before
// g_logLock, never while holding it.
struct LogWriteLock
{
	CRITICAL_SECTION cs;
	LogWriteLock() { InitializeCriticalSection(&cs); }
};
static LogWriteLock g_logWriteLock;

static void AppendLogFile(const char *text, size_t len)
{
	if (len == 0)
		return;
	FILE* fp = NULL;
	fopen_s(&fp, LOG_FILE_PATH, "ab");
	if (fp)
	{
		fwrite(text, 1, len, fp);
		fclose(fp);
	}
}

void LogFlush()
{
	static char writing[LOG_PENDING_SIZE];
	EnterCriticalSection(&g_logWriteLock.cs);
	EnterCriticalSection(&g_logLock.cs);
	const size_t len = g_logPendingLen;
	memcpy(writing, g_logPending, len);
	g_logPendingLen = 0;
	LeaveCriticalSection(&g_logLock.cs);
	AppendLogFile(writing, len);

	// The debug channel, a line at a time, as printf sent them before.
	size_t at = 0;
	while (at < len)
	{
		char line[1024];
		size_t n = 0;
		while (at < len && n < sizeof(line) - 1)
		{
			const char c = writing[at++];
			line[n++] = c;
			if (c == '\n')
				break;
		}
		line[n] = '\0';
		OutputDebugStringA(line);
	}
	LeaveCriticalSection(&g_logWriteLock.cs);
}

static DWORD WINAPI LogWriterEntry(LPVOID)
{
	for (;;)
	{
		Sleep(250);
		LogFlush();
	}
}

static void StartLogWriter()
{
	if (g_logBuffered)
		return;
	HANDLE thread = CreateThread(NULL, 32 * 1024, LogWriterEntry, NULL, CREATE_SUSPENDED, NULL);
	if (thread == NULL)
		return; // each line written as it comes, as before
	XSetThreadProcessor(thread, 5);
	ResumeThread(thread);
	CloseHandle(thread);
	g_logBuffered = true;
}

#ifdef USE_UNICODE
WCHAR buf[TEXTBUFFER_SIZE]; // Text buffer
#else
char buf[TEXTBUFFER_SIZE]; // Text buffer
#endif

// buf, just formatted, to the file or the pending lines. Called holding
// g_logLock, which it may let go of and take again: when the pending lines
// are full - a burst faster than the writer - they're written out first,
// by LogFlush so as to stay in order, with this line kept aside meanwhile.
static void LogLineLocked()
{
#ifndef USE_UNICODE
	const size_t len = strlen(buf);
	if (!g_logBuffered)
	{
		AppendLogFile(buf, len);
		return;
	}
	if (g_logPendingLen + len > LOG_PENDING_SIZE)
	{
		char *line = (char *)malloc(len + 1);
		if (line == NULL)
			return;
		memcpy(line, buf, len + 1);
		LeaveCriticalSection(&g_logLock.cs);
		LogFlush();
		EnterCriticalSection(&g_logLock.cs);
		if (g_logPendingLen + len <= LOG_PENDING_SIZE)
		{
			memcpy(g_logPending + g_logPendingLen, line, len);
			g_logPendingLen += len;
		}
		free(line);
		return;
	}
	memcpy(g_logPending + g_logPendingLen, buf, len);
	g_logPendingLen += len;
#endif
}

void SetConsoleQuiet(int quiet)
{
	g_consoleQuiet = (quiet != 0);
	if (g_consoleQuiet)
		StartLogWriter();
}

#ifdef USE_UNICODE
void  __cdecl dprintf(const wchar_t* strFormat, ...)
#else
void  __cdecl dprintf(const char* strFormat, ...)
#endif
{
	FILE* flog;
	va_list pArglist;
	EnterCriticalSection(&g_logLock.cs);
	va_start(pArglist, strFormat);
#ifdef USE_UNICODE
	_vsnwprintf_s(buf, TEXTBUFFER_SIZE, strFormat, pArglist);
#else
	vsnprintf_s(buf, TEXTBUFFER_SIZE, strFormat, pArglist);
#endif
	va_end(pArglist);

	// Display() repaints the console to the framebuffer, so in quiet mode it
	// is skipped entirely and printf carries the line instead - printf reaches
	// the debug channel without drawing anything, which is exactly the split
	// we want once the drawn UI owns the screen.
	//
	// Only ever ONE of these two runs. Display() already forwards to the debug
	// channel itself (MakeConsole turns on SendOutputToDebugChannel), so doing
	// both would emit every line twice - which is a bug this file used to have.
	//
	// With the writer thread running, it sends the lines to the debug channel
	// too: with XBDM loaded that took ~17 ms a line, on whichever thread
	// logged it - the UI's, for the slow-frame lines themselves.
	if (g_logBuffered)
		;
	else if (g_consoleQuiet)
		printf("%s", buf);
	else
		g_console.Display(buf);
 	// g_console.Format("%s", buf);
#ifdef USE_UNICODE
	if (!fexists("game:\\Simple 360 NAND Flasher.log"))
	{
		fopen_s(&flog, "game:\\Simple 360 NAND Flasher.log", "wb");
		char* cStart = "\xfe\xff";
		fwrite(cStart, strlen(cStart), 1, flog);
		fclose(flog);
	}
	if (wcsncmp(strFormat, MSG_PROCESSING_START, wcslen(MSG_PROCESSING_START)) != 0 && wcsncmp(strFormat, MSG_PROCESSED_START, wcslen(MSG_PROCESSED_START)) != 0)
	{
		fopen_s(&flog, "game:\\Simple 360 NAND Flasher.log", "ab");
		if (flog != NULL)
		{
			fwrite(buf, wcslen(buf) * sizeof(wchar_t), 1, flog);
			fclose(flog);
		}
	}
#else

	// No printf("%s", buf) here: g_console.Display() above ALREADY writes this
	// same line to the debug channel, because MakeConsole() turns on
	// SendOutputToDebugChannel (AtgConsole.cpp's Display calls
	// OutputDebugStringA when that flag is set). printf on this platform goes
	// to the same channel, so having both emitted every single line twice in
	// the VS output window - and paid twice for output on a path that already
	// measurably throttles download throughput.
	//
	// Note this does NOT apply to log_printf below, which has no Display call
	// at all: its printf is that function's only console output and must stay.

	LogLineLocked();

#endif
	LeaveCriticalSection(&g_logLock.cs);
}

void MakeConsole(const char* font, unsigned long BackgroundColor, unsigned long TextColor)
{
	g_console.Create(font, BackgroundColor, TextColor);
	g_console.SendOutputToDebugChannel(TRUE);
}

void ClearConsole()
{
	g_console.Clear();
}

void __cdecl log_printf(const char* strFormat, ...) {
	va_list pArglist;
	EnterCriticalSection(&g_logLock.cs);
	va_start(pArglist, strFormat);
#ifdef USE_UNICODE
	_vsnwprintf_s(buf, TEXTBUFFER_SIZE, strFormat, pArglist);
#else
	vsnprintf_s(buf, TEXTBUFFER_SIZE, strFormat, pArglist);
#endif
	va_end(pArglist);

	if (!g_logBuffered)
		printf("%s", buf);
	LogLineLocked();
	LeaveCriticalSection(&g_logLock.cs);
}
