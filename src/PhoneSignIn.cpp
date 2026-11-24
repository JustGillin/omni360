#include "PhoneSignIn.h"
#include "ArchiveOrgSignIn.h"
#include "ArchiveOrgDLC.h" // BuildIas3AuthHeader, CheckArchiveOrgKeys
#include "Strings.h"
#include "OutputConsole.h"

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XBOX_SO_BYPASS_SECURITY 0x5801
#define REQUEST_MAX (16 * 1024) // a browser's headers and a small form
#define KEYS_URL "archive.org/account/s3.php"

static CRITICAL_SECTION g_lock;
static bool g_lockReady = false;
static SOCKET g_listen = INVALID_SOCKET;
static HANDLE g_thread = NULL;
static volatile LONG g_stop = 0;
static char g_code[16];

// What the phone signed in with, until the question takes it.
static bool g_done = false;
static std::string g_access, g_secret, g_screenName;

static void EnsureLock()
{
    if (!g_lockReady)
    {
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
    }
}

static void Wipe(std::string &s)
{
    if (!s.empty())
        SecureZeroMemory(&s[0], s.size());
    s.clear();
}

// ---------------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------------

static void AppendEscaped(std::string &out, const char *text)
{
    for (const char *p = text; p != NULL && *p != '\0'; ++p)
    {
        switch (*p)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += *p; break;
        }
    }
}

static void PageStart(std::string &page, const char *title)
{
    page = "<!doctype html><html><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>";
    AppendEscaped(page, title);
    page += "</title><style>"
            "body{margin:0;background:#111;color:#eee;font:17px/1.45 system-ui,-apple-system,'Segoe UI',sans-serif}"
            "main{max-width:420px;margin:0 auto;padding:28px 22px 40px}"
            ".brand{font-weight:700;letter-spacing:.02em;color:#9bd88f;margin-bottom:18px}"
            "h1{font-size:24px;margin:0 0 8px}p{color:#bbb;margin:0 0 18px}"
            "label{display:block;margin:0 0 14px;font-size:15px;color:#ccc}"
            "input{display:block;box-sizing:border-box;width:100%;margin-top:6px;padding:12px;border-radius:8px;"
            "border:1px solid #444;background:#1d1d1d;color:#fff;font-size:17px}"
            "button{width:100%;padding:13px;border:0;border-radius:24px;background:#107c10;color:#fff;"
            "font-size:17px;font-weight:600;margin-top:4px}button:disabled{opacity:.6}"
            ".error{background:#3a2412;border-left:4px solid #e8a33d;padding:10px 12px;border-radius:6px;color:#f3d6ad}"
            "details{margin-top:26px}summary{color:#9bd88f;cursor:pointer;margin-bottom:12px}"
            "a{color:#9bd88f}.small{font-size:14px;color:#888;margin-top:26px}"
            "</style></head><body><main><div class=\"brand\">Omni360</div><h1>";
    AppendEscaped(page, title);
    page += "</h1>";
}

static void PageEnd(std::string &page)
{
    page += "</main><script>for(const f of document.forms)f.onsubmit=()=>{const b=f.querySelector('button');"
            "b.disabled=true;b.textContent=";
    // The button's words while it waits, as a JS string: quotes and
    // backslashes escaped, and no "<" that could end the script.
    page += "\"";
    for (const char *p = Tr(STR_PHONE_SIGNING_IN); *p != '\0'; ++p)
    {
        if (*p == '"' || *p == '\\')
            page += '\\';
        if (*p == '<')
            page += "\\u003c";
        else
            page += *p;
    }
    page += "\"};</script></body></html>";
}

