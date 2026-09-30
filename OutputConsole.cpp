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

void SetConsoleQuiet(int quiet)
{
	g_consoleQuiet = (quiet != 0);
}

#ifdef USE_UNICODE
WCHAR buf[TEXTBUFFER_SIZE]; // Text buffer
void  __cdecl dprintf(const wchar_t* strFormat, ...)
#else
char buf[TEXTBUFFER_SIZE]; // Text buffer
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
	if (g_consoleQuiet)
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

	FILE* fp = NULL;
    errno_t err = fopen_s(&fp, LOG_FILE_PATH, "a+");
    if(fp) {
        fprintf(fp, "%s", buf);
        fclose(fp);
    } else {
		printf("Failed to open log file\n");
	}

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

	printf("%s", buf);
	
	FILE* fp = NULL;
    errno_t err = fopen_s(&fp, LOG_FILE_PATH, "a+");
    if(fp) {
        fprintf(fp, "%s", buf);
        fclose(fp);
    }

	LeaveCriticalSection(&g_logLock.cs);
}
