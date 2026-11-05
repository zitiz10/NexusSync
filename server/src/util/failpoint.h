/*
 * failpoint.h — named crash points for recovery tests (SPEC §15).
 *
 * Code that writes durable state calls nx_fail_hit("name") at interesting
 * moments: between the steps of a commit, a checkpoint, an append. A test
 * arms one name with a countdown; when that point is reached for the
 * (countdown+1)-th time the process is considered "crashed":
 *   - nx_fail_hit returns true there, and the caller stops and returns -1;
 *   - from then on nx_fail_crashed() is true and every crash point returns
 *     true, so no further durable write happens.
 * The test then abandons the engine (nx_engine_abandon: no checkpoint, no
 * writes) and reopens it, which runs recovery. This models kill -9: data
 * already handed to the OS stays, nothing after the crash point is written.
 *
 * "Torn" points (names ending in ".torn") make the caller write only part
 * of its buffer before stopping, which models a crash in the middle of a
 * write: the case CRCs and the doublewrite file exist for.
 *
 * Unarmed cost: one relaxed atomic load. Thread-safe.
 */
#ifndef NX_FAILPOINT_H
#define NX_FAILPOINT_H

#include <stdbool.h>
#include <stdint.h>

/* Arms `name`: the (skip+1)-th hit crashes. Clears an earlier crash. */
void nx_fail_arm(const char *name, uint32_t skip);
/* Disarms and clears the crashed state. */
void nx_fail_reset(void);
/* True when this point should crash (or a crash already happened). */
bool nx_fail_hit(const char *name);
bool nx_fail_crashed(void);
/* Name of the point that crashed, or NULL. */
const char *nx_fail_crashed_at(void);

#endif
