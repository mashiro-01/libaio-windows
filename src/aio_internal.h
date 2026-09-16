/* aio_internal.h -- internal plumbing for the Windows libaio port.
 *
 * Backend architecture (see README.md):
 *
 *   io_setup()    -> CreateIoCompletionPort()            (one IOCP per ctx)
 *   io_submit()   -> ReadFile()/WriteFile() + OVERLAPPED, in three tiers:
 *                    1. native:  fd came from aio_open() (FILE_FLAG_OVERLAPPED
 *                       at CreateFile time) and is bound to this context's
 *                       port -- zero extra syscalls per op;
 *                    2. reopen:  foreign fd (e.g. CRT open()); each request
 *                       gets a ReOpenFile()'d FILE_FLAG_OVERLAPPED handle;
 *                    3. worker:  blocking emulation on a per-context worker
 *                       thread (fsync/fdsync and fds where tiers 1/2 fail);
 *   io_getevents()-> GetQueuedCompletionStatusEx()
 *   io_cancel()   -> CancelIoEx()
 *
 * Every accepted io_submit() produces exactly one completion packet in the
 * context's IOCP -- either the kernel's, or one we post ourselves with
 * PostQueuedCompletionStatus() (inline errors, noop, worker results).  The
 * packet's lpOverlapped is &req->ov, and aio_request.ov is the first
 * struct member, so the request is recovered with a plain cast.
 */
#ifndef AIO_INTERNAL_H
#define AIO_INTERNAL_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "libaio.h"

/*
 * OVERLAPPED.Internal carries the final I/O status in its low 32 bits.
 * Use our own ULONG type + status constants so nothing depends on
 * SDK-specific NTSTATUS declarations.
 */
#define AIO_STATUS_SUCCESS		0x00000000ul
#define AIO_STATUS_END_OF_FILE		0xC0000011ul
#define AIO_STATUS_CANCELLED		0xC0000120ul

#define AIO_COMPLETION_KEY	((ULONG_PTR)1)

#define IOCB_FLAG_RESFD		(1 << 0)

/* aio_request.state */
#define AIO_REQ_INFLIGHT	1	/* packet still expected */
#define AIO_REQ_DONE		2	/* event filled; packet posted by us */

/* aio_request.kind */
#define AIO_KIND_NATIVE		1	/* issued on the fd's own handle */
#define AIO_KIND_REOPEN		2	/* issued on a per-request reopened handle */
#define AIO_KIND_WORKER		3	/* executed by the context worker thread */

struct aio_request {
	OVERLAPPED		ov;	/* must stay the first member */
	struct iocb		*iocb;
	struct io_event		event;
	struct aio_context	*ctx;
	HANDLE			file;	/* handle the I/O was issued on */
	int			kind;
	int			state;
	int			close_file;	/* CloseHandle(file) at reap */
	/* worker payload */
	void			*emu_buf;
	unsigned long long	emu_bytes;
	long long		emu_offset;
	struct iovec		*emu_iov;	/* owned copy, vectored ops */
	int			emu_iovcnt;
	void			*emu_tmp;	/* coalescing buffer */
	struct aio_request	*next;		/* ctx->inflight (ctx->lock) */
	struct aio_request	*work_next;	/* ctx->work_* (ctx->lock) */
};

struct aio_context {
	HANDLE			iocp;
	CRITICAL_SECTION	lock;
	struct aio_request	*inflight;		/* ctx->lock */
	struct aio_request	*work_head, *work_tail;	/* ctx->lock */
	volatile LONG		outstanding;		/* submits minus reaps */
	int			maxevents;
	HANDLE			sem;			/* worker wakeup */
	HANDLE			worker;
	volatile LONG		stop;
};

/* per-fd record for fds created by aio_open() */
struct aio_fd {
	HANDLE	handle;
	int	overlapped;	/* always 1 for aio_open() */
	int	no_buffering;	/* O_DIRECT */
	int	sector_size;
	HANDLE	bound_port;	/* IOCP bound to, NULL=not yet,
				 * INVALID_HANDLE_VALUE=bind failed (use reopen) */
};

int  aio_fdtab_lookup(int fd, struct aio_fd *out);
void aio_fdtab_insert(int fd, HANDLE h, int overlapped, int no_buffering,
		      int sector_size);
void aio_fdtab_remove(int fd);
void aio_fdtab_set_bound(int fd, HANDLE port);

int  aio_winerr_to_errno(DWORD err);
int  aio_ntstatus_to_errno(ULONG st);

/* _get_osfhandle() without the MSVC abort-on-bad-fd behaviour */
intptr_t aio_safe_get_osfhandle(int fd);

/* fill req->event with res and post the completion packet ourselves */
void aio_complete_inline(struct aio_context *ctx, struct aio_request *req,
			 long long res);

/* queue req on the context worker thread (blocking emulation path) */
void aio_worker_enqueue(struct aio_context *ctx, struct aio_request *req);

/* unlink req from ctx->inflight (ctx->lock taken internally) */
void aio_inflight_remove(struct aio_context *ctx, struct aio_request *req);

#endif /* AIO_INTERNAL_H */
