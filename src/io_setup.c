/* io_setup.c -- io_setup() for the Windows libaio port.
 *
 * Upstream wraps the io_setup(2) syscall, which creates a kernel AIO
 * context.  On Windows the context is an I/O completion port; every
 * submitted request ultimately lands in it as exactly one packet.
 *
 * Derived from libaio 0.3.113 (https://pagure.io/libaio.git); LGPL-2.1-or-later.
 */
#include <windows.h>
#include <stdlib.h>
#include <errno.h>
#include "aio_internal.h"

int io_setup(int maxevents, io_context_t *ctxp)
{
	if (ctxp == NULL || maxevents <= 0)
		return -EINVAL;

	struct aio_context *ctx = (struct aio_context *)calloc(1, sizeof(*ctx));
	if (ctx == NULL)
		return -ENOMEM;

	ctx->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (ctx->iocp == NULL) {
		DWORD e = GetLastError();
		free(ctx);
		return -aio_winerr_to_errno(e);
	}
	ctx->sem = CreateSemaphoreW(NULL, 0, LONG_MAX, NULL);
	if (ctx->sem == NULL) {
		DWORD e = GetLastError();
		CloseHandle(ctx->iocp);
		free(ctx);
		return -aio_winerr_to_errno(e);
	}
	InitializeCriticalSection(&ctx->lock);
	ctx->maxevents = maxevents;

	*ctxp = (io_context_t)ctx;
	return 0;
}
