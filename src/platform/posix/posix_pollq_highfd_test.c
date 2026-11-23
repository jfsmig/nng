//
// Copyright 2026 Staysail Systems, Inc. <info@staysail.tech>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include <fcntl.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nuts.h>

// Pollers may size their tables from the open-file limit in force when the
// library is initialized.  Descriptors numbered above that limit (inherited,
// or opened after the limit was raised) must work like any other.

#define HIGH_FD 300    // below FD_SETSIZE, so the select poller qualifies
#define INIT_LIMIT 256 // the limit in force at initialization
#define WANTED_LIMIT 512

// ensure_limit_above makes sure descriptor fd can be opened, raising the
// soft limit if needed.  It returns false if that is not allowed.
static bool
ensure_limit_above(int fd)
{
	struct rlimit limit;

	if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
		return (false);
	}
	if ((limit.rlim_cur == RLIM_INFINITY) ||
	    (limit.rlim_cur > (rlim_t) fd)) {
		return (true);
	}
	if ((limit.rlim_max != RLIM_INFINITY) &&
	    (limit.rlim_max < WANTED_LIMIT)) {
		return (false);
	}
	limit.rlim_cur = WANTED_LIMIT;
	return (setrlimit(RLIMIT_NOFILE, &limit) == 0);
}

// reinit_with_limit restarts the library while the soft open-file limit is
// lowered to soft, then restores the limit.
static void
reinit_with_limit(rlim_t soft)
{
	struct rlimit saved;
	struct rlimit lowered;

	NUTS_ASSERT(getrlimit(RLIMIT_NOFILE, &saved) == 0);
	lowered          = saved;
	lowered.rlim_cur = soft;

	nng_fini();
	NUTS_ASSERT(setrlimit(RLIMIT_NOFILE, &lowered) == 0);
	NUTS_PASS(nng_init(NULL));
	NUTS_ASSERT(setrlimit(RLIMIT_NOFILE, &saved) == 0);
}

void
test_pollq_fd_above_init_limit(void)
{
	nng_stream_listener *l;
	nng_stream          *s;
	nng_aio             *aio;
	nng_iov              iov;
	char                 buf[4];
	int                  fds[2];
	int                  high;

	if (!ensure_limit_above(HIGH_FD)) {
		NUTS_SKIP("cannot open enough descriptors");
		return;
	}
	NUTS_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
	NUTS_ASSERT((high = fcntl(fds[0], F_DUPFD, HIGH_FD)) >= HIGH_FD);
	(void) close(fds[0]);

	reinit_with_limit(INIT_LIMIT);

	NUTS_PASS(nng_aio_alloc(&aio, NULL, NULL));
	nng_aio_set_timeout(aio, 5000);
	NUTS_PASS(nng_stream_listener_alloc(&l, "socket://"));
	NUTS_PASS(nng_stream_listener_listen(l));
	NUTS_PASS(nng_stream_listener_set_int(l, NNG_OPT_SOCKET_FD, high));
	nng_stream_listener_accept(l, aio);
	nng_aio_wait(aio);
	NUTS_PASS(nng_aio_result(aio));
	s = nng_aio_get_output(aio, 0);

	// Post the receive before the data exists, so that it has to wait
	// for the poller to report the descriptor readable.
	iov.iov_buf = buf;
	iov.iov_len = sizeof(buf);
	NUTS_PASS(nng_aio_set_iov(aio, 1, &iov));
	nng_stream_recv(s, aio);
	nng_msleep(50);
	NUTS_TRUE(write(fds[1], "ping", 4) == 4);
	nng_aio_wait(aio);
	NUTS_PASS(nng_aio_result(aio));
	NUTS_TRUE(nng_aio_count(aio) == 4);
	NUTS_TRUE(memcmp(buf, "ping", 4) == 0);

	nng_stream_free(s);
	nng_stream_listener_free(l);
	nng_aio_free(aio);
	(void) close(fds[1]);
}

NUTS_TESTS = {
	{ "pollq descriptor above the limit at init",
	    test_pollq_fd_above_init_limit },
	{ NULL, NULL },
};
