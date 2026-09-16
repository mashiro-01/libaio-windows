/* io_queue_wait.c -- deprecated alias, as in upstream libaio.
 *
 * Derived from libaio 0.3.113 (https://pagure.io/libaio.git); LGPL-2.1-or-later.
 */
#include <time.h>
#include "libaio.h"

int io_queue_wait(io_context_t ctx, struct timespec *timeout)
{
	struct io_event ev;
	return io_getevents(ctx, 1, 1, &ev, timeout);
}
