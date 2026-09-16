/* test_basic.c -- functional tests for the Windows libaio port.
 *
 * Covers the three submission tiers (native aio_open() fd, ReOpenFile on
 * a foreign CRT fd, worker emulation), completion semantics, EOF
 * behaviour, cancellation, timeouts, alignment and error reporting.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <malloc.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <windows.h>
#include "libaio.h"

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s (errno=%d GetLastError=%lu)\n", \
			__FILE__, __LINE__, #cond, errno, GetLastError()); \
		exit(1); \
	} \
} while (0)

#define BUFSZ 4096
#define NBUFS 4

static void test_native_o_direct(void)
{
	const char *path = "aio_test_native.bin";
	int fd = aio_open(path, O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, 0600);
	CHECK(fd >= 0);

	io_context_t ctx = 0;
	CHECK(io_setup(NBUFS * 2, &ctx) == 0);

	char *wbuf = (char *)_aligned_malloc(BUFSZ * NBUFS, BUFSZ);
	char *rbuf = (char *)_aligned_malloc(BUFSZ * NBUFS, BUFSZ);
	CHECK(wbuf != NULL && rbuf != NULL);
	memset(rbuf, 0, BUFSZ * NBUFS);

	/* 4 concurrent async writes: the core io_submit/io_getevents loop */
	struct iocb cbs[NBUFS];
	struct iocb *cbp[NBUFS];
	struct io_event ev[NBUFS];
	struct timespec ts;

	for (int i = 0; i < NBUFS; i++) {
		memset(wbuf + i * BUFSZ, 'A' + i, BUFSZ);
		io_prep_pwrite(&cbs[i], fd, wbuf + i * BUFSZ, BUFSZ,
			       (long long)i * BUFSZ);
		io_set_callback(&cbs[i],
				(io_callback_t)(uintptr_t)(0x1234 + i));
		cbp[i] = &cbs[i];
	}
	CHECK(io_submit(ctx, NBUFS, cbp) == NBUFS);

	ts.tv_sec = 10; ts.tv_nsec = 0;
	CHECK(io_getevents(ctx, NBUFS, NBUFS, ev, &ts) == NBUFS);
	for (int i = 0; i < NBUFS; i++) {
		struct iocb *icb = (struct iocb *)(uintptr_t)ev[i].obj;
		CHECK(icb >= &cbs[0] && icb <= &cbs[NBUFS - 1]);
		int idx = (int)(icb - cbs);
		CHECK(ev[i].res == BUFSZ);
		CHECK(ev[i].res2 == 0);
		CHECK((uintptr_t)ev[i].data == 0x1234 + idx);
	}

	/* async reads + data verification (reap in a loop like DeepSpeed,
	 * since io_getevents(min=1, nr=4) may return as few as 1 event) */
	memset(cbs, 0, sizeof(cbs));
	for (int i = 0; i < NBUFS; i++) {
		io_prep_pread(&cbs[i], fd, rbuf + i * BUFSZ, BUFSZ,
			      (long long)i * BUFSZ);
		cbp[i] = &cbs[i];
	}
	CHECK(io_submit(ctx, NBUFS, cbp) == NBUFS);
	int reaped = 0;
	ts.tv_sec = 10; ts.tv_nsec = 0;
	while (reaped < NBUFS) {
		int n = io_getevents(ctx, 1, NBUFS - reaped, ev + reaped, &ts);
		CHECK(n >= 1);
		for (int k = reaped; k < reaped + n; k++)
			CHECK(ev[k].res == BUFSZ);
		reaped += n;
	}
	for (int i = 0; i < NBUFS; i++)
		for (int j = 0; j < BUFSZ; j++)
			CHECK(rbuf[i * BUFSZ + j] == (char)('A' + i));
	printf("  native O_DIRECT path: submit/getevents/verify OK\n");

	/* short read near EOF: last page + one page beyond the file */
	io_prep_pread(&cbs[0], fd, rbuf, BUFSZ * 2,
		      (long long)(NBUFS - 1) * BUFSZ);
	cbp[0] = &cbs[0];
	CHECK(io_submit(ctx, 1, cbp) == 1);
	ts.tv_sec = 10; ts.tv_nsec = 0;
	CHECK(io_getevents(ctx, 1, 1, ev, &ts) == 1);
	CHECK(ev[0].res == BUFSZ);
	/* no double completion: queue must be empty now */
	ts.tv_sec = 0; ts.tv_nsec = 0;
	CHECK(io_getevents(ctx, 0, 8, ev, &ts) == 0);

	/* read entirely past EOF: res == 0, like Linux pread */
	io_prep_pread(&cbs[0], fd, rbuf, BUFSZ, (long long)NBUFS * BUFSZ);
	cbp[0] = &cbs[0];
	CHECK(io_submit(ctx, 1, cbp) == 1);
	ts.tv_sec = 10; ts.tv_nsec = 0;
	CHECK(io_getevents(ctx, 1, 1, ev, &ts) == 1);
	CHECK(ev[0].res == 0);
	ts.tv_sec = 0; ts.tv_nsec = 0;
	CHECK(io_getevents(ctx, 0, 8, ev, &ts) == 0);
	printf("  EOF semantics OK\n");

	/* fsync through the AIO context */
	io_prep_fsync(&cbs[0], fd);
	cbp[0] = &cbs[0];
	CHECK(io_submit(ctx, 1, cbp) == 1);
	ts.tv_sec = 10; ts.tv_nsec = 0;
	CHECK(io_getevents(ctx, 1, 1, ev, &ts) == 1);
	CHECK(ev[0].res == 0);
	printf("  IO_CMD_FSYNC OK\n");

	/* alignment: O_DIRECT demands sector-aligned buffer/length/offset */
	io_prep_pwrite(&cbs[0], fd, wbuf + 1, BUFSZ, 0);
	cbp[0] = &cbs[0];
	CHECK(io_submit(ctx, 1, cbp) == -EINVAL);
	io_prep_pwrite(&cbs[0], fd, wbuf, BUFSZ, 1);
	CHECK(io_submit(ctx, 1, cbp) == -EINVAL);
	printf("  O_DIRECT alignment checks OK\n");

	/* bad fd */
	io_prep_pread(&cbs[0], 4321, rbuf, BUFSZ, 0);
	CHECK(io_submit(ctx, 1, cbp) == -EBADF);

	/* unsupported opcode */
	io_prep_poll(&cbs[0], fd, 1);
	CHECK(io_submit(ctx, 1, cbp) == -EINVAL);

	/* cancel of a request that was never submitted */
	struct iocb other;
	io_prep_pread(&other, fd, rbuf, BUFSZ, 0);
	CHECK(io_cancel(ctx, &other, &ev[0]) == -ENOENT);

	/* timeout with nothing outstanding */
	ts.tv_sec = 0; ts.tv_nsec = 200000000;
	DWORD t0 = GetTickCount();
	CHECK(io_getevents(ctx, 1, 1, ev, &ts) == 0);
	CHECK(GetTickCount() - t0 >= 150);
	printf("  timeout + cancel + error paths OK\n");

	CHECK(io_destroy(ctx) == 0);
	CHECK(aio_close(fd) == 0);
	_aligned_free(wbuf);
	_aligned_free(rbuf);
	_unlink(path);
}