static void FormPage(std::string &page, const char *error, const char *email)
{
    PageStart(page, Tr(STR_SIGN_IN_TO_ARCHIVE_ORG));
    page += "<p>";
    AppendEscaped(page, Tr(STR_PHONE_PAGE_INTRO));
    page += "</p>";
    if (error != NULL && error[0] != '\0')
    {
        page += "<p class=\"error\">";
        AppendEscaped(page, error);
        page += "</p>";
    }
    page += "<form method=\"post\"><input type=\"hidden\" name=\"mode\" value=\"password\"><label>";
    AppendEscaped(page, Tr(STR_PHONE_EMAIL));
    page += "<input type=\"email\" name=\"email\" autocomplete=\"username\" autocapitalize=\"off\" required value=\"";
    AppendEscaped(page, email);
    page += "\"></label><label>";
    AppendEscaped(page, Tr(STR_PHONE_PASSWORD));
    page += "<input type=\"password\" name=\"password\" autocomplete=\"current-password\" required></label><button>";
    AppendEscaped(page, Tr(STR_SIGN_IN));
    page += "</button></form><details><summary>";
    AppendEscaped(page, Tr(STR_PHONE_USE_KEYS));
    page += "</summary><p>";
    char help[256];
    TrFormat(help, sizeof(help), STR_PHONE_KEYS_HELP, "\x01"); // the link goes where \x01 is
    help[sizeof(help) - 1] = '\0';
    const char *mark = strchr(help, '\x01');
    if (mark != NULL)
    {
        std::string before(help, mark - help);
        AppendEscaped(page, before.c_str());
        page += "<a href=\"https://" KEYS_URL "\">" KEYS_URL "</a>";
        AppendEscaped(page, mark + 1);
    }
    page += "</p><form method=\"post\"><input type=\"hidden\" name=\"mode\" value=\"keys\"><label>";
    AppendEscaped(page, Tr(STR_PHONE_ACCESS_KEY));
    page += "<input name=\"access\" autocomplete=\"off\" autocapitalize=\"off\" required></label><label>";
    AppendEscaped(page, Tr(STR_PHONE_SECRET_KEY));
    page += "<input name=\"secret\" autocomplete=\"off\" autocapitalize=\"off\" required></label><button>";
    AppendEscaped(page, Tr(STR_PHONE_USE_THESE_KEYS));
    page += "</button></form></details><p class=\"small\">";
    AppendEscaped(page, Tr(STR_SIGN_IN_PRIVACY));
    page += "</p>";
    PageEnd(page);
}

static void MessagePage(std::string &page, const char *title, const char *text)
{
    PageStart(page, title);
    page += "<p>";
    AppendEscaped(page, text);
    page += "</p>";
    PageEnd(page);
}

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

static int HexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// One field of an application/x-www-form-urlencoded body, decoded.
static std::string FormField(const char *body, const char *name)
{
    const size_t nameLen = strlen(name);
    for (const char *p = body; p != NULL && *p != '\0';)
    {
        const char *end = strchr(p, '&');
        const size_t len = end != NULL ? (size_t)(end - p) : strlen(p);
        if (len > nameLen && strncmp(p, name, nameLen) == 0 && p[nameLen] == '=')
        {
            std::string value;
            for (size_t i = nameLen + 1; i < len; ++i)
            {
                if (p[i] == '+')
                    value += ' ';
                else if (p[i] == '%' && i + 2 < len && HexValue(p[i + 1]) >= 0 && HexValue(p[i + 2]) >= 0)
                {
                    value += (char)(HexValue(p[i + 1]) * 16 + HexValue(p[i + 2]));
                    i += 2;
                }
                else
                    value += p[i];
            }
            return value;
        }
        p = end != NULL ? end + 1 : NULL;
    }
    return "";
}

static void Trim(std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n'))
        --b;
    s = s.substr(a, b - a);
}

static void SendAll(SOCKET s, const char *data, int len)
{
    while (len > 0)
    {
        const int sent = send(s, data, len, 0);
        if (sent <= 0)
            return;
        data += sent;
        len -= sent;
    }
}

static void Reply(SOCKET s, const char *status, const std::string &page)
{
    char head[256];
    _snprintf(head, sizeof(head),
              "HTTP/1.1 %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %u\r\n"
              "Cache-Control: no-store\r\nReferrer-Policy: no-referrer\r\nConnection: close\r\n\r\n",
              status, (unsigned)page.size());
    head[sizeof(head) - 1] = '\0';
    SendAll(s, head, (int)strlen(head));
    SendAll(s, page.data(), (int)page.size());
}

