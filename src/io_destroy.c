/* io_destroy.c -- io_destroy() for the Windows libaio port.
 *
 * Upstream wraps io_destroy(2), which refuses (-EBUSY) while requests
 * are still active.  Same here: outstanding requests block destruction;
 * completed-but-unreaped packets (there should be none, since unreaped
 * packets keep the outstanding count nonzero) are drained defensively.
 *
 * Derived from libaio 0.3.113 (https://pagure.io/libaio.git); LGPL-2.1-or-later.
 */
#include <windows.h>
#include <stdlib.h>
#include <errno.h>
#include "aio_internal.h"

int io_destroy(io_context_t ctx)
{
	if (ctx == NULL)
		return -EINVAL;
	struct aio_context *c = (struct aio_context *)ctx;

	EnterCriticalSection(&c->lock);
	if (c->outstanding != 0) {
		LeaveCriticalSection(&c->lock);
		return -EBUSY;
	}
	c->stop = 1;
	LeaveCriticalSection(&c->lock);

	if (c->worker != NULL) {
		ReleaseSemaphore(c->sem, 1, NULL);
		WaitForSingleObject(c->worker, INFINITE);
		CloseHandle(c->worker);
	}

	for (;;) {
		OVERLAPPED_ENTRY ent[16];
		ULONG n = 0;
		if (!GetQueuedCompletionStatusEx(c->iocp, ent, 16, &n, 0, FALSE) ||
		    n == 0)
			break;
		for (ULONG i = 0; i < n; i++) {
			struct aio_request *req =
				(struct aio_request *)ent[i].lpOverlapped;
			aio_inflight_remove(c, req);
			if (req->close_file)
				CloseHandle(req->file);
			free(req);
		}
	}

	CloseHandle(c->sem);
	CloseHandle(c->iocp);
	DeleteCriticalSection(&c->lock);
	free(c);
	return 0;
}
