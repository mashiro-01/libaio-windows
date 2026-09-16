/* aio_backend.c -- fd table, aio_open()/aio_close(), errno mapping and
 * the per-context worker thread used by the Windows libaio port.
 *
 * Derived from libaio 0.3.113 (https://pagure.io/libaio.git); LGPL-2.1-or-later.
 */
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <errno.h>
#include <limits.h>
#include "aio_internal.h"

/* ------------------------------------------------------------ errno map */

int aio_winerr_to_errno(DWORD e)
{
	switch (e) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:	return ENOENT;
	case ERROR_ACCESS_DENIED:	return EACCES;
	case ERROR_INVALID_HANDLE:	return EBADF;
	case ERROR_NOT_ENOUGH_MEMORY:
	case ERROR_OUTOFMEMORY:		return ENOMEM;
	case ERROR_DISK_FULL:		return ENOSPC;
	case ERROR_WRITE_PROTECT:	return EROFS;
	case ERROR_SHARING_VIOLATION:
	case ERROR_LOCK_VIOLATION:	return EBUSY;
	case ERROR_FILE_EXISTS:
	case ERROR_ALREADY_EXISTS:	return EEXIST;
	case ERROR_INVALID_PARAMETER:
	case ERROR_INVALID_USER_BUFFER:
	case ERROR_NEGATIVE_SEEK:	return EINVAL;
	case ERROR_OPERATION_ABORTED:	return ECANCELED;
	case ERROR_NO_SYSTEM_RESOURCES:	return EAGAIN;
	case ERROR_NOT_SUPPORTED:	return ENOSYS;
	case ERROR_BROKEN_PIPE:		return EPIPE;
	case ERROR_HANDLE_EOF:		return 0;
	default:			return EIO;
	}
}

int aio_ntstatus_to_errno(ULONG st)
{
	typedef ULONG (WINAPI *rtl_ntstatus_fn)(LONG);
	static rtl_ntstatus_fn fn;	/* benign race: idempotent store */

	if (fn == NULL) {
		HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
		if (ntdll == NULL)
			return EIO;
		fn = (rtl_ntstatus_fn)(void *)GetProcAddress(ntdll, "RtlNtStatusToDosError");
		if (fn == NULL)
			return EIO;
	}
	ULONG e = fn((LONG)st);
	if (e == 0 || e == ERROR_MR_MID_NOT_FOUND)
		return EIO;
	return aio_winerr_to_errno(e);
}

/* ------------------------------------------------------------- fd table */

/*
 * MSVC's _get_osfhandle() invokes the invalid-parameter handler (which
 * aborts the process) for fds that are not open, instead of returning
 * -1 like Linux open() callers expect.  Swap in a silent handler around
 * the call so bad fds surface as -EBADF from io_submit().
 */
static void aio_silent_iph(const wchar_t *expr, const wchar_t *func,
			   const wchar_t *file, unsigned int line,
			   uintptr_t pReserved)
{
	(void)expr; (void)func; (void)file; (void)line; (void)pReserved;
}

intptr_t aio_safe_get_osfhandle(int fd)
{
	_invalid_parameter_handler old =
		_set_invalid_parameter_handler(aio_silent_iph);
	intptr_t h = _get_osfhandle(fd);
	_set_invalid_parameter_handler(old);
	return h;
}

static struct {
	CRITICAL_SECTION	lock;
	struct aio_fd		**slots;
	int			cap;
} g_fdtab;

static INIT_ONCE g_fdtab_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK fdtab_once_cb(PINIT_ONCE once, PVOID param, PVOID *pctx)
{
	(void)once; (void)param; (void)pctx;
	InitializeCriticalSection(&g_fdtab.lock);
	g_fdtab.cap = 64;
	g_fdtab.slots = (struct aio_fd **)calloc((size_t)g_fdtab.cap,
						 sizeof(*g_fdtab.slots));
	return g_fdtab.slots != NULL;
}

static void fdtab_ensure(void)
{
	PVOID pending;
	InitOnceExecuteOnce(&g_fdtab_once, fdtab_once_cb, NULL, &pending);
}

