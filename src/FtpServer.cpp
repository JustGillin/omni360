/*
FILE : FtpServer.cpp
PROJECT : Omni360
DESCRIPTION : An FTP server for the console's drives - see FtpServer.h.

One thread accepts connections; each client gets a thread of its own for its
control connection, and its transfers run on that thread too, one at a time,
as FTP has them. Paths the client sees are "/Hdd1/Games/Halo 3"; on the
console that's "Hdd1:\Games\Halo 3".
*/

#include "FtpServer.h"
#include "OutputConsole.h"
#include "parsing.h" // LogEscapePercent
#include <xtl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XBOX_SO_BYPASS_SECURITY 0x5801

#define FTP_MAX_CLIENTS  4
#define FTP_BUFFER       (256 * 1024) // a transfer's chunk; the console's window per socket is about this
#define FTP_PATH_SIZE    512
#define FTP_LINE_SIZE    1024
#define FTP_IDLE_MS      (15 * 60 * 1000) // a client that says nothing for this long is dropped
#define FTP_DATA_WAIT_MS 15000            // for the client to open the data connection it asked for
#define FTP_PASV_FIRST   36200            // passive ports handed out in turn from here
#define FTP_PASV_COUNT   100

// The drives at the top level: what the client sees, and the console's alias.
struct FtpDrive
{
    const char *name;
    const char *alias;
};
static const FtpDrive kFtpDrives[] = {
    { "Hdd1", "Hdd1:" }, { "Usb0", "Usb0:" }, { "Usb1", "Usb1:" }, { "Usb2", "Usb2:" }, { "Game", "game:" },
};
#define FTP_DRIVE_COUNT ((int)(sizeof(kFtpDrives) / sizeof(kFtpDrives[0])))

static const char *const kMonths[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

// ---------------------------------------------------------------------------
// The server's state
// ---------------------------------------------------------------------------

struct FtpSession
{
    SOCKET control;
    SOCKET passive;      // listening for the next data connection, or INVALID_SOCKET
    SOCKET data;         // the data connection while a transfer runs
    HANDLE thread;
    bool used;

    char cwd[FTP_PATH_SIZE]; // as the client sees it: "/" or "/Hdd1/Games"
    bool gotUser;
    bool userMatched;
    bool loggedIn;
    unsigned long long restAt;           // REST: where the next transfer starts
    char renameFrom[FTP_PATH_SIZE];      // RNFR's path, on the console; "" for none
    char clientName[24];                 // its address, for the log

    char in[FTP_LINE_SIZE];              // what's come in on the control connection
    int inLength;
    char *buffer;                        // FTP_BUFFER bytes, for transfers and listings
};

static CRITICAL_SECTION g_ftpLock;
static bool g_ftpLockReady = false;
static volatile LONG g_ftpStop = 0;
static SOCKET g_ftpListen = INVALID_SOCKET;
static HANDLE g_ftpThread = NULL;
static FtpSession g_ftpSessions[FTP_MAX_CLIENTS];
static char g_ftpUser[64];
static char g_ftpPassword[64];
static LONG g_ftpNextPort = 0;

static void EnsureFtpLock()
{
    if (!g_ftpLockReady)
    {
        InitializeCriticalSection(&g_ftpLock);
        g_ftpLockReady = true;
    }
}

static bool Stopping()
{
    return InterlockedCompareExchange(&g_ftpStop, 0, 0) != 0;
}

// A path or name for a log line: dprintf formats its output a second time,
// so a % in a file name has to be doubled first.
static const char *ForLog(const char *text, char *buffer, unsigned long long size)
{
    if (!LogEscapePercent(text, buffer, size))
        return "(name too long to log)";
    return buffer;
}

// ---------------------------------------------------------------------------
// Sockets
// ---------------------------------------------------------------------------

static bool SendAll(SOCKET s, const char *data, int length)
{
    while (length > 0)
    {
        const int sent = send(s, data, length, 0);
        if (sent <= 0)
            return false;
        data += sent;
        length -= sent;
    }
    return true;
}

static bool Reply(FtpSession &session, const char *format, ...)
{
    char line[FTP_PATH_SIZE + 128];
    va_list args;
    va_start(args, format);
    int length = _vsnprintf(line, sizeof(line) - 2, format, args);
    va_end(args);
    if (length < 0)
        length = (int)sizeof(line) - 3;
    line[length++] = '\r';
    line[length++] = '\n';
    return SendAll(session.control, line, length);
}

static void CloseSocket(SOCKET &s)
{
    if (s != INVALID_SOCKET)
    {
        closesocket(s);
        s = INVALID_SOCKET;
    }
}

static void BypassSecurity(SOCKET s)
{
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, XBOX_SO_BYPASS_SECURITY, (PCSTR)&yes, sizeof(yes));
}