// The sign-in the phone asked for. True if it worked; error says why not.
static bool SignInFromPhone(const char *body, std::string &error, std::string &email)
{
    std::string access, secret, screenName;
    if (FormField(body, "mode") == "keys")
    {
        access = FormField(body, "access");
        secret = FormField(body, "secret");
        Trim(access);
        Trim(secret);
        char header[IAS3_AUTH_HEADER_MAX];
        if (access.empty() || secret.empty() || !BuildIas3AuthHeader(access, secret, header, sizeof(header)))
        {
            error = Tr(STR_THOSE_ARE_TOO_LONG_TO);
            Wipe(secret);
            return false;
        }
        // Only keys Internet Archive says are good. Settings saves keys it
        // couldn't check, so an offline console isn't refused - but here a
        // phone is typing for someone who may already be signed in, and a
        // pair that can't be checked must not replace keys that work.
        char reason[160] = "";
        const KeyCheckResult check = CheckArchiveOrgKeys(header, reason, sizeof(reason), dprintf);
        SecureZeroMemory(header, sizeof(header));
        if (check != KEYS_ACCEPTED)
        {
            error = check == KEYS_REJECTED ? Tr(STR_ARCHIVE_ORG_DIDNT_ACCEPT_THOSE) : Tr(STR_COULDNT_REACH_ARCHIVE_ORG);
            Wipe(secret);
            return false;
        }
        dprintf("[phone] keys accepted from the phone\n");
    }
    else
    {
        email = FormField(body, "email");
        Trim(email);
        std::string password = FormField(body, "password");
        std::string reason;
        const ArchiveOrgSignInResult result =
            ArchiveOrgSignIn(email.c_str(), password.c_str(), access, secret, screenName, reason);
        Wipe(password);
        if (result != SIGN_IN_OK)
        {
            char message[256];
            switch (result)
            {
            case SIGN_IN_NO_ACCOUNT:
                TrFormat(message, sizeof(message), STR_ARCHIVE_ORG_DOESNT_KNOW, email.c_str());
                break;
            case SIGN_IN_BAD_PASSWORD:
                TrFormat(message, sizeof(message), STR_THAT_PASSWORD_ISNT_RIGHT, email.c_str());
                break;
            case SIGN_IN_WRONG_DETAILS:
                TrFormat(message, sizeof(message), STR_EMAIL_OR_PASSWORD_WRONG, email.c_str());
                break;
            case SIGN_IN_REFUSED:
                TrFormat(message, sizeof(message), STR_IT_SAID, reason.c_str());
                break;
            default:
                _snprintf(message, sizeof(message), "%s", Tr(STR_COULDNT_REACH_ARCHIVE_ORG));
                break;
            }
            message[sizeof(message) - 1] = '\0';
            error = message;
            return false;
        }
    }

    EnterCriticalSection(&g_lock);
    Wipe(g_secret);
    g_access = access;
    g_secret = secret;
    g_screenName = screenName;
    g_done = true;
    LeaveCriticalSection(&g_lock);
    Wipe(secret);
    return true;
}

static void Serve(SOCKET client)
{
    // A few seconds to send it all: a phone, not a slow link.
    DWORD timeout = 5000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));

    static char request[REQUEST_MAX + 1];
    int have = 0;
    char *bodyStart = NULL;
    int contentLength = 0;
    for (;;)
    {
        const int got = recv(client, request + have, REQUEST_MAX - have, 0);
        if (got <= 0)
            break;
        have += got;
        request[have] = '\0';
        if (bodyStart == NULL)
        {
            char *end = strstr(request, "\r\n\r\n");
            if (end != NULL)
            {
                bodyStart = end + 4;
                const char *cl = strstr(request, "\nContent-Length:");
                if (cl == NULL)
                    cl = strstr(request, "\ncontent-length:");
                if (cl != NULL && cl < end)
                    contentLength = atoi(cl + 16);
            }
        }
        if (bodyStart != NULL && (request + have) - bodyStart >= contentLength)
            break;
        if (have >= REQUEST_MAX)
            break;
    }
    request[have] = '\0';

    // "GET /code HTTP/1.1" - the path, then the code alone.
    char method[8] = "", path[64] = "";
    sscanf(request, "%7s %63s", method, path);
    std::string page;
    const bool ours = path[0] == '/' && strcmp(path + 1, g_code) == 0;
    if (!ours)
    {
        MessagePage(page, Tr(STR_SIGN_IN_TO_ARCHIVE_ORG), Tr(STR_PHONE_PAGE_CLOSED));
        Reply(client, "404 Not Found", page);
    }
    else if (strcmp(method, "POST") == 0 && bodyStart != NULL)
    {
        std::string error, email;
        if (SignInFromPhone(bodyStart, error, email))
            MessagePage(page, Tr(STR_PHONE_DONE_TITLE), Tr(STR_PHONE_DONE_TEXT));
        else
            FormPage(page, error.c_str(), email.c_str());
        Reply(client, "200 OK", page);
    }
    else
    {
        FormPage(page, NULL, "");
        Reply(client, "200 OK", page);
    }

    // The form carried a password: gone from the buffer as soon as it's done.
    SecureZeroMemory(request, sizeof(request));
}