int aio_fdtab_lookup(int fd, struct aio_fd *out)
{
	fdtab_ensure();
	if (fd < 0)
		return 0;
	EnterCriticalSection(&g_fdtab.lock);
	struct aio_fd *rec = (fd < g_fdtab.cap) ? g_fdtab.slots[fd] : NULL;
	if (rec)
		*out = *rec;
	LeaveCriticalSection(&g_fdtab.lock);
	return rec != NULL;
}

void aio_fdtab_insert(int fd, HANDLE h, int overlapped, int no_buffering,
		      int sector_size)
{
	fdtab_ensure();
	EnterCriticalSection(&g_fdtab.lock);
	if (fd >= g_fdtab.cap) {
		int ncap = g_fdtab.cap;
		while (ncap <= fd)
			ncap *= 2;
		struct aio_fd **ns = (struct aio_fd **)realloc(
			g_fdtab.slots, (size_t)ncap * sizeof(*ns));
		if (ns == NULL) {
			LeaveCriticalSection(&g_fdtab.lock);
			return;	/* fd simply loses its fast path */
		}
		memset(ns + g_fdtab.cap, 0,
		       (size_t)(ncap - g_fdtab.cap) * sizeof(*ns));
		g_fdtab.slots = ns;
		g_fdtab.cap = ncap;
	}
	if (g_fdtab.slots[fd] == NULL) {
		g_fdtab.slots[fd] = (struct aio_fd *)calloc(1, sizeof(struct aio_fd));
		if (g_fdtab.slots[fd] == NULL) {
			LeaveCriticalSection(&g_fdtab.lock);
			return;
		}
	}
	g_fdtab.slots[fd]->handle = h;
	g_fdtab.slots[fd]->overlapped = overlapped;
	g_fdtab.slots[fd]->no_buffering = no_buffering;
	g_fdtab.slots[fd]->sector_size = sector_size;
	g_fdtab.slots[fd]->bound_port = NULL;
	LeaveCriticalSection(&g_fdtab.lock);
}

void aio_fdtab_remove(int fd)
{
	fdtab_ensure();
	if (fd < 0)
		return;
	EnterCriticalSection(&g_fdtab.lock);
	if (fd < g_fdtab.cap) {
		free(g_fdtab.slots[fd]);
		g_fdtab.slots[fd] = NULL;
	}
	LeaveCriticalSection(&g_fdtab.lock);
}

void aio_fdtab_set_bound(int fd, HANDLE port)
{
	fdtab_ensure();
	if (fd < 0)
		return;
	EnterCriticalSection(&g_fdtab.lock);
	if (fd < g_fdtab.cap && g_fdtab.slots[fd] != NULL)
		g_fdtab.slots[fd]->bound_port = port;
	LeaveCriticalSection(&g_fdtab.lock);
}

/* --------------------------------------------------------- inflight list */

void aio_inflight_remove(struct aio_context *ctx, struct aio_request *req)
{
	EnterCriticalSection(&ctx->lock);
	struct aio_request **pp = &ctx->inflight;
	while (*pp && *pp != req)
		pp = &(*pp)->next;
	if (*pp)
		*pp = req->next;
	LeaveCriticalSection(&ctx->lock);
}

/* ------------------------------------------------- completion posting */

void aio_complete_inline(struct aio_context *ctx, struct aio_request *req,
			 long long res)
{
	req->event.res = (unsigned long long)res;
	req->event.res2 = 0;
	req->state = AIO_REQ_DONE;
	PostQueuedCompletionStatus(ctx->iocp, 0, AIO_COMPLETION_KEY, &req->ov);
}

/* ------------------------------------------------------------ worker */