// One command line from the client, without its CRLF. False when the client
// has gone, said nothing for too long, or the server is stopping.
static bool ReadLine(FtpSession &session, char *out, int outSize)
{
    for (;;)
    {
        for (int i = 0; i < session.inLength; ++i)
        {
            if (session.in[i] != '\n')
                continue;
            int length = i;
            if (length > 0 && session.in[length - 1] == '\r')
                length--;
            if (length > outSize - 1)
                length = outSize - 1;
            memcpy(out, session.in, length);
            out[length] = '\0';
            memmove(session.in, session.in + i + 1, session.inLength - (i + 1));
            session.inLength -= i + 1;
            return true;
        }
        if (session.inLength >= (int)sizeof(session.in))
            session.inLength = 0; // a line longer than any command - throw it away

        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(session.control, &readable);
        timeval wait;
        wait.tv_sec = FTP_IDLE_MS / 1000;
        wait.tv_usec = 0;
        if (select(0, &readable, NULL, NULL, &wait) <= 0 || Stopping())
            return false;
        const int got = recv(session.control, session.in + session.inLength,
                             (int)sizeof(session.in) - session.inLength, 0);
        if (got <= 0)
            return false;
        session.inLength += got;
    }
}

// The console's own address, for PASV's reply.
static bool ConsoleAddress(IN_ADDR *out)
{
    XNADDR xna;
    memset(&xna, 0, sizeof(xna));
    const DWORD flags = XNetGetTitleXnAddr(&xna);
    if (flags == XNET_GET_XNADDR_PENDING || (flags & XNET_GET_XNADDR_NONE) || xna.ina.s_addr == 0)
        return false;
    *out = xna.ina;
    return true;
}

// A socket listening for the data connection: the next port of the range,
// trying on past one that's still taken. Its port in *port.
static SOCKET OpenPassive(int *port)
{
    for (int tries = 0; tries < 20; ++tries)
    {
        const int p = FTP_PASV_FIRST + (int)(InterlockedIncrement(&g_ftpNextPort) % FTP_PASV_COUNT);
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET)
            return INVALID_SOCKET;
        BypassSecurity(s);
        // The receive window is agreed as the connection opens, so it has to
        // be set here, on the socket the data connection is accepted from -
        // set on the accepted one it came too late, and uploads crawled at
        // 0.2 MB/s on the default window while downloads ran at 7.
        int window = FTP_BUFFER;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, (PCSTR)&window, sizeof(window));
        setsockopt(s, SOL_SOCKET, SO_SNDBUF, (PCSTR)&window, sizeof(window));
        sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons((u_short)p);
        sa.sin_addr.s_addr = INADDR_ANY;
        if (bind(s, (sockaddr *)&sa, sizeof(sa)) == 0 && listen(s, 1) == 0)
        {
            *port = p;
            return s;
        }
        closesocket(s);
    }
    return INVALID_SOCKET;
}

// The data connection the client was told to open, once it has. Replies
// 150 first, as clients wait for it before connecting on some setups.
static bool AcceptData(FtpSession &session, const char *what)
{
    if (session.passive == INVALID_SOCKET)
    {
        Reply(session, "425 Use PASV or EPSV first.");
        return false;
    }
    Reply(session, "150 Opening data connection for %s.", what);

    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(session.passive, &readable);
    timeval wait;
    wait.tv_sec = FTP_DATA_WAIT_MS / 1000;
    wait.tv_usec = 0;
    SOCKET data = INVALID_SOCKET;
    if (select(0, &readable, NULL, NULL, &wait) > 0)
        data = accept(session.passive, NULL, NULL);

    EnterCriticalSection(&g_ftpLock);
    CloseSocket(session.passive);
    session.data = data;
    LeaveCriticalSection(&g_ftpLock);

    if (data == INVALID_SOCKET)
    {
        Reply(session, "425 The data connection didn't open.");
        return false;
    }
    BypassSecurity(data);
    int window = FTP_BUFFER;
    setsockopt(data, SOL_SOCKET, SO_SNDBUF, (PCSTR)&window, sizeof(window));
    setsockopt(data, SOL_SOCKET, SO_RCVBUF, (PCSTR)&window, sizeof(window));
    return true;
}