static void test_reopen_crt_fd(void)
{
	/* foreign fd: CRT _open(), so io_submit() must take the
	 * ReOpenFile() tier and still deliver full async semantics */
	const char *path = "aio_test_crt.bin";
	int fd = _open(path, _O_RDWR | _O_CREAT | _O_TRUNC | _O_BINARY,
		       _S_IREAD | _S_IWRITE);
	CHECK(fd >= 0);

	io_context_t ctx = 0;
	CHECK(io_setup(8, &ctx) == 0);

	char *buf = (char *)malloc(BUFSZ);
	char *rbuf = (char *)malloc(BUFSZ);
	CHECK(buf != NULL && rbuf != NULL);
	memset(buf, 'B', BUFSZ);
	memset(rbuf, 0, BUFSZ);

	struct iocb cb;
	struct iocb *cbp[1];
	struct io_event ev;
	struct timespec ts;
	ts.tv_sec = 10; ts.tv_nsec = 0;
	cbp[0] = &cb;

	io_prep_pwrite(&cb, fd, buf, BUFSZ, 0);
	CHECK(io_submit(ctx, 1, cbp) == 1);
	CHECK(io_getevents(ctx, 1, 1, &ev, &ts) == 1);
	CHECK(ev.res == BUFSZ);

	io_prep_pread(&cb, fd, rbuf, BUFSZ, 0);
	CHECK(io_submit(ctx, 1, cbp) == 1);
	CHECK(io_getevents(ctx, 1, 1, &ev, &ts) == 1);
	CHECK(ev.res == BUFSZ);
	CHECK(rbuf[0] == 'B' && rbuf[BUFSZ - 1] == 'B');
	printf("  CRT-fd ReOpenFile path OK\n");

	CHECK(io_destroy(ctx) == 0);
	_close(fd);
	free(buf);
	free(rbuf);
	_unlink(path);
}

