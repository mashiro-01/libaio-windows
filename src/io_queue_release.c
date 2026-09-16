/* io_queue_release.c -- deprecated alias, as in upstream libaio.
 *
 * Derived from libaio 0.3.113 (https://pagure.io/libaio.git); LGPL-2.1-or-later.
 */
#include "libaio.h"

int io_queue_release(io_context_t ctx)
{
	return io_destroy(ctx);
}
