/* io_submit.c -- io_submit() for the Windows libaio port.
 *
 * Upstream wraps io_submit(2).  Here each iocb is translated into a
 * Windows async I/O request; every accepted iocb produces exactly one
 * completion packet in the context's IOCP (see aio_internal.h).
 *
 * Derived from libaio 0.3.113 (https://pagure.io/libaio.git); LGPL-2.1-or-later.
 */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <io.h>
#include <errno.h>
#include <stdio.h>
#include "aio_internal.h"

#define AIO_MAX_IOV		64
#define AIO_MAX_COALESCE	(32ull << 20)	/* temp buffer cap, multi-iov */

static struct aio_request *req_new(struct aio_context *ctx, struct iocb *iocb)
{
	struct aio_request *req = (struct aio_request *)calloc(1, sizeof(*req));
	if (req == NULL)
		return NULL;
	memset(&req->ov, 0, sizeof(req->ov));
	req->iocb = iocb;
	req->ctx = ctx;
	req->event.data = iocb->data;	/* io_event.data/obj are pointers */
	req->event.obj = iocb;
	req->state = AIO_REQ_INFLIGHT;
	return req;
}

static void req_link(struct aio_context *ctx, struct aio_request *req)
{
	EnterCriticalSection(&ctx->lock);
	req->next = ctx->inflight;
	ctx->inflight = req;
	LeaveCriticalSection(&ctx->lock);
}

static int iocb_check(struct iocb *iocb)
{
	if (iocb == NULL)
		return -EINVAL;
	switch (iocb->aio_lio_opcode) {
	case IO_CMD_PREAD:
	case IO_CMD_PWRITE:
		if (iocb->u.c.nbytes > 0xFFFFFFFFull)
			return -EINVAL;
		if (iocb->u.c.offset < 0)
			return -EINVAL;
		if (iocb->u.c.buf == NULL && iocb->u.c.nbytes != 0)
			return -EINVAL;
		if (iocb->u.c.flags & IOCB_FLAG_RESFD)
			return -EINVAL;		/* eventfd: Linux-only */
		if (iocb->aio_rw_flags != 0)
			return -EINVAL;		/* RWF_*: Linux-only */
		break;
	case IO_CMD_PREADV:
	case IO_CMD_PWRITEV:
		if (iocb->u.c.nbytes > AIO_MAX_IOV || iocb->u.c.nbytes == 0)
			return -EINVAL;
		if (iocb->u.c.offset < 0)
			return -EINVAL;
		if (iocb->u.c.buf == NULL)
			return -EINVAL;
		if (iocb->u.c.flags & IOCB_FLAG_RESFD)
			return -EINVAL;
		if (iocb->aio_rw_flags != 0)
			return -EINVAL;
		break;
	case IO_CMD_FSYNC:
	case IO_CMD_FDSYNC:
	case IO_CMD_NOOP:
		break;
	default:
		return -EINVAL;		/* IO_CMD_POLL etc: not supported */
	}
	return 0;
}

static void issue_native(struct aio_context *ctx, struct aio_request *req,
			 int write, void *buf, unsigned long long nbytes,
			 long long offset)
{
	req->ov.Offset = (DWORD)((unsigned long long)offset & 0xFFFFFFFFu);
	req->ov.OffsetHigh = (DWORD)((unsigned long long)offset >> 32);

	/*
	 * For an overlapped handle the completion packet arrives at the
	 * context's IOCP even when ReadFile/WriteFile succeeds inline
	 * (one packet per operation, that is the FILE_FLAG_OVERLAPPED
	 * contract), so no bookkeeping is needed on immediate success.
	 */
	BOOL ok = write ? WriteFile(req->file, buf, (DWORD)nbytes, NULL, &req->ov)
			: ReadFile(req->file, buf, (DWORD)nbytes, NULL, &req->ov);
#ifdef AIO_DEBUG_TRACE
	fprintf(stderr, "[aio] issue_native op=%s ok=%d gle=%lu bytes=%llu off=%lld\n",
		write ? "write" : "read", ok, ok ? 0 : GetLastError(), nbytes,
		offset);
#endif
	if (ok)
		return;
	DWORD e = GetLastError();
	if (e == ERROR_IO_PENDING)
		return;		/* in flight; packet on completion */

	/*
	 * Failed without starting (includes reads past EOF reported as
	 * ERROR_HANDLE_EOF): no kernel packet will arrive, so complete
	 * inline.  ERROR_HANDLE_EOF maps to errno 0 => res == 0, the same
	 * "0 bytes at EOF" result Linux reports.
	 */
	aio_complete_inline(ctx, req, -aio_winerr_to_errno(e));
}

