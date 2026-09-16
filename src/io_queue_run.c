/* io_queue_run.c -- deprecated alias, as in upstream libaio: drain all
 * ready events and invoke their callbacks.
 *
 * Derived from libaio 0.3.113 (https://pagure.io/libaio.git); LGPL-2.1-or-later.
 */
#include <stdint.h>
#include <time.h>
#include "libaio.h"

int io_queue_run(io_context_t ctx)
{
	if (ctx == NULL)
		return -EINVAL;

	struct io_event ev[32];
	struct timespec ts;
	ts.tv_sec = 0;
	ts.tv_nsec = 0;

	long total = 0;
	int r;
	while ((r = io_getevents(ctx, 0, 32, ev, &ts)) > 0) {
		for (int i = 0; i < r; i++) {
			io_callback_t cb =
				(io_callback_t)(uintptr_t)ev[i].data;
			if (cb != NULL)
				cb(ctx, (struct iocb *)ev[i].obj,
				   (long)ev[i].res, (long)ev[i].res2);
		}
		total += r;
		if (r < 32)
			break;
	}
	return total;
}
