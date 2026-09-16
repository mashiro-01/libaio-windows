/* test_deepspeed.c -- mirrors the aio smoke test that DeepSpeed's
 * async_io op builder compiles when it probes for libaio (io_setup,
 * io_prep_pwrite/io_submit/io_getevents, io_prep_pread/.../io_destroy),
 * with Linux open(O_DIRECT) swapped for the Windows port's aio_open().
 *
 * This is the exact call sequence DeepSpeed's deepspeed_aio.cpp uses,
 * so passing here means the DeepSpeed AIO backend can build on top of
 * aio.lib unchanged.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <malloc.h>
#include <io.h>
#include "libaio.h"

int main(void)
{
	io_context_t ctx = 0;
	struct iocb cb;
	struct iocb *cbp[1];
	struct io_event e;
	struct timespec tms;
	int fd = -1;
	int ret = 1;
	cbp[0] = &cb;

	char *buffer = (char *)_aligned_malloc(4096, 4096);
	if (buffer == NULL) {
		fprintf(stderr, "aligned alloc failed\n");
		return 1;
	}
	memset(buffer, 'A', 4096);

	fd = aio_open("aio_deepspeed_test.bin",
		      O_DIRECT | O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd == -1) {
		fprintf(stderr, "aio_open: %s\n", strerror(errno));
		goto out;
	}
	if (io_setup(128, &ctx) != 0) {
		fprintf(stderr, "io_setup failed\n");
		goto out;
	}

	tms.tv_sec = 1;
	tms.tv_nsec = 0;
	io_prep_pwrite(&cb, fd, buffer, 4096, 0);
	if (io_submit(ctx, 1, cbp) != 1) {
		fprintf(stderr, "io_submit(write) failed\n");
		goto out;
	}
	if (io_getevents(ctx, 1, 1, &e, &tms) != 1) {
		fprintf(stderr, "io_getevents(write) failed\n");
		goto out;
	}
	if ((long long)e.res != 4096) {
		fprintf(stderr, "write res=%lld\n", (long long)e.res);
		goto out;
	}

	memset(buffer, 0, 4096);
	tms.tv_sec = 1;
	tms.tv_nsec = 0;
	io_prep_pread(&cb, fd, buffer, 4096, 0);
	if (io_submit(ctx, 1, cbp) != 1) {
		fprintf(stderr, "io_submit(read) failed\n");
		goto out;
	}
	if (io_getevents(ctx, 1, 1, &e, &tms) != 1) {
		fprintf(stderr, "io_getevents(read) failed\n");
		goto out;
	}
	if ((long long)e.res != 4096 || buffer[0] != 'A' ||
	    buffer[4095] != 'A') {
		fprintf(stderr, "read verify failed (res=%lld)\n",
			(long long)e.res);
		goto out;
	}

	printf("DeepSpeed aio smoke test PASSED\n");
	ret = 0;
out:
	if (ctx != 0)
		io_destroy(ctx);
	if (fd != -1)
		aio_close(fd);
	_aligned_free(buffer);
	_unlink("aio_deepspeed_test.bin");
	return ret;
}
