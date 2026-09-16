/* include/libaio.h -- Windows port of the libaio public interface.
 *
 * Derived from libaio 0.3.113 (https://pagure.io/libaio.git).  Upstream
 * is a Linux-only userspace wrapper around the kernel AIO syscalls; this
 * port keeps the same API names, structure layout and semantics on
 * Windows, where the backend is implemented on top of OVERLAPPED I/O and
 * I/O completion ports instead.  See README.md in the port tree for the
 * full Linux-syscall -> Windows-API mapping.
 *
 * Upstream header copyright 2000,2001,2002 Red Hat, Inc.
 * (Benjamin LaHaise <bcrl@redhat.com>).  Windows port changes:
 *   - PADDED/PADDEDptr/PADDEDul defined for Win32/Win64 (unsigned long
 *     is 32-bit on Windows, so 64-bit fields use unsigned long long to
 *     keep the same layout as Linux x86_64);
 *   - <time.h>/<io.h>/<fcntl.h> pulled in, struct iovec provided;
 *   - io_pgetevents() takes void *sigmask (no POSIX sigset_t);
 *   - AIO_API dllexport/dllimport decoration + MSVC autolink pragma;
 *   - aio_open()/aio_close() extension for opening files with
 *     O_DIRECT / async-friendly flags on Windows.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */
#ifndef __LIBAIO_H
#define __LIBAIO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#if defined(_WIN32)
#include <time.h>		/* struct timespec (UCRT) */
#include <io.h>			/* _open_osfhandle, used by aio_open() */
#include <fcntl.h>		/* O_RDONLY/O_CREAT/... */
#else
#include <sys/types.h>
#include <signal.h>
struct timespec;
#endif

/* struct iovec: MSVC has none; MinGW-w64 provides it in <sys/uio.h>. */
#if defined(_WIN32)
#if defined(__MINGW32__)
#include <sys/uio.h>
#else
struct iovec { void *iov_base; size_t iov_len; };
#endif
#endif

/* dllexport/dllimport plumbing */
#if !defined(_WIN32) || defined(LIBAIO_STATIC)
#define AIO_API
#elif defined(LIBAIO_BUILDING)
#define AIO_API __declspec(dllexport)
#else
#define AIO_API __declspec(dllimport)
#endif

#if defined(_MSC_VER) && defined(_WIN32) && !defined(LIBAIO_BUILDING) && \
    !defined(LIBAIO_STATIC) && !defined(LIBAIO_NO_AUTOLINK)
#pragma comment(lib, "aio.lib")
#endif

typedef struct io_context *io_context_t;

typedef enum io_iocb_cmd {
	IO_CMD_PREAD = 0,
	IO_CMD_PWRITE = 1,

	IO_CMD_FSYNC = 2,
	IO_CMD_FDSYNC = 3,

	IO_CMD_POLL = 5,
	IO_CMD_NOOP = 6,
	IO_CMD_PREADV = 7,
	IO_CMD_PWRITEV = 8,
} io_iocb_cmd_t;

/*
 * struct layout: the Win32/Win64 cases below keep the same field sizes
 * and offsets that upstream produces on Linux x86_64, so sources (and
 * binary expectations, e.g. DeepSpeed's struct iocb usage) behave the
 * same.  Note `unsigned long` is 32-bit on Windows, hence PADDEDul
 * widens to unsigned long long.
 */
#if defined(_WIN64)
#define PADDED(x, y)	x, y
#define PADDEDptr(x, y)	x
#define PADDEDul(x, y)	unsigned long long x

#elif defined(_WIN32)
#define PADDED(x, y)	x; unsigned y
#define PADDEDptr(x, y)	x; unsigned y
#define PADDEDul(x, y)	unsigned long long x

