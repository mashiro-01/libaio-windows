/* io_queue_init.c -- deprecated alias, as in upstream libaio.
 *
 * Derived from libaio 0.3.113 (https://pagure.io/libaio.git); LGPL-2.1-or-later.
 */
#include "libaio.h"

int io_queue_init(int maxevents, io_context_t *ctxp)
{
	return io_setup(maxevents, ctxp);
}
