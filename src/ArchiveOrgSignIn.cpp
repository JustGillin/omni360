#include "ArchiveOrgSignIn.h"
#include "downloadFile.h"
#include "parsing.h" // UrlEncodeFormValue
#include "cJSON.h"
#include "OutputConsole.h"

#include <xtl.h>
#include <stdlib.h>
#include <string.h>

#define SIGN_IN_URL "https://archive.org/services/xauthn/?op=login"

// The reply is small - the keys, two cookies, a screen name.
#define SIGN_IN_REPLY_MAX (16 * 1024)

// Quiet: httpRequestHTTPS narrates its steps through this, and none of them
// is worth having in the log next to a sign-in.
static void Quiet(const char *, ...) {}

static std::string StringField(const cJSON *object, const char *name)
{
    const cJSON *item = cJSON_GetObjectItem(object, name);
    return (item != NULL && cJSON_IsString(item) && item->valuestring != NULL) ? item->valuestring : "";
}

ArchiveOrgSignInResult ArchiveOrgSignIn(const char *email, const char *password, std::string &accessKey,
                                        std::string &secretKey, std::string &screenName, std::string &reason)
{
    accessKey.clear();
    secretKey.clear();
    screenName.clear();
    reason.clear();

    // email=...&password=..., each percent-encoded. Wiped once sent.
    static char encodedEmail[512], encodedPassword[512], body[1100];
    if (!UrlEncodeFormValue(email, encodedEmail, sizeof(encodedEmail)) ||
        !UrlEncodeFormValue(password, encodedPassword, sizeof(encodedPassword)))
    {
        reason = "too long";
        return SIGN_IN_REFUSED;
    }
    _snprintf(body, sizeof(body), "email=%s&password=%s", encodedEmail, encodedPassword);
    body[sizeof(body) - 1] = '\0';
    SecureZeroMemory(encodedPassword, sizeof(encodedPassword));

    char *reply = (char *)malloc(SIGN_IN_REPLY_MAX + 1);
    if (reply == NULL)
    {
        SecureZeroMemory(body, sizeof(body));
        return SIGN_IN_NO_ANSWER;
    }
    unsigned long long replyLen = SIGN_IN_REPLY_MAX;
    // httpRequestHTTPS gives every POST its form Content-Type itself. A second
    // one here was a 400 from archive.org's front end - "Bad Request", before
    // the sign-in service saw anything (2026-11-24).
    const int status = httpRequestHTTPS(SIGN_IN_URL, HTTP_POST, body, "", "", reply, &replyLen, false, NULL, 0,
                                        Quiet);
    SecureZeroMemory(body, sizeof(body));

    // 200 for a sign-in, 401 for a refusal; both are JSON.
    ArchiveOrgSignInResult result = SIGN_IN_NO_ANSWER;
    cJSON *root = NULL;
    if ((status == 200 || status == 401) && replyLen > 0 && replyLen <= SIGN_IN_REPLY_MAX)
    {
        reply[replyLen] = '\0';
        root = cJSON_Parse(reply);
    }
    if (root != NULL)
    {
        const cJSON *success = cJSON_GetObjectItem(root, "success");
        const cJSON *values = cJSON_GetObjectItem(root, "values");
        if (cJSON_IsTrue(success) && values != NULL)
        {
            const cJSON *s3 = cJSON_GetObjectItem(values, "s3");
            accessKey = StringField(s3, "access");
            secretKey = StringField(s3, "secret");
            screenName = StringField(values, "screenname");
            result = (!accessKey.empty() && !secretKey.empty()) ? SIGN_IN_OK : SIGN_IN_NO_ANSWER;
        }
        else
        {
            reason = values != NULL ? StringField(values, "reason") : "";
            if (reason.empty())
                reason = StringField(root, "error");
            if (reason == "account_not_found")
                result = SIGN_IN_NO_ACCOUNT;
            else if (reason == "account_bad_password")
                result = SIGN_IN_BAD_PASSWORD;
            else
                result = SIGN_IN_REFUSED;
        }
        cJSON_Delete(root);
    }
    else if (status == 401)
    {
        result = SIGN_IN_WRONG_DETAILS;
    }

    // The reply held the keys and session cookies.
    SecureZeroMemory(reply, SIGN_IN_REPLY_MAX + 1);
    free(reply);

    // The outcome only - never the email, the password or the keys.
    if (result == SIGN_IN_OK)
        dprintf("[archive.org] signed in\n");
    else
        dprintf("[archive.org] sign-in failed: HTTP %d, %s\n", status, reason.empty() ? "no reason given" : reason.c_str());
    if (accessKey.empty() || secretKey.empty())
    {
        accessKey.clear();
        secretKey.clear();
    }
    return result;
}