/* little endian, 32 bits */
#elif defined(__i386__) || (defined(__arm__) && !defined(__ARMEB__)) || \
    defined(__sh__) || defined(__bfin__) || defined(__MIPSEL__) || \
    defined(__cris__) || defined(__loongarch32) || \
    (defined(__riscv) && __riscv_xlen == 32) || \
    (defined(__GNUC__) && defined(__BYTE_ORDER__) && \
         __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ && __SIZEOF_LONG__ == 4)
#define PADDED(x, y)	x; unsigned y
#define PADDEDptr(x, y)	x; unsigned y
#define PADDEDul(x, y)	unsigned long x; unsigned y

/* little endian, 64 bits */
#elif defined(__ia64__) || defined(__x86_64__) || defined(__alpha__) || \
      (defined(__aarch64__) && defined(__AARCH64EL__)) || \
      defined(__loongarch64) || \
      (defined(__riscv) && __riscv_xlen == 64) || \
      (defined(__GNUC__) && defined(__BYTE_ORDER__) && \
          __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ && __SIZEOF_LONG__ == 8)
#define PADDED(x, y)	x, y
#define PADDEDptr(x, y)	x
#define PADDEDul(x, y)	unsigned long x

/* big endian, 64 bits */
#elif defined(__powerpc64__) || defined(__s390x__) || \
      (defined(__sparc__) && defined(__arch64__)) || \
      (defined(__aarch64__) && defined(__AARCH64EB__)) || \
      (defined(__GNUC__) && defined(__BYTE_ORDER__) && \
           __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__ && __SIZEOF_LONG__ == 8)
#define PADDED(x, y)	unsigned y; x
#define PADDEDptr(x,y)	x
#define PADDEDul(x, y)	unsigned long x

/* big endian, 32 bits */
#elif defined(__PPC__) || defined(__s390__) || \
      (defined(__arm__) && defined(__ARMEB__)) || \
      defined(__sparc__) || defined(__MIPSEB__) || defined(__m68k__) || \
      defined(__hppa__) || defined(__frv__) || defined(__avr32__) || \
      (defined(__GNUC__) && defined(__BYTE_ORDER__) && \
           __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__ && __SIZEOF_LONG__ == 4)
#define PADDED(x, y)	unsigned y; x
#define PADDEDptr(x, y)	unsigned y; x
#define PADDEDul(x, y)	unsigned y; unsigned long x

#else
#error	endian?
#endif

struct io_iocb_poll {
	PADDED(int events, __pad1);
};	/* result code is the set of result flags or -'ve errno */

struct io_iocb_sockaddr {
	PADDEDptr(struct sockaddr *addr, __pad1);
	PADDEDul(len, __pad2);
};	/* result code is the length of the sockaddr, or -'ve errno */

struct io_iocb_common {
	PADDEDptr(void	*buf, __pad1);
	PADDEDul(nbytes, __pad2);
	long long	offset;
	long long	__pad3;
	unsigned	flags;
	unsigned	resfd;
};	/* result code is the amount read or -'ve errno */

struct io_iocb_vector {
	PADDEDptr(const struct iovec *vec, __pad1);
	PADDEDul(nr, __pad2);
	long long		offset;
};	/* result code is the amount read or -'ve errno */

struct iocb {
	PADDEDptr(void *data, __pad1);	/* Return in the io completion event */
	/* key: For use in identifying io requests */
	/* aio_rw_flags: RWF_* flags (such as RWF_NOWAIT) */
	PADDED(unsigned key, aio_rw_flags);

	short		aio_lio_opcode;
	short		aio_reqprio;
	int		aio_fildes;

	union {
		struct io_iocb_common	c;
		struct io_iocb_vector	v;
		struct io_iocb_poll	poll;
		struct io_iocb_sockaddr	saddr;
	} u;
};

struct io_event {
	PADDEDptr(void *data, __pad1);
	PADDEDptr(struct iocb *obj,  __pad2);
	PADDEDul(res,  __pad3);
	PADDEDul(res2, __pad4);
};

#undef PADDED
#undef PADDEDptr
#undef PADDEDul

