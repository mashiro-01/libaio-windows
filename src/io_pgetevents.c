/* io_pgetevents.c -- io_pgetevents() for the Windows libaio port.
 *
 * Upstream wraps io_pgetevents(2) (libaio 0.5 symbol).  The sigmask is
 * meaningless on Windows (no POSIX signals); everything else matches
 * io_getevents().
 *
 * Derived from libaio 0.3.113 (https://pagure.io/libaio.git); LGPL-2.1-or-later.
 */
#include "libaio.h"

int io_pgetevents(io_context_t ctx, long min_nr, long nr,
		  struct io_event *events, struct timespec *timeout,
		  void *sigmask)
{
	(void)sigmask;
	return io_getevents(ctx, min_nr, nr, events, timeout);
}
