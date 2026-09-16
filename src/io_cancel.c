/* io_cancel.c -- io_cancel() for the Windows libaio port.
 *
 * Upstream wraps io_cancel(2): on success the request is torn down and
 * the completion event still surfaces through io_getevents() with
 * res == -ECANCELED.  Same contract here via CancelIoEx() for native /
 * reopened requests; queued worker requests are cancelled outright.
 *
 * Derived from libaio 0.3.113 (https://pagure.io/libaio.git); LGPL-2.1-or-later.
 */
#include <windows.h>
#include <stdint.h>
#include <errno.h>
#include "aio_internal.h"

int io_cancel(io_context_t ctx, struct iocb *iocb, struct io_event *evt)
{
	if (ctx == NULL || iocb == NULL)
		return -EINVAL;
	struct aio_context *c = (struct aio_context *)ctx;

	struct aio_request *req = NULL;
	int ret = -ENOENT;

	EnterCriticalSection(&c->lock);
	req = c->inflight;
	while (req != NULL && req->iocb != iocb)
		req = req->next;
	if (req != NULL) {
		if (req->kind == AIO_KIND_WORKER) {
			struct aio_request **pp = &c->work_head;
			struct aio_request *prev = NULL;
			int queued = 0;
			while (*pp != NULL) {
				if (*pp == req) {
					*pp = req->work_next;
					if (c->work_tail == req)
						c->work_tail = prev;
					queued = 1;
					break;
				}
				prev = *pp;
				pp = &(*pp)->work_next;
			}
			if (queued) {
				req->event.res = (unsigned long long)-ECANCELED;
				req->event.res2 = 0;
				req->state = AIO_REQ_DONE;
				ret = 0;
			} else {
				ret = -EINVAL;	/* already executing */
			}
		} else {
			if (CancelIoEx(req->file, &req->ov)) {
				ret = 0;	/* packet arrives with
						 * STATUS_CANCELLED */
			} else {
				DWORD e = GetLastError();
				ret = (e == ERROR_NOT_FOUND)
					  ? -ENOENT
					  : -aio_winerr_to_errno(e);
			}
		}
	}
	LeaveCriticalSection(&c->lock);

	if (ret == 0) {
		if (req->kind == AIO_KIND_WORKER)
			PostQueuedCompletionStatus(c->iocp, 0,
						   AIO_COMPLETION_KEY,
						   &req->ov);
		if (evt != NULL) {
			evt->data = req->event.data;
			evt->obj = iocb;
			evt->res = 0;
			evt->res2 = 0;
		}
	}
	return ret;
}