typedef void (*io_callback_t)(io_context_t ctx, struct iocb *iocb, long res, long res2);

/* library wrappers */
AIO_API extern int io_queue_init(int maxevents, io_context_t *ctxp);
/*extern int io_queue_grow(io_context_t ctx, int new_maxevents);*/
AIO_API extern int io_queue_release(io_context_t ctx);
AIO_API extern int io_queue_wait(io_context_t ctx, struct timespec *timeout);
AIO_API extern int io_queue_run(io_context_t ctx);

/* Actual syscalls (Windows: IOCP-backed operations) */
AIO_API extern int io_setup(int maxevents, io_context_t *ctxp);
AIO_API extern int io_destroy(io_context_t ctx);
AIO_API extern int io_submit(io_context_t ctx, long nr, struct iocb *ios[]);
AIO_API extern int io_cancel(io_context_t ctx, struct iocb *iocb, struct io_event *evt);
AIO_API extern int io_getevents(io_context_t ctx, long min_nr, long nr, struct io_event *events, struct timespec *timeout);
#if defined(_WIN32)
/* no POSIX sigset_t on Windows; sigmask is ignored */
AIO_API extern int io_pgetevents(io_context_t ctx, long min_nr, long nr,
		struct io_event *events, struct timespec *timeout,
		void *sigmask);
#else
AIO_API extern int io_pgetevents(io_context_t ctx, long min_nr, long nr,
		struct io_event *events, struct timespec *timeout,
		sigset_t *sigmask);
#endif

#if defined(_WIN32)
/*
 * Windows port extension.  On Linux, O_DIRECT lives in the open() flags
 * and libaio consumes whatever fd the caller opened.  On Windows the
 * async/uncached behaviour is also decided at CreateFile time, so the
 * port provides aio_open()/aio_close(): the same open(2)-style flags,
 * backed by FILE_FLAG_OVERLAPPED plus FILE_FLAG_NO_BUFFERING for
 * O_DIRECT.  fds opened this way take the fast path (no per-request
 * handle reopen).  fds from CRT open()/_open() still work: io_submit()
 * reopens them per request with FILE_FLAG_OVERLAPPED, and as a last
 * resort a worker thread emulates the async op with blocking I/O.
 *
 * O_DIRECT/O_SYNC have no MSVC <fcntl.h> definition; private bit values
 * are defined here (no collision with the CRT's O_* bits).
 */
#if !defined(O_DIRECT)
#define O_DIRECT	0x01000000
#endif
#if !defined(O_SYNC)
#define O_SYNC		0x02000000
#endif

AIO_API extern int aio_open(const char *path, int flags, ...);
AIO_API extern int aio_close(int fd);
#endif

static inline void io_set_callback(struct iocb *iocb, io_callback_t cb)
{
	iocb->data = (void *)cb;
}

static inline void io_prep_pread(struct iocb *iocb, int fd, void *buf, size_t count, long long offset)
{
	memset(iocb, 0, sizeof(*iocb));
	iocb->aio_fildes = fd;
	iocb->aio_lio_opcode = IO_CMD_PREAD;
	iocb->aio_reqprio = 0;
	iocb->u.c.buf = buf;
	iocb->u.c.nbytes = count;
	iocb->u.c.offset = offset;
}

static inline void io_prep_pwrite(struct iocb *iocb, int fd, void *buf, size_t count, long long offset)
{
	memset(iocb, 0, sizeof(*iocb));
	iocb->aio_fildes = fd;
	iocb->aio_lio_opcode = IO_CMD_PWRITE;
	iocb->aio_reqprio = 0;
	iocb->u.c.buf = buf;
	iocb->u.c.nbytes = count;
	iocb->u.c.offset = offset;
}

