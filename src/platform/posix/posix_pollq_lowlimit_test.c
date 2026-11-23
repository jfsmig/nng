//
// Copyright 2026 Staysail Systems, Inc. <info@staysail.tech>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nuts.h>

// Pollers may size their tables from the open-file limit in force when the
// library is initialized.  With a low limit, a poller must still be able to
// watch as many descriptors as that limit allows.

#define LOW_LIMIT 100
#define NSTREAMS (LOW_LIMIT - 1) // the waker pipe may take one slot

void
test_pollq_low_limit_at_init(void)
{
	nng_init_params      params = { 0 };
	struct rlimit        saved;
	struct rlimit        lowered;
	nng_stream_listener *l;
	nng_stream          *streams[NSTREAMS];
	nng_aio             *aios[NSTREAMS];
	int                  peers[NSTREAMS];
	char                 bufs[NSTREAMS][4];
	nng_iov              iov;

	// Each stream uses the two descriptors of a socketpair, so the
	// limit must be back above twice NSTREAMS once nng is initialized.
	// No check may run before a skip, so test these by hand.
	if ((getrlimit(RLIMIT_NOFILE, &saved) != 0) ||
	    ((saved.rlim_cur != RLIM_INFINITY) &&
	        (saved.rlim_cur < 2 * NSTREAMS + 64))) {
		NUTS_SKIP("open-file limit too low");
		return;
	}
	lowered          = saved;
	lowered.rlim_cur = LOW_LIMIT;

	// Restart the library under the low limit, with a single poller
	// thread so that all the descriptors share the same table, then
	// restore the limit so that the test can open enough descriptors.
	nng_fini();
	NUTS_ASSERT(setrlimit(RLIMIT_NOFILE, &lowered) == 0);
	params.num_poller_threads = 1;
	NUTS_PASS(nng_init(&params));
	NUTS_ASSERT(setrlimit(RLIMIT_NOFILE, &saved) == 0);

	NUTS_PASS(nng_stream_listener_alloc(&l, "socket://"));
	NUTS_PASS(nng_stream_listener_listen(l));
	for (int i = 0; i < NSTREAMS; i++) {
		int fds[2];

		NUTS_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
		peers[i] = fds[1];
		NUTS_PASS(nng_aio_alloc(&aios[i], NULL, NULL));
		nng_aio_set_timeout(aios[i], 5000);
		NUTS_PASS(
		    nng_stream_listener_set_int(l, NNG_OPT_SOCKET_FD, fds[0]));
		nng_stream_listener_accept(l, aios[i]);
		nng_aio_wait(aios[i]);
		NUTS_PASS(nng_aio_result(aios[i]));
		streams[i] = nng_aio_get_output(aios[i], 0);
	}

	// Post every receive before any data exists, so that all the
	// descriptors are watched by the poller at the same time.
	for (int i = 0; i < NSTREAMS; i++) {
		iov.iov_buf = bufs[i];
		iov.iov_len = sizeof(bufs[i]);
		NUTS_PASS(nng_aio_set_iov(aios[i], 1, &iov));
		nng_stream_recv(streams[i], aios[i]);
	}
	nng_msleep(50);
	for (int i = 0; i < NSTREAMS; i++) {
		NUTS_TRUE(write(peers[i], "ping", 4) == 4);
	}
	for (int i = 0; i < NSTREAMS; i++) {
		nng_aio_wait(aios[i]);
		NUTS_PASS(nng_aio_result(aios[i]));
		NUTS_TRUE(nng_aio_count(aios[i]) == 4);
		NUTS_TRUE(memcmp(bufs[i], "ping", 4) == 0);
	}

	for (int i = 0; i < NSTREAMS; i++) {
		nng_stream_free(streams[i]);
		nng_aio_free(aios[i]);
		(void) close(peers[i]);
	}
	nng_stream_listener_free(l);
}

NUTS_TESTS = {
	{ "pollq low open-file limit at init", test_pollq_low_limit_at_init },
	{ NULL, NULL },
};