static DWORD WINAPI ListenThread(LPVOID)
{
    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0)
    {
        EnterCriticalSection(&g_lock);
        const SOCKET listening = g_listen;
        LeaveCriticalSection(&g_lock);
        if (listening == INVALID_SOCKET)
            break;
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listening, &readable);
        timeval wait = { 1, 0 };
        if (select(0, &readable, NULL, NULL, &wait) <= 0)
            continue;
        SOCKET client = accept(listening, NULL, NULL);
        if (client == INVALID_SOCKET)
            continue;
        BOOL yes = TRUE;
        setsockopt(client, SOL_SOCKET, XBOX_SO_BYPASS_SECURITY, (PCSTR)&yes, sizeof(yes));
        Serve(client);
        shutdown(client, SD_SEND);
        closesocket(client);
    }
    return 0;
}

bool StartPhoneSignIn(char *url, unsigned int urlSize)
{
    EnsureLock();
    StopPhoneSignIn();

    XNADDR xna;
    memset(&xna, 0, sizeof(xna));
    const DWORD flags = XNetGetTitleXnAddr(&xna);
    if (flags == XNET_GET_XNADDR_PENDING || (flags & XNET_GET_XNADDR_NONE) || xna.ina.s_addr == 0)
        return false;

    // A new code each time: 8 characters, no look-alikes, for an address that
    // can be typed as well as scanned.
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    BYTE random[8];
    XNetRandom(random, sizeof(random));
    for (int i = 0; i < 8; ++i)
        g_code[i] = alphabet[random[i] % (sizeof(alphabet) - 1)];
    g_code[8] = '\0';

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return false;
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, XBOX_SO_BYPASS_SECURITY, (PCSTR)&yes, sizeof(yes));
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (PCSTR)&yes, sizeof(yes));
    sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(PHONE_SIGN_IN_PORT);
    sa.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (sockaddr *)&sa, sizeof(sa)) != 0 || listen(s, 4) != 0)
    {
        dprintf("[phone] couldn't listen on port %d: WSA error %d\n", PHONE_SIGN_IN_PORT, WSAGetLastError());
        closesocket(s);
        return false;
    }

    EnterCriticalSection(&g_lock);
    g_listen = s;
    g_done = false;
    LeaveCriticalSection(&g_lock);
    InterlockedExchange(&g_stop, 0);
    // The page is built in strings and a sign-in runs TLS on this thread.
    g_thread = CreateThread(NULL, 128 * 1024, ListenThread, NULL, 0, NULL);
    if (g_thread == NULL)
    {
        EnterCriticalSection(&g_lock);
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        LeaveCriticalSection(&g_lock);
        return false;
    }

    const unsigned char *ip = (const unsigned char *)&xna.ina.s_addr;
    _snprintf(url, urlSize, "http://%u.%u.%u.%u:%d/%s", ip[0], ip[1], ip[2], ip[3], PHONE_SIGN_IN_PORT, g_code);
    url[urlSize - 1] = '\0';
    dprintf("[phone] sign-in page up on port %d\n", PHONE_SIGN_IN_PORT); // not the code
    return true;
}

bool PhoneSignInDone(std::string &accessKey, std::string &secretKey, std::string &screenName)
{
    EnsureLock();
    EnterCriticalSection(&g_lock);
    const bool done = g_done;
    if (done)
    {
        accessKey = g_access;
        secretKey = g_secret;
        screenName = g_screenName;
        Wipe(g_access);
        Wipe(g_secret);
        g_screenName.clear();
        g_done = false;
    }
    LeaveCriticalSection(&g_lock);
    return done;
}

void StopPhoneSignIn()
{
    EnsureLock();
    if (g_thread == NULL)
        return;
    InterlockedExchange(&g_stop, 1);
    EnterCriticalSection(&g_lock);
    if (g_listen != INVALID_SOCKET)
    {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
    }
    LeaveCriticalSection(&g_lock);
    // A sign-in in flight can take a few seconds; past that, it's left to end
    // by itself - it only ever writes g_done, which the next start clears.
    WaitForSingleObject(g_thread, 8000);
    CloseHandle(g_thread);
    g_thread = NULL;
    EnterCriticalSection(&g_lock);
    Wipe(g_access);
    Wipe(g_secret);
    g_screenName.clear();
    g_done = false;
    LeaveCriticalSection(&g_lock);
    memset(g_code, 0, sizeof(g_code));
    dprintf("[phone] sign-in page closed\n");
}