static void emu_positioned_io(struct aio_request *req, int write, long long *res)
{
	static const DWORD share_all = FILE_SHARE_READ | FILE_SHARE_WRITE |
				       FILE_SHARE_DELETE;
	HANDLE h = req->file;
	HANDLE hsync = INVALID_HANDLE_VALUE;

	/*
	 * The worker only ever sees handles for which the async paths
	 * failed.  Reopen synchronously for the blocking call so we do not
	 * issue unoverlapped ReadFile() on an overlapped handle.
	 */
	hsync = ReOpenFile(h, write ? GENERIC_WRITE : GENERIC_READ,
			   share_all, 0);
	HANDLE use = (hsync != INVALID_HANDLE_VALUE) ? hsync : h;

	LARGE_INTEGER li;
	li.QuadPart = req->emu_offset;
	if (!SetFilePointerEx(use, li, NULL, FILE_BEGIN)) {
		DWORD e = GetLastError();
		if (hsync != INVALID_HANDLE_VALUE)
			CloseHandle(hsync);
		*res = -aio_winerr_to_errno(e);
		return;
	}
	DWORD want = (req->emu_bytes > 0xFFFFFFFFull) ? 0xFFFFFFFFu
						     : (DWORD)req->emu_bytes;
	DWORD got = 0;
	BOOL ok = write ? WriteFile(use, req->emu_buf, want, &got, NULL)
			: ReadFile(use, req->emu_buf, want, &got, NULL);
	if (hsync != INVALID_HANDLE_VALUE)
		CloseHandle(hsync);
	if (!ok) {
		*res = -aio_winerr_to_errno(GetLastError());
		return;
	}
	*res = (long long)got;
}

static void emu_fsync(struct aio_request *req, long long *res)
{
	if (FlushFileBuffers(req->file)) {
		*res = 0;
		return;
	}
	DWORD e = GetLastError();
	static const DWORD share_all = FILE_SHARE_READ | FILE_SHARE_WRITE |
				       FILE_SHARE_DELETE;
	HANDLE hsync = ReOpenFile(req->file, GENERIC_WRITE, share_all, 0);
	if (hsync != INVALID_HANDLE_VALUE) {
		if (FlushFileBuffers(hsync)) {
			CloseHandle(hsync);
			*res = 0;
			return;
		}
		CloseHandle(hsync);
	}
	*res = -aio_winerr_to_errno(e);
}

static void emu_vectored(struct aio_request *req, int write, long long *res)
{
	if (write) {
		unsigned long long off = 0;
		for (int i = 0; i < req->emu_iovcnt; i++) {
			memcpy((char *)req->emu_tmp + off,
			       req->emu_iov[i].iov_base,
			       req->emu_iov[i].iov_len);
			off += req->emu_iov[i].iov_len;
		}
	}
	req->emu_buf = req->emu_tmp;
	emu_positioned_io(req, write, res);
	if (!write && *res > 0) {
		unsigned long long off = 0;
		long long done = *res;
		for (int i = 0; i < req->emu_iovcnt && done > 0; i++) {
			size_t n = req->emu_iov[i].iov_len < (size_t)done
				       ? req->emu_iov[i].iov_len
				       : (size_t)done;
			memcpy(req->emu_iov[i].iov_base,
			       (const char *)req->emu_tmp + off, n);
			off += n;
			done -= (long long)n;
		}
	}
}

static void aio_worker_run(struct aio_context *ctx, struct aio_request *req)
{
	long long res;
	int op = req->iocb->aio_lio_opcode;

	switch (op) {
	case IO_CMD_PREAD:	emu_positioned_io(req, 0, &res); break;
	case IO_CMD_PWRITE:	emu_positioned_io(req, 1, &res); break;
	case IO_CMD_FSYNC:
	case IO_CMD_FDSYNC:	emu_fsync(req, &res); break;
	case IO_CMD_PREADV:	emu_vectored(req, 0, &res); break;
	case IO_CMD_PWRITEV:	emu_vectored(req, 1, &res); break;
	default:		res = -EINVAL; break;
	}
	if (req->emu_iov) {
		free(req->emu_iov);
		req->emu_iov = NULL;
	}
	if (req->emu_tmp) {
		_aligned_free(req->emu_tmp);
		req->emu_tmp = NULL;
	}
	aio_complete_inline(ctx, req, res);
}

static DWORD WINAPI aio_worker_proc(LPVOID arg)
{
	struct aio_context *ctx = (struct aio_context *)arg;

	for (;;) {
		WaitForSingleObject(ctx->sem, INFINITE);
		if (ctx->stop)
			break;
		EnterCriticalSection(&ctx->lock);
		struct aio_request *req = ctx->work_head;
		if (req) {
			ctx->work_head = req->work_next;
			if (ctx->work_head == NULL)
				ctx->work_tail = NULL;
		}
		LeaveCriticalSection(&ctx->lock);
		if (req)
			aio_worker_run(ctx, req);
	}
	return 0;
}