static void test_vectored(void)
{
	const char *path = "aio_test_vec.bin";
	int fd = _open(path, _O_RDWR | _O_CREAT | _O_TRUNC | _O_BINARY,
		       _S_IREAD | _S_IWRITE);
	CHECK(fd >= 0);

	io_context_t ctx = 0;
	CHECK(io_setup(8, &ctx) == 0);

	char *a = (char *)malloc(1024);
	char *b = (char *)malloc(3072);
	char *ra = (char *)malloc(1024);
	char *rb = (char *)malloc(3072);
	CHECK(a && b && ra && rb);
	memset(a, 'C', 1024);
	memset(b, 'D', 3072);
	memset(ra, 0, 1024);
	memset(rb, 0, 3072);

	struct iovec wiov[2] = { { a, 1024 }, { b, 3072 } };
	struct iovec riov[2] = { { ra, 1024 }, { rb, 3072 } };

	struct iocb cb;
	struct iocb *cbp[1];
	cbp[0] = &cb;
	struct io_event ev;
	struct timespec ts;
	ts.tv_sec = 10; ts.tv_nsec = 0;

	io_prep_pwritev(&cb, fd, wiov, 2, 0);	/* multi-iov: worker tier */
	CHECK(io_submit(ctx, 1, cbp) == 1);
	CHECK(io_getevents(ctx, 1, 1, &ev, &ts) == 1);
	CHECK(ev.res == 4096);

	io_prep_preadv(&cb, fd, riov, 2, 0);
	CHECK(io_submit(ctx, 1, cbp) == 1);
	CHECK(io_getevents(ctx, 1, 1, &ev, &ts) == 1);
	CHECK(ev.res == 4096);
	CHECK(ra[0] == 'C' && ra[1023] == 'C');
	CHECK(rb[0] == 'D' && rb[3071] == 'D');

	/* single-iov preadv degenerates to the plain path */
	io_prep_preadv(&cb, fd, riov, 1, 0);
	CHECK(io_submit(ctx, 1, cbp) == 1);
	CHECK(io_getevents(ctx, 1, 1, &ev, &ts) == 1);
	CHECK(ev.res == 1024);
	printf("  vectored ops OK\n");

	CHECK(io_destroy(ctx) == 0);
	_close(fd);
	free(a); free(b); free(ra); free(rb);
	_unlink(path);
}

static void test_misc(void)
{
	io_context_t ctx = 0;
	CHECK(io_setup(0, &ctx) == -EINVAL);
	CHECK(io_setup(4, NULL) == -EINVAL);
	CHECK(io_destroy(NULL) == -EINVAL);
	CHECK(io_submit(NULL, 1, NULL) == -EINVAL);

	CHECK(io_setup(4, &ctx) == 0);
	struct io_event ev;
	struct timespec ts;
	ts.tv_sec = 0; ts.tv_nsec = 0;
	CHECK(io_getevents(ctx, 1, 1, NULL, &ts) == -EINVAL);
	CHECK(io_getevents(ctx, -1, 1, &ev, &ts) == -EINVAL);
	CHECK(io_getevents(ctx, 2, 1, &ev, &ts) == -EINVAL);
	/* NULL timeout with an empty queue must never be attempted here:
	 * like Linux, that means "block forever".  Use a zero timeout. */
	ts.tv_sec = 0; ts.tv_nsec = 0;
	CHECK(io_getevents(ctx, 1, 1, &ev, &ts) == 0);	/* nothing outstanding */

	CHECK(io_destroy(ctx) == 0);
	printf("  argument validation OK\n");
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	printf("test_basic: Windows libaio port\n");
	test_native_o_direct();
	test_reopen_crt_fd();
	test_vectored();
	test_misc();
	printf("ALL TESTS PASSED\n");
	return 0;
}