static int submit_one(struct aio_context *ctx, struct iocb *iocb)
{
	int rc = iocb_check(iocb);
	if (rc < 0)
		return rc;
	int op = iocb->aio_lio_opcode;
	int fd = iocb->aio_fildes;

	struct aio_fd af;
	int have_af = aio_fdtab_lookup(fd, &af);
	HANDLE h;
	if (have_af) {
		h = af.handle;
	} else {
		intptr_t oh = aio_safe_get_osfhandle(fd);
		if (oh == -1)
			return -EBADF;
		h = (HANDLE)oh;
	}

	struct aio_request *req = req_new(ctx, iocb);
	if (req == NULL)
		return -EAGAIN;

	if (op == IO_CMD_FSYNC || op == IO_CMD_FDSYNC) {
		req->kind = AIO_KIND_WORKER;
		req->file = h;
		req_link(ctx, req);
		InterlockedIncrement(&ctx->outstanding);
		aio_worker_enqueue(ctx, req);
		return 0;
	}
	if (op == IO_CMD_NOOP) {
		req_link(ctx, req);
		InterlockedIncrement(&ctx->outstanding);
		aio_complete_inline(ctx, req, 0);
		return 0;
	}

	void *buf;
	unsigned long long nbytes;
	long long offset = iocb->u.c.offset;
	struct iovec *iov = NULL;
	int iovcnt = 0;
	int multi_iov = 0;
	int is_write = (op == IO_CMD_PWRITE || op == IO_CMD_PWRITEV);

	if (op == IO_CMD_PREAD || op == IO_CMD_PWRITE) {
		buf = iocb->u.c.buf;
		nbytes = iocb->u.c.nbytes;
	} else {
		iov = (struct iovec *)iocb->u.c.buf;
		iovcnt = (int)iocb->u.c.nbytes;
		unsigned long long total = 0;
		for (int i = 0; i < iovcnt; i++) {
			total += (unsigned long long)iov[i].iov_len;
			if (total > 0xFFFFFFFFull || total > AIO_MAX_COALESCE) {
				free(req);
				return -EINVAL;
			}
		}
		if (total == 0) {
			free(req);
			return -EINVAL;
		}
		nbytes = total;
		if (iovcnt == 1) {	/* degenerate to the plain path */
			buf = iov[0].iov_base;
		} else {
			multi_iov = 1;
			buf = NULL;
		}
	}

	if (nbytes == 0) {	/* Linux: pread/pwrite of 0 bytes returns 0 */
		req_link(ctx, req);
		InterlockedIncrement(&ctx->outstanding);
		aio_complete_inline(ctx, req, 0);
		return 0;
	}

	/* O_DIRECT fds need sector-aligned offset/length/buffer, like Linux */
	if (have_af && af.no_buffering) {
		int ss = af.sector_size > 0 ? af.sector_size : 512;
		int bad = ((unsigned long long)offset & (unsigned)(ss - 1)) != 0 ||
			  (nbytes & (unsigned)(ss - 1)) != 0;
		if (!multi_iov)
			bad |= ((uintptr_t)buf & (unsigned)(ss - 1)) != 0;
		if (bad) {
			free(req);
			return -EINVAL;
		}
	}

	/* tier 1: aio_open() fd already bound to this context's IOCP */
	int native = 0;
	if (have_af && af.overlapped && !multi_iov) {
		if (af.bound_port != ctx->iocp &&
		    af.bound_port != INVALID_HANDLE_VALUE) {
			HANDLE bound = CreateIoCompletionPort(h, ctx->iocp,
							      AIO_COMPLETION_KEY, 0);
			aio_fdtab_set_bound(fd, bound ? ctx->iocp : INVALID_HANDLE_VALUE);
			af.bound_port = bound ? ctx->iocp : INVALID_HANDLE_VALUE;
		}
		native = (af.bound_port == ctx->iocp);
	}

	req_link(ctx, req);
	InterlockedIncrement(&ctx->outstanding);

	if (native) {
		req->kind = AIO_KIND_NATIVE;
		req->file = h;
#ifdef AIO_DEBUG_TRACE
		fprintf(stderr, "[aio] submit fd=%d op=%d tier=native\n", fd, op);
#endif
		issue_native(ctx, req, is_write, buf, nbytes, offset);
		return 0;
	}

	if (!multi_iov) {
		/* tier 2: fresh overlapped handle per request */
		DWORD acc = is_write ? GENERIC_WRITE : GENERIC_READ;
		HANDLE h2 = ReOpenFile(h, acc,
				       FILE_SHARE_READ | FILE_SHARE_WRITE |
					   FILE_SHARE_DELETE,
				       FILE_FLAG_OVERLAPPED);
		if (h2 != NULL && h2 != INVALID_HANDLE_VALUE) {
			if (CreateIoCompletionPort(h2, ctx->iocp,
						   AIO_COMPLETION_KEY, 0)) {
				req->kind = AIO_KIND_REOPEN;
				req->file = h2;
				req->close_file = 1;
				issue_native(ctx, req, is_write, buf, nbytes,
					     offset);
				return 0;
			}
			CloseHandle(h2);
		}
		/* tier 3 below */
	}

	/* tier 3: blocking emulation on the context worker thread */
	req->kind = AIO_KIND_WORKER;
	req->file = h;
	req->emu_buf = buf;
	req->emu_bytes = nbytes;
	req->emu_offset = offset;
	if (multi_iov) {
		int ss = (have_af && af.no_buffering && af.sector_size > 0)
			     ? af.sector_size : 16;
		if (ss < 16)
			ss = 16;
		req->emu_iov = (struct iovec *)malloc(
			(size_t)iovcnt * sizeof(struct iovec));
		req->emu_tmp = _aligned_malloc((size_t)nbytes, ss);
		if (req->emu_iov == NULL || req->emu_tmp == NULL) {
			free(req->emu_iov);
			_aligned_free(req->emu_tmp);
			aio_inflight_remove(ctx, req);
			InterlockedDecrement(&ctx->outstanding);
			free(req);
			return -EAGAIN;
		}
		memcpy(req->emu_iov, iov, (size_t)iovcnt * sizeof(struct iovec));
		req->emu_iovcnt = iovcnt;
		if (is_write) {
			unsigned long long off = 0;
			for (int i = 0; i < iovcnt; i++) {
				memcpy((char *)req->emu_tmp + off,
				       iov[i].iov_base, iov[i].iov_len);
				off += iov[i].iov_len;
			}
		}
		req->emu_buf = req->emu_tmp;
	}
	aio_worker_enqueue(ctx, req);
	return 0;
}

int io_submit(io_context_t ctx, long nr, struct iocb *ios[])
{
	if (ctx == NULL || ios == NULL || nr < 0)
		return -EINVAL;
	struct aio_context *c = (struct aio_context *)ctx;

	long done = 0;
	for (long i = 0; i < nr; i++) {
		int r = submit_one(c, ios[i]);
		if (r < 0)
			return done > 0 ? (int)done : r;
		done++;
	}
	return (int)done;
}
