/* failpoint.c — see failpoint.h. */
#include "util/failpoint.h"

#include "util/log.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

static atomic_int g_state; /* 0 unarmed, 1 armed, 2 crashed */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_name[64];
static char g_crashed_at[64];
static uint32_t g_skip;

void nx_fail_arm(const char *name, uint32_t skip)
{
    pthread_mutex_lock(&g_mu);
    strncpy(g_name, name, sizeof g_name - 1);
    g_name[sizeof g_name - 1] = '\0';
    g_crashed_at[0] = '\0';
    g_skip = skip;
    atomic_store(&g_state, 1);
    pthread_mutex_unlock(&g_mu);
}

void nx_fail_reset(void)
{
    pthread_mutex_lock(&g_mu);
    g_name[0] = '\0';
    g_crashed_at[0] = '\0';
    atomic_store(&g_state, 0);
    pthread_mutex_unlock(&g_mu);
}

bool nx_fail_hit(const char *name)
{
    int st = atomic_load_explicit(&g_state, memory_order_relaxed);
    if (st == 0)
        return false;
    if (st == 2)
        return true;
    bool crash = false;
    pthread_mutex_lock(&g_mu);
    if (atomic_load(&g_state) == 2) {
        crash = true;
    } else if (strcmp(g_name, name) == 0) {
        if (g_skip == 0) {
            crash = true;
            strncpy(g_crashed_at, name, sizeof g_crashed_at - 1);
            g_crashed_at[sizeof g_crashed_at - 1] = '\0';
            atomic_store(&g_state, 2);
            LOG_WARN("failpoint", "simulated crash point=%s", name);
        } else {
            g_skip--;
        }
    }
    pthread_mutex_unlock(&g_mu);
    return crash;
}

bool nx_fail_crashed(void) { return atomic_load(&g_state) == 2; }

const char *nx_fail_crashed_at(void)
{
    /* Written once, before the state becomes 2; read only after that. */
    return nx_fail_crashed() ? g_crashed_at : NULL;
}
