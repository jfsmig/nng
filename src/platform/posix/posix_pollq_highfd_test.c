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
// lowered to soft, then restores the limit.  A non-zero pollers fixes the
// number of poller threads.
static void
reinit_with_limit(rlim_t soft, int16_t pollers)
{
	nng_init_params params = { 0 };
	struct rlimit   saved;
	struct rlimit   lowered;

	NUTS_ASSERT(getrlimit(RLIMIT_NOFILE, &saved) == 0);
	lowered          = saved;
	lowered.rlim_cur = soft;

	params.num_poller_threads = pollers;
	nng_fini();
	NUTS_ASSERT(setrlimit(RLIMIT_NOFILE, &lowered) == 0);
	NUTS_PASS(nng_init(&params));
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

	reinit_with_limit(INIT_LIMIT, 0);

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

// With the poll poller, a poller thread watches at most as many descriptors
// as its table holds, and its table is sized for the limit in force at
// initialization.  Past that, registrations must fail cleanly, and freed
// descriptors must give their slot back.
#define CAPACITY_LIMIT 128
#define CAPACITY_STREAMS CAPACITY_LIMIT

// A build that defines NNG_MAX_OPEN sizes the tables from it instead of the
// limit, so the exact count is only known without it.
#if defined(NNG_POLLQ_POLL) && (!defined(NNG_MAX_OPEN) || !NNG_MAX_OPEN)
#define CAPACITY_FROM_LIMIT 1
#endif

// accept_socket hands one end of a new socketpair to the listener and
// accepts it.  It returns the result, and on success the stream and the
// other end of the pair.
static nng_err
accept_socket(nng_stream_listener *l, nng_aio *aio, nng_stream **sp, int *peer)
{
	int     fds[2];
	nng_err rv;

	NUTS_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
	NUTS_PASS(nng_stream_listener_set_int(l, NNG_OPT_SOCKET_FD, fds[0]));
	nng_stream_listener_accept(l, aio);
	nng_aio_wait(aio);
	if ((rv = nng_aio_result(aio)) != NNG_OK) {
		(void) close(fds[1]);
		return (rv);
	}
	*sp   = nng_aio_get_output(aio, 0);
	*peer = fds[1];
	return (NNG_OK);
}

void
test_pollq_capacity(void)
{
	nng_stream_listener *l;
	nng_stream          *streams[CAPACITY_STREAMS];
	nng_aio             *aios[CAPACITY_STREAMS];
	int                  peers[CAPACITY_STREAMS];
	char                 bufs[CAPACITY_STREAMS][4];
	nng_iov              iov;
	nng_stream          *s;
	int                  peer;
	int                  n = 0;
	nng_err              rv;

	// Each stream uses the two descriptors of a socketpair.
	if (!ensure_limit_above(2 * CAPACITY_STREAMS + 32)) {
		NUTS_SKIP("cannot open enough descriptors");
		return;
	}
	// A single poller thread, so that all the streams share its table.
	reinit_with_limit(CAPACITY_LIMIT, 1);

	NUTS_PASS(nng_stream_listener_alloc(&l, "socket://"));
	NUTS_PASS(nng_stream_listener_listen(l));
	for (int i = 0; i < CAPACITY_STREAMS; i++) {
		NUTS_PASS(nng_aio_alloc(&aios[n], NULL, NULL));
		nng_aio_set_timeout(aios[n], 5000);
		rv = accept_socket(l, aios[n], &streams[n], &peers[n]);
#ifdef CAPACITY_FROM_LIMIT
		// The waker pipe takes one slot of the table.
		if (i == CAPACITY_LIMIT - 1) {
			NUTS_FAIL(rv, NNG_ENOFILES);
		} else {
			NUTS_PASS(rv);
		}
#else
		NUTS_PASS(rv);
#endif
		if (rv != NNG_OK) {
			nng_aio_free(aios[n]);
			continue;
		}
		n++;
	}

	// Post every receive before any data exists, so that all the
	// accepted descriptors are watched by the poller at the same time.
	for (int i = 0; i < n; i++) {
		iov.iov_buf = bufs[i];
		iov.iov_len = sizeof(bufs[i]);
		NUTS_PASS(nng_aio_set_iov(aios[i], 1, &iov));
		nng_stream_recv(streams[i], aios[i]);
	}
	nng_msleep(50);
	for (int i = 0; i < n; i++) {
		NUTS_TRUE(write(peers[i], "ping", 4) == 4);
	}
	for (int i = 0; i < n; i++) {
		nng_aio_wait(aios[i]);
		NUTS_PASS(nng_aio_result(aios[i]));
		NUTS_TRUE(nng_aio_count(aios[i]) == 4);
		NUTS_TRUE(memcmp(bufs[i], "ping", 4) == 0);
	}

	// Freed streams give their slots back, once they have been reaped.
	for (int i = 0; i < n; i++) {
		nng_stream_free(streams[i]);
		(void) close(peers[i]);
	}
	for (int tries = 0; tries < 100; tries++) {
		if ((rv = accept_socket(l, aios[0], &s, &peer)) !=
		    NNG_ENOFILES) {
			break;
		}
		nng_msleep(10);
	}
	NUTS_PASS(rv);
	nng_stream_free(s);
	(void) close(peer);

	for (int i = 0; i < n; i++) {
		nng_aio_free(aios[i]);
	}
	nng_stream_listener_free(l);
}

NUTS_TESTS = {
	{ "pollq descriptor above the limit at init",
	    test_pollq_fd_above_init_limit },
	{ "pollq poller capacity", test_pollq_capacity },
	{ NULL, NULL },
};