static inline void io_prep_preadv(struct iocb *iocb, int fd, const struct iovec *iov, int iovcnt, long long offset)
{
	memset(iocb, 0, sizeof(*iocb));
	iocb->aio_fildes = fd;
	iocb->aio_lio_opcode = IO_CMD_PREADV;
	iocb->aio_reqprio = 0;
	iocb->u.c.buf = (void *)iov;
	iocb->u.c.nbytes = iovcnt;
	iocb->u.c.offset = offset;
}

static inline void io_prep_pwritev(struct iocb *iocb, int fd, const struct iovec *iov, int iovcnt, long long offset)
{
	memset(iocb, 0, sizeof(*iocb));
	iocb->aio_fildes = fd;
	iocb->aio_lio_opcode = IO_CMD_PWRITEV;
	iocb->aio_reqprio = 0;
	iocb->u.c.buf = (void *)iov;
	iocb->u.c.nbytes = iovcnt;
	iocb->u.c.offset = offset;
}

static inline void io_prep_preadv2(struct iocb *iocb, int fd, const struct iovec *iov, int iovcnt, long long offset, int flags)
{
	memset(iocb, 0, sizeof(*iocb));
	iocb->aio_fildes = fd;
	iocb->aio_lio_opcode = IO_CMD_PREADV;
	iocb->aio_reqprio = 0;
	iocb->aio_rw_flags = flags;
	iocb->u.c.buf = (void *)iov;
	iocb->u.c.nbytes = iovcnt;
	iocb->u.c.offset = offset;
}

static inline void io_prep_pwritev2(struct iocb *iocb, int fd, const struct iovec *iov, int iovcnt, long long offset, int flags)
{
	memset(iocb, 0, sizeof(*iocb));
	iocb->aio_fildes = fd;
	iocb->aio_lio_opcode = IO_CMD_PWRITEV;
	iocb->aio_reqprio = 0;
	iocb->aio_rw_flags = flags;
	iocb->u.c.buf = (void *)iov;
	iocb->u.c.nbytes = iovcnt;
	iocb->u.c.offset = offset;
}

static inline void io_prep_poll(struct iocb *iocb, int fd, int events)
{
        memset(iocb, 0, sizeof(*iocb));
        iocb->aio_fildes = fd;
        iocb->aio_lio_opcode = IO_CMD_POLL;
        iocb->aio_reqprio = 0;
        iocb->u.poll.events = events;
}

static inline int io_poll(io_context_t ctx, struct iocb *iocb, io_callback_t cb, int fd, int events)
{
        io_prep_poll(iocb, fd, events);
        io_set_callback(iocb, cb);
        return io_submit(ctx, 1, &iocb);
}

static inline void io_prep_fsync(struct iocb *iocb, int fd)
{
	memset(iocb, 0, sizeof(*iocb));
	iocb->aio_fildes = fd;
	iocb->aio_lio_opcode = IO_CMD_FSYNC;
	iocb->aio_reqprio = 0;
}

static inline int io_fsync(io_context_t ctx, struct iocb *iocb, io_callback_t cb, int fd)
{
	io_prep_fsync(iocb, fd);
	io_set_callback(iocb, cb);
	return io_submit(ctx, 1, &iocb);
}

static inline void io_prep_fdsync(struct iocb *iocb, int fd)
{
	memset(iocb, 0, sizeof(*iocb));
	iocb->aio_fildes = fd;
	iocb->aio_lio_opcode = IO_CMD_FDSYNC;
	iocb->aio_reqprio = 0;
}

static inline int io_fdsync(io_context_t ctx, struct iocb *iocb, io_callback_t cb, int fd)
{
	io_prep_fdsync(iocb, fd);
	io_set_callback(iocb, cb);
	return io_submit(ctx, 1, &iocb);
}

static inline void io_set_eventfd(struct iocb *iocb, int eventfd)
{
	iocb->u.c.flags |= (1 << 0) /* IOCB_FLAG_RESFD */;
	iocb->u.c.resfd = eventfd;
}

#ifdef __cplusplus
}
#endif

#endif /* __LIBAIO_H */