static void CloseData(FtpSession &session)
{
    EnterCriticalSection(&g_ftpLock);
    CloseSocket(session.data);
    LeaveCriticalSection(&g_ftpLock);
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

// The client's path against the current folder, made whole: "/Hdd1/Games"
// for "Games" in "/Hdd1", with "." and ".." worked out. Always starts with
// "/"; the top level is "/" alone.
static bool ResolvePath(const char *cwd, const char *arg, char *out, size_t outSize)
{
    char joined[FTP_PATH_SIZE * 2];
    if (arg == NULL || arg[0] == '\0')
        _snprintf(joined, sizeof(joined), "%s", cwd);
    else if (arg[0] == '/' || arg[0] == '\\')
        _snprintf(joined, sizeof(joined), "%s", arg);
    else
        _snprintf(joined, sizeof(joined), "%s/%s", cwd, arg);
    joined[sizeof(joined) - 1] = '\0';

    // Down the parts, keeping a stack of where each one started in out.
    size_t length = 0;
    int starts[64];
    int depth = 0;
    out[0] = '\0';
    char *part = joined;
    while (part != NULL)
    {
        char *next = part + strcspn(part, "/\\");
        const bool last = (*next == '\0');
        *next = '\0';
        if (strcmp(part, "..") == 0)
        {
            if (depth > 0)
                length = (size_t)starts[--depth];
            out[length] = '\0';
        }
        else if (part[0] != '\0' && strcmp(part, ".") != 0)
        {
            if (depth >= 64 || length + strlen(part) + 2 > outSize)
                return false;
            starts[depth++] = (int)length;
            out[length++] = '/';
            strcpy(out + length, part);
            length += strlen(part);
        }
        part = last ? NULL : next + 1;
    }
    if (length == 0)
        strcpy(out, "/");
    return true;
}

// The console's path for one the client sees: "Hdd1:\Games" for
// "/Hdd1/Games". False for the top level, which is no folder on the console,
// and for a drive that isn't one of the list.
static bool ToConsolePath(const char *path, char *out, size_t outSize)
{
    const char *rest = path + 1;
    const size_t nameLength = strcspn(rest, "/");
    if (nameLength == 0)
        return false;
    const FtpDrive *drive = NULL;
    for (int i = 0; i < FTP_DRIVE_COUNT && drive == NULL; ++i)
    {
        if (strlen(kFtpDrives[i].name) == nameLength && _strnicmp(rest, kFtpDrives[i].name, nameLength) == 0)
            drive = &kFtpDrives[i];
    }
    if (drive == NULL)
        return false;

    const int written = _snprintf(out, outSize, "%s%s", drive->alias, rest + nameLength);
    if (written < 0 || (size_t)written >= outSize)
        return false;
    for (char *p = out; *p != '\0'; ++p)
    {
        if (*p == '/')
            *p = '\\';
    }
    return true;
}

// GetFileAttributes for a console path - a bare drive needs its separator.
static DWORD Attributes(const char *consolePath)
{
    char probe[FTP_PATH_SIZE + 2];
    const size_t length = strlen(consolePath);
    _snprintf(probe, sizeof(probe), (length > 0 && consolePath[length - 1] == ':') ? "%s\\" : "%s", consolePath);
    probe[sizeof(probe) - 1] = '\0';
    return GetFileAttributesA(probe);
}

static bool IsFolder(const char *consolePath)
{
    const DWORD a = Attributes(consolePath);
    return a != (DWORD)-1 && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// The size and time of one file, as a listing would give them.
static bool FileInfo(const char *consolePath, unsigned long long *size, FILETIME *written)
{
    WIN32_FIND_DATAA found;
    HANDLE h = FindFirstFileA(consolePath, &found);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    FindClose(h);
    if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return false;
    if (size != NULL)
        *size = ((unsigned long long)found.nFileSizeHigh << 32) | found.nFileSizeLow;
    if (written != NULL)
        *written = found.ftLastWriteTime;
    return true;
}

// ---------------------------------------------------------------------------
// Listings
// ---------------------------------------------------------------------------

// One line of LIST, as ls -l writes it - what every client parses.
static int ListLine(char *out, int outSize, bool folder, unsigned long long size, const FILETIME *when,
                    const char *name, const SYSTEMTIME &now)
{
    SYSTEMTIME t;
    if (when == NULL || !FileTimeToSystemTime(when, &t))
        t = now;
    // The time for this year's files, the year for older ones, as ls does.
    char stamp[16];
    if (t.wYear == now.wYear)
        _snprintf(stamp, sizeof(stamp), "%02d:%02d", t.wHour, t.wMinute);
    else
        _snprintf(stamp, sizeof(stamp), " %04d", t.wYear);
    stamp[sizeof(stamp) - 1] = '\0';
    const int month = (t.wMonth >= 1 && t.wMonth <= 12) ? t.wMonth - 1 : 0;
    return _snprintf(out, outSize, "%s 1 xbox xbox %13I64u %s %2d %5s %s\r\n", folder ? "drwxr-xr-x" : "-rw-r--r--",
                     size, kMonths[month], t.wDay, stamp, name);
}

// LIST or NLST of a path the client gave, over the data connection. Lines
// are gathered in the session's buffer and sent as it fills.
static void SendListing(FtpSession &session, const char *arg, bool namesOnly)
{
    // "LIST -la" and the like: the options are ls's, and the listing is the
    // same either way.
    while (arg != NULL && arg[0] == '-')
    {
        arg = strchr(arg, ' ');
        if (arg != NULL)
            while (*arg == ' ')
                arg++;
    }

    char path[FTP_PATH_SIZE], consolePath[FTP_PATH_SIZE];
    if (!ResolvePath(session.cwd, arg, path, sizeof(path)))
    {
        Reply(session, "550 That path is too long.");
        return;
    }
    const bool top = (strcmp(path, "/") == 0);
    if (!top && (!ToConsolePath(path, consolePath, sizeof(consolePath)) || !IsFolder(consolePath)))
    {
        Reply(session, "550 No such folder.");
        return;
    }
    if (!AcceptData(session, "the listing"))
        return;

    SYSTEMTIME now;
    GetSystemTime(&now);
    int used = 0;
    bool ok = true;
    char line[FTP_PATH_SIZE];

    if (top)
    {
        for (int i = 0; i < FTP_DRIVE_COUNT && ok; ++i)
        {
            if (!IsFolder(kFtpDrives[i].alias))
                continue; // a USB port with nothing in it
            int n = namesOnly ? _snprintf(line, sizeof(line), "%s\r\n", kFtpDrives[i].name)
                              : ListLine(line, sizeof(line), true, 0, NULL, kFtpDrives[i].name, now);
            if (n > 0)
                ok = SendAll(session.data, line, n);
        }
    }
    else
    {
        char pattern[FTP_PATH_SIZE + 4];
        _snprintf(pattern, sizeof(pattern), "%s\\*", consolePath);
        pattern[sizeof(pattern) - 1] = '\0';
        WIN32_FIND_DATAA found;
        HANDLE h = FindFirstFileA(pattern, &found);
        if (h != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (strcmp(found.cFileName, ".") == 0 || strcmp(found.cFileName, "..") == 0)
                    continue;
                const bool folder = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                const unsigned long long size =
                    folder ? 0 : (((unsigned long long)found.nFileSizeHigh << 32) | found.nFileSizeLow);
                int n = namesOnly ? _snprintf(line, sizeof(line), "%s\r\n", found.cFileName)
                                  : ListLine(line, sizeof(line), folder, size, &found.ftLastWriteTime,
                                             found.cFileName, now);
                if (n <= 0)
                    continue;
                if (used + n > FTP_BUFFER)
                {
                    ok = SendAll(session.data, session.buffer, used);
                    used = 0;
                }
                memcpy(session.buffer + used, line, n);
                used += n;
            } while (ok && !Stopping() && FindNextFileA(h, &found));
            FindClose(h);
        }
        if (ok && used > 0)
            ok = SendAll(session.data, session.buffer, used);
    }

    CloseData(session);
    Reply(session, ok ? "226 Listing sent." : "426 The listing was cut off.");
}

// ---------------------------------------------------------------------------
// Transfers
// ---------------------------------------------------------------------------

static void SendFile(FtpSession &session, const char *consolePath)
{
    HANDLE file = CreateFileA(consolePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        Reply(session, "550 Couldn't open that file.");
        return;
    }
    if (session.restAt > 0)
    {
        LONG high = (LONG)(session.restAt >> 32);
        SetFilePointer(file, (LONG)(session.restAt & 0xFFFFFFFF), &high, FILE_BEGIN);
    }
    if (!AcceptData(session, "the file"))
    {
        CloseHandle(file);
        return;
    }

    bool ok = true;
    for (;;)
    {
        DWORD got = 0;
        if (!ReadFile(file, session.buffer, FTP_BUFFER, &got, NULL))
        {
            ok = false;
            break;
        }
        if (got == 0)
            break;
        if (Stopping() || !SendAll(session.data, session.buffer, (int)got))
        {
            ok = false;
            break;
        }
    }
    CloseHandle(file);
    CloseData(session);
    Reply(session, ok ? "226 Sent." : "426 The transfer was cut off.");
}

// STOR, or APPE with append: the file from the client, onto the drive.
static void ReceiveFile(FtpSession &session, const char *consolePath, bool append)
{
    const bool resume = append || session.restAt > 0;
    HANDLE file = CreateFileA(consolePath, GENERIC_WRITE, 0, NULL, resume ? OPEN_ALWAYS : CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
    {
        Reply(session, "550 Couldn't write there.");
        return;
    }
    if (append)
        SetFilePointer(file, 0, NULL, FILE_END);
    else if (session.restAt > 0)
    {
        LONG high = (LONG)(session.restAt >> 32);
        SetFilePointer(file, (LONG)(session.restAt & 0xFFFFFFFF), &high, FILE_BEGIN);
        SetEndOfFile(file); // anything past where it resumes is redone
    }
    if (!AcceptData(session, "the upload"))
    {
        CloseHandle(file);
        return;
    }

    // recv hands over a few KB at a time on a LAN; written to the drive as
    // they come, that's thousands of small writes a second, and an upload
    // crawled at 0.2 MB/s. Gathered into the whole buffer first, each write
    // is 256KB.
    bool ok = true;
    bool full = false;
    bool ended = false;
    while (!ended)
    {
        int filled = 0;
        while (filled < FTP_BUFFER)
        {
            const int got = recv(session.data, session.buffer + filled, FTP_BUFFER - filled, 0);
            if (got == 0)
            {
                ended = true; // the client closed it: that's the end of the file
                break;
            }
            if (got < 0 || Stopping())
            {
                ok = false;
                break;
            }
            filled += got;
        }
        if (!ok)
            break;
        DWORD written = 0;
        if (filled > 0 && (!WriteFile(file, session.buffer, (DWORD)filled, &written, NULL) || written != (DWORD)filled))
        {
            ok = false;
            full = true;
            break;
        }
    }
    CloseHandle(file);
    CloseData(session);
    if (ok)
        Reply(session, "226 Received.");
    else if (full)
        Reply(session, "452 Couldn't write it all - the drive may be full.");
    else
        Reply(session, "426 The transfer was cut off.");
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

// The client's path as the console's, replying with the error and returning
// false when it doesn't name anything on a drive.
static bool ArgToConsole(FtpSession &session, const char *arg, char *out, size_t outSize)
{
    char path[FTP_PATH_SIZE];
    if (arg == NULL || arg[0] == '\0')
    {
        Reply(session, "501 Which file?");
        return false;
    }
    if (!ResolvePath(session.cwd, arg, path, sizeof(path)) || !ToConsolePath(path, out, outSize) ||
        strchr(out, '\\') == NULL)
    {
        // The top level and the drives themselves can't be written,
        // deleted or renamed.
        Reply(session, "550 Not there - only inside a drive.");
        return false;
    }
    return true;
}

static bool HandleCommand(FtpSession &session, char *line)
{
    // The command, upper-cased, and its argument.
    char *arg = line + strcspn(line, " ");
    if (*arg != '\0')
        *arg++ = '\0';
    for (char *p = line; *p != '\0'; ++p)
    {
        if (*p >= 'a' && *p <= 'z')
            *p = (char)(*p - 'a' + 'A');
    }
    const char *cmd = line;

    if (strcmp(cmd, "QUIT") == 0)
    {
        Reply(session, "221 Bye.");
        return false;
    }
    if (strcmp(cmd, "USER") == 0)
    {
        session.gotUser = true;
        session.userMatched = (strcmp(arg, g_ftpUser) == 0);
        session.loggedIn = false;
        return Reply(session, "331 Password, please.");
    }
    if (strcmp(cmd, "PASS") == 0)
    {
        if (!session.gotUser)
            return Reply(session, "503 USER first.");
        session.loggedIn = session.userMatched && strcmp(arg, g_ftpPassword) == 0;
        if (!session.loggedIn)
        {
            dprintf("[ftp] %s: wrong username or password\n", session.clientName);
            return Reply(session, "530 Wrong username or password.");
        }
        dprintf("[ftp] %s signed in\n", session.clientName);
        return Reply(session, "230 Signed in.");
    }
    if (strcmp(cmd, "SYST") == 0)
        return Reply(session, "215 UNIX Type: L8");
    if (strcmp(cmd, "FEAT") == 0)
    {
        Reply(session, "211-Features:");
        Reply(session, " PASV");
        Reply(session, " EPSV");
        Reply(session, " SIZE");
        Reply(session, " MDTM");
        Reply(session, " REST STREAM");
        Reply(session, " UTF8");
        return Reply(session, "211 End");
    }
    if (strcmp(cmd, "OPTS") == 0 || strcmp(cmd, "CLNT") == 0 || strcmp(cmd, "NOOP") == 0)
        return Reply(session, "200 OK.");
    if (!session.loggedIn)
        return Reply(session, "530 Sign in first.");

    if (strcmp(cmd, "PWD") == 0 || strcmp(cmd, "XPWD") == 0)
        return Reply(session, "257 \"%s\" is the current folder.", session.cwd);
    if (strcmp(cmd, "CWD") == 0 || strcmp(cmd, "XCWD") == 0 || strcmp(cmd, "CDUP") == 0 || strcmp(cmd, "XCUP") == 0)
    {
        const bool up = (cmd[0] == 'C' && cmd[1] == 'D') || strcmp(cmd, "XCUP") == 0;
        char path[FTP_PATH_SIZE], consolePath[FTP_PATH_SIZE];
        if (!ResolvePath(session.cwd, up ? ".." : arg, path, sizeof(path)))
            return Reply(session, "550 That path is too long.");
        if (strcmp(path, "/") != 0 && (!ToConsolePath(path, consolePath, sizeof(consolePath)) || !IsFolder(consolePath)))
            return Reply(session, "550 No such folder.");
        strcpy(session.cwd, path);
        return Reply(session, "250 Now in \"%s\".", session.cwd);
    }
    if (strcmp(cmd, "TYPE") == 0 || strcmp(cmd, "MODE") == 0 || strcmp(cmd, "STRU") == 0)
        return Reply(session, "200 OK."); // everything goes as it is, binary
    if (strcmp(cmd, "ALLO") == 0)
        return Reply(session, "202 No need.");
    if (strcmp(cmd, "PASV") == 0 || strcmp(cmd, "EPSV") == 0)
    {
        IN_ADDR address;
        if (!ConsoleAddress(&address))
            return Reply(session, "425 The console has no network address.");
        EnterCriticalSection(&g_ftpLock);
        CloseSocket(session.passive);
        int port = 0;
        session.passive = OpenPassive(&port);
        const bool opened = (session.passive != INVALID_SOCKET);
        LeaveCriticalSection(&g_ftpLock);
        if (!opened)
            return Reply(session, "425 Couldn't open a port for the data connection.");
        if (cmd[0] == 'E')
            return Reply(session, "229 Entering Extended Passive Mode (|||%d|)", port);
        const unsigned char *ip = (const unsigned char *)&address.s_addr;
        return Reply(session, "227 Entering Passive Mode (%u,%u,%u,%u,%d,%d).", ip[0], ip[1], ip[2], ip[3],
                     port >> 8, port & 0xFF);
    }
    if (strcmp(cmd, "PORT") == 0 || strcmp(cmd, "EPRT") == 0)
        return Reply(session, "502 Use passive mode.");
    if (strcmp(cmd, "REST") == 0)
    {
        session.restAt = _strtoui64(arg, NULL, 10);
        return Reply(session, "350 Starting at %I64u.", session.restAt);
    }
    if (strcmp(cmd, "LIST") == 0 || strcmp(cmd, "NLST") == 0)
    {
        SendListing(session, arg, cmd[0] == 'N');
        return true;
    }

    char consolePath[FTP_PATH_SIZE];
    char logged[FTP_PATH_SIZE * 2];
    if (strcmp(cmd, "SIZE") == 0 || strcmp(cmd, "MDTM") == 0)
    {
        if (!ArgToConsole(session, arg, consolePath, sizeof(consolePath)))
            return true;
        unsigned long long size = 0;
        FILETIME written;
        if (!FileInfo(consolePath, &size, &written))
            return Reply(session, "550 No such file.");
        if (cmd[0] == 'S')
            return Reply(session, "213 %I64u", size);
        SYSTEMTIME t;
        FileTimeToSystemTime(&written, &t);
        return Reply(session, "213 %04d%02d%02d%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                     t.wSecond);
    }
    if (strcmp(cmd, "RETR") == 0)
    {
        if (ArgToConsole(session, arg, consolePath, sizeof(consolePath)))
        {
            dprintf("[ftp] %s: sending %s\n", session.clientName, ForLog(consolePath, logged, sizeof(logged)));
            SendFile(session, consolePath);
        }
        session.restAt = 0;
        return true;
    }
    if (strcmp(cmd, "STOR") == 0 || strcmp(cmd, "APPE") == 0)
    {
        if (ArgToConsole(session, arg, consolePath, sizeof(consolePath)))
        {
            dprintf("[ftp] %s: receiving %s\n", session.clientName, ForLog(consolePath, logged, sizeof(logged)));
            ReceiveFile(session, consolePath, cmd[0] == 'A');
        }
        session.restAt = 0;
        return true;
    }
    if (strcmp(cmd, "DELE") == 0)
    {
        if (!ArgToConsole(session, arg, consolePath, sizeof(consolePath)))
            return true;
        if (!DeleteFileA(consolePath))
            return Reply(session, "550 Couldn't delete that.");
        dprintf("[ftp] %s: deleted %s\n", session.clientName, ForLog(consolePath, logged, sizeof(logged)));
        return Reply(session, "250 Deleted.");
    }
    if (strcmp(cmd, "MKD") == 0 || strcmp(cmd, "XMKD") == 0)
    {
        if (!ArgToConsole(session, arg, consolePath, sizeof(consolePath)))
            return true;
        if (!CreateDirectoryA(consolePath, NULL))
            return Reply(session, "550 Couldn't make that folder.");
        char path[FTP_PATH_SIZE];
        ResolvePath(session.cwd, arg, path, sizeof(path));
        return Reply(session, "257 \"%s\" made.", path);
    }
    if (strcmp(cmd, "RMD") == 0 || strcmp(cmd, "XRMD") == 0)
    {
        if (!ArgToConsole(session, arg, consolePath, sizeof(consolePath)))
            return true;
        if (!RemoveDirectoryA(consolePath))
            return Reply(session, "550 Couldn't remove that folder - is it empty?");
        dprintf("[ftp] %s: removed %s\n", session.clientName, ForLog(consolePath, logged, sizeof(logged)));
        return Reply(session, "250 Removed.");
    }
    if (strcmp(cmd, "RNFR") == 0)
    {
        session.renameFrom[0] = '\0';
        if (!ArgToConsole(session, arg, consolePath, sizeof(consolePath)))
            return true;
        if (Attributes(consolePath) == (DWORD)-1)
            return Reply(session, "550 No such file or folder.");
        strcpy(session.renameFrom, consolePath);
        return Reply(session, "350 And the new name?");
    }
    if (strcmp(cmd, "RNTO") == 0)
    {
        if (session.renameFrom[0] == '\0')
            return Reply(session, "503 RNFR first.");
        const bool ok = ArgToConsole(session, arg, consolePath, sizeof(consolePath)) &&
                        MoveFileA(session.renameFrom, consolePath);
        session.renameFrom[0] = '\0';
        return ok ? Reply(session, "250 Renamed.") : Reply(session, "553 Couldn't rename that.");
    }
    if (strcmp(cmd, "ABOR") == 0)
        return Reply(session, "226 Nothing to stop.");

    return Reply(session, "502 Not something this server does.");
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------

static DWORD WINAPI SessionThread(LPVOID param)
{
    FtpSession &session = *(FtpSession *)param;
    session.buffer = (char *)malloc(FTP_BUFFER);
    if (session.buffer != NULL)
    {
        Reply(session, "220 Omni360 FTP - the console's drives.");
        char line[FTP_LINE_SIZE];
        while (ReadLine(session, line, sizeof(line)) && HandleCommand(session, line))
        {
        }
        free(session.buffer);
        session.buffer = NULL;
    }
    else
        Reply(session, "421 Out of memory.");

    dprintf("[ftp] %s disconnected\n", session.clientName);
    EnterCriticalSection(&g_ftpLock);
    CloseSocket(session.passive);
    CloseSocket(session.data);
    CloseSocket(session.control);
    CloseHandle(session.thread);
    session.thread = NULL;
    session.used = false;
    LeaveCriticalSection(&g_ftpLock);
    return 0;
}

static DWORD WINAPI ListenThread(LPVOID)
{
    while (!Stopping())
    {
        fd_set readable;
        FD_ZERO(&readable);
        EnterCriticalSection(&g_ftpLock);
        const SOCKET listening = g_ftpListen;
        LeaveCriticalSection(&g_ftpLock);
        if (listening == INVALID_SOCKET)
            break;
        FD_SET(listening, &readable);
        timeval wait = { 1, 0 };
        if (select(0, &readable, NULL, NULL, &wait) <= 0)
            continue;

        sockaddr_in from;
        int fromLength = sizeof(from);
        SOCKET client = accept(listening, (sockaddr *)&from, &fromLength);
        if (client == INVALID_SOCKET)
            continue;
        BypassSecurity(client);

        EnterCriticalSection(&g_ftpLock);
        FtpSession *session = NULL;
        for (int i = 0; i < FTP_MAX_CLIENTS && session == NULL; ++i)
        {
            if (!g_ftpSessions[i].used)
                session = &g_ftpSessions[i];
        }
        if (session != NULL)
        {
            memset(session, 0, sizeof(*session));
            session->used = true;
            session->control = client;
            session->passive = INVALID_SOCKET;
            session->data = INVALID_SOCKET;
            strcpy(session->cwd, "/");
            const unsigned char *ip = (const unsigned char *)&from.sin_addr.s_addr;
            _snprintf(session->clientName, sizeof(session->clientName), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
            session->thread = CreateThread(NULL, 64 * 1024, SessionThread, session, CREATE_SUSPENDED, NULL);
            if (session->thread == NULL)
            {
                session->used = false;
                session = NULL;
            }
        }
        LeaveCriticalSection(&g_ftpLock);

        if (session == NULL)
        {
            const char *busy = "421 Too many connections - try again in a moment.\r\n";
            send(client, busy, (int)strlen(busy), 0);
            closesocket(client);
            continue;
        }
        dprintf("[ftp] %s connected\n", session->clientName);
        ResumeThread(session->thread);
    }
    return 0;
}

bool StartFtpServer(const char *user, const char *password)
{
    EnsureFtpLock();
    if (FtpServerRunning())
        return true;

    _snprintf(g_ftpUser, sizeof(g_ftpUser), "%s", user != NULL ? user : "");
    _snprintf(g_ftpPassword, sizeof(g_ftpPassword), "%s", password != NULL ? password : "");
    g_ftpUser[sizeof(g_ftpUser) - 1] = g_ftpPassword[sizeof(g_ftpPassword) - 1] = '\0';

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
    {
        dprintf("[ftp] couldn't make a socket: WSA error %d\n", WSAGetLastError());
        return false;
    }
    BypassSecurity(s);
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (PCSTR)&yes, sizeof(yes));
    sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(FTP_PORT);
    sa.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (sockaddr *)&sa, sizeof(sa)) != 0 || listen(s, 4) != 0)
    {
        dprintf("[ftp] couldn't listen on port %d: WSA error %d\n", FTP_PORT, WSAGetLastError());
        closesocket(s);
        return false;
    }

    InterlockedExchange(&g_ftpStop, 0);
    EnterCriticalSection(&g_ftpLock);
    g_ftpListen = s;
    LeaveCriticalSection(&g_ftpLock);
    g_ftpThread = CreateThread(NULL, 32 * 1024, ListenThread, NULL, 0, NULL);
    if (g_ftpThread == NULL)
    {
        EnterCriticalSection(&g_ftpLock);
        CloseSocket(g_ftpListen);
        LeaveCriticalSection(&g_ftpLock);
        return false;
    }
    dprintf("[ftp] listening on port %d\n", FTP_PORT);
    return true;
}

void StopFtpServer(unsigned long timeoutMs)
{
    EnsureFtpLock();
    if (g_ftpThread == NULL)
        return;
    InterlockedExchange(&g_ftpStop, 1);

    // Every socket closed under the threads' feet ends their waits.
    HANDLE threads[FTP_MAX_CLIENTS + 1];
    int count = 0;
    EnterCriticalSection(&g_ftpLock);
    CloseSocket(g_ftpListen);
    for (int i = 0; i < FTP_MAX_CLIENTS; ++i)
    {
        FtpSession &session = g_ftpSessions[i];
        if (!session.used)
            continue;
        CloseSocket(session.data);
        CloseSocket(session.passive);
        CloseSocket(session.control);
    }
    LeaveCriticalSection(&g_ftpLock);

    threads[count++] = g_ftpThread;
    WaitForMultipleObjects(count, threads, TRUE, timeoutMs);
    CloseHandle(g_ftpThread);
    g_ftpThread = NULL;

    // The sessions tidy themselves away; give them the same time.
    const DWORD until = GetTickCount() + timeoutMs;
    while (FtpClientCount() > 0 && GetTickCount() < until)
        Sleep(50);
    dprintf("[ftp] stopped\n");
}

bool FtpServerRunning()
{
    return g_ftpThread != NULL;
}

int FtpClientCount()
{
    EnsureFtpLock();
    int count = 0;
    EnterCriticalSection(&g_ftpLock);
    for (int i = 0; i < FTP_MAX_CLIENTS; ++i)
    {
        if (g_ftpSessions[i].used)
            count++;
    }
    LeaveCriticalSection(&g_ftpLock);
    return count;
}
