/* io_getevents.c -- io_getevents() for the Windows libaio port.
 *
 * Upstream wraps io_getevents(2), which waits on the kernel AIO ring.
 * Here completions are drained with GetQueuedCompletionStatusEx(): block
 * until at least min_nr events or the timeout expires, then opportuni-
 * stically drain up to nr events.  Each entry's OVERLAPPED is the first
 * member of the request, so the request comes back with a plain cast.
 *
 * Derived from libaio 0.3.113 (https://pagure.io/libaio.git); LGPL-2.1-or-later.
 */
#include <windows.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include "aio_internal.h"

static void reap_one(struct aio_context *ctx, OVERLAPPED_ENTRY *ent,
		     struct io_event *out)
{
	struct aio_request *req = (struct aio_request *)ent->lpOverlapped;

	if (req->state != AIO_REQ_DONE) {
		/* kernel completion: OVERLAPPED.Internal holds the final
		 * I/O status in its low 32 bits (STATUS_PENDING would mean
		 * "not completed", which GQCSE never returns) */
		ULONG st = (ULONG)req->ov.Internal;
		unsigned long long bytes = ent->dwNumberOfBytesTransferred;
		long long res;
		if (st == AIO_STATUS_SUCCESS || st == AIO_STATUS_END_OF_FILE)
			res = (long long)bytes;	/* EOF: short/zero read */
		else if (st == AIO_STATUS_CANCELLED)
			res = -ECANCELED;
		else
			res = -aio_ntstatus_to_errno(st);
		req->event.res = (unsigned long long)res;
		req->event.res2 = 0;
	}
	*out = req->event;

	aio_inflight_remove(ctx, req);
	InterlockedDecrement(&ctx->outstanding);
	if (req->close_file)
		CloseHandle(req->file);
	free(req);
}

static unsigned long long timespec_to_ms(const struct timespec *ts)
{
	unsigned long long ns = (unsigned long long)ts->tv_sec * 1000000000ull +
				(unsigned long long)ts->tv_nsec;
	return (ns + 999999ull) / 1000000ull;	/* round up, Linux-style */
}

int io_getevents(io_context_t ctx, long min_nr, long nr,
		 struct io_event *events, struct timespec *timeout)
{
	if (ctx == NULL || events == NULL)
		return -EINVAL;
	if (min_nr < 0 || nr <= 0 || min_nr > nr)
		return -EINVAL;
	if (timeout != NULL && (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
				(unsigned long long)timeout->tv_nsec >= 1000000000ull))
		return -EINVAL;

	struct aio_context *c = (struct aio_context *)ctx;

	int timed = 0;
	unsigned long long budget_ms = 0;
	LARGE_INTEGER freq, start;
	if (timeout != NULL) {
		budget_ms = timespec_to_ms(timeout);
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&start);
		timed = 1;
	}

	OVERLAPPED_ENTRY * ent = NULL;
	OVERLAPPED_ENTRY stack_ent[64];
	if ((unsigned long)nr > 64) {
		ent = (OVERLAPPED_ENTRY *)malloc((size_t)nr * sizeof(*ent));
		if (ent == NULL)
			return -EAGAIN;
	}
	OVERLAPPED_ENTRY * ents = ent ? ent : stack_ent;

	long got = 0;
	for (;;) {
		DWORD ms;
		if (got < min_nr) {
			if (!timed) {
				ms = INFINITE;
			} else {
				LARGE_INTEGER now;
				QueryPerformanceCounter(&now);
				unsigned long long el = (unsigned long long)
					((now.QuadPart - start.QuadPart) /
					 (freq.QuadPart / 1000));
				ms = el >= budget_ms
					 ? 0
					 : (DWORD)((budget_ms - el) > 0x7FFFFFFFull
						       ? 0x7FFFFFFF
						       : budget_ms - el);
			}
		} else {
			ms = 0;	/* opportunistic drain of ready events */
		}

		ULONG n = 0;
		if (!GetQueuedCompletionStatusEx(c->iocp, ents,
						 (ULONG)(nr - got), &n, ms,
						 FALSE)) {
#ifdef AIO_DEBUG_TRACE
			fprintf(stderr, "[aio] getevents GQCSE fail ms=%lu got=%ld gle=%lu\n",
				ms, got, GetLastError());
#endif
			break;	/* WAIT_TIMEOUT (or port error): done */
		}
#ifdef AIO_DEBUG_TRACE
		fprintf(stderr, "[aio] getevents ms=%lu got=%ld n=%lu\n", ms, got, n);
#endif
		if (n == 0)
			break;
		for (ULONG i = 0; i < n; i++)
			reap_one(c, &ents[i], &events[got++]);
		if (got >= nr)
			break;
	}

	free(ent);
	return (int)got;
}