void aio_worker_enqueue(struct aio_context *ctx, struct aio_request *req)
{
	EnterCriticalSection(&ctx->lock);
	req->work_next = NULL;
	if (ctx->work_tail)
		ctx->work_tail->work_next = req;
	else
		ctx->work_head = req;
	ctx->work_tail = req;
	if (ctx->worker == NULL)
		ctx->worker = CreateThread(NULL, 0, aio_worker_proc, ctx, 0, NULL);
	LeaveCriticalSection(&ctx->lock);
	ReleaseSemaphore(ctx->sem, 1, NULL);
}

/* ------------------------------------------------------- aio_open/close */

static wchar_t *utf8_to_wide(const char *s)
{
	int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
	if (n <= 0)
		return NULL;
	wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
	if (w == NULL)
		return NULL;
	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) <= 0) {
		free(w);
		return NULL;
	}
	return w;
}

static int sector_size_of(HANDLE h)
{
#ifdef FileFsSectorSizeInformation
	FILE_FS_SECTOR_SIZE_INFORMATION si;
	if (GetFileInformationByHandleEx(h, FileFsSectorSizeInformation,
					 &si, sizeof(si)) &&
	    si.LogicalBytesPerSector)
		return (int)si.LogicalBytesPerSector;
#endif
	return 512;
}

int aio_open(const char *path, int flags, ...)
{
	if (path == NULL) {
		errno = EINVAL;
		return -1;
	}
	fdtab_ensure();

	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		(void)va_arg(ap, int);	/* mode: no umask on Windows */
		va_end(ap);
	}

	DWORD access = 0;
	if (flags & O_WRONLY)
		access = GENERIC_WRITE;
	else if (flags & O_RDWR)
		access = GENERIC_READ | GENERIC_WRITE;
	else
		access = GENERIC_READ;
	if (flags & O_APPEND)
		access |= FILE_APPEND_DATA;

	DWORD create;
	if (flags & O_CREAT)
		create = (flags & O_EXCL) ? CREATE_NEW
					  : ((flags & O_TRUNC) ? CREATE_ALWAYS
							       : OPEN_ALWAYS);
	else if (flags & O_TRUNC)
		create = TRUNCATE_EXISTING;
	else
		create = OPEN_EXISTING;

	DWORD attrs = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED;
	if (flags & O_DIRECT)
		attrs |= FILE_FLAG_NO_BUFFERING;
	if (flags & O_SYNC)
		attrs |= FILE_FLAG_WRITE_THROUGH;
#ifdef O_SEQUENTIAL
	if (flags & O_SEQUENTIAL)
		attrs |= FILE_FLAG_SEQUENTIAL_SCAN;
#endif
#ifdef O_RANDOM
	if (flags & O_RANDOM)
		attrs |= FILE_FLAG_RANDOM_ACCESS;
#endif

	wchar_t *wpath = utf8_to_wide(path);
	if (wpath == NULL) {
		errno = (GetLastError() == ERROR_NO_UNICODE_TRANSLATION)
			    ? EINVAL : ENOMEM;
		return -1;
	}
	HANDLE h = CreateFileW(wpath, access,
			       FILE_SHARE_READ | FILE_SHARE_WRITE |
				   FILE_SHARE_DELETE,
			       NULL, create, attrs, NULL);
	free(wpath);
	if (h == INVALID_HANDLE_VALUE) {
		errno = aio_winerr_to_errno(GetLastError());
		return -1;
	}

	int crt_flags = _O_BINARY | _O_NOINHERIT;
	if (flags & O_WRONLY)
		crt_flags |= _O_WRONLY;
	else if (flags & O_RDWR)
		crt_flags |= _O_RDWR;
	else
		crt_flags |= _O_RDONLY;
	if (flags & O_APPEND)
		crt_flags |= _O_APPEND;

	int fd = _open_osfhandle((intptr_t)h, crt_flags);
	if (fd < 0) {
		CloseHandle(h);
		errno = EMFILE;
		return -1;
	}
	aio_fdtab_insert(fd, h, 1, (flags & O_DIRECT) ? 1 : 0, sector_size_of(h));
	return fd;
}

int aio_close(int fd)
{
	fdtab_ensure();
	aio_fdtab_remove(fd);
	return _close(fd);
}
