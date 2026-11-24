#ifndef ARCHIVE_ORG_SIGN_IN_H
#define ARCHIVE_ORG_SIGN_IN_H

// ---------------------------------------------------------------------------
// Signing in to archive.org with an email and password
// ---------------------------------------------------------------------------
//
// Downloads authenticate with IAS3 keys (ArchiveOrgDLC.h), which until now
// the user had to copy from archive.org/account/s3.php and type in with a
// controller. This gets the same keys from an email and password, the way
// archive.org's own `ia configure` does (internetarchive 5.13.0,
// config.get_auth_config): one URL-encoded POST to
//
//     https://archive.org/services/xauthn/?op=login   email=...&password=...
//
// answered with JSON - {"success": true, "values": {"s3": {"access",
// "secret"}, "screenname", ...}} - or, on a failure, a 401 with
// values.reason ("account_not_found", "account_bad_password"). No CSRF token,
// no cookies first, no captcha; checked against a real account on
// 2026-11-24, which got back exactly the keys saved on its console.
//
// That is not the route the early comment in ArchiveOrgDLC.h describes as a
// dead end (/account/login, and the web page's /services/account/login/).
//
// Nothing here logs the password or the keys, and nothing keeps the password:
// the caller saves the keys and forgets the rest.

#include <string>

enum ArchiveOrgSignInResult
{
    SIGN_IN_OK,
    SIGN_IN_NO_ACCOUNT,   // no account with that email
    SIGN_IN_BAD_PASSWORD,
    // A 401 whose reason didn't come through: httpRequestHTTPS reads no body
    // for a status other than 200/206, and this endpoint's 401 only ever
    // means one of the two above. The console sees this one.
    SIGN_IN_WRONG_DETAILS,
    SIGN_IN_REFUSED,      // another reason archive.org gave - in reason
    SIGN_IN_NO_ANSWER     // no connection, or a reply that wasn't the JSON expected
};

// Blocks for as long as the request takes (a few seconds). On SIGN_IN_OK,
// accessKey, secretKey and screenName are filled in. reason gets archive.org's
// own word for a refusal, for the log and a detail line.
ArchiveOrgSignInResult ArchiveOrgSignIn(const char *email, const char *password, std::string &accessKey,
                                        std::string &secretKey, std::string &screenName, std::string &reason);

#endif
