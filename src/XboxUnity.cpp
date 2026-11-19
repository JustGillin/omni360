/*
FILE : XboxUnity.cpp
PROJECT : Omni360
DESCRIPTION : Title updates from XboxUnity - see XboxUnity.h.
*/

#include "XboxUnity.h"
#include "HttpPlain.h"
#include "OutputConsole.h"
#include "cJSON.h"
#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNITY_INFO_URL "http://xboxunity.net/Resources/Lib/TitleUpdateInfo.php?titleid=%08lX"
#define UNITY_TU_URL   "http://xboxunity.net/api/tu/%08lX/%08lX"
#define UNITY_MD5_URL  "http://xboxunity.net/api/tumd5/%s"
#define UNITY_REPLY_MAX (256 * 1024)

// A reply into a buffer of its own, NUL-terminated; the caller frees it.
static char *Fetch(const char *url, int *status)
{
    char *buffer = (char *)malloc(UNITY_REPLY_MAX + 1);
    if (buffer == NULL)
    {
        *status = 0;
        return NULL;
    }
    unsigned long long length = 0;
    *status = HttpGetPlain(url, NULL, buffer, UNITY_REPLY_MAX, &length);
    if (*status != 200)
    {
        dprintf("[unity] %s -> HTTP %d\n", url, *status);
        free(buffer);
        return NULL;
    }
    buffer[length] = '\0';
    return buffer;
}

static const char *StringOf(const cJSON *object, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return (cJSON_IsString(item) && item->valuestring != NULL) ? item->valuestring : NULL;
}

// A number that may come as a string ("3") or a number (3).
static long long NumberOf(const cJSON *object, const char *key, int base)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (cJSON_IsNumber(item))
        return (long long)item->valuedouble;
    if (cJSON_IsString(item) && item->valuestring != NULL)
        return _strtoi64(item->valuestring, NULL, base);
    return 0;
}

static int CompareUnity(const void *a, const void *b)
{
    const UnityTitleUpdate &x = *(const UnityTitleUpdate *)a;
    const UnityTitleUpdate &y = *(const UnityTitleUpdate *)b;
    if (x.forThisCopy != y.forThisCopy)
        return x.forThisCopy ? -1 : 1;
    if (x.mediaId != y.mediaId)
        return x.mediaId < y.mediaId ? -1 : 1;
    return y.version - x.version;
}

UnityResult FindUnityTitleUpdates(unsigned long titleId, unsigned long mediaId, UnityTitleUpdate *out, int max,
                                  int *count)
{
    *count = 0;
    char url[160];
    _snprintf(url, sizeof(url), UNITY_INFO_URL, titleId);
    url[sizeof(url) - 1] = '\0';
    int status = 0;
    char *reply = Fetch(url, &status);
    if (reply == NULL)
        return UNITY_UNREACHABLE;

    cJSON *root = cJSON_Parse(reply);
    free(reply);
    if (root == NULL)
    {
        dprintf("[unity] title %08lX: the reply wasn't JSON\n", titleId);
        return UNITY_UNREACHABLE;
    }

    const cJSON *medias = cJSON_GetObjectItemCaseSensitive(root, "MediaIDS");
    const cJSON *media = NULL;
    cJSON_ArrayForEach(media, medias)
    {
        const char *mediaText = StringOf(media, "MediaID");
        const unsigned long thisMedia = mediaText != NULL ? strtoul(mediaText, NULL, 16) : 0;
        const cJSON *updates = cJSON_GetObjectItemCaseSensitive(media, "Updates");
        const cJSON *u = NULL;
        cJSON_ArrayForEach(u, updates)
        {
            if (*count >= max)
                break;
            const char *id = StringOf(u, "TitleUpdateID");
            if (id == NULL)
                continue;
            UnityTitleUpdate &t = out[(*count)++];
            memset(&t, 0, sizeof(t));
            _snprintf(t.id, sizeof(t.id), "%s", id);
            t.mediaId = thisMedia;
            t.baseVersion = (unsigned long)NumberOf(u, "BaseVersion", 16);
            t.version = (int)NumberOf(u, "Version", 10);
            const char *hash = StringOf(u, "hash");
            _snprintf(t.contentId, sizeof(t.contentId), "%s", hash != NULL ? hash : "");
            t.size = (unsigned long long)NumberOf(u, "Size", 10) * 1024ULL;
            t.forThisCopy = (mediaId != 0 && thisMedia == mediaId);
        }
    }
    cJSON_Delete(root);

    qsort(out, *count, sizeof(UnityTitleUpdate), CompareUnity);
    dprintf("[unity] title %08lX: %d updates\n", titleId, *count);
    return *count > 0 ? UNITY_OK : UNITY_NONE;
}

bool UnityTitleUpdateFile(unsigned long titleId, const UnityTitleUpdate &update, char *fileName,
                          size_t fileNameSize, char *url, size_t urlSize)
{
    char listUrl[160];
    _snprintf(listUrl, sizeof(listUrl), UNITY_TU_URL, titleId, update.baseVersion);
    listUrl[sizeof(listUrl) - 1] = '\0';
    int status = 0;
    char *reply = Fetch(listUrl, &status);
    if (reply == NULL)
        return false;
    cJSON *root = cJSON_Parse(reply);
    free(reply);
    if (root == NULL)
        return false;

    bool found = false;
    const cJSON *u = NULL;
    cJSON_ArrayForEach(u, root)
    {
        const char *id = StringOf(u, "TitleUpdateID");
        if (id == NULL || strcmp(id, update.id) != 0)
            continue;
        const char *name = StringOf(u, "filename");
        const char *link = StringOf(u, "url");
        if (name == NULL || link == NULL)
            break;
        _snprintf(fileName, fileNameSize, "%s", name);
        _snprintf(url, urlSize, "%s", link);
        fileName[fileNameSize - 1] = url[urlSize - 1] = '\0';
        found = true;
        break;
    }
    cJSON_Delete(root);
    if (!found)
        dprintf("[unity] update %s isn't in %s\n", update.id, listUrl);
    return found;
}

bool UnityTitleUpdateMd5(const UnityTitleUpdate &update, char *md5, size_t md5Size, unsigned long long *size)
{
    char url[160];
    _snprintf(url, sizeof(url), UNITY_MD5_URL, update.id);
    url[sizeof(url) - 1] = '\0';
    int status = 0;
    char *reply = Fetch(url, &status);
    if (reply == NULL)
        return false;
    cJSON *root = cJSON_Parse(reply);
    free(reply);
    if (root == NULL)
        return false;
    const char *hex = StringOf(root, "md5");
    const long long bytes = NumberOf(root, "size", 10);
    const bool ok = hex != NULL && strlen(hex) == 32 && bytes > 0;
    if (ok)
    {
        _snprintf(md5, md5Size, "%s", hex);
        md5[md5Size - 1] = '\0';
        *size = (unsigned long long)bytes;
    }
    cJSON_Delete(root);
    return ok;
}
