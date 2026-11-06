/* json_in.c — see json_in.h. */
#include "util/json_in.h"

#include <pthread.h>

static pthread_mutex_t g_parse_mu = PTHREAD_MUTEX_INITIALIZER;

cJSON *nx_json_parse(const void *buf, size_t len)
{
    if (!buf || len == 0)
        return NULL;
    pthread_mutex_lock(&g_parse_mu);
    cJSON *j = cJSON_ParseWithLength((const char *)buf, len);
    pthread_mutex_unlock(&g_parse_mu);
    return j;
}
